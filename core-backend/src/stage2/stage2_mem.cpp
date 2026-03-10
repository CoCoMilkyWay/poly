#include "stage2_builder.hpp"

#include "../core/mem.hpp"
#include "../core/mem_json.hpp"

#include <algorithm>

namespace stage2::mem {

int64_t estimate_string_vector_extra(const std::vector<std::string> &v) {
  return core::mem::estimate_vector(v, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
}

int64_t estimate_condition_info_extra(const ConditionInfo &info) {
  int64_t bytes = core::mem::estimate_string_extra(info.question_id);
  bytes += static_cast<int64_t>(info.payout_numerators.capacity()) * static_cast<int64_t>(sizeof(int64_t));
  return bytes;
}

int64_t estimate_split_info_extra(const SplitInfo &x) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_string_extra(x.stakeholder);
  bytes += core::mem::estimate_string_extra(x.collateral_token);
  bytes += core::mem::estimate_string_extra(x.parent_collection_id);
  bytes += core::mem::estimate_string_extra(x.cond_id);
  bytes += estimate_string_vector_extra(x.partition);
  return bytes;
}

int64_t estimate_merge_info_extra(const MergeInfo &x) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_string_extra(x.stakeholder);
  bytes += core::mem::estimate_string_extra(x.collateral_token);
  bytes += core::mem::estimate_string_extra(x.parent_collection_id);
  bytes += core::mem::estimate_string_extra(x.cond_id);
  bytes += estimate_string_vector_extra(x.partition);
  return bytes;
}

int64_t estimate_redemption_info_extra(const RedemptionInfo &x) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_string_extra(x.redeemer);
  bytes += core::mem::estimate_string_extra(x.collateral_token);
  bytes += core::mem::estimate_string_extra(x.parent_collection_id);
  bytes += core::mem::estimate_string_extra(x.cond_id);
  bytes += estimate_string_vector_extra(x.index_sets);
  return bytes;
}

int64_t estimate_convert_info_extra(const ConvertInfo &x) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_string_extra(x.market_id);
  bytes += core::mem::estimate_string_extra(x.index_set);
  bytes += core::mem::estimate_string_extra(x.stakeholder);
  return bytes;
}

int64_t estimate_order_info_extra(const OrderInfo &x) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_string_extra(x.token_id);
  bytes += core::mem::estimate_string_extra(x.maker);
  bytes += core::mem::estimate_string_extra(x.taker);
  return bytes;
}

int64_t estimate_fpmm_trade_info_extra(const FPMMTradeInfo &x) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_string_extra(x.fpmm_addr);
  bytes += core::mem::estimate_string_extra(x.trader);
  return bytes;
}

int64_t estimate_fpmm_funding_info_extra(const FPMMFundingInfo &x) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_string_extra(x.fpmm_addr);
  bytes += core::mem::estimate_string_extra(x.funder);
  bytes += static_cast<int64_t>(x.amounts.capacity()) * static_cast<int64_t>(sizeof(int64_t));
  return bytes;
}

} // namespace stage2::mem

namespace stage2 {

json EventBuilder::memory_breakdown() const {
  std::lock_guard<std::mutex> lock(mem_mu_);
  return mem_snapshot_;
}

void EventBuilder::refresh_memory_snapshot(const char *phase) {
  auto no_extra = [](const auto &) -> int64_t { return 0; };

  const int64_t conditions_bytes =
      core::mem::estimate_vector(conditions_, [](const ConditionInfo &info) { return mem::estimate_condition_info_extra(info); });
  const int64_t cond_ids_bytes =
      core::mem::estimate_vector(cond_ids_, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
  const int64_t cond_map_bytes =
      core::mem::estimate_unordered_map(cond_map_, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra);
  const int64_t token_map_bytes =
      core::mem::estimate_unordered_map(token_map_, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra);
  const int64_t fpmm_map_bytes =
      core::mem::estimate_unordered_map(fpmm_map_, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra);
  const int64_t cond_to_market_bytes = core::mem::estimate_unordered_map(
      cond_to_market_, [](const std::string &k) { return core::mem::estimate_string_extra(k); },
      [](const std::string &v) { return core::mem::estimate_string_extra(v); });
  const int64_t seen_users_bytes =
      core::mem::estimate_unordered_set(seen_users_, [](const std::string &k) { return core::mem::estimate_string_extra(k); });
  const int64_t fpmm_cond_idxs_bytes = core::mem::estimate_unordered_set(fpmm_cond_idxs_, no_extra);
  const int64_t negrisk_cond_idxs_bytes = core::mem::estimate_unordered_set(negrisk_cond_idxs_, no_extra);
  const int64_t cond_collateral_bytes = core::mem::estimate_unordered_map(cond_collateral_, no_extra, no_extra);
  const int64_t collateral_addr_to_id_bytes = core::mem::estimate_unordered_map(
      collateral_addr_to_id_, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra);
  const int64_t collateral_id_to_addr_bytes = core::mem::estimate_unordered_map(
      collateral_id_to_addr_, no_extra, [](const std::string &v) { return core::mem::estimate_string_extra(v); });

  const int64_t tx_split_bytes =
      core::mem::estimate_unordered_map(tx_split_, no_extra, [](const std::vector<SplitInfo> &v) {
        return core::mem::estimate_vector(v, [](const SplitInfo &x) { return mem::estimate_split_info_extra(x); });
      });
  const int64_t tx_merge_bytes =
      core::mem::estimate_unordered_map(tx_merge_, no_extra, [](const std::vector<MergeInfo> &v) {
        return core::mem::estimate_vector(v, [](const MergeInfo &x) { return mem::estimate_merge_info_extra(x); });
      });
  const int64_t tx_redemption_bytes =
      core::mem::estimate_unordered_map(tx_redemption_, no_extra, [](const std::vector<RedemptionInfo> &v) {
        return core::mem::estimate_vector(v, [](const RedemptionInfo &x) { return mem::estimate_redemption_info_extra(x); });
      });
  const int64_t tx_convert_bytes = core::mem::estimate_unordered_map(
      tx_convert_, [](const TxMarketKey &k) { return core::mem::estimate_string_extra(k.market_id); },
      [](const std::vector<ConvertInfo> &v) {
        return core::mem::estimate_vector(v, [](const ConvertInfo &x) { return mem::estimate_convert_info_extra(x); });
      });
  const int64_t tx_order_bytes = core::mem::estimate_unordered_map(
      tx_order_, [](const TxTokenKey &k) { return core::mem::estimate_string_extra(k.token_id); },
      [](const std::vector<OrderInfo> &v) {
        return core::mem::estimate_vector(v, [](const OrderInfo &x) { return mem::estimate_order_info_extra(x); });
      });
  const int64_t tx_convert_by_tx_bytes =
      core::mem::estimate_unordered_map(tx_convert_by_tx_, no_extra, [](const std::vector<ConvertInfo *> &v) {
        return core::mem::estimate_vector_plain(v);
      });
  const int64_t tx_order_by_amount_bytes = core::mem::estimate_unordered_map(
      tx_order_by_amount_, [](const TxTokenKey &k) { return core::mem::estimate_string_extra(k.token_id); },
      [&](const std::unordered_map<int64_t, std::vector<OrderInfo *>> &inner) {
        return core::mem::estimate_unordered_map(inner, no_extra, [](const std::vector<OrderInfo *> &v) {
          return core::mem::estimate_vector_plain(v);
        });
      });
  const int64_t tx_split_by_actor_amount_bytes =
      core::mem::estimate_unordered_map(tx_split_by_actor_amount_, no_extra,
                                        [&](const std::unordered_map<std::string, std::vector<SplitInfo *>> &inner) {
                                          return core::mem::estimate_unordered_map(
                                              inner, [](const std::string &k) { return core::mem::estimate_string_extra(k); },
                                              [](const std::vector<SplitInfo *> &v) { return core::mem::estimate_vector_plain(v); });
                                        });
  const int64_t tx_merge_by_actor_amount_bytes =
      core::mem::estimate_unordered_map(tx_merge_by_actor_amount_, no_extra,
                                        [&](const std::unordered_map<std::string, std::vector<MergeInfo *>> &inner) {
                                          return core::mem::estimate_unordered_map(
                                              inner, [](const std::string &k) { return core::mem::estimate_string_extra(k); },
                                              [](const std::vector<MergeInfo *> &v) { return core::mem::estimate_vector_plain(v); });
                                        });
  const int64_t tx_redemption_by_actor_bytes =
      core::mem::estimate_unordered_map(tx_redemption_by_actor_, no_extra,
                                        [&](const std::unordered_map<std::string, std::vector<RedemptionInfo *>> &inner) {
                                          return core::mem::estimate_unordered_map(
                                              inner, [](const std::string &k) { return core::mem::estimate_string_extra(k); },
                                              [](const std::vector<RedemptionInfo *> &v) { return core::mem::estimate_vector_plain(v); });
                                        });
  const int64_t tx_fpmm_trade_by_leg_bytes = core::mem::estimate_unordered_map(
      tx_fpmm_trade_by_leg_, [](const TxFPMMKey &k) { return core::mem::estimate_string_extra(k.fpmm_addr); },
      [&](const std::unordered_map<std::string, std::vector<FPMMTradeInfo *>> &inner) {
        return core::mem::estimate_unordered_map(
            inner, [](const std::string &k) { return core::mem::estimate_string_extra(k); },
            [](const std::vector<FPMMTradeInfo *> &v) { return core::mem::estimate_vector_plain(v); });
      });
  const int64_t tx_fpmm_trade_bytes = core::mem::estimate_unordered_map(
      tx_fpmm_trade_, [](const TxFPMMKey &k) { return core::mem::estimate_string_extra(k.fpmm_addr); },
      [](const std::vector<FPMMTradeInfo> &v) {
        return core::mem::estimate_vector(v, [](const FPMMTradeInfo &x) { return mem::estimate_fpmm_trade_info_extra(x); });
      });
  const int64_t tx_fpmm_funding_bytes = core::mem::estimate_unordered_map(
      tx_fpmm_funding_, [](const TxFPMMKey &k) { return core::mem::estimate_string_extra(k.fpmm_addr); },
      [](const std::vector<FPMMFundingInfo> &v) {
        return core::mem::estimate_vector(v, [](const FPMMFundingInfo &x) { return mem::estimate_fpmm_funding_info_extra(x); });
      });
  const int64_t tx_op_bounds_bytes =
      core::mem::estimate_unordered_map(tx_op_bounds_, no_extra, [](const std::vector<TxOpBounds> &v) {
        return core::mem::estimate_vector_plain(v);
      });

  const int64_t new_conditions_bytes = core::mem::estimate_vector(new_conditions_, [](const NewCondition &x) {
    return core::mem::estimate_string_extra(x.cond_id) + mem::estimate_condition_info_extra(x.info);
  });
  const int64_t new_tokens_bytes =
      core::mem::estimate_vector(new_tokens_, [](const NewToken &x) { return core::mem::estimate_string_extra(x.token_id); });
  const int64_t new_condition_pos_bytes = core::mem::estimate_unordered_map(new_condition_pos_, no_extra, no_extra);
  const int64_t new_token_pos_bytes = core::mem::estimate_unordered_map(
      new_token_pos_, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra);
  const int64_t new_fpmms_bytes =
      core::mem::estimate_vector(new_fpmms_, [](const NewFPMM &x) { return core::mem::estimate_string_extra(x.addr); });
  const int64_t new_collaterals_bytes =
      core::mem::estimate_vector(new_collaterals_, [](const NewCollateral &x) { return core::mem::estimate_string_extra(x.addr); });
  const int64_t new_cond_collaterals_bytes = core::mem::estimate_vector_plain(new_cond_collaterals_);
  const int64_t new_neg_risk_markets_bytes = core::mem::estimate_vector(new_neg_risk_markets_, [](const NewNegRiskMarket &x) {
    return core::mem::estimate_string_extra(x.question_id) + core::mem::estimate_string_extra(x.market_id);
  });
  const int64_t new_events_bytes = core::mem::estimate_vector(new_events_, [](const std::tuple<std::string, RawEvent> &x) {
    return core::mem::estimate_string_extra(std::get<0>(x));
  });

  const int64_t chunk_token_visible_bytes = core::mem::estimate_unordered_map(
      chunk_token_known_visible_from_sort_, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra);
  const int64_t chunk_fpmm_visible_bytes = core::mem::estimate_unordered_map(
      chunk_fpmm_visible_from_sort_, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra);
  const int64_t chunk_negrisk_visible_bytes = core::mem::estimate_unordered_map(chunk_negrisk_visible_from_sort_, no_extra, no_extra);

  auto estimate_payload = [&](const std::optional<CommitPayload> &payload_opt) -> int64_t {
    if (!payload_opt.has_value()) {
      return 0;
    }
    const auto &p = *payload_opt;
    int64_t bytes = static_cast<int64_t>(sizeof(CommitPayload)) + static_cast<int64_t>(sizeof(BuildProgress));
    bytes += core::mem::estimate_vector(p.new_conditions, [](const NewCondition &x) {
      return core::mem::estimate_string_extra(x.cond_id) + mem::estimate_condition_info_extra(x.info);
    });
    bytes += core::mem::estimate_vector(p.new_tokens, [](const NewToken &x) { return core::mem::estimate_string_extra(x.token_id); });
    bytes += core::mem::estimate_vector(p.new_fpmms, [](const NewFPMM &x) { return core::mem::estimate_string_extra(x.addr); });
    bytes += core::mem::estimate_vector(p.new_collaterals, [](const NewCollateral &x) { return core::mem::estimate_string_extra(x.addr); });
    bytes += core::mem::estimate_vector_plain(p.new_cond_collaterals);
    bytes += core::mem::estimate_vector(p.new_neg_risk_markets, [](const NewNegRiskMarket &x) {
      return core::mem::estimate_string_extra(x.question_id) + core::mem::estimate_string_extra(x.market_id);
    });
    bytes += core::mem::estimate_vector(p.new_events, [](const std::tuple<std::string, RawEvent> &x) {
      return core::mem::estimate_string_extra(std::get<0>(x));
    });
    return bytes;
  };
  std::optional<CommitPayload> commit_payload_copy;
  std::optional<CommitPayload> commit_reusable_payload_copy;
  {
    std::lock_guard<std::mutex> lock(commit_mu_);
    if (commit_payload_.has_value()) {
      commit_payload_copy = *commit_payload_;
    }
    if (commit_reusable_payload_.has_value()) {
      commit_reusable_payload_copy = *commit_reusable_payload_;
    }
  }
  const int64_t commit_payload_bytes = estimate_payload(commit_payload_copy);
  const int64_t commit_reusable_payload_bytes = estimate_payload(commit_reusable_payload_copy);

  const int64_t persistent_bytes =
      conditions_bytes + cond_ids_bytes + cond_map_bytes + token_map_bytes + fpmm_map_bytes + cond_to_market_bytes +
      seen_users_bytes + fpmm_cond_idxs_bytes + negrisk_cond_idxs_bytes + cond_collateral_bytes +
      collateral_addr_to_id_bytes + collateral_id_to_addr_bytes;

  const int64_t chunk_working_set_bytes =
      tx_split_bytes + tx_merge_bytes + tx_redemption_bytes + tx_convert_bytes + tx_order_bytes +
      tx_convert_by_tx_bytes + tx_order_by_amount_bytes + tx_split_by_actor_amount_bytes +
      tx_merge_by_actor_amount_bytes + tx_redemption_by_actor_bytes + tx_fpmm_trade_by_leg_bytes +
      tx_fpmm_trade_bytes + tx_fpmm_funding_bytes + tx_op_bounds_bytes + new_conditions_bytes +
      new_tokens_bytes + new_condition_pos_bytes + new_token_pos_bytes + new_fpmms_bytes + new_collaterals_bytes +
      new_cond_collaterals_bytes + new_neg_risk_markets_bytes + new_events_bytes + chunk_token_visible_bytes +
      chunk_fpmm_visible_bytes + chunk_negrisk_visible_bytes;

  const int64_t pending_commit_bytes = commit_payload_bytes + commit_reusable_payload_bytes;
  mem_peak_chunk_bytes_ = std::max(mem_peak_chunk_bytes_, chunk_working_set_bytes + pending_commit_bytes);

  std::vector<std::pair<std::string, int64_t>> rows = {
      {"conditions_", conditions_bytes},
      {"cond_ids_", cond_ids_bytes},
      {"cond_map_", cond_map_bytes},
      {"token_map_", token_map_bytes},
      {"fpmm_map_", fpmm_map_bytes},
      {"cond_to_market_", cond_to_market_bytes},
      {"seen_users_", seen_users_bytes},
      {"tx_split_", tx_split_bytes},
      {"tx_merge_", tx_merge_bytes},
      {"tx_redemption_", tx_redemption_bytes},
      {"tx_convert_", tx_convert_bytes},
      {"tx_order_", tx_order_bytes},
      {"tx_order_by_amount_", tx_order_by_amount_bytes},
      {"tx_split_by_actor_amount_", tx_split_by_actor_amount_bytes},
      {"tx_merge_by_actor_amount_", tx_merge_by_actor_amount_bytes},
      {"tx_redemption_by_actor_", tx_redemption_by_actor_bytes},
      {"tx_fpmm_trade_by_leg_", tx_fpmm_trade_by_leg_bytes},
      {"tx_fpmm_trade_", tx_fpmm_trade_bytes},
      {"tx_fpmm_funding_", tx_fpmm_funding_bytes},
      {"tx_op_bounds_", tx_op_bounds_bytes},
      {"new_events_", new_events_bytes},
      {"new_tokens_", new_tokens_bytes},
      {"commit_payload_", commit_payload_bytes},
      {"commit_reusable_payload_", commit_reusable_payload_bytes},
  };
  core::mem::sort_mem_rows_desc(rows);

  json snapshot = {
      {"phase", phase},
      {"persistent_bytes", persistent_bytes},
      {"chunk_working_set_bytes", chunk_working_set_bytes},
      {"pending_commit_bytes", pending_commit_bytes},
      {"peak_chunk_plus_commit_bytes", mem_peak_chunk_bytes_},
      {"estimated_total_bytes", persistent_bytes + chunk_working_set_bytes + pending_commit_bytes},
      {"items", core::mem::build_items_json(rows, 20)},
      {"hint", "Estimated from container capacities and string capacities; good for trend/ranking, not exact allocator bytes."},
  };

  std::lock_guard<std::mutex> lock(mem_mu_);
  mem_snapshot_ = std::move(snapshot);
}

} // namespace stage2
