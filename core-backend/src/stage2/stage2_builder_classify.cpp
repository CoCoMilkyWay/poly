#include "stage2_builder.hpp"
#include <algorithm>
#include <cmath>
#include <type_traits>

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

  uint8_t coll = static_cast<uint8_t>(collateral);
  const std::string *cond_id = known_token ? &cond_ids_[cond_idx] : nullptr;
  TxKey tx_key{block, tx_hash};
  bool op_is_exchange = (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE);
  bool op_is_fpmm = is_fpmm_visible_at(op, sort_key);
  int64_t transfer_log_index = sort_key - block * SORT_KEY_SCALE;
  stage2_assert(transfer_log_index >= 0, AssertLevel::L0, "Input", "TransferLogIndexNonNegative");
  int64_t sub_idx = transfer_log_index % TRANSFER_FLAT_LOG_SCALE;
  stage2_assert(sub_idx >= 0 && sub_idx < TRANSFER_FLAT_LOG_SCALE, AssertLevel::L0, "Input", "TransferSubIdxRange");
  int64_t base_log_index = transfer_log_index / TRANSFER_FLAT_LOG_SCALE;
  if (from == to) {
    return (amount == 0) ? TransferClass::InternalTransferZero
                         : TransferClass::InternalTransferOther;
  }

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

  // Lambda: 匹配 Split/Merge/Redemption 时,已知 token 需要检查 cond_id
  auto cond_matches = [&](const std::string &info_cond_id) {
    return !known_token || *cond_id == info_cond_id;
  };
  auto semantic_log_matches = [&](int64_t info_log_index) {
    return active_semantic_log >= 0 && info_log_index == active_semantic_log;
  };
  auto semantic_log_distance = [&](int64_t info_log_index) {
    if (semantic_log_matches(info_log_index)) {
      return int64_t{0};
    }
    bool is_future = info_log_index >= base_log_index;
    return is_future ? (info_log_index - base_log_index)
                     : (base_log_index - info_log_index + SORT_KEY_SCALE);
  };
  auto select_window_only = [&](auto *window_matched,
                                int window_match_count,
                                const char *window_rule) {
    stage2_assert(window_match_count <= 1, AssertLevel::L2, "Match", window_rule);
    return window_matched;
  };
  auto select_window_or_forward = [&](auto &rows, auto &&match_candidate,
                                      const char *window_rule,
                                      const char *forward_rule) {
    using Row = typename std::decay_t<decltype(rows)>::value_type;
    Row *window_matched = nullptr;
    int window_match_count = 0;
    Row *forward_matched = nullptr;
    int64_t forward_log = -1;
    int forward_same_log_count = 0;
    for (auto &info : rows) {
      if (!match_candidate(info)) {
        continue;
      }
      if (semantic_log_matches(info.log_index)) {
        window_matched = &info;
        window_match_count++;
        continue;
      }
      if (forward_matched == nullptr || info.log_index < forward_log) {
        forward_matched = &info;
        forward_log = info.log_index;
        forward_same_log_count = 1;
      } else if (info.log_index == forward_log) {
        forward_same_log_count++;
      }
    }
    Row *window_only = select_window_only(window_matched, window_match_count, window_rule);
    if (window_only != nullptr) {
      return window_only;
    }
    stage2_assert(forward_same_log_count <= 1, AssertLevel::L2, "Match", forward_rule);
    return forward_matched;
  };
  auto select_window_or_forward_ptr = [&](const auto &rows, auto &&match_candidate,
                                          const char *window_rule,
                                          const char *forward_rule) {
    using Ptr = typename std::decay_t<decltype(rows)>::value_type;
    using Row = std::remove_pointer_t<Ptr>;
    Row *window_matched = nullptr;
    int window_match_count = 0;
    Row *forward_matched = nullptr;
    int64_t forward_log = -1;
    int forward_same_log_count = 0;
    for (Row *info_ptr : rows) {
      Row &info = *info_ptr;
      if (!match_candidate(info)) {
        continue;
      }
      if (semantic_log_matches(info.log_index)) {
        window_matched = &info;
        window_match_count++;
        continue;
      }
      if (forward_matched == nullptr || info.log_index < forward_log) {
        forward_matched = &info;
        forward_log = info.log_index;
        forward_same_log_count = 1;
      } else if (info.log_index == forward_log) {
        forward_same_log_count++;
      }
    }
    Row *window_only = select_window_only(window_matched, window_match_count, window_rule);
    if (window_only != nullptr) {
      return window_only;
    }
    stage2_assert(forward_same_log_count <= 1, AssertLevel::L2, "Match", forward_rule);
    return forward_matched;
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
  // Price calculation using double: price_1e6 = collateral_amount * 1e6 / token_amount
  // Double has 52-bit mantissa (~15 decimal digits), sufficient for USDC amounts up to $9 trillion.
  auto calc_price_if_usdc_collateral = [&](int64_t collateral_amount, int64_t token_amount) -> int64_t {
    if (!is_usdc_collateral(collateral)) {
      return 0;
    }
    stage2_assert(collateral_amount >= 0, AssertLevel::L0, "Input", "CollateralAmountNonNegativeForPrice");
    stage2_assert(token_amount > 0, AssertLevel::L0, "Input", "TokenAmountPositiveForPrice");
    double px = static_cast<double>(collateral_amount) * 1e6 / static_cast<double>(token_amount);
    return static_cast<int64_t>(std::round(px));
  };
  auto patch_collateral_from_semantic = [&](const std::string &semantic_collateral_addr) {
    if (coll != static_cast<uint8_t>(Collateral::Unknown)) {
      return;
    }
    if (known_token) {
      coll = require_cond_collateral(cond_idx);
      collateral = static_cast<Collateral>(coll);
      split_price = is_usdc_collateral(collateral) ? (1000000 / outcome_cnt) : 0;
      return;
    }
    if (semantic_collateral_addr.empty() || semantic_collateral_addr == ZERO_ADDR) {
      return;
    }
    uint8_t patched = intern_collateral(semantic_collateral_addr);
    stage2_assert(patched != static_cast<uint8_t>(Collateral::Unknown),
                  AssertLevel::L1, "Mapping", "SemanticCollateralPatchedNotUnknown");
    coll = patched;
    collateral = static_cast<Collateral>(coll);
  };
  auto classify_transfer_by_counterparty = [&](TransferClass in_cls, TransferClass out_cls,
                                               TransferClass internal_cls) {
    if (is_user_addr(to))
      return in_cls;
    if (is_user_addr(from))
      return out_cls;
    return internal_cls;
  };
  auto generic_transfer_classes = [&]() {
    if (known_token) {
      return std::array<TransferClass, 2>{TransferClass::TransferInOther,
                                          TransferClass::TransferOutOther};
    }
    return std::array<TransferClass, 2>{TransferClass::TransferInNonPoly,
                                        TransferClass::TransferOutNonPoly};
  };
  auto generic_transfer_event_types = [&](bool is_inbound) {
    if (known_token) {
      return is_inbound ? EventType::TransferInOther : EventType::TransferOutOther;
    }
    return is_inbound ? EventType::TransferInNonPoly : EventType::TransferOutNonPoly;
  };
  auto emit_generic_transfer_event = [&](const std::string &user_addr, bool is_inbound) {
    int64_t signed_amount = is_inbound ? amount : -amount;
    EventType event_type = generic_transfer_event_types(is_inbound);
    emit_if_user(user_addr, RawEvent{sort_key, cond_idx, event_type, token_idx, coll, 0,
                                     signed_amount, 0});
  };
  auto emit_and_classify_generic_single = [&](const std::string &user_addr,
                                              bool is_inbound,
                                              TransferClass internal_cls) {
    emit_generic_transfer_event(user_addr, is_inbound);
    auto classes = generic_transfer_classes();
    return classify_transfer_by_counterparty(classes[0], classes[1], internal_cls);
  };
  auto emit_and_classify_generic_in = [&](const std::string &user_addr,
                                          TransferClass internal_cls) {
    return emit_and_classify_generic_single(user_addr, true, internal_cls);
  };
  auto emit_and_classify_generic_out = [&](const std::string &user_addr,
                                           TransferClass internal_cls) {
    return emit_and_classify_generic_single(user_addr, false, internal_cls);
  };
  auto emit_and_classify_generic_in_out = [&](TransferClass internal_cls) {
    emit_generic_transfer_event(to, true);
    emit_generic_transfer_event(from, false);
    auto classes = generic_transfer_classes();
    return classify_transfer_by_counterparty(classes[0], classes[1], internal_cls);
  };
  auto emit_negrisk_transfer = [&](const std::string &user_addr, bool is_inbound) {
    EventType event_type = is_inbound ? EventType::TransferInNegRisk : EventType::TransferOutNegRisk;
    int64_t signed_amount = is_inbound ? amount : -amount;
    emit_if_user(user_addr, RawEvent{sort_key, cond_idx, event_type, token_idx, coll, 0,
                                     signed_amount, 0});
  };
  auto emit_and_classify_negrisk_in_out = [&](TransferClass internal_cls) {
    emit_negrisk_transfer(to, true);
    emit_negrisk_transfer(from, false);
    return classify_transfer_by_counterparty(TransferClass::TransferInNegRisk,
                                             TransferClass::TransferOutNegRisk,
                                             internal_cls);
  };
  auto emit_split_or_merge = [&](bool is_split, const std::string &user_addr,
                                 int64_t signed_amount) {
    EventType event_type;
    TransferClass cls;
    if (is_split) {
      event_type = known_token ? EventType::SplitNormal : EventType::SplitNonPoly;
      cls = known_token ? TransferClass::SplitNormal : TransferClass::SplitNonPoly;
    } else {
      event_type = known_token ? EventType::MergeNormal : EventType::MergeNonPoly;
      cls = known_token ? TransferClass::MergeNormal : TransferClass::MergeNonPoly;
    }
    emit_if_user(user_addr, RawEvent{sort_key, cond_idx, event_type, token_idx, coll, 0,
                                     signed_amount, split_price});
    return cls;
  };
  std::vector<SplitInfo> *tx_split_rows = nullptr;
  if (auto split_it = tx_split_.find(tx_key); split_it != tx_split_.end()) {
    tx_split_rows = &split_it->second;
  }
  std::unordered_map<std::string, std::vector<SplitInfo *>> *tx_split_actor_amount_rows = nullptr;
  if (auto split_idx_it = tx_split_by_actor_amount_.find(tx_key);
      split_idx_it != tx_split_by_actor_amount_.end()) {
    tx_split_actor_amount_rows = &split_idx_it->second;
  }
  std::vector<MergeInfo> *tx_merge_rows = nullptr;
  if (auto merge_it = tx_merge_.find(tx_key); merge_it != tx_merge_.end()) {
    tx_merge_rows = &merge_it->second;
  }
  std::unordered_map<std::string, std::vector<MergeInfo *>> *tx_merge_actor_amount_rows = nullptr;
  if (auto merge_idx_it = tx_merge_by_actor_amount_.find(tx_key);
      merge_idx_it != tx_merge_by_actor_amount_.end()) {
    tx_merge_actor_amount_rows = &merge_idx_it->second;
  }
  std::vector<RedemptionInfo> *tx_redemption_rows = nullptr;
  if (auto redemption_it = tx_redemption_.find(tx_key); redemption_it != tx_redemption_.end()) {
    tx_redemption_rows = &redemption_it->second;
  }
  std::unordered_map<std::string, std::vector<RedemptionInfo *>> *tx_redemption_actor_rows = nullptr;
  if (auto redemption_idx_it = tx_redemption_by_actor_.find(tx_key);
      redemption_idx_it != tx_redemption_by_actor_.end()) {
    tx_redemption_actor_rows = &redemption_idx_it->second;
  }
  std::unordered_map<int64_t, std::vector<OrderInfo *>> *tx_order_amount_rows = nullptr;
  if (op_is_exchange) {
    TxTokenKey tx_token_key{block, tx_hash, token_id};
    if (auto order_amount_it = tx_order_by_amount_.find(tx_token_key);
        order_amount_it != tx_order_by_amount_.end()) {
      tx_order_amount_rows = &order_amount_it->second;
    }
  }
  auto find_conditional_info_with_amount = [&](auto *rows,
                                               auto *actor_amount_rows,
                                               const std::string &stakeholder,
                                               int64_t amt,
                                               const char *window_rule,
                                               const char *nearest_rule) {
    using Row = typename std::remove_reference_t<decltype(*rows)>::value_type;
    if (rows == nullptr) {
      return static_cast<Row *>(nullptr);
    }
    const std::vector<Row *> *indexed_candidates = nullptr;
    if (actor_amount_rows != nullptr) {
      auto key = actor_amount_index_key(stakeholder, amt);
      auto idx_it = actor_amount_rows->find(key);
      if (idx_it != actor_amount_rows->end()) {
        indexed_candidates = &idx_it->second;
      }
    }
    // Single-pass scan: collect best match per priority tier.
    // Priority: unconsumed+strict > unconsumed+relaxed > consumed+strict > consumed+relaxed
    Row *best[4] = {nullptr, nullptr, nullptr, nullptr};
    int64_t best_dist[4] = {1LL << 62, 1LL << 62, 1LL << 62, 1LL << 62};
    int best_count[4] = {0, 0, 0, 0};
    Row *window_best[4] = {nullptr, nullptr, nullptr, nullptr};
    int window_count[4] = {0, 0, 0, 0};
    auto scan_row = [&](Row &info) {
      if (info.stakeholder != stakeholder || info.amount != amt || !cond_matches(info.cond_id))
        return;
      bool unconsumed = (info.consumed_count == 0);
      bool strict_coll = collateral_matches(info.collateral_token);
      int tier = (unconsumed ? 0 : 2) + (strict_coll ? 0 : 1);
      if (semantic_log_matches(info.log_index)) {
        window_best[tier] = &info;
        window_count[tier]++;
        return;
      }
      int64_t dist = semantic_log_distance(info.log_index);
      if (best[tier] == nullptr || dist < best_dist[tier]) {
        best[tier] = &info;
        best_dist[tier] = dist;
        best_count[tier] = 1;
      } else if (dist == best_dist[tier]) {
        best_count[tier]++;
      }
    };
    if (indexed_candidates != nullptr) {
      for (Row *ptr : *indexed_candidates)
        scan_row(*ptr);
    } else {
      for (auto &info : *rows)
        scan_row(info);
    }
    for (int t = 0; t < 4; ++t) {
      if (window_count[t] > 0) {
        stage2_assert(window_count[t] <= 1, AssertLevel::L2, "Match", window_rule);
        return window_best[t];
      }
    }
    for (int t = 0; t < 4; ++t) {
      if (best[t] != nullptr) {
        stage2_assert(best_count[t] <= 1, AssertLevel::L2, "Match", nearest_rule);
        return best[t];
      }
    }
    return static_cast<Row *>(nullptr);
  };
  auto find_split_info = [&](const std::string &stakeholder, int64_t amt) -> SplitInfo * {
    return find_conditional_info_with_amount(
        tx_split_rows, tx_split_actor_amount_rows, stakeholder, amt,
        "SplitWindowUniqueCandidate",
        "SplitForwardUniqueCandidate");
  };
  auto find_merge_info = [&](const std::string &stakeholder, int64_t amt) -> MergeInfo * {
    return find_conditional_info_with_amount(
        tx_merge_rows, tx_merge_actor_amount_rows, stakeholder, amt,
        "MergeWindowUniqueCandidate",
        "MergeForwardUniqueCandidate");
  };
  auto find_redemption_info = [&](const std::string &redeemer) -> RedemptionInfo * {
    if (tx_redemption_rows == nullptr)
      return nullptr;
    const std::vector<RedemptionInfo *> *indexed_candidates = nullptr;
    if (tx_redemption_actor_rows != nullptr) {
      auto idx_it = tx_redemption_actor_rows->find(redeemer);
      if (idx_it != tx_redemption_actor_rows->end()) {
        indexed_candidates = &idx_it->second;
      }
    }
    // Single-pass: unconsumed > consumed
    RedemptionInfo *best[2] = {nullptr, nullptr};
    int64_t best_dist[2] = {1LL << 62, 1LL << 62};
    int best_count[2] = {0, 0};
    RedemptionInfo *window_matched = nullptr;
    int window_count = 0;
    auto scan_row = [&](RedemptionInfo &info) {
      if (info.redeemer != redeemer || !cond_matches(info.cond_id))
        return;
      int tier = (info.consumed_count == 0) ? 0 : 1;
      if (semantic_log_matches(info.log_index)) {
        if (tier == 0) {
          window_matched = &info;
          window_count++;
        }
        return;
      }
      int64_t dist = semantic_log_distance(info.log_index);
      if (best[tier] == nullptr || dist < best_dist[tier]) {
        best[tier] = &info;
        best_dist[tier] = dist;
        best_count[tier] = 1;
      } else if (dist == best_dist[tier]) {
        best_count[tier]++;
      }
    };
    if (indexed_candidates != nullptr) {
      for (RedemptionInfo *ptr : *indexed_candidates)
        scan_row(*ptr);
    } else {
      for (auto &info : *tx_redemption_rows)
        scan_row(info);
    }
    if (window_count > 0) {
      stage2_assert(window_count <= 1, AssertLevel::L2, "Match", "RedeemWindowUniqueCandidate");
      return window_matched;
    }
    for (int t = 0; t < 2; ++t) {
      if (best[t] != nullptr) {
        stage2_assert(best_count[t] <= 1, AssertLevel::L2, "Match", "RedeemForwardUniqueCandidate");
        return best[t];
      }
    }
    return nullptr;
  };
  auto fpmm_trade_rows = [&](const TxFPMMKey &key) -> std::vector<FPMMTradeInfo> * {
    auto it = tx_fpmm_trade_.find(key);
    if (it == tx_fpmm_trade_.end()) {
      return nullptr;
    }
    return &it->second;
  };
  auto fpmm_trade_leg_rows = [&](const TxFPMMKey &key)
      -> std::unordered_map<std::string, std::vector<FPMMTradeInfo *>> * {
    auto it = tx_fpmm_trade_by_leg_.find(key);
    if (it == tx_fpmm_trade_by_leg_.end()) {
      return nullptr;
    }
    return &it->second;
  };
  auto fpmm_funding_rows = [&](const TxFPMMKey &key) -> std::vector<FPMMFundingInfo> * {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end()) {
      return nullptr;
    }
    return &it->second;
  };
  auto find_fpmm_trade_info = [&](const TxFPMMKey &key, int side,
                                  const std::string &trader, int64_t token_amount) -> FPMMTradeInfo * {
    auto *rows = fpmm_trade_rows(key);
    if (rows == nullptr)
      return nullptr;
    const std::vector<FPMMTradeInfo *> *indexed_candidates = nullptr;
    auto *leg_map = fpmm_trade_leg_rows(key);
    if (leg_map != nullptr) {
      auto idx_it = leg_map->find(fpmm_trade_leg_index_key(side, trader, token_amount));
      if (idx_it != leg_map->end()) {
        indexed_candidates = &idx_it->second;
      }
    }
    if (indexed_candidates != nullptr) {
      return select_window_or_forward_ptr(
          *indexed_candidates,
          [&](const FPMMTradeInfo &info) {
            if (!info.requires_erc1155_leg || info.consumed) {
              return false;
            }
            if (info.log_index < base_log_index) {
              return false;
            }
            return info.side == side && info.trader == trader && info.tokens == token_amount;
          },
          "FPMMTradeWindowUniqueCandidate",
          "FPMMTradeUniqueCandidate");
    }
    return select_window_or_forward(
        *rows,
        [&](const FPMMTradeInfo &info) {
          if (!info.requires_erc1155_leg || info.consumed) {
            return false;
          }
          // A trade row may be pre-marked as explained by internal legs (0->FPMM /
          // FPMM->0). If a concrete direct leg appears later in the same tx, it
          // should still be allowed to consume the semantic row.
          // FPMM emits FPMMBuy/FPMMSell after transfer legs in the same tx.
          // Only allow forward matching within this tx; never match past semantic logs.
          if (info.log_index < base_log_index) {
            return false;
          }
          return info.side == side && info.trader == trader && info.tokens == token_amount;
        },
        "FPMMTradeWindowUniqueCandidate",
        "FPMMTradeUniqueCandidate");
  };
  auto mark_fpmm_trade_explained = [&](const TxFPMMKey &key, int side) -> bool {
    auto *rows = fpmm_trade_rows(key);
    if (rows == nullptr)
      return false;
    FPMMTradeInfo *matched = nullptr;
    int64_t matched_log = 0;
    for (auto &info : *rows) {
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
    auto *rows = fpmm_trade_rows(key);
    if (rows == nullptr)
      return false;
    for (const auto &info : *rows) {
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
  auto funding_matches_add_amount = [&](const FPMMFundingInfo &info, int64_t transfer_amount,
                                        bool expect_refund) {
    int64_t split_amount = funding_split_amount(info);
    if (!expect_refund) {
      return split_amount == transfer_amount;
    }
    if (known_token && token_idx != UNKNOWN_TOKEN_IDX && token_idx < info.amounts.size()) {
      int64_t expected_refund = split_amount - info.amounts[token_idx];
      return expected_refund == transfer_amount;
    }
    for (int64_t amount_i : info.amounts) {
      if (split_amount - amount_i == transfer_amount) {
        return true;
      }
    }
    return false;
  };
  auto find_fpmm_funding_info = [&](const TxFPMMKey &key, int64_t transfer_amount,
                                    bool expect_refund) -> FPMMFundingInfo * {
    auto *rows = fpmm_funding_rows(key);
    if (rows == nullptr)
      return nullptr;
    return select_window_or_forward(
        *rows,
        [&](const FPMMFundingInfo &info) {
          if (info.log_index < base_log_index) {
            return false;
          }
          if (info.side != 1 || info.amounts.empty()) {
            return false;
          }
          return funding_matches_add_amount(info, transfer_amount, expect_refund);
        },
        "FPMMFundingWindowUniqueCandidate",
        "FPMMFundingUniqueCandidate");
  };
  auto has_pending_future_fpmm_add = [&](const TxFPMMKey &key,
                                         int64_t transfer_amount,
                                         bool expect_refund) {
    auto *rows = fpmm_funding_rows(key);
    if (rows == nullptr)
      return false;
    for (const auto &info : *rows) {
      if (info.log_index < base_log_index)
        continue;
      if (info.side != 1 || info.amounts.empty())
        continue;
      if (info.consumed_count != 0)
        continue;
      if (funding_matches_add_amount(info, transfer_amount, expect_refund))
        return true;
    }
    return false;
  };
  auto has_pending_future_fpmm_remove_for_transfer = [&](const TxFPMMKey &key,
                                                         const std::string &funder,
                                                         int64_t transfer_amount) {
    auto *rows = fpmm_funding_rows(key);
    if (rows == nullptr)
      return false;
    for (const auto &info : *rows) {
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
    auto *rows = fpmm_funding_rows(key);
    if (rows == nullptr)
      return nullptr;
    return select_window_or_forward(
        *rows,
        [&](const FPMMFundingInfo &info) {
          if (info.log_index < base_log_index) {
            return false;
          }
          if (info.side != 2 || info.funder != funder) {
            return false;
          }
          return funding_matches_remove_amount(info, transfer_amount);
        },
        "FPMMRemoveWindowUniqueCandidate",
        "FPMMRemoveUniqueCandidate");
  };
  auto is_exchange_addr = [](const std::string &addr) {
    return addr == CTF_EXCHANGE || addr == NEG_RISK_CTF_EXCHANGE;
  };
  auto order_leg_matches = [&](const OrderInfo &info) {
    if (info.maker_side == 1) {
      // BUY maker leg can be either:
      // - direct taker -> maker (fillOrder/fillOrders)
      // - exchange -> maker (matchOrders via _fillFacingExchange)
      return to == info.maker && (from == info.taker || is_exchange_addr(from));
    }
    // SELL maker leg can be either:
    // - direct maker -> taker (fillOrder/fillOrders)
    // - maker -> exchange (matchOrders via _fillFacingExchange)
    return from == info.maker && (to == info.taker || is_exchange_addr(to));
  };
  bool order_window_conflict = false;
  auto find_order_info = [&]() -> OrderInfo * {
    order_window_conflict = false;
    if (tx_order_amount_rows == nullptr)
      return nullptr;
    auto amount_it = tx_order_amount_rows->find(amount);
    if (amount_it == tx_order_amount_rows->end()) {
      return nullptr;
    }
    const auto &candidates = amount_it->second;
    bool has_unconsumed_window_same_amount = false;
    OrderInfo *window_matched = nullptr;
    int window_match_count = 0;
    OrderInfo *forward_matched = nullptr;
    int64_t forward_log = -1;
    int forward_same_log_count = 0;
    for (OrderInfo *info_ptr : candidates) {
      OrderInfo &info = *info_ptr;
      if (info.consumed)
        continue;
      if (semantic_log_matches(info.log_index)) {
        has_unconsumed_window_same_amount = true;
      }
      if (!order_leg_matches(info)) {
        continue;
      }
      // In matchOrders paths, transfer legs can happen before the maker order's
      // own OrderFilled log. Allow nearest-future fallback when window misses.
      if (semantic_log_matches(info.log_index)) {
        window_matched = &info;
        window_match_count++;
        continue;
      }
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
    OrderInfo *matched = select_window_only(
        window_matched, window_match_count, "OrderWindowUniqueCandidate");
    if (matched == nullptr) {
      stage2_assert(forward_same_log_count <= 1, AssertLevel::L2, "Match",
                    "OrderForwardUniqueCandidate");
      matched = forward_matched;
    }
    // Only treat as hard conflict when the current semantic window has same-amount
    // order legs but none can match address constraints and no future fallback exists.
    order_window_conflict =
        has_unconsumed_window_same_amount && (matched == nullptr);
    return matched;
  };
  auto find_convert_window_match = [&](auto &&visit_rows,
                                       const std::string &stakeholder,
                                       int64_t amt) -> ConvertInfo * {
    ConvertInfo *window_matched = nullptr;
    int window_match_count = 0;
    visit_rows([&](ConvertInfo &info) {
      if (info.stakeholder != stakeholder || info.amount != amt) {
        return;
      }
      if (!semantic_log_matches(info.log_index)) {
        return;
      }
      window_matched = &info;
      window_match_count++;
    });
    return select_window_only(window_matched, window_match_count,
                              "ConvertWindowUniqueCandidate");
  };
  auto find_convert_info = [&](const std::string &market_id,
                               const std::string &stakeholder,
                               int64_t amt) -> ConvertInfo * {
    TxMarketKey tx_market_key{block, tx_hash, market_id};
    auto it = tx_convert_.find(tx_market_key);
    if (it == tx_convert_.end())
      return nullptr;
    return find_convert_window_match(
        [&](auto &&accept) {
          for (auto &info : it->second) {
            accept(info);
          }
        },
        stakeholder, amt);
  };
  auto find_convert_info_any_market = [&](const std::string &stakeholder,
                                          int64_t amt) -> ConvertInfo * {
    TxKey tx_key{block, tx_hash};
    auto it = tx_convert_by_tx_.find(tx_key);
    if (it == tx_convert_by_tx_.end()) {
      return nullptr;
    }
    return find_convert_window_match(
        [&](auto &&accept) {
          for (ConvertInfo *info : it->second) {
            accept(*info);
          }
        },
        stakeholder, amt);
  };
  auto inc_consumed_count = [&](auto *info) {
    if (info != nullptr) {
      info->consumed_count++;
    }
  };
  auto mark_covered_by_parent = [&](auto *info) {
    if (info != nullptr) {
      info->covered_by_parent = true;
    }
  };
  auto consume_split = [&](SplitInfo *info) { inc_consumed_count(info); };
  auto consume_merge = [&](MergeInfo *info) { inc_consumed_count(info); };
  auto consume_redeem = [&](RedemptionInfo *info) { inc_consumed_count(info); };
  auto cover_split = [&](SplitInfo *info) { mark_covered_by_parent(info); };
  auto cover_merge = [&](MergeInfo *info) { mark_covered_by_parent(info); };
  enum class CondLegKind {
    None,
    Split,
    Merge,
    Redeem,
  };
  auto choose_cond_leg = [&](SplitInfo *sit, MergeInfo *mit, RedemptionInfo *rit,
                             const char *rule_tag) {
    CondLegKind chosen = CondLegKind::None;
    int64_t best_dist = (1LL << 62);
    auto consider = [&](CondLegKind kind, int64_t log_index) {
      int64_t dist = semantic_log_distance(log_index);
      if (chosen == CondLegKind::None || dist < best_dist) {
        chosen = kind;
        best_dist = dist;
        return;
      }
      if (dist == best_dist) {
        stage2_assert(false, AssertLevel::L2, "Match", rule_tag);
      }
    };
    if (sit != nullptr)
      consider(CondLegKind::Split, sit->log_index);
    if (mit != nullptr)
      consider(CondLegKind::Merge, mit->log_index);
    if (rit != nullptr)
      consider(CondLegKind::Redeem, rit->log_index);
    return chosen;
  };

  if (amount == 0) {
    if (op_is_fpmm) {
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
    SplitInfo *split_match = find_split_info(to, amount);
    MergeInfo *merge_match = find_merge_info(to, amount);
    // Redemption does not mint ERC1155 legs.
    CondLegKind chosen = choose_cond_leg(split_match, merge_match, nullptr,
                                         "MintCondLegUniqueCandidate");

    if (chosen == CondLegKind::Merge) {
      MergeInfo *mit = merge_match;
      bind_root(RootOpType::Merge, "mint_parent");
      consume_merge(mit);
      patch_collateral_from_semantic(mit->collateral_token);
      return emit_split_or_merge(false, to, amount);
    }

    if (chosen == CondLegKind::Split) {
      bind_root(RootOpType::Split, "mint_child");
      consume_split(split_match);
      patch_collateral_from_semantic(split_match->collateral_token);
      return emit_split_or_merge(true, to, amount);
    }

    return emit_and_classify_generic_in(to, TransferClass::InternalTransferOther);
  }

  // ========== burn 分支 (to == 0x0, 非FPMM operator) ==========
  if (to == ZERO_ADDR && !op_is_fpmm) {
    SplitInfo *split_match = find_split_info(from, amount);
    MergeInfo *merge_match = find_merge_info(from, amount);
    RedemptionInfo *redeem_match = find_redemption_info(from);
    CondLegKind chosen = choose_cond_leg(split_match, merge_match, redeem_match,
                                         "BurnCondLegUniqueCandidate");
    if (chosen == CondLegKind::Split) {
      SplitInfo *sit = split_match;
      bind_root(RootOpType::Split, "burn_parent");
      consume_split(sit);
      patch_collateral_from_semantic(sit->collateral_token);
      return emit_split_or_merge(true, from, -amount);
    }

    if (chosen == CondLegKind::Merge) {
      bind_root(RootOpType::Merge, "burn_child");
      consume_merge(merge_match);
      patch_collateral_from_semantic(merge_match->collateral_token);
      return emit_split_or_merge(false, from, -amount);
    }

    if (chosen == CondLegKind::Redeem) {
      RedemptionInfo *rit = redeem_match;
      bind_root(RootOpType::Redemption, "burn_child");
      consume_redeem(rit);
      patch_collateral_from_semantic(rit->collateral_token);
      if (known_token) {
        if (is_user_addr(from)) {
          auto &payouts = conditions_[cond_idx].payout_numerators;
          // Redemption payout in price_1e6:
          // payout_price = payout_numerator[token_idx] * 1e6 / payout_denominator,
          // where payout_denominator is the sum of all payout numerators.
          // Losing legs are valid and carry zero payout.
          // If payout is unknown (condition_resolution not yet processed), use price=0.
          int64_t payout_price = 0;
          bool payout_known = token_idx < static_cast<int>(payouts.size()) && payouts[token_idx] >= 0;
          if (is_usdc_collateral(collateral) && payout_known) {
            double payout_denominator = 0.0;
            for (int64_t p : payouts) {
              if (p > 0) {
                payout_denominator += static_cast<double>(p);
              }
            }
            assert(payout_denominator > 0.0);
            payout_price = static_cast<int64_t>(std::llround(
                static_cast<double>(payouts[token_idx]) * 1e6 / payout_denominator));
            assert(payout_price >= 0);
          }
          emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Redemption, token_idx, coll, 0, -amount, payout_price});
        }
        return TransferClass::Redemption;
      } else {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::RedemptionNonPoly, token_idx, coll, 0, -amount, 0});
        return TransferClass::RedemptionNonPoly;
      }
    }

    if (from == NEG_RISK_ADAPTER && chosen == CondLegKind::None)
      return TransferClass::InternalBurnNegRisk;

    return emit_and_classify_generic_out(from, TransferClass::InternalTransferOther);
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

      int64_t price = calc_price_if_usdc_collateral(oit->quote_amount, oit->tokens);
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::OrderBuy, token_idx, coll, 0, amount, price});
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::OrderSell, token_idx, coll, 0, -amount, price});
      return classify_transfer_by_counterparty(TransferClass::OrderBuy, TransferClass::OrderSell,
                                               TransferClass::InternalTransferOrder);
    }

    if (order_window_conflict) {
      stage2_assert(false, AssertLevel::L3, "Order", "OrderWindowAddressConflict");
      return TransferClass::Unclassified;
    }
    // Without order semantics, fall through to generic transfer classification.
  }

  // ========== NegRisk Adapter operator ==========
  if (op == NEG_RISK_ADAPTER) {
    if (to == NO_TOKEN_BURN_ADDRESS) {
      if (from == NEG_RISK_ADAPTER)
        return TransferClass::InternalBurnConvert;

      ConvertInfo *convert_info = nullptr;
      bool has_market_hint = false;
      if (known_token && !conditions_[cond_idx].question_id.empty()) {
        auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
        if (market_it != cond_to_market_.end()) {
          has_market_hint = true;
          convert_info = find_convert_info(market_it->second, from, amount);
        }
      }
      if (convert_info == nullptr && !has_market_hint) {
        convert_info = find_convert_info_any_market(from, amount);
      }
      if (convert_info != nullptr) {
        bind_root(RootOpType::Convert, "convert_no_burn");
        if (known_token) {
          stage2_assert(token_idx == 1, AssertLevel::L3, "Convert", "BurnTokenIsNO");
        }
        convert_info->consumed_count++;
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Convert, token_idx, coll, 0, -amount, 0});
        return TransferClass::Convert;
      }
      stage2_assert(false, AssertLevel::L3, "Convert", "BurnWithoutSemanticEvent");
      return TransferClass::Unclassified;
    }

    if (from == NEG_RISK_ADAPTER) {
      SplitInfo *split_info = find_split_info(NEG_RISK_ADAPTER, amount);
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
      emit_negrisk_transfer(to, true);
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
      emit_negrisk_transfer(from, false);
      return TransferClass::TransferOutNegRisk;
    }
    // Adapter may operate transfers on behalf of users/vaults in paths that are
    // not split/merge/convert legs. Only tag as NegRisk when token's condition
    // is explicitly in NegRisk set; otherwise use generic transfer classes.
    bool is_negrisk_known = known_token && is_negrisk_cond_visible_at(cond_idx, sort_key);
    if (is_negrisk_known) {
      return emit_and_classify_negrisk_in_out(TransferClass::InternalTransferNegRisk);
    }
    return emit_and_classify_generic_in_out(TransferClass::InternalTransferOther);
  }

  // ========== FPMM operator ==========
  if (op_is_fpmm) {
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
      stage2_assert(!has_pending_future_fpmm_add(tx_fpmm_key, amount, false),
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
        int64_t price = calc_price_if_usdc_collateral(buy_info->collateral_amount, buy_info->tokens);
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMBuy, token_idx, coll, 0, amount, price});
        return TransferClass::FPMMBuy;
      }
      if (chosen == FromFPMMMatch::Remove) {
        bind_root(RootOpType::FPMMFunding, "lp_remove_from_pool");
        remove_info->consumed_count++;
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, coll, 0, amount, split_price});
        return TransferClass::FPMMLPRemove;
      }

      stage2_assert(!has_pending_future_fpmm_add(tx_fpmm_key, amount, true),
                    AssertLevel::L3, "FPMMFunding", "TransferWithoutFundingLegMatch");
      stage2_assert(!has_pending_future_fpmm_remove_for_transfer(tx_fpmm_key, to, amount),
                    AssertLevel::L3, "FPMMFunding", "TransferWithoutFundingLegMatch");
      return emit_and_classify_generic_in(to, TransferClass::InternalTransferFPMM);
    }

    if (to == op) {
      FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 2, from, amount);
      if (tit != nullptr) {
        bind_root(RootOpType::FPMMTrade, "sell_to_pool");
        tit->consumed = true;
        stage2_assert(amount == tit->tokens, AssertLevel::L3, "FPMMTrade", "SellAmountMatch");
        int64_t price = calc_price_if_usdc_collateral(tit->collateral_amount, tit->tokens);
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::FPMMSell, token_idx, coll, 0, -amount, price});
        return TransferClass::FPMMSell;
      }
      return emit_and_classify_generic_out(from, TransferClass::InternalTransferFPMM);
    }
    // FPMM as operator but neither from nor to is FPMM: emit to both parties
    return emit_and_classify_generic_in_out(TransferClass::InternalTransferFPMM);
  }

  // ========== 普通用户转账 ==========
  stage2_assert(root_bind.op == RootOpType::None, AssertLevel::L2, "Bind",
                "FallbackWithoutRootBind", root_bind.leg);
  return emit_and_classify_generic_in_out(TransferClass::InternalTransferOther);
}

} // namespace stage2
