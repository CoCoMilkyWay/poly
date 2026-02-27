#include "stage2_builder.hpp"
#include <algorithm>

namespace stage2 {

TransferClass EventBuilder::classify_and_emit(
    int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
    int64_t block, const std::string &op,
    const std::string &from, const std::string &to,
    const std::string &token_id, int64_t amount,
    uint32_t cond_idx, uint8_t token_idx, Collateral collateral) {

  stage2_assert(amount >= 0, AssertLevel::L0, "Input", "NonNegativeAmount");

  // Validate token identity before any semantic matching.
  bool known_token = (cond_idx != UNKNOWN_COND_IDX);
  uint8_t outcome_cnt = 0;
  if (known_token) {
    stage2_assert(cond_idx < conditions_.size(), AssertLevel::L0, "Input", "CondIdxInRange");
    outcome_cnt = conditions_[cond_idx].outcome_count;
    stage2_assert(outcome_cnt > 0, AssertLevel::L0, "Input", "OutcomeCountPositive");
    stage2_assert(token_idx < outcome_cnt, AssertLevel::L0, "Input", "TokenIdxInOutcomeRange");
  } else {
    stage2_assert(token_idx == UNKNOWN_TOKEN_IDX, AssertLevel::L0, "Input", "UnknownTokenIdx255");
  }
  stage2_assert(from != to, AssertLevel::L0, "Input", "FromToDifferent");

  uint8_t coll = static_cast<uint8_t>(collateral);
  std::string cond_id = known_token ? cond_ids_[cond_idx] : "";
  TxKey tx_key{block, tx_hash};
  TxTokenKey tx_token_key{block, tx_hash, token_id};
  bool op_is_exchange = (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE);
  bool op_is_fpmm = (fpmm_map_.find(op) != fpmm_map_.end());
  int64_t transfer_log_index = sort_key - block * SORT_KEY_SCALE;
  stage2_assert(transfer_log_index >= 0, AssertLevel::L0, "Input", "TransferLogIndexNonNegative");
  int64_t sub_idx = transfer_log_index % TRANSFER_FLAT_LOG_SCALE;
  stage2_assert(sub_idx >= 0 && sub_idx < TRANSFER_FLAT_LOG_SCALE, AssertLevel::L0, "Input", "TransferSubIdxRange");
  int64_t base_log_index = transfer_log_index / TRANSFER_FLAT_LOG_SCALE;

  enum class RootOpType : uint8_t {
    None = 0,
    Split = 1,
    Merge = 2,
    Redemption = 3,
    Convert = 4,
    Order = 5,
    FPMMTrade = 6,
    FPMMFunding = 7,
  };
  struct RootBind {
    RootOpType op = RootOpType::None;
    const char *leg = "none";
  };
  RootBind root_bind;
  auto bind_root = [&](RootOpType op_type, const char *leg) {
    stage2_assert(root_bind.op == RootOpType::None, AssertLevel::L2, "Bind",
                  "TransferBindAtMostOneRootOp", root_bind.leg);
    root_bind.op = op_type;
    root_bind.leg = leg;
  };

  int64_t active_semantic_log = -1;
  auto op_bounds_it = tx_op_bounds_.find(tx_key);
  if (op_bounds_it != tx_op_bounds_.end()) {
    const auto &bounds = op_bounds_it->second;
    auto bound_it = std::lower_bound(
        bounds.begin(), bounds.end(), base_log_index,
        [](const TxOpBounds &b, int64_t transfer_base_log) {
          return b.right_inclusive < transfer_base_log;
        });
    if (bound_it != bounds.end()) {
      if (base_log_index > bound_it->left_exclusive &&
          base_log_index <= bound_it->right_inclusive) {
        active_semantic_log = bound_it->right_inclusive;
      }
    }
  }
  int64_t split_price = (known_token && is_usdc_collateral(collateral)) ? (1000000 / outcome_cnt) : 0;

  // Lambda: 匹配 Split/Merge/Redemption 时，已知 token 需要检查 cond_id
  auto cond_matches = [&](const std::string &info_cond_id) {
    return !known_token || cond_id == info_cond_id;
  };
  auto semantic_log_matches = [&](int64_t info_log_index) {
    return active_semantic_log >= 0 && info_log_index == active_semantic_log;
  };
  auto select_window_only = [&](auto *window_matched,
                                int window_match_count,
                                const char *window_rule) {
    stage2_assert(window_match_count <= 1, AssertLevel::L2, "Match", window_rule);
    return window_matched;
  };
  auto collateral_matches = [&](const std::string &semantic_collateral_addr) {
    if (semantic_collateral_addr.empty())
      return true;
    if (!known_token)
      return true;
    std::string current_coll = to_lower(std::string(collateral_addr(collateral)));
    if (current_coll == ZERO_ADDR)
      return true;
    return semantic_collateral_addr == current_coll;
  };
  auto is_user_addr = [&](const std::string &addr) { return !is_protocol_contract(addr); };
  auto emit_if_user = [&](const std::string &addr, const RawEvent &evt) {
    if (is_user_addr(addr))
      push_event(addr, evt);
  };
  auto usdc_price = [&](int64_t usdc_amount, int64_t token_amount) -> int64_t {
    return is_usdc_collateral(collateral) ? (usdc_amount * 1000000 / token_amount) : 0;
  };
  auto classify_transfer_by_counterparty = [&](TransferClass in_cls, TransferClass out_cls,
                                               TransferClass internal_cls) {
    if (is_user_addr(to))
      return in_cls;
    if (is_user_addr(from))
      return out_cls;
    return internal_cls;
  };
  auto find_split_info = [&](const std::string &stakeholder, int64_t amt) -> SplitInfo * {
    auto it = tx_split_.find(tx_key);
    if (it == tx_split_.end())
      return nullptr;
    SplitInfo *window_matched = nullptr;
    int window_match_count = 0;
    for (auto &info : it->second) {
      if (!collateral_matches(info.collateral_token))
        continue;
      if (info.stakeholder == stakeholder && info.amount == amt && cond_matches(info.cond_id)) {
        if (semantic_log_matches(info.log_index)) {
          window_matched = &info;
          window_match_count++;
        }
      }
    }
    return select_window_only(window_matched, window_match_count,
                              "SplitWindowUniqueCandidate");
  };
  auto find_merge_info = [&](const std::string &stakeholder, int64_t amt) -> MergeInfo * {
    auto it = tx_merge_.find(tx_key);
    if (it == tx_merge_.end())
      return nullptr;
    MergeInfo *window_matched = nullptr;
    int window_match_count = 0;
    for (auto &info : it->second) {
      if (!collateral_matches(info.collateral_token))
        continue;
      if (info.stakeholder == stakeholder && info.amount == amt && cond_matches(info.cond_id)) {
        if (semantic_log_matches(info.log_index)) {
          window_matched = &info;
          window_match_count++;
        }
      }
    }
    return select_window_only(window_matched, window_match_count,
                              "MergeWindowUniqueCandidate");
  };
  auto find_redemption_info = [&](const std::string &redeemer) -> RedemptionInfo * {
    auto it = tx_redemption_.find(tx_key);
    if (it == tx_redemption_.end())
      return nullptr;
    RedemptionInfo *window_matched = nullptr;
    int window_match_count = 0;
    for (auto &info : it->second) {
      if (!collateral_matches(info.collateral_token))
        continue;
      if (info.redeemer == redeemer && cond_matches(info.cond_id)) {
        if (semantic_log_matches(info.log_index)) {
          window_matched = &info;
          window_match_count++;
        }
      }
    }
    return select_window_only(window_matched, window_match_count,
                              "RedeemWindowUniqueCandidate");
  };
  auto find_fpmm_trade_info = [&](const TxFPMMKey &key, int side,
                                  const std::string &trader, int64_t token_amount) -> FPMMTradeInfo * {
    auto it = tx_fpmm_trade_.find(key);
    if (it == tx_fpmm_trade_.end())
      return nullptr;
    FPMMTradeInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (!info.requires_erc1155_leg)
        continue;
      if (info.consumed)
        continue;
      if (info.explained_without_direct_leg)
        continue;
      // FPMM emits FPMMBuy/FPMMSell after transfer legs in the same tx.
      // Only allow forward matching within this tx; never match past semantic logs.
      if (info.log_index < base_log_index)
        continue;
      if (info.side == side && info.trader == trader && info.tokens == token_amount) {
        matched = &info;
        match_count++;
      }
    }
    stage2_assert(match_count <= 1, AssertLevel::L2, "Match", "FPMMTradeUniqueCandidate");
    return matched;
  };
  auto mark_fpmm_trade_explained = [&](const TxFPMMKey &key, int side) -> bool {
    auto it = tx_fpmm_trade_.find(key);
    if (it == tx_fpmm_trade_.end())
      return false;
    FPMMTradeInfo *matched = nullptr;
    int64_t matched_log = 0;
    for (auto &info : it->second) {
      if (!info.requires_erc1155_leg)
        continue;
      if (info.log_index < base_log_index)
        continue;
      if (info.side != side)
        continue;
      if (info.consumed || info.explained_without_direct_leg)
        continue;
      if (matched == nullptr || info.log_index < matched_log) {
        matched = &info;
        matched_log = info.log_index;
        continue;
      }
      stage2_assert(info.log_index != matched_log, AssertLevel::L2, "Match",
                    "FPMMTradeExplainSameLogTie");
    }
    if (matched == nullptr)
      return false;
    matched->explained_without_direct_leg = true;
    return true;
  };
  auto has_pending_future_fpmm_trade_side = [&](const TxFPMMKey &key, int side) {
    auto it = tx_fpmm_trade_.find(key);
    if (it == tx_fpmm_trade_.end())
      return false;
    for (const auto &info : it->second) {
      if (!info.requires_erc1155_leg)
        continue;
      if (info.log_index < base_log_index)
        continue;
      if (info.side != side)
        continue;
      if (!info.consumed && !info.explained_without_direct_leg)
        return true;
    }
    return false;
  };
  auto funding_split_amount = [&](const FPMMFundingInfo &info) -> int64_t {
    stage2_assert(!info.amounts.empty(), AssertLevel::L0, "Input", "FundingAmountsNonEmpty");
    return *std::max_element(info.amounts.begin(), info.amounts.end());
  };
  auto funding_matches_remove_amount = [&](const FPMMFundingInfo &info, int64_t transfer_amount) {
    for (int64_t amount_i : info.amounts) {
      if (amount_i == transfer_amount) {
        return true;
      }
    }
    return false;
  };
  auto find_fpmm_funding_info = [&](const TxFPMMKey &key, int64_t transfer_amount,
                                    bool expect_refund) -> FPMMFundingInfo * {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end())
      return nullptr;
    FPMMFundingInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (info.log_index < base_log_index)
        continue;
      if (info.side != 1 || info.amounts.empty())
        continue;
      int64_t split_amount = funding_split_amount(info);
      if (!expect_refund) {
        if (split_amount == transfer_amount) {
          matched = &info;
          match_count++;
        }
        continue;
      }
      bool refund_match = false;
      if (known_token && token_idx != UNKNOWN_TOKEN_IDX &&
          token_idx < info.amounts.size()) {
        int64_t expected_refund = split_amount - info.amounts[token_idx];
        refund_match = (expected_refund == transfer_amount);
      } else {
        for (int64_t amount_i : info.amounts) {
          if (split_amount - amount_i == transfer_amount) {
            refund_match = true;
            break;
          }
        }
      }
      if (refund_match) {
        matched = &info;
        match_count++;
      }
    }
    stage2_assert(match_count <= 1, AssertLevel::L2, "Match", "FPMMFundingUniqueCandidate");
    return matched;
  };
  auto has_pending_future_fpmm_add_for_mint = [&](const TxFPMMKey &key, int64_t transfer_amount) {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end())
      return false;
    for (const auto &info : it->second) {
      if (info.log_index < base_log_index)
        continue;
      if (info.side != 1 || info.amounts.empty())
        continue;
      if (info.consumed_count != 0)
        continue;
      int64_t split_amount = funding_split_amount(info);
      if (split_amount == transfer_amount)
        return true;
    }
    return false;
  };
  auto has_pending_future_fpmm_add_for_refund = [&](const TxFPMMKey &key, int64_t transfer_amount) {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end())
      return false;
    for (const auto &info : it->second) {
      if (info.log_index < base_log_index)
        continue;
      if (info.side != 1 || info.amounts.empty())
        continue;
      if (info.consumed_count != 0)
        continue;
      int64_t split_amount = funding_split_amount(info);
      if (known_token && token_idx != UNKNOWN_TOKEN_IDX &&
          token_idx < info.amounts.size()) {
        int64_t expected_refund = split_amount - info.amounts[token_idx];
        if (expected_refund == transfer_amount)
          return true;
        continue;
      }
      for (int64_t amount_i : info.amounts) {
        if (split_amount - amount_i == transfer_amount)
          return true;
      }
    }
    return false;
  };
  auto has_pending_future_fpmm_remove_for_transfer = [&](const TxFPMMKey &key,
                                                         const std::string &funder,
                                                         int64_t transfer_amount) {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end())
      return false;
    for (const auto &info : it->second) {
      if (info.log_index < base_log_index)
        continue;
      if (info.side != 2)
        continue;
      if (info.funder != funder)
        continue;
      bool all_zero = true;
      for (int64_t amount_i : info.amounts) {
        if (amount_i != 0) {
          all_zero = false;
          break;
        }
      }
      if (all_zero || info.consumed_count != 0)
        continue;
      if (funding_matches_remove_amount(info, transfer_amount))
        return true;
    }
    return false;
  };
  auto find_fpmm_remove_info = [&](const TxFPMMKey &key, const std::string &funder,
                                   int64_t transfer_amount) -> FPMMFundingInfo * {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end())
      return nullptr;
    FPMMFundingInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (info.log_index < base_log_index)
        continue;
      if (info.side != 2)
        continue;
      if (info.funder != funder)
        continue;
      bool amount_match = funding_matches_remove_amount(info, transfer_amount);
      if (amount_match) {
        matched = &info;
        match_count++;
      }
    }
    stage2_assert(match_count <= 1, AssertLevel::L2, "Match", "FPMMRemoveUniqueCandidate");
    return matched;
  };
  auto order_leg_matches = [&](const OrderInfo &info) {
    if (info.maker_side == 1) {
      // BUY maker leg can be either:
      // - direct taker -> maker (fillOrder/fillOrders)
      // - exchange -> maker (matchOrders via _fillFacingExchange)
      return to == info.maker && (from == info.taker || from == op);
    }
    // SELL maker leg can be either:
    // - direct maker -> taker (fillOrder/fillOrders)
    // - maker -> exchange (matchOrders via _fillFacingExchange)
    return from == info.maker && (to == info.taker || to == op);
  };
  bool order_window_conflict = false;
  auto find_order_info = [&]() -> OrderInfo * {
    order_window_conflict = false;
    auto it = tx_order_.find(tx_token_key);
    if (it == tx_order_.end())
      return nullptr;
    OrderInfo *window_matched = nullptr;
    int window_match_count = 0;
    bool has_unconsumed_window_same_amount = false;
    OrderInfo *forward_matched = nullptr;
    int64_t forward_log = -1;
    int forward_same_log_count = 0;
    for (auto &info : it->second) {
      if (info.consumed)
        continue;
      if (info.tokens != amount)
        continue;
      bool in_window = semantic_log_matches(info.log_index);
      if (in_window) {
        has_unconsumed_window_same_amount = true;
      }
      bool addr_match = order_leg_matches(info);
      if (!addr_match)
        continue;
      if (in_window) {
        window_matched = &info;
        window_match_count++;
      }
      // In matchOrders paths, transfer legs can happen before the maker order's
      // own OrderFilled log. Allow nearest-future fallback when window misses.
      if (info.log_index >= base_log_index) {
        if (forward_matched == nullptr || info.log_index < forward_log) {
          forward_matched = &info;
          forward_log = info.log_index;
          forward_same_log_count = 1;
        } else if (info.log_index == forward_log) {
          forward_same_log_count++;
        }
      }
    }
    OrderInfo *window_only = select_window_only(window_matched, window_match_count,
                                                "OrderWindowUniqueCandidate");
    if (window_only != nullptr) {
      return window_only;
    }
    stage2_assert(forward_same_log_count <= 1, AssertLevel::L2, "Match",
                  "OrderForwardUniqueCandidate");
    // Only treat as hard conflict when the current semantic window has same-amount
    // order legs but none can match address constraints and no future fallback exists.
    order_window_conflict =
        has_unconsumed_window_same_amount && (forward_matched == nullptr);
    return forward_matched;
  };
  auto find_convert_info = [&](const std::string &market_id,
                               const std::string &stakeholder,
                               int64_t amt) -> ConvertInfo * {
    TxMarketKey tx_market_key{block, tx_hash, market_id};
    auto it = tx_convert_.find(tx_market_key);
    if (it == tx_convert_.end())
      return nullptr;
    ConvertInfo *window_matched = nullptr;
    int window_match_count = 0;
    for (auto &info : it->second) {
      if (info.stakeholder != stakeholder || info.amount != amt)
        continue;
      if (semantic_log_matches(info.log_index)) {
        window_matched = &info;
        window_match_count++;
      }
    }
    return select_window_only(window_matched, window_match_count,
                              "ConvertWindowUniqueCandidate");
  };
  auto consume_split = [&](SplitInfo *info) {
    if (info != nullptr)
      info->consumed_count++;
  };
  auto consume_merge = [&](MergeInfo *info) {
    if (info != nullptr)
      info->consumed_count++;
  };
  auto consume_redeem = [&](RedemptionInfo *info) {
    if (info != nullptr)
      info->consumed_count++;
  };
  auto cover_split = [&](SplitInfo *info) {
    if (info != nullptr)
      info->covered_by_parent = true;
  };
  auto cover_merge = [&](MergeInfo *info) {
    if (info != nullptr)
      info->covered_by_parent = true;
  };

  if (amount == 0) {
    auto fpmm_zero_it = fpmm_map_.find(op);
    if (fpmm_zero_it != fpmm_map_.end()) {
      TxFPMMKey tx_fpmm_key{block, tx_hash, op};
      if (to == op) {
        if (FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 2, from, 0); tit != nullptr) {
          bind_root(RootOpType::FPMMTrade, "sell_zero_to_pool");
          tit->consumed = true;
        }
      } else if (from == op) {
        if (FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 1, to, 0); tit != nullptr) {
          bind_root(RootOpType::FPMMTrade, "buy_zero_from_pool");
          tit->consumed = true;
        } else if (to == ZERO_ADDR) {
          bind_root(RootOpType::FPMMTrade, "sell_zero_internal_burn");
          (void)mark_fpmm_trade_explained(tx_fpmm_key, 2);
        }
      }
    }
    return TransferClass::InternalTransferZero;
  }

  // ========== mint 分支 (from == 0x0, 非FPMM operator) ==========
  if (from == ZERO_ADDR && !op_is_fpmm) {
    if (to == NEG_RISK_ADAPTER)
      return TransferClass::InternalMintNegRisk;

    if (MergeInfo *mit = find_merge_info(to, amount); mit != nullptr) {
      bind_root(RootOpType::Merge, "mint_parent");
      consume_merge(mit);
      if (known_token) {
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::MergeNormal, token_idx, coll, 0, amount, split_price});
        return TransferClass::MergeNormal;
      }
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::MergeNonPoly, token_idx, coll, 0, amount, split_price});
      return TransferClass::MergeNonPoly;
    }

    if (RedemptionInfo *rit = find_redemption_info(to); rit != nullptr) {
      bind_root(RootOpType::Redemption, "mint_parent");
      consume_redeem(rit);
      if (known_token) {
        int64_t payout_price = 0;
        if (is_usdc_collateral(collateral)) {
          auto &payouts = conditions_[cond_idx].payout_numerators;
          payout_price = (token_idx < payouts.size() && payouts[token_idx] >= 0) ? payouts[token_idx] : 1000000;
        }
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::Redemption, token_idx, coll, 0, amount, payout_price});
        return TransferClass::Redemption;
      }
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::RedemptionNonPoly, token_idx, coll, 0, amount, 0});
      return TransferClass::RedemptionNonPoly;
    }

    if (SplitInfo *split_match = find_split_info(to, amount); split_match != nullptr) {
      bind_root(RootOpType::Split, "mint_child");
      consume_split(split_match);
      if (known_token) {
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::SplitNormal, token_idx, coll, 0, amount, split_price});
        return TransferClass::SplitNormal;
      } else {
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::SplitNonPoly, token_idx, coll, 0, amount, split_price});
        return TransferClass::SplitNonPoly;
      }
    }

    if (known_token) {
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInOther, token_idx, coll, 0, amount, 0});
      return classify_transfer_by_counterparty(TransferClass::TransferInOther,
                                               TransferClass::TransferOutOther,
                                               TransferClass::InternalTransferOther);
    } else {
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInNonPoly, token_idx, coll, 0, amount, 0});
      return classify_transfer_by_counterparty(TransferClass::TransferInNonPoly,
                                               TransferClass::TransferOutNonPoly,
                                               TransferClass::InternalTransferOther);
    }
  }

  // ========== burn 分支 (to == 0x0, 非FPMM operator) ==========
  if (to == ZERO_ADDR && !op_is_fpmm) {
    if (SplitInfo *sit = find_split_info(from, amount); sit != nullptr) {
      bind_root(RootOpType::Split, "burn_parent");
      consume_split(sit);
      if (known_token) {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::SplitNormal, token_idx, coll, 0, -amount, split_price});
        return TransferClass::SplitNormal;
      }
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::SplitNonPoly, token_idx, coll, 0, -amount, split_price});
      return TransferClass::SplitNonPoly;
    }

    if (from == NEG_RISK_ADAPTER)
      return TransferClass::InternalBurnNegRisk;

    if (MergeInfo *merge_match = find_merge_info(from, amount); merge_match != nullptr) {
      bind_root(RootOpType::Merge, "burn_child");
      consume_merge(merge_match);
      if (known_token) {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::MergeNormal, token_idx, coll, 0, -amount, split_price});
        return TransferClass::MergeNormal;
      } else {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::MergeNonPoly, token_idx, coll, 0, -amount, split_price});
        return TransferClass::MergeNonPoly;
      }
    }

    if (RedemptionInfo *rit = find_redemption_info(from); rit != nullptr) {
      bind_root(RootOpType::Redemption, "burn_child");
      consume_redeem(rit);
      if (known_token) {
        if (is_user_addr(from)) {
          auto &payouts = conditions_[cond_idx].payout_numerators;
          int64_t payout_price = is_usdc_collateral(collateral)
                                     ? ((token_idx < payouts.size() && payouts[token_idx] >= 0)
                                            ? payouts[token_idx]
                                            : 1000000)
                                     : 0;
          emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Redemption, token_idx, coll, 0, -amount, payout_price});
        }
        return TransferClass::Redemption;
      } else {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::RedemptionNonPoly, token_idx, coll, 0, -amount, 0});
        return TransferClass::RedemptionNonPoly;
      }
    }

    if (known_token) {
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutOther, token_idx, coll, 0, -amount, 0});
      return classify_transfer_by_counterparty(TransferClass::TransferInOther,
                                               TransferClass::TransferOutOther,
                                               TransferClass::InternalTransferOther);
    } else {
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutNonPoly, token_idx, coll, 0, -amount, 0});
      return classify_transfer_by_counterparty(TransferClass::TransferInNonPoly,
                                               TransferClass::TransferOutNonPoly,
                                               TransferClass::InternalTransferOther);
    }
  }

  // ========== Exchange operator ==========
  if (op_is_exchange) {
    OrderInfo *oit = find_order_info();
    if (oit != nullptr) {
      bind_root(RootOpType::Order, "exchange_transfer");
      stage2_assert(oit->tokens > 0, AssertLevel::L3, "Order", "TokensPositive");
      stage2_assert(amount == oit->tokens, AssertLevel::L3, "Order", "TransferAmountMatch");
      stage2_assert(order_leg_matches(*oit), AssertLevel::L3, "Order", "OrderLegAddressMatch");
      oit->consumed = true;

      int64_t price = usdc_price(oit->usdc, oit->tokens);
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::OrderBuy, token_idx, coll, 0, amount, price});
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::OrderSell, token_idx, coll, 0, -amount, price});
      return classify_transfer_by_counterparty(TransferClass::OrderBuy, TransferClass::OrderSell,
                                               TransferClass::InternalTransferOrder);
    }

    if (order_window_conflict) {
      std::cerr << "[ERROR] Exchange transfer conflicts with window order leg: block=" << block
                << ", op=" << op << ", from=" << from << ", to=" << to << std::endl;
      stage2_assert(false, AssertLevel::L3, "Order", "OrderWindowAddressConflict");
      return TransferClass::Unclassified;
    }
    // Without order semantics, fall through to generic transfer classification.
  }

  // ========== NegRisk Adapter operator ==========
  if (op == NEG_RISK_ADAPTER) {
    if (from == NEG_RISK_ADAPTER) {
      SplitInfo *split_info = find_split_info(NEG_RISK_ADAPTER, amount);
      if (split_info == nullptr) {
        auto sit = tx_split_.find(tx_key);
        if (sit != tx_split_.end()) {
          for (auto &info : sit->second) {
            if (!semantic_log_matches(info.log_index))
              continue;
            if (!collateral_matches(info.collateral_token))
              continue;
            if (info.stakeholder == NEG_RISK_ADAPTER && cond_matches(info.cond_id)) {
              split_info = &info;
              break;
            }
          }
        }
      }
      if (split_info != nullptr) {
        bind_root(RootOpType::Split, "adapter_out_split_child");
        consume_split(split_info);
        bool is_convert_output = false;
        if (known_token && token_idx == 0 && !conditions_[cond_idx].question_id.empty()) {
          auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
          if (market_it != cond_to_market_.end()) {
            TxMarketKey tx_market_key{block, tx_hash, market_it->second};
            is_convert_output = (tx_convert_.count(tx_market_key) > 0);
          }
        }
        if (is_convert_output) {
          stage2_assert(amount <= split_info->amount, AssertLevel::L3, "Convert", "YesOutputWithinSplit");
        } else {
          stage2_assert(amount == split_info->amount, AssertLevel::L3, "NegRisk", "SplitAmountMatch");
        }
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::SplitNegRisk, token_idx, coll, 0, amount, split_price});
        return TransferClass::SplitNegRisk;
      }
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInNegRisk, token_idx, coll, 0, amount, 0});
      return TransferClass::TransferInNegRisk;
    }

    if (to == NEG_RISK_ADAPTER) {
      if (MergeInfo *merge_info = find_merge_info(NEG_RISK_ADAPTER, amount); merge_info != nullptr) {
        bind_root(RootOpType::Merge, "adapter_in_merge_child");
        consume_merge(merge_info);
        (void)merge_info;
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::MergeNegRisk, token_idx, coll, 0, -amount, split_price});
        return TransferClass::MergeNegRisk;
      }
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutNegRisk, token_idx, coll, 0, -amount, 0});
      return TransferClass::TransferOutNegRisk;
    }

    if (to == NO_TOKEN_BURN_ADDRESS) {
      if (from == NEG_RISK_ADAPTER)
        return TransferClass::InternalBurnConvert;

      stage2_assert(known_token && token_idx == 1, AssertLevel::L3, "Convert", "BurnTokenIsNO");
      if (!conditions_[cond_idx].question_id.empty()) {
        auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
        if (market_it != cond_to_market_.end()) {
          if (ConvertInfo *ci = find_convert_info(market_it->second, from, amount); ci != nullptr) {
            bind_root(RootOpType::Convert, "convert_no_burn");
            ci->consumed_count++;
            emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Convert, token_idx, coll, 0, -amount, 0});
            return TransferClass::Convert;
          }
        }
      }
      std::cerr << "[ERROR] Convert burn without event: block=" << block
                << ", from=" << from << ", amount=" << amount << std::endl;
      stage2_assert(false, AssertLevel::L3, "Convert", "BurnWithoutSemanticEvent");
      return TransferClass::Unclassified;
    }
    stage2_assert(false, AssertLevel::L3, "NegRisk", "UnexpectedAdapterTransferPattern");
    return TransferClass::Unclassified;
  }

  // ========== FPMM operator ==========
  auto fpmm_it = fpmm_map_.find(op);
  if (fpmm_it != fpmm_map_.end()) {
    TxFPMMKey tx_fpmm_key{block, tx_hash, op};

    // 0x0 -> FPMM: LP add / internal split mint leg
    if (from == ZERO_ADDR && to == op) {
      SplitInfo *split_match = find_split_info(to, amount);
      FPMMFundingInfo *fit = find_fpmm_funding_info(tx_fpmm_key, amount, false);
      if (fit != nullptr) {
        bind_root(RootOpType::FPMMFunding, "lp_add_mint_to_pool");
        cover_split(split_match);
        int64_t split_amt = funding_split_amount(*fit);
        stage2_assert(amount == split_amt, AssertLevel::L3, "FPMMFunding", "LPAddSplitAmountMatch");
        fit->consumed_count++;
        if (known_token) {
          stage2_assert(token_idx < fit->amounts.size(), AssertLevel::L3, "FPMMFunding", "LPAddTokenIdxInRange");
          if (is_user_addr(fit->funder)) {
            int64_t pool_amt = fit->amounts[token_idx];
            RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, coll, 0, pool_amt, split_price};
            push_event(fit->funder, evt);
          }
          return TransferClass::FPMMLPAdd;
        }
        return TransferClass::InternalMintFPMM;
      }
      stage2_assert(!has_pending_future_fpmm_add_for_mint(tx_fpmm_key, amount),
                    AssertLevel::L3, "FPMMFunding", "LPAddMintLegMatchOrNoFunding");
      bind_root(RootOpType::FPMMTrade, "buy_internal_split_mint");
      cover_split(split_match);
      bool explained = mark_fpmm_trade_explained(tx_fpmm_key, 1);
      stage2_assert(explained || !has_pending_future_fpmm_trade_side(tx_fpmm_key, 1),
                    AssertLevel::L3, "FPMMTrade", "BuyExplainableOnInternalMint");
      return TransferClass::InternalMintFPMM;
    }

    // FPMM -> 0x0: internal merge burn leg
    if (from == op && to == ZERO_ADDR) {
      bind_root(RootOpType::FPMMTrade, "sell_internal_merge_burn");
      cover_merge(find_merge_info(from, amount));
      bool explained = mark_fpmm_trade_explained(tx_fpmm_key, 2);
      stage2_assert(explained || !has_pending_future_fpmm_trade_side(tx_fpmm_key, 2),
                    AssertLevel::L3, "FPMMTrade", "SellExplainableOnInternalBurn");
      return TransferClass::InternalBurnFPMM;
    }

    if (from == op) {
      FPMMFundingInfo *refund_info = find_fpmm_funding_info(tx_fpmm_key, amount, true);
      FPMMTradeInfo *buy_info = find_fpmm_trade_info(tx_fpmm_key, 1, to, amount);
      FPMMFundingInfo *remove_info = find_fpmm_remove_info(tx_fpmm_key, to, amount);

      enum class FromFPMMMatch { None,
                                 Refund,
                                 TradeBuy,
                                 Remove };
      FromFPMMMatch chosen = FromFPMMMatch::None;
      int64_t chosen_log = 0;
      auto consider = [&](FromFPMMMatch kind, int64_t log_index) {
        if (chosen == FromFPMMMatch::None || log_index < chosen_log) {
          chosen = kind;
          chosen_log = log_index;
          return;
        }
        stage2_assert(log_index != chosen_log, AssertLevel::L2, "Match", "FPMMSemanticSameLogTie");
      };
      if (refund_info != nullptr)
        consider(FromFPMMMatch::Refund, refund_info->log_index);
      if (buy_info != nullptr)
        consider(FromFPMMMatch::TradeBuy, buy_info->log_index);
      if (remove_info != nullptr)
        consider(FromFPMMMatch::Remove, remove_info->log_index);

      if (chosen == FromFPMMMatch::Refund) {
        bind_root(RootOpType::FPMMFunding, "lp_add_refund_from_pool");
        int64_t split_amt = funding_split_amount(*refund_info);
        refund_info->consumed_count++;
        if (known_token) {
          stage2_assert(token_idx < refund_info->amounts.size(), AssertLevel::L3, "FPMMFunding", "LPReturnTokenIdxInRange");
          int64_t expected_refund = split_amt - refund_info->amounts[token_idx];
          stage2_assert(amount == expected_refund, AssertLevel::L3, "FPMMFunding", "LPReturnAmountMatch");
          emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPReturn, token_idx, coll, 0, amount, split_price});
          return TransferClass::FPMMLPReturn;
        }
        bool refund_match = false;
        for (int64_t amount_i : refund_info->amounts) {
          if (split_amt - amount_i == amount) {
            refund_match = true;
            break;
          }
        }
        stage2_assert(refund_match, AssertLevel::L3, "FPMMFunding", "LPReturnAmountMatch");
        return TransferClass::InternalTransferFPMM;
      }
      if (chosen == FromFPMMMatch::TradeBuy) {
        bind_root(RootOpType::FPMMTrade, "buy_from_pool");
        buy_info->consumed = true;
        stage2_assert(amount == buy_info->tokens, AssertLevel::L3, "FPMMTrade", "BuyAmountMatch");
        int64_t price = usdc_price(buy_info->usdc, buy_info->tokens);
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMBuy, token_idx, coll, 0, amount, price});
        return TransferClass::FPMMBuy;
      }
      if (chosen == FromFPMMMatch::Remove) {
        bind_root(RootOpType::FPMMFunding, "lp_remove_from_pool");
        remove_info->consumed_count++;
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, coll, 0, amount, split_price});
        return TransferClass::FPMMLPRemove;
      }

      stage2_assert(!has_pending_future_fpmm_add_for_refund(tx_fpmm_key, amount),
                    AssertLevel::L3, "FPMMFunding", "TransferWithoutFundingLegMatch");
      stage2_assert(!has_pending_future_fpmm_remove_for_transfer(tx_fpmm_key, to, amount),
                    AssertLevel::L3, "FPMMFunding", "TransferWithoutFundingLegMatch");
      if (known_token) {
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInOther, token_idx, coll, 0, amount, 0});
        return classify_transfer_by_counterparty(TransferClass::TransferInOther,
                                                 TransferClass::TransferOutOther,
                                                 TransferClass::InternalTransferFPMM);
      }
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInNonPoly, token_idx, coll, 0, amount, 0});
      return classify_transfer_by_counterparty(TransferClass::TransferInNonPoly,
                                               TransferClass::TransferOutNonPoly,
                                               TransferClass::InternalTransferFPMM);
    }

    if (to == op) {
      FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 2, from, amount);
      if (tit != nullptr) {
        bind_root(RootOpType::FPMMTrade, "sell_to_pool");
        tit->consumed = true;
        stage2_assert(amount == tit->tokens, AssertLevel::L3, "FPMMTrade", "SellAmountMatch");
        int64_t price = usdc_price(tit->usdc, tit->tokens);
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::FPMMSell, token_idx, coll, 0, -amount, price});
        return TransferClass::FPMMSell;
      }
      if (known_token) {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutOther, token_idx, coll, 0, -amount, 0});
        return classify_transfer_by_counterparty(TransferClass::TransferInOther,
                                                 TransferClass::TransferOutOther,
                                                 TransferClass::InternalTransferFPMM);
      }
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutNonPoly, token_idx, coll, 0, -amount, 0});
      return classify_transfer_by_counterparty(TransferClass::TransferInNonPoly,
                                               TransferClass::TransferOutNonPoly,
                                               TransferClass::InternalTransferFPMM);
    }
    return TransferClass::InternalTransferFPMM;
  }

  // ========== 普通用户转账 ==========
  if (known_token) {
    stage2_assert(root_bind.op == RootOpType::None, AssertLevel::L2, "Bind",
                  "FallbackWithoutRootBind", root_bind.leg);
    emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInOther, token_idx, coll, 0, amount, 0});
    emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutOther, token_idx, coll, 0, -amount, 0});
    return classify_transfer_by_counterparty(TransferClass::TransferInOther,
                                             TransferClass::TransferOutOther,
                                             TransferClass::InternalTransferOther);
  } else {
    stage2_assert(root_bind.op == RootOpType::None, AssertLevel::L2, "Bind",
                  "FallbackWithoutRootBind", root_bind.leg);
    emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInNonPoly, token_idx, coll, 0, amount, 0});
    emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutNonPoly, token_idx, coll, 0, -amount, 0});
    return classify_transfer_by_counterparty(TransferClass::TransferInNonPoly,
                                             TransferClass::TransferOutNonPoly,
                                             TransferClass::InternalTransferOther);
  }
}

} // namespace stage2
