#include "stage2_builder.hpp"
#include <algorithm>

namespace stage2 {

TransferClass EventBuilder::classify_and_emit(
    int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
    int64_t block, const std::string &op,
    const std::string &from, const std::string &to,
    const std::string &token_id, int64_t amount,
    uint32_t cond_idx, uint8_t token_idx, Collateral collateral) {

  assert_transfer(amount >= 0, "Transfer amount must be non-negative");

  // Validate token identity before any semantic matching.
  bool known_token = (cond_idx != UNKNOWN_COND_IDX);
  uint8_t outcome_cnt = 2;
  if (known_token) {
    assert_transfer(cond_idx < conditions_.size(), "Invalid cond_idx");
    outcome_cnt = conditions_[cond_idx].outcome_count;
    assert_transfer(outcome_cnt > 0, "Invalid outcome_count");
    assert_transfer(token_idx < outcome_cnt, "Invalid token_idx");
  } else {
    assert_transfer(token_idx == UNKNOWN_TOKEN_IDX, "Unknown token must use token_idx=255");
  }
  assert_transfer(from != to, "from and to must be different");

  uint8_t coll = static_cast<uint8_t>(collateral);
  std::string cond_id = known_token ? cond_ids_[cond_idx] : "";
  TxKey tx_key{block, tx_hash};
  TxTokenKey tx_token_key{block, tx_hash, token_id};
  int64_t transfer_log_index = sort_key - block * SORT_KEY_SCALE;
  assert_transfer(transfer_log_index >= 0, "Invalid transfer sort_key/log_index");
  int64_t sub_idx = transfer_log_index % TRANSFER_FLAT_LOG_SCALE;
  assert_transfer(sub_idx >= 0 && sub_idx < TRANSFER_FLAT_LOG_SCALE, "Invalid transfer sub_idx");
  int64_t base_log_index = transfer_log_index / TRANSFER_FLAT_LOG_SCALE;

  int64_t active_semantic_log = -1;
  uint32_t semantic_mask = 0;
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
    if (active_semantic_log >= 0) {
      TxLogKey log_key{block, tx_hash, active_semantic_log};
      auto mask_it = tx_op_type_mask_.find(log_key);
      if (mask_it != tx_op_type_mask_.end()) {
        semantic_mask = mask_it->second;
      }
    }
  }

  int64_t split_price = (known_token && is_usdc_collateral(collateral)) ? (1000000 / outcome_cnt) : 0;

  // Lambda: 匹配 Split/Merge/Redemption 时，已知 token 需要检查 cond_id
  auto cond_matches = [&](const std::string &info_cond_id) {
    return !known_token || cond_id == info_cond_id;
  };
  auto has_semantic = [&](SemanticKind kind) {
    return (semantic_mask & semantic_mask_bit(kind)) != 0;
  };
  auto semantic_log_matches = [&](int64_t info_log_index) {
    return active_semantic_log >= 0 && info_log_index == active_semantic_log;
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
    if (!has_semantic(SemanticKind::Split))
      return nullptr;
    auto it = tx_split_.find(tx_key);
    if (it == tx_split_.end())
      return nullptr;
    SplitInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (!semantic_log_matches(info.log_index))
        continue;
      if (!collateral_matches(info.collateral_token))
        continue;
      if (info.stakeholder == stakeholder && info.amount == amt && cond_matches(info.cond_id)) {
        matched = &info;
        match_count++;
      }
    }
    assert_transfer(match_count <= 1, "Ambiguous split semantic candidates");
    return matched;
  };
  auto find_merge_info = [&](const std::string &stakeholder, int64_t amt) -> MergeInfo * {
    if (!has_semantic(SemanticKind::Merge))
      return nullptr;
    auto it = tx_merge_.find(tx_key);
    if (it == tx_merge_.end())
      return nullptr;
    MergeInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (!semantic_log_matches(info.log_index))
        continue;
      if (!collateral_matches(info.collateral_token))
        continue;
      if (info.stakeholder == stakeholder && info.amount == amt && cond_matches(info.cond_id)) {
        matched = &info;
        match_count++;
      }
    }
    assert_transfer(match_count <= 1, "Ambiguous merge semantic candidates");
    return matched;
  };
  auto find_redemption_info = [&](const std::string &redeemer) -> RedemptionInfo * {
    if (!has_semantic(SemanticKind::Redemption))
      return nullptr;
    auto it = tx_redemption_.find(tx_key);
    if (it == tx_redemption_.end())
      return nullptr;
    RedemptionInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (!semantic_log_matches(info.log_index))
        continue;
      if (!collateral_matches(info.collateral_token))
        continue;
      if (info.redeemer == redeemer && cond_matches(info.cond_id)) {
        matched = &info;
        match_count++;
      }
    }
    assert_transfer(match_count <= 1, "Ambiguous redemption semantic candidates");
    return matched;
  };
  auto find_fpmm_trade_info = [&](const TxFPMMKey &key, int side,
                                  const std::string &trader, int64_t token_amount) -> FPMMTradeInfo * {
    auto it = tx_fpmm_trade_.find(key);
    if (it == tx_fpmm_trade_.end())
      return nullptr;
    FPMMTradeInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (info.consumed)
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
    assert_transfer(match_count <= 1, "Ambiguous FPMM trade semantic candidates");
    return matched;
  };
  auto mark_fpmm_trade_explained = [&](const TxFPMMKey &key, int side) -> bool {
    auto it = tx_fpmm_trade_.find(key);
    if (it == tx_fpmm_trade_.end())
      return false;
    FPMMTradeInfo *matched = nullptr;
    int match_count = 0;
    bool has_consumed_match = false;
    for (auto &info : it->second) {
      if (info.log_index < base_log_index)
        continue;
      if (info.side != side)
        continue;
      if (info.consumed) {
        has_consumed_match = true;
        continue;
      }
      matched = &info;
      match_count++;
    }
    assert_transfer(match_count <= 1, "Ambiguous explainable FPMM trade semantic candidates");
    if (has_consumed_match)
      return true;
    if (matched == nullptr)
      return false;
    matched->explained_without_direct_leg = true;
    return true;
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
      if (info.side != 1 || info.amounts_count != 2)
        continue;
      int64_t expected_amount = expect_refund
                                    ? std::abs(info.amount0 - info.amount1)
                                    : std::max(info.amount0, info.amount1);
      if (expected_amount == transfer_amount) {
        matched = &info;
        match_count++;
      }
    }
    assert_transfer(match_count <= 1, "Ambiguous FPMM funding semantic candidates");
    return matched;
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
      bool amount_match = (transfer_amount == info.amount0) ||
                          (transfer_amount == info.amount1) ||
                          (info.amounts_count == 1 && transfer_amount == info.amount0);
      if (amount_match) {
        matched = &info;
        match_count++;
      }
    }
    assert_transfer(match_count <= 1, "Ambiguous FPMM remove semantic candidates");
    return matched;
  };
  auto consume_active_funding_semantic = [&](const TxFPMMKey &key) {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end())
      return false;
    FPMMFundingInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (info.log_index < base_log_index)
        continue;
      matched = &info;
      match_count++;
    }
    assert_transfer(match_count <= 1, "Ambiguous active FPMM funding semantic");
    if (matched == nullptr)
      return false;
    matched->consumed_count++;
    return true;
  };
  auto find_order_info = [&]() -> OrderInfo * {
    if (!has_semantic(SemanticKind::Order))
      return nullptr;
    auto it = tx_order_.find(tx_token_key);
    if (it == tx_order_.end())
      return nullptr;
    OrderInfo *matched = nullptr;
    int match_count = 0;
    for (auto &info : it->second) {
      if (info.consumed)
        continue;
      if (!semantic_log_matches(info.log_index))
        continue;
      if (info.tokens == amount) {
        matched = &info;
        match_count++;
      }
    }
    assert_transfer(match_count <= 1, "Ambiguous order semantic candidates");
    return matched;
  };

  if (amount == 0) {
    auto fpmm_zero_it = fpmm_map_.find(op);
    if (fpmm_zero_it != fpmm_map_.end()) {
      TxFPMMKey tx_fpmm_key{block, tx_hash, op};
      if (to == op) {
        if (FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 2, from, 0); tit != nullptr) {
          tit->consumed = true;
        }
      } else if (from == op) {
        if (FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 1, to, 0); tit != nullptr) {
          tit->consumed = true;
        } else if (to == ZERO_ADDR) {
          (void)mark_fpmm_trade_explained(tx_fpmm_key, 2);
        }
      }
    }
    return TransferClass::InternalTransferZero;
  }

  // ========== mint 分支 (from == 0x0) ==========
  if (from == ZERO_ADDR) {
    if (to == NEG_RISK_ADAPTER)
      return TransferClass::InternalMintNegRisk;

    if (MergeInfo *mit = find_merge_info(to, amount); mit != nullptr) {
      mit->consumed_count++;
      if (known_token) {
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::MergeNormal, token_idx, coll, 0, amount, split_price});
        return TransferClass::MergeNormal;
      }
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::MergeNonPoly, token_idx, coll, 0, amount, split_price});
      return TransferClass::MergeNonPoly;
    }

    if (RedemptionInfo *rit = find_redemption_info(to); rit != nullptr) {
      rit->consumed_count++;
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

    auto fpmm_mint_it = fpmm_map_.find(to);
    if (fpmm_mint_it != fpmm_map_.end()) {
      TxFPMMKey tx_fpmm_key{block, tx_hash, to};
      FPMMFundingInfo *fit = find_fpmm_funding_info(tx_fpmm_key, amount, false);
      if (fit != nullptr) {
        int64_t split_amt = std::max(fit->amount0, fit->amount1);
        assert_transfer(amount == split_amt, "LP Add split amount mismatch");
        fit->consumed_count++;
        if (known_token) {
          assert_transfer(token_idx < 2, "LP Add requires binary token_idx");
          if (is_user_addr(fit->funder)) {
            int64_t pool_amt = (token_idx == 0) ? fit->amount0 : fit->amount1;
            RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, coll, 0, pool_amt, split_price};
            push_event(fit->funder, evt);
          }
          return TransferClass::FPMMLPAdd;
        }
        return TransferClass::InternalMintFPMM;
      }
      bool explained = mark_fpmm_trade_explained(tx_fpmm_key, 1);
      assert_transfer(explained || !tx_fpmm_trade_.count(tx_fpmm_key),
                      "FPMM buy semantic not explainable on internal mint");
      return TransferClass::InternalMintFPMM;
    }

    if (SplitInfo *sit = find_split_info(to, amount); sit != nullptr) {
      sit->consumed_count++;
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

  // ========== burn 分支 (to == 0x0) ==========
  if (to == ZERO_ADDR) {
    if (SplitInfo *sit = find_split_info(from, amount); sit != nullptr) {
      sit->consumed_count++;
      if (known_token) {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::SplitNormal, token_idx, coll, 0, -amount, split_price});
        return TransferClass::SplitNormal;
      }
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::SplitNonPoly, token_idx, coll, 0, -amount, split_price});
      return TransferClass::SplitNonPoly;
    }

    if (from == NEG_RISK_ADAPTER)
      return TransferClass::InternalBurnNegRisk;

    auto fpmm_burn_it = fpmm_map_.find(from);
    if (fpmm_burn_it != fpmm_map_.end()) {
      TxFPMMKey tx_fpmm_key{block, tx_hash, from};
      bool explained = mark_fpmm_trade_explained(tx_fpmm_key, 2);
      assert_transfer(explained || !tx_fpmm_trade_.count(tx_fpmm_key),
                      "FPMM sell semantic not explainable on internal burn");
      return TransferClass::InternalBurnFPMM;
    }

    if (MergeInfo *mit = find_merge_info(from, amount); mit != nullptr) {
      mit->consumed_count++;
      if (known_token) {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::MergeNormal, token_idx, coll, 0, -amount, split_price});
        return TransferClass::MergeNormal;
      } else {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::MergeNonPoly, token_idx, coll, 0, -amount, split_price});
        return TransferClass::MergeNonPoly;
      }
    }

    if (RedemptionInfo *rit = find_redemption_info(from); rit != nullptr) {
      rit->consumed_count++;
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
  if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
    OrderInfo *oit = find_order_info();
    if (oit != nullptr) {
      assert_transfer(oit->tokens > 0, "Order tokens must be positive");
      assert_transfer(amount == oit->tokens, "Order amount mismatch");
      if (oit->maker_side == 1) {
        assert_transfer(to == oit->maker, "Order buyer mismatch");
        assert_transfer(from == oit->taker, "Order seller mismatch");
      } else {
        assert_transfer(from == oit->maker, "Order seller mismatch");
        assert_transfer(to == oit->taker, "Order buyer mismatch");
      }
      oit->consumed = true;

      int64_t price = usdc_price(oit->usdc, oit->tokens);
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::OrderBuy, token_idx, coll, 0, amount, price});
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::OrderSell, token_idx, coll, 0, -amount, price});
      return classify_transfer_by_counterparty(TransferClass::OrderBuy, TransferClass::OrderSell,
                                               TransferClass::InternalTransferOrder);
    }

    if (has_semantic(SemanticKind::Order)) {
      std::cerr << "[ERROR] Exchange transfer without matching order: block=" << block
                << ", op=" << op << ", from=" << from << ", to=" << to << std::endl;
      fail_transfer_assert("Exchange transfer without order");
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
        split_info->consumed_count++;
        bool is_convert_output = false;
        if (known_token && token_idx == 0 && !conditions_[cond_idx].question_id.empty()) {
          auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
          if (market_it != cond_to_market_.end()) {
            TxMarketKey tx_market_key{block, tx_hash, market_it->second};
            is_convert_output = (tx_convert_.count(tx_market_key) > 0);
          }
        }
        if (is_convert_output) {
          assert_transfer(amount <= split_info->amount, "Convert YES output exceeds split");
        } else {
          assert_transfer(amount == split_info->amount, "NegRisk Split mismatch");
        }
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::SplitNegRisk, token_idx, coll, 0, amount, split_price});
        return TransferClass::SplitNegRisk;
      }
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInNegRisk, token_idx, coll, 0, amount, 0});
      return TransferClass::TransferInNegRisk;
    }

    if (to == NEG_RISK_ADAPTER) {
      if (MergeInfo *merge_info = find_merge_info(NEG_RISK_ADAPTER, amount); merge_info != nullptr) {
        merge_info->consumed_count++;
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

      assert_transfer(known_token && token_idx == 1, "Convert should only burn NO tokens");
      assert_transfer(has_semantic(SemanticKind::Convert), "Convert burn requires semantic convert op");
      if (!conditions_[cond_idx].question_id.empty()) {
        auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
        if (market_it != cond_to_market_.end()) {
          TxMarketKey tx_market_key{block, tx_hash, market_it->second};
          auto cit = tx_convert_.find(tx_market_key);
          if (cit != tx_convert_.end()) {
            for (auto &ci : cit->second) {
              if (!semantic_log_matches(ci.log_index))
                continue;
              if (ci.stakeholder == from && ci.amount == amount) {
                ci.consumed_count++;
                emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Convert, token_idx, coll, 0, -amount, 0});
                return TransferClass::Convert;
              }
            }
          }
        }
      }
      std::cerr << "[ERROR] Convert burn without event: block=" << block
                << ", from=" << from << ", amount=" << amount << std::endl;
      fail_transfer_assert("Convert burn without event");
      return TransferClass::Unclassified;
    }
    fail_transfer_assert("Unexpected NegRisk adapter transfer pattern");
    return TransferClass::Unclassified;
  }

  // ========== FPMM operator ==========
  auto fpmm_it = fpmm_map_.find(op);
  if (fpmm_it != fpmm_map_.end()) {
    TxFPMMKey tx_fpmm_key{block, tx_hash, op};

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
        assert_transfer(log_index != chosen_log, "Ambiguous FPMM semantic candidates at same log");
      };
      if (refund_info != nullptr)
        consider(FromFPMMMatch::Refund, refund_info->log_index);
      if (buy_info != nullptr)
        consider(FromFPMMMatch::TradeBuy, buy_info->log_index);
      if (remove_info != nullptr)
        consider(FromFPMMMatch::Remove, remove_info->log_index);

      if (chosen == FromFPMMMatch::Refund) {
        assert_transfer(refund_info->amount0 != refund_info->amount1, "LP Add should have asymmetric amounts");
        int64_t refund_amt = std::abs(refund_info->amount0 - refund_info->amount1);
        assert_transfer(amount == refund_amt, "LP Add refund amount mismatch");
        int64_t refund_idx = (refund_info->amount0 < refund_info->amount1) ? 0 : 1;
        refund_info->consumed_count++;
        if (known_token && token_idx == refund_idx) {
          emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPReturn, token_idx, coll, 0, amount, split_price});
          return TransferClass::FPMMLPReturn;
        }
        return TransferClass::InternalTransferFPMM;
      }
      if (chosen == FromFPMMMatch::TradeBuy) {
        buy_info->consumed = true;
        assert_transfer(amount == buy_info->tokens, "FPMM Buy amount mismatch");
        int64_t price = usdc_price(buy_info->usdc, buy_info->tokens);
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMBuy, token_idx, coll, 0, amount, price});
        return TransferClass::FPMMBuy;
      }
      if (chosen == FromFPMMMatch::Remove) {
        remove_info->consumed_count++;
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, coll, 0, amount, split_price});
        return TransferClass::FPMMLPRemove;
      }

      if (tx_fpmm_funding_.count(tx_fpmm_key) > 0) {
        bool consumed = consume_active_funding_semantic(tx_fpmm_key);
        assert_transfer(consumed, "FPMM funding semantic without matching funding row");
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, coll, 0, amount, split_price});
        return TransferClass::FPMMLPRemove;
      }
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
        tit->consumed = true;
        assert_transfer(amount == tit->tokens, "FPMM Sell amount mismatch");
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
    emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInOther, token_idx, coll, 0, amount, 0});
    emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutOther, token_idx, coll, 0, -amount, 0});
    return classify_transfer_by_counterparty(TransferClass::TransferInOther,
                                             TransferClass::TransferOutOther,
                                             TransferClass::InternalTransferOther);
  } else {
    emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferInNonPoly, token_idx, coll, 0, amount, 0});
    emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOutNonPoly, token_idx, coll, 0, -amount, 0});
    return classify_transfer_by_counterparty(TransferClass::TransferInNonPoly,
                                             TransferClass::TransferOutNonPoly,
                                             TransferClass::InternalTransferOther);
  }
}

} // namespace stage2
