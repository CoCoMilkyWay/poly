#pragma once

#include "../core/ctf_helpers.hpp"
#include "../core/database.hpp"
#include "../core/keccak256.hpp"
#include "../core/rocks_store.hpp"
#include "stage2_assert.hpp"
#include "stage2_models.hpp"
#include "stage2_types.hpp"
#include "stage2_utils.hpp"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace stage2 {

class EventBuilder {
public:
  EventBuilder(Database &stage1_db, Database &stage2_db);
  ~EventBuilder();

  void init_schema();

  void load_from_rb();

  int64_t cursor() const;
  const core::rocks::Stage2UserEventStore &user_event_store() const;
  bool is_building() const;
  bool has_pending_commit() const;

  bool build_chunk(int64_t target_block);
  void wait_for_pending_commit();
  void request_stop();
  void clear_stop();
  void persist_restore_cache_snapshot();

  const BuildProgress &progress() const;
  const BuildProgress &committed_progress() const;
  json memory_breakdown() const;
  json rocksdb_memory_breakdown() const;

private:
  Database &stage1_db_;
  Database &stage2_db_;
  std::unique_ptr<Database> restore_cache_db_;
  std::unique_ptr<core::rocks::Stage2UserEventStore> user_event_store_;
  BuildProgress progress_;
  BuildProgress committed_progress_;

  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<std::string, uint32_t> cond_map_;
  std::unordered_map<std::string, TokenInfo> token_map_;
  std::unordered_map<std::string, FPMMInfo> fpmm_map_;
  std::unordered_map<std::string, std::string> cond_to_market_; // question_id -> market_id
  std::unordered_set<std::string> seen_users_;                  // Users seen in this process
  std::unordered_set<uint32_t> fpmm_cond_idxs_;                 // Polymarket AMM 对应的 cond_idx
  std::unordered_set<uint32_t> negrisk_cond_idxs_;              // NegRisk 对应的 cond_idx
  std::unordered_map<uint32_t, uint8_t> cond_collateral_;       // cond_idx -> collateral_id
  std::unordered_map<std::string, uint8_t> collateral_addr_to_id_;
  std::unordered_map<uint8_t, std::string> collateral_id_to_addr_;
  uint8_t next_collateral_id_ = static_cast<uint8_t>(Collateral::WrappedUSDCe) + 1;

  // 按 TxKey (tx_hash) 索引,统一处理已知和未知 token
  std::unordered_map<TxKey, std::vector<SplitInfo>> tx_split_;
  std::unordered_map<TxKey, std::vector<MergeInfo>> tx_merge_;
  std::unordered_map<TxKey, std::vector<RedemptionInfo>> tx_redemption_;
  std::unordered_map<TxMarketKey, std::vector<ConvertInfo>> tx_convert_;
  std::unordered_map<TxTokenKey, std::vector<OrderInfo>> tx_order_;
  std::unordered_map<TxKey, std::vector<ConvertInfo *>> tx_convert_by_tx_;
  std::unordered_map<TxTokenKey, std::unordered_map<int64_t, std::vector<OrderInfo *>>> tx_order_by_amount_;
  std::unordered_map<TxKey, std::unordered_map<std::string, std::vector<SplitInfo *>>> tx_split_by_actor_amount_;
  std::unordered_map<TxKey, std::unordered_map<std::string, std::vector<MergeInfo *>>> tx_merge_by_actor_amount_;
  std::unordered_map<TxKey, std::unordered_map<std::string, std::vector<RedemptionInfo *>>> tx_redemption_by_actor_;
  std::unordered_map<TxFPMMKey, std::unordered_map<std::string, std::vector<FPMMTradeInfo *>>> tx_fpmm_trade_by_leg_;
  std::unordered_map<TxFPMMKey, std::vector<FPMMTradeInfo>> tx_fpmm_trade_;
  std::unordered_map<TxFPMMKey, std::vector<FPMMFundingInfo>> tx_fpmm_funding_;
  // Tx-level semantic bounds built from ordered semantic log_index sequence.
  std::unordered_map<TxKey, std::vector<TxOpBounds>> tx_op_bounds_;
  TransferStats chunk_xfer_stats_;             // 当前 chunk 的 transfer 统计
  SplitSemanticTree chunk_split_sem_tree_;     // 当前 chunk 的 split 语义统计
  MergeSemanticTree chunk_merge_sem_tree_;     // 当前 chunk 的 merge 语义统计
  ConvertSemanticTree chunk_convert_sem_tree_; // 当前 chunk 的 convert 语义统计
  OrderSemanticTree chunk_order_sem_tree_;     // 当前 chunk 的 order 语义统计
  ChunkLog chunk_log_;                         // 当前 chunk 的日志
  std::string log_dir_ = "data/stage2/log";    // 日志目录

  struct NewCondition {
    uint32_t idx;
    std::string cond_id;
    ConditionInfo info;
  };
  struct NewToken {
    std::string token_id;
    uint32_t cond_idx;
    uint8_t token_idx;
    TokenSource source;
  };
  struct NewFPMM {
    std::string addr;
    uint32_t cond_idx;
    uint8_t collateral;
  };

  struct NewCollateral {
    uint8_t coll_id;
    std::string addr;
  };

  struct NewCondCollateral {
    uint32_t cond_idx;
    uint8_t coll_id;
  };

  struct NewNegRiskMarket {
    std::string question_id;
    std::string market_id;
  };

  std::vector<NewCondition> new_conditions_;
  std::vector<NewToken> new_tokens_;
  std::unordered_map<uint32_t, size_t> new_condition_pos_;
  std::unordered_map<std::string, size_t> new_token_pos_;
  std::vector<NewFPMM> new_fpmms_;
  std::vector<NewCollateral> new_collaterals_;
  std::vector<NewCondCollateral> new_cond_collaterals_;
  std::unordered_map<uint32_t, size_t> new_cond_collateral_pos_;
  std::vector<NewNegRiskMarket> new_neg_risk_markets_;
  std::vector<std::tuple<std::string, RawEvent>> new_events_;
  std::string current_transfer_context_;
  int64_t build_cursor_ = 0;
  std::unordered_map<std::string, int64_t> chunk_token_known_visible_from_sort_;
  std::unordered_map<std::string, int64_t> chunk_fpmm_visible_from_sort_;
  std::unordered_map<uint32_t, int64_t> chunk_negrisk_visible_from_sort_;

  struct CommitPayload {
    int64_t new_cursor = 0;
    BuildProgress progress;
    std::vector<NewCondition> new_conditions;
    std::vector<NewToken> new_tokens;
    std::vector<NewFPMM> new_fpmms;
    std::vector<NewCollateral> new_collaterals;
    std::vector<NewCondCollateral> new_cond_collaterals;
    std::vector<NewNegRiskMarket> new_neg_risk_markets;
    std::vector<std::tuple<std::string, RawEvent>> new_events;
  };
  std::thread commit_thread_;
  std::mutex commit_mu_;
  std::condition_variable commit_cv_;
  bool commit_stop_ = false;
  bool commit_busy_ = false;
  std::optional<CommitPayload> commit_payload_;
  std::optional<BuildProgress> commit_result_;
  std::optional<CommitPayload> commit_reusable_payload_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> build_running_{false};
  std::atomic<bool> commit_pending_{false};
  mutable std::mutex mem_mu_;
  json mem_snapshot_ = json::object();
  int64_t mem_peak_chunk_bytes_ = 0;
  void refresh_memory_snapshot(const char *phase);
  void init_restore_cache_schema();
  void clear_restore_cache_locked(duckdb::Connection &conn) const;
  void purge_restore_cache_db_files();
  bool load_users_and_event_stats_from_cache_if_cursor_match(int64_t expected_cursor);

  void update_xfer_tree(TransferClass cls) {
    chunk_xfer_stats_.add(cls);
  }

  void update_xfer_tree(const RawEvent &evt) {
    uint16_t coll_key = static_cast<uint16_t>(evt.type) * 256 + evt.collateral;
    progress_.event_by_collateral[coll_key]++;
  }

  uint32_t intern_condition(const std::string &cond_id, uint8_t outcome_cnt,
                            ConditionSource source = ConditionSource::ConditionPrep,
                            const std::string &question_id = "") {
    stage2_assert(outcome_cnt > 0 && outcome_cnt <= MAX_OUTCOMES,
                  AssertLevel::L1, "Mapping", "OutcomeCountRange");
    std::string lower = to_lower(cond_id);
    auto it = cond_map_.find(lower);
    if (it != cond_map_.end()) {
      uint32_t idx = it->second;
      bool changed = false;
      auto source_rank = [](ConditionSource s) -> int {
        switch (s) {
        case ConditionSource::ConditionPrep:
          return 0;
        case ConditionSource::SplitEvent:
        case ConditionSource::MergeEvent:
        case ConditionSource::RedemptionEvent:
          return 1;
        case ConditionSource::OtherFPMM:
          return 2;
        case ConditionSource::PolymarketFPMM:
          return 3;
        case ConditionSource::PolymarketTokenReg:
          return 4;
        case ConditionSource::TransferInferred:
          return 0;
        }
        return 0;
      };
      if (conditions_[idx].outcome_count < outcome_cnt) {
        conditions_[idx].outcome_count = outcome_cnt;
        changed = true;
      }
      // 更新question_id(如果之前没有)
      if (!question_id.empty() && conditions_[idx].question_id.empty()) {
        conditions_[idx].question_id = question_id;
        changed = true;
      }
      if (source_rank(source) > source_rank(conditions_[idx].source)) {
        conditions_[idx].source = source;
        changed = true;
      }
      if (changed) {
        auto pos_it = new_condition_pos_.find(idx);
        if (pos_it != new_condition_pos_.end()) {
          new_conditions_[pos_it->second].info = conditions_[idx];
        } else {
          new_condition_pos_[idx] = new_conditions_.size();
          new_conditions_.push_back({idx, cond_ids_[idx], conditions_[idx]});
        }
      }
      return idx;
    }

    uint32_t idx = static_cast<uint32_t>(conditions_.size());
    ConditionInfo info;
    info.outcome_count = outcome_cnt;
    info.question_id = question_id;
    info.source = source;
    conditions_.push_back(info);
    cond_ids_.push_back(lower);
    cond_map_[lower] = idx;

    new_condition_pos_[idx] = new_conditions_.size();
    new_conditions_.push_back({idx, lower, info});
    progress_.total_conditions = conditions_.size();
    return idx;
  }

  void update_condition_payout(uint32_t idx, const std::vector<int64_t> &payouts) {
    if (idx < conditions_.size()) {
      conditions_[idx].payout_numerators = payouts;
      auto pos_it = new_condition_pos_.find(idx);
      if (pos_it != new_condition_pos_.end()) {
        new_conditions_[pos_it->second].info.payout_numerators = payouts;
        return;
      }
      new_condition_pos_[idx] = new_conditions_.size();
      new_conditions_.push_back({idx, cond_ids_[idx], conditions_[idx]});
    }
  }

  void intern_token(const std::string &token_id, uint32_t cond_idx, uint8_t token_idx,
                    TokenSource source, int64_t evidence_sort_key = -1) {
    std::string lower = to_lower(token_id);
    auto is_known_mapping = [](const TokenInfo &info) {
      return info.cond_idx != UNKNOWN_COND_IDX && info.token_idx != UNKNOWN_TOKEN_IDX;
    };
    auto note_known_visibility = [&](const std::string &token_lower, int64_t sort_key) {
      if (sort_key < 0) {
        return;
      }
      auto it_vis = chunk_token_known_visible_from_sort_.find(token_lower);
      if (it_vis == chunk_token_known_visible_from_sort_.end() || sort_key < it_vis->second) {
        chunk_token_known_visible_from_sort_[token_lower] = sort_key;
      }
    };
    auto it = token_map_.find(lower);
    if (it != token_map_.end()) {
      TokenInfo &existing = it->second;
      bool was_known = is_known_mapping(existing);
      auto source_rank = [](TokenSource s) -> int {
        switch (s) {
        case TokenSource::TransferInferred:
          return 0;
        case TokenSource::PolymarketFPMM:
        case TokenSource::OtherFPMM:
          return 1;
        case TokenSource::PolymarketTokenReg:
          return 2;
        case TokenSource::SplitEvent:
        case TokenSource::MergeEvent:
        case TokenSource::RedemptionEvent:
          return 3;
        }
        return 0;
      };
      auto patch_pending = [&](uint32_t new_cond_idx, uint8_t new_token_idx, TokenSource new_source) {
        auto pos_it = new_token_pos_.find(lower);
        if (pos_it != new_token_pos_.end()) {
          auto &nt = new_tokens_[pos_it->second];
          nt.cond_idx = new_cond_idx;
          nt.token_idx = new_token_idx;
          nt.source = new_source;
        } else {
          new_token_pos_[lower] = new_tokens_.size();
          new_tokens_.push_back({lower, new_cond_idx, new_token_idx, new_source});
        }
      };

      bool same_assignment = (existing.cond_idx == cond_idx && existing.token_idx == token_idx);
      int existing_rank = source_rank(existing.source);
      int new_rank = source_rank(source);
      bool existing_is_inferred = (existing.source == TokenSource::TransferInferred);
      bool new_is_inferred = (source == TokenSource::TransferInferred);

      // TokenRegistered can be emitted in both token/complement orders for the
      // same pair. Treat same-condition TokenReg remaps as harmless duplicates.
      if (existing.source == TokenSource::PolymarketTokenReg &&
          source == TokenSource::PolymarketTokenReg &&
          existing.cond_idx == cond_idx) {
        return;
      }

      // Non-inferred sources must agree on condition mapping.
      if (!existing_is_inferred && !new_is_inferred) {
        stage2_assert(existing.cond_idx == cond_idx, AssertLevel::L1, "Mapping", "TokenCondIdxConsistent");
      }

      // If mapping already agrees, keep the best available source label.
      if (same_assignment) {
        if (new_rank > existing_rank) {
          existing.source = source;
          patch_pending(existing.cond_idx, existing.token_idx, existing.source);
        }
        return;
      }

      // Refine mapping only when new evidence has higher confidence.
      if (!same_assignment && new_rank > existing_rank) {
        existing = {cond_idx, token_idx, source};
        patch_pending(cond_idx, token_idx, source);
        if (!was_known && is_known_mapping(existing)) {
          note_known_visibility(lower, evidence_sort_key);
        }
        return;
      }

      // Strong semantic sources should never disagree on the same token.
      if (!same_assignment && existing_rank >= 3 && new_rank >= 3) {
        stage2_assert(false, AssertLevel::L1, "Mapping", "TokenStrongSourceConflict");
      }
      if (!was_known && is_known_mapping(existing)) {
        note_known_visibility(lower, evidence_sort_key);
      }
      return;
    }
    token_map_[lower] = {cond_idx, token_idx, source};
    if (is_known_mapping(token_map_[lower])) {
      note_known_visibility(lower, evidence_sort_key);
    }
    new_token_pos_[lower] = new_tokens_.size();
    new_tokens_.push_back({lower, cond_idx, token_idx, source});
    progress_.total_tokens = token_map_.size();
  }

  uint8_t intern_collateral(const std::string &collateral_addr) {
    std::string lower = to_lower(collateral_addr);
    auto it = collateral_addr_to_id_.find(lower);
    if (it != collateral_addr_to_id_.end()) {
      return it->second;
    }
    stage2_assert(next_collateral_id_ != 0, AssertLevel::L1, "Mapping", "CollateralIdNotOverflow");
    uint8_t coll_id = next_collateral_id_++;
    collateral_addr_to_id_[lower] = coll_id;
    collateral_id_to_addr_[coll_id] = lower;
    new_collaterals_.push_back({coll_id, lower});
    return coll_id;
  }

  uint8_t infer_known_collateral_from_atomic_token(const std::string &lower_cid,
                                                   uint8_t token_idx,
                                                   const std::string &token_id) const {
    if (lower_cid.empty() || token_id.empty()) {
      return static_cast<uint8_t>(Collateral::Unknown);
    }
    stage2_assert(token_idx < 31, AssertLevel::L1, "Mapping", "AtomicTokenIdxLt31");
    const std::string cond_bytes = hex_to_blob(lower_cid);
    const std::string token_lower = to_lower(token_id);
    const std::string collection_id = ctf::get_collection_id(cond_bytes, 1u << token_idx);

    uint8_t matched = static_cast<uint8_t>(Collateral::Unknown);
    int matched_count = 0;
    auto try_match = [&](Collateral candidate) {
      const std::string collateral_bytes = hex_to_blob(collateral_addr(candidate));
      const auto position_hash = ctf::get_position_id(collateral_bytes, collection_id);
      const std::string expected_token = to_lower(crypto::Keccak256::to_hex(position_hash));
      if (expected_token == token_lower) {
        matched = static_cast<uint8_t>(candidate);
        matched_count++;
      }
    };

    try_match(Collateral::USDC);
    try_match(Collateral::USDCe);
    try_match(Collateral::USDT);
    try_match(Collateral::WrappedUSDCe);

    stage2_assert(matched_count <= 1, AssertLevel::L1, "Mapping", "AtomicTokenCollateralUnique");
    return matched;
  }

  std::string build_negrisk_condition_id(const std::string &question_id) const {
    auto oracle_bytes = hex_to_blob(NEG_RISK_ADAPTER);
    auto qid_bytes = hex_to_blob(question_id);
    std::string input(84, '\0');
    std::memcpy(input.data(), oracle_bytes.data(), std::min(size_t(20), oracle_bytes.size()));
    std::memcpy(input.data() + 20, qid_bytes.data(), std::min(size_t(32), qid_bytes.size()));
    input[83] = 2;
    return to_lower(crypto::Keccak256::to_hex(crypto::keccak256(input)));
  }

  void note_negrisk_condition(uint32_t cond_idx, int64_t evidence_sort_key = -1) {
    negrisk_cond_idxs_.insert(cond_idx);
    if (evidence_sort_key < 0) {
      return;
    }
    auto vis_it = chunk_negrisk_visible_from_sort_.find(cond_idx);
    if (vis_it == chunk_negrisk_visible_from_sort_.end() || evidence_sort_key < vis_it->second) {
      chunk_negrisk_visible_from_sort_[cond_idx] = evidence_sort_key;
    }
  }

  std::string canonical_condition_collateral_addr(uint32_t cond_idx,
                                                  const std::string &collateral_addr) const {
    if (negrisk_cond_idxs_.count(cond_idx) > 0) {
      return WRAPPED_USDC_E;
    }
    return to_lower(collateral_addr);
  }

  void set_cond_collateral(uint32_t cond_idx, uint8_t coll_id, bool record_pending = true) {
    auto it = cond_collateral_.find(cond_idx);
    if (it != cond_collateral_.end() && it->second == coll_id) {
      return;
    }
    cond_collateral_[cond_idx] = coll_id;
    if (!record_pending) {
      return;
    }
    auto pos_it = new_cond_collateral_pos_.find(cond_idx);
    if (pos_it != new_cond_collateral_pos_.end()) {
      new_cond_collaterals_[pos_it->second].coll_id = coll_id;
      return;
    }
    new_cond_collaterals_.push_back({cond_idx, coll_id});
    new_cond_collateral_pos_[cond_idx] = new_cond_collaterals_.size() - 1;
  }

  uint8_t require_cond_collateral(uint32_t cond_idx) const {
    auto it = cond_collateral_.find(cond_idx);
    stage2_assert(it != cond_collateral_.end(), AssertLevel::L1, "Mapping", "KnownConditionCollateralPresent");
    return it->second;
  }

  void backfill_missing_condition_collateral_from_token_map() {
    std::unordered_map<uint32_t, uint8_t> inferred;
    inferred.reserve(token_map_.size() / 16 + 1);
    for (const auto &[token_id, info] : token_map_) {
      if (info.source != TokenSource::PolymarketTokenReg) {
        continue;
      }
      if (info.cond_idx == UNKNOWN_COND_IDX || info.token_idx == UNKNOWN_TOKEN_IDX) {
        continue;
      }
      if (cond_collateral_.count(info.cond_idx) > 0) {
        continue;
      }
      stage2_assert(info.cond_idx < cond_ids_.size(), AssertLevel::L1, "Mapping", "TokenRegCondIdxInRange");
      const std::string lower_cid = to_lower(cond_ids_[info.cond_idx]);
      uint8_t coll_id = infer_known_collateral_from_atomic_token(lower_cid, info.token_idx, token_id);
      if (coll_id == static_cast<uint8_t>(Collateral::Unknown) &&
          conditions_[info.cond_idx].outcome_count == 2 && info.token_idx < 2) {
        coll_id = infer_known_collateral_from_atomic_token(lower_cid, static_cast<uint8_t>(info.token_idx ^ 1), token_id);
      }
      if (coll_id == static_cast<uint8_t>(Collateral::Unknown)) {
        continue;
      }
      auto [it, inserted] = inferred.try_emplace(info.cond_idx, coll_id);
      if (!inserted) {
        stage2_assert(it->second == coll_id, AssertLevel::L1, "Mapping", "TokenRegCollateralConsistent");
      }
    }
    for (const auto &[cond_idx, coll_id] : inferred) {
      set_cond_collateral(cond_idx, coll_id);
    }
  }

  void intern_fpmm(const std::string &addr, uint32_t cond_idx, uint8_t collateral,
                   int64_t evidence_sort_key = -1) {
    std::string lower = to_lower(addr);
    if (fpmm_map_.count(lower))
      return;
    fpmm_map_[lower] = {cond_idx, collateral};
    if (evidence_sort_key >= 0) {
      auto it = chunk_fpmm_visible_from_sort_.find(lower);
      if (it == chunk_fpmm_visible_from_sort_.end() || evidence_sort_key < it->second) {
        chunk_fpmm_visible_from_sort_[lower] = evidence_sort_key;
      }
    }
    new_fpmms_.push_back({lower, cond_idx, collateral});
    fpmm_cond_idxs_.insert(cond_idx);
    set_cond_collateral(cond_idx, collateral);
  }

  void intern_condition_tokens(const std::string &lower_cid, const std::string &collateral_hex,
                               uint32_t cond_idx, TokenSource source,
                               int64_t evidence_sort_key = -1) {
    auto cond_bytes = hex_to_blob(lower_cid);
    auto collateral_bytes = hex_to_blob(collateral_hex);
    stage2_assert(cond_idx < conditions_.size(), AssertLevel::L1, "Mapping", "CondIdxInRangeForTokenIntern");
    uint8_t outcome_count = conditions_[cond_idx].outcome_count;
    stage2_assert(outcome_count > 0 && outcome_count <= MAX_OUTCOMES,
                  AssertLevel::L1, "Mapping", "OutcomeCountRangeForTokenIntern");
    for (uint8_t outcome = 0; outcome < outcome_count; ++outcome) {
      stage2_assert(outcome < 31, AssertLevel::L1, "Mapping", "OutcomeBitWidthLt31");
      int index_set = (1 << outcome);
      auto collection_id = ctf::get_collection_id(cond_bytes, index_set);
      auto position_hash = ctf::get_position_id(collateral_bytes, collection_id);
      std::string token_id = crypto::Keccak256::to_hex(position_hash);
      intern_token(token_id, cond_idx, outcome, source, evidence_sort_key);
    }
  }

  void intern_fpmm_tokens(const std::vector<std::string> &condition_ids,
                          const std::string &collateral_hex,
                          uint32_t primary_cond_idx,
                          int64_t evidence_sort_key = -1) {
    stage2_assert(!condition_ids.empty(), AssertLevel::L1, "Mapping", "FPMMConditionIdsNonEmpty");
    auto collateral_bytes = hex_to_blob(collateral_hex);

    std::vector<std::string> cond_bytes;
    std::vector<uint8_t> outcome_counts;
    cond_bytes.reserve(condition_ids.size());
    outcome_counts.reserve(condition_ids.size());
    for (const auto &cid : condition_ids) {
      std::string lower = to_lower(cid);
      auto it = cond_map_.find(lower);
      stage2_assert(it != cond_map_.end(), AssertLevel::L1, "Mapping", "FPMMCondKnown");
      cond_bytes.push_back(hex_to_blob(lower));
      outcome_counts.push_back(conditions_[it->second].outcome_count);
      stage2_assert(outcome_counts.back() > 0 && outcome_counts.back() <= MAX_OUTCOMES,
                    AssertLevel::L1, "Mapping", "FPMMOutcomeCountRange");
    }

    // 必须与 FPMMFactory._recordCollectionIDsForAllConditions 一致:
    // 递归时按 conditionIds 的倒序处理(conditionsLeft-- 后取 conditionIds[conditionsLeft]).
    std::function<void(int, const std::string &, int)> dfs =
        [&](int cond_pos, const std::string &parent_collection_id, int first_condition_outcome) {
          if (cond_pos < 0) {
            auto position_hash = ctf::get_position_id(collateral_bytes, parent_collection_id);
            std::string token_id = crypto::Keccak256::to_hex(position_hash);
            stage2_assert(first_condition_outcome >= 0 &&
                              first_condition_outcome <= std::numeric_limits<uint8_t>::max(),
                          AssertLevel::L1, "Mapping", "FPMMFirstOutcomeFitsU8");
            uint8_t token_idx = static_cast<uint8_t>(first_condition_outcome);
            intern_token(token_id, primary_cond_idx, token_idx, TokenSource::PolymarketFPMM,
                        evidence_sort_key);
            return;
          }

          uint8_t outcome_cnt = outcome_counts[cond_pos];
          for (uint8_t outcome = 0; outcome < outcome_cnt; ++outcome) {
            stage2_assert(outcome < 31, AssertLevel::L1, "Mapping", "FPMMOutcomeBitWidthLt31");
            uint32_t index_set = (1u << outcome);
            std::string child_collection_id = ctf::get_collection_id(parent_collection_id, cond_bytes[cond_pos], index_set);
            int next_first = (cond_pos == 0) ? static_cast<int>(outcome) : first_condition_outcome;
            dfs(cond_pos - 1, child_collection_id, next_first);
          }
        };

    std::string root_collection_id(32, '\0');
    dfs(static_cast<int>(cond_bytes.size()) - 1, root_collection_id, 0);
  }

  void update_cond_type_stats() {
    // 条件主树:condition 是唯一主事实层.
    // coverage.raw_* 在 phase1 中按 condition_preparation 行累积,需要跨轮次保留.
    ConditionTree ct{};
    ct.coverage = progress_.cond_tree.coverage;
    ct.coverage.observed = 0;

    std::unordered_map<uint32_t, uint16_t> cond_token_mask;
    cond_token_mask.reserve(token_map_.size());
    for (const auto &[_, tinfo] : token_map_) {
      if (tinfo.cond_idx == UNKNOWN_COND_IDX || tinfo.token_idx == UNKNOWN_TOKEN_IDX) {
        continue;
      }
      if (tinfo.token_idx >= MAX_OUTCOMES) {
        continue;
      }
      cond_token_mask[tinfo.cond_idx] |= static_cast<uint16_t>(1u << tinfo.token_idx);
    }

    std::unordered_map<std::string, int64_t> market_question_count;
    market_question_count.reserve(cond_to_market_.size());
    for (const auto &[question_id, market_id] : cond_to_market_) {
      (void)question_id;
      market_question_count[market_id]++;
    }

    for (size_t i = 0; i < conditions_.size(); ++i) {
      ct.total++;
      uint32_t idx = static_cast<uint32_t>(i);
      const auto &info = conditions_[i];
      auto src = info.source;
      bool has_fpmm = fpmm_cond_idxs_.count(idx) > 0;
      bool has_negrisk = negrisk_cond_idxs_.count(idx) > 0;
      // TokenReg evidence is persisted on condition source and must be sticky.
      bool has_token_reg = (src == ConditionSource::PolymarketTokenReg);

      if (has_token_reg) {
        // 有 TokenReg 的都是 Polymarket
        ct.polymarket.total++;
        ct.polymarket.token_reg.total++;
        if (has_fpmm) {
          ct.polymarket.token_reg.amm++;
        } else if (has_negrisk) {
          ct.polymarket.token_reg.negrisk++;
        } else {
          ct.polymarket.token_reg.orderbook++;
        }
      } else if (has_fpmm) {
        // 没有 TokenReg 但有 FPMM(早期 Polymarket 或只创建了池子)
        ct.polymarket.total++;
        ct.polymarket.fpmm_poly++;
      } else {
        // 既没有 TokenReg 也没有 FPMM → 其他协议
        ct.other.total++;
        switch (src) {
        case ConditionSource::ConditionPrep:
          ct.other.prep++;
          break;
        case ConditionSource::OtherFPMM:
          ct.other.fpmm_other++;
          break;
        case ConditionSource::SplitEvent:
          ct.other.split++;
          break;
        case ConditionSource::MergeEvent:
          ct.other.merge++;
          break;
        case ConditionSource::RedemptionEvent:
          ct.other.redemption++;
          break;
        default:
          break;
        }
      }

      if (!info.payout_numerators.empty()) {
        ct.resolve.resolved++;
      } else {
        ct.resolve.unresolved++;
      }

      uint8_t coll = static_cast<uint8_t>(Collateral::Unknown);
      auto coll_it = cond_collateral_.find(idx);
      if (coll_it != cond_collateral_.end()) {
        coll = coll_it->second;
      }
      ct.by_collateral[coll]++;

      int tokenized = 0;
      auto tm_it = cond_token_mask.find(idx);
      if (tm_it != cond_token_mask.end()) {
        tokenized = __builtin_popcount(static_cast<unsigned int>(tm_it->second));
      }
      if (tokenized == 0) {
        ct.tokenized.none++;
      } else if (tokenized >= info.outcome_count) {
        ct.tokenized.full++;
      } else {
        ct.tokenized.partial++;
      }
    }
    ct.coverage.observed = ct.total;

    auto &nr = ct.polymarket.token_reg.negrisk_stats;
    nr.market_count = static_cast<int64_t>(market_question_count.size());
    nr.question_count = static_cast<int64_t>(cond_to_market_.size());
    nr.condition_count = static_cast<int64_t>(negrisk_cond_idxs_.size());
    nr.by_questions_per_market.clear();
    for (const auto &[market_id, qcnt] : market_question_count) {
      (void)market_id;
      nr.by_questions_per_market[qcnt]++;
    }

    // 恒等式验证
    stage2_assert(ct.total == ct.polymarket.total + ct.other.total,
                  AssertLevel::L5, "Partition", "ConditionTreeTotal");
    stage2_assert(ct.polymarket.total == ct.polymarket.token_reg.total + ct.polymarket.fpmm_poly,
                  AssertLevel::L5, "Partition", "ConditionTreePolymarketTotal");
    stage2_assert(ct.polymarket.token_reg.total ==
                      ct.polymarket.token_reg.amm + ct.polymarket.token_reg.negrisk +
                          ct.polymarket.token_reg.orderbook + ct.polymarket.token_reg.other,
                  AssertLevel::L5, "Partition", "ConditionTreeTokenRegTotal");
    stage2_assert(ct.other.total == ct.other.prep + ct.other.fpmm_other + ct.other.split +
                                        ct.other.merge + ct.other.redemption,
                  AssertLevel::L5, "Partition", "ConditionTreeOtherTotal");
    stage2_assert(ct.total == ct.resolve.resolved + ct.resolve.unresolved,
                  AssertLevel::L5, "Partition", "ConditionTreeResolveTotal");
    stage2_assert(ct.total == ct.tokenized.none + ct.tokenized.partial + ct.tokenized.full,
                  AssertLevel::L5, "Partition", "ConditionTreeTokenizedTotal");
    {
      int64_t by_collateral_sum = 0;
      for (const auto &[_, v] : ct.by_collateral) {
        by_collateral_sum += v;
      }
      stage2_assert(ct.total == by_collateral_sum,
                    AssertLevel::L5, "Partition", "ConditionTreeByCollateralTotal");
    }
    stage2_assert(ct.coverage.observed == ct.total,
                  AssertLevel::L5, "Partition", "ConditionTreeCoverageObservedTotal");
    {
      int64_t qpm_market_sum = 0;
      for (const auto &[_, cnt] : nr.by_questions_per_market) {
        qpm_market_sum += cnt;
      }
      stage2_assert(nr.market_count == qpm_market_sum,
                    AssertLevel::L5, "Partition", "NegRiskStatsQPMTotal");
      stage2_assert(nr.question_count == static_cast<int64_t>(cond_to_market_.size()),
                    AssertLevel::L5, "Partition", "NegRiskStatsQuestionTotal");
      stage2_assert(nr.question_count >= nr.condition_count,
                    AssertLevel::L5, "Partition", "NegRiskStatsConditionBound");
    }
    progress_.cond_tree = ct;

    // 代币树状partition: total = polymarket + other
    // 优先检查 fpmm_cond_idxs_ 来判断是否是 Polymarket
    // TransferInferred 的 token 用 UNKNOWN_COND_IDX 标识
    TokenTree tt{};
    for (const auto &[tid, info] : token_map_) {
      tt.total++;
      auto src = info.source;

      // TransferInferred 的 token 没有 condition 信息
      if (info.cond_idx == UNKNOWN_COND_IDX) {
        tt.other.total++;
        tt.other.transfer_inferred++;
        continue;
      }

      stage2_assert(info.cond_idx < conditions_.size(),
                    AssertLevel::L1, "Mapping", "TokenCondIdxInRange");
      bool has_fpmm = fpmm_cond_idxs_.count(info.cond_idx) > 0;
      // Use condition-level TokenReg evidence for tree partition, so condition/token trees
      // share one consistent Polymarket classification system.
      bool has_token_reg =
          (conditions_[info.cond_idx].source == ConditionSource::PolymarketTokenReg);

      if (has_token_reg) {
        // 有 TokenReg 的都是 Polymarket
        tt.polymarket.total++;
        tt.polymarket.token_reg.total++;
        if (has_fpmm) {
          tt.polymarket.token_reg.amm++;
        } else if (negrisk_cond_idxs_.count(info.cond_idx)) {
          tt.polymarket.token_reg.negrisk++;
        } else {
          tt.polymarket.token_reg.orderbook++;
        }
      } else if (has_fpmm) {
        // 没有 TokenReg 但有 FPMM(早期 Polymarket 或只创建了池子)
        tt.polymarket.total++;
        tt.polymarket.fpmm_poly.total++;
        auto cit = cond_collateral_.find(info.cond_idx);
        uint8_t coll = cit != cond_collateral_.end() ? cit->second : static_cast<uint8_t>(Collateral::Unknown);
        tt.polymarket.fpmm_poly.by_collateral[coll]++;
      } else {
        // 既没有 TokenReg 也没有 FPMM → 其他协议
        tt.other.total++;
        switch (src) {
        case TokenSource::OtherFPMM:
          tt.other.fpmm_other++;
          break;
        case TokenSource::SplitEvent:
          tt.other.split++;
          break;
        case TokenSource::MergeEvent:
          tt.other.merge++;
          break;
        case TokenSource::RedemptionEvent:
          tt.other.redemption++;
          break;
        default:
          break;
        }
      }
    }
    // 恒等式验证
    stage2_assert(tt.total == tt.polymarket.total + tt.other.total,
                  AssertLevel::L5, "Partition", "TokenTreeTotal");
    stage2_assert(tt.polymarket.total == tt.polymarket.token_reg.total + tt.polymarket.fpmm_poly.total,
                  AssertLevel::L5, "Partition", "TokenTreePolymarketTotal");
    stage2_assert(tt.polymarket.token_reg.total ==
                      tt.polymarket.token_reg.amm + tt.polymarket.token_reg.negrisk +
                          tt.polymarket.token_reg.orderbook + tt.polymarket.token_reg.other,
                  AssertLevel::L5, "Partition", "TokenTreeTokenRegTotal");
    {
      int64_t sum = 0;
      for (const auto &[k, v] : tt.polymarket.fpmm_poly.by_collateral)
        sum += v;
      stage2_assert(tt.polymarket.fpmm_poly.total == sum,
                    AssertLevel::L5, "Partition", "TokenTreeFPMMByCollateralTotal");
    }
    stage2_assert(tt.other.total == tt.other.fpmm_other + tt.other.split + tt.other.merge +
                                        tt.other.redemption + tt.other.transfer_inferred,
                  AssertLevel::L5, "Partition", "TokenTreeOtherTotal");
    progress_.token_tree = tt;
  }

  void push_event(const std::string &user_addr, const RawEvent &evt) {
    std::string lower = to_lower(user_addr);
    if (evt.cond_idx != UNKNOWN_COND_IDX) {
      stage2_assert(evt.collateral != static_cast<uint8_t>(Collateral::Unknown),
                    AssertLevel::L1, "Mapping", "KnownEventCollateralResolved");
    }
    new_events_.emplace_back(lower, evt);
    progress_.total_events++;

    // Only the first post-startup event for a user needs a Rocks existence probe.
    if (seen_users_.insert(lower).second) {
      if (!user_event_store_->has_user(hex_to_blob(lower))) {
        progress_.total_users++;
      }
    }

    update_xfer_tree(evt);

    bump_event_counter(static_cast<EventType>(evt.type), 1);
  }

  void phase1_update_mappings(int64_t start, int64_t end);
  void phase2_build_semantic_index(int64_t start, int64_t end);
  void phase3_process_transfers(int64_t start, int64_t end);
  void restore_users_and_event_stats_parallel();
  BuildProgress commit_chunk(CommitPayload payload);
  void commit_worker_loop();
  void reap_commit_result_locked();
  void bump_event_counter(EventType type, int64_t delta) {
    stage2_assert(delta >= 0, AssertLevel::L0, "Input", "CounterDeltaNonNegative");
    switch (type) {
    case EventType::OrderBuy:
    case EventType::OrderSell:
      progress_.cnt_order += delta;
      break;
    case EventType::SplitNormal:
    case EventType::SplitNegRisk:
    case EventType::SplitNonPoly:
      progress_.cnt_split += delta;
      break;
    case EventType::MergeNormal:
    case EventType::MergeNegRisk:
    case EventType::MergeNonPoly:
      progress_.cnt_merge += delta;
      break;
    case EventType::Redemption:
    case EventType::RedemptionNonPoly:
      progress_.cnt_redemption += delta;
      break;
    case EventType::FPMMBuy:
    case EventType::FPMMSell:
      progress_.cnt_fpmm_trade += delta;
      break;
    case EventType::FPMMLPAdd:
    case EventType::FPMMLPRemove:
    case EventType::FPMMLPReturn:
      progress_.cnt_fpmm_funding += delta;
      break;
    case EventType::Convert:
      progress_.cnt_convert += delta;
      break;
    case EventType::TransferInNegRisk:
    case EventType::TransferInOther:
    case EventType::TransferInNonPoly:
    case EventType::TransferOutNegRisk:
    case EventType::TransferOutOther:
    case EventType::TransferOutNonPoly:
      progress_.cnt_transfer += delta;
      break;
    default:
      stage2_assert(false, AssertLevel::L0, "Input", "UnknownEventTypeInCounter");
      break;
    }
  }

  bool is_protocol_contract(const std::string &addr) const {
    return addr == ZERO_ADDR || addr == CTF_EXCHANGE || addr == NEG_RISK_CTF_EXCHANGE ||
           addr == NEG_RISK_ADAPTER || addr == CONDITIONAL_TOKENS ||
           addr == NO_TOKEN_BURN_ADDRESS || fpmm_map_.count(addr) > 0;
  }
  bool is_known_fpmm(const std::string &addr) const {
    return fpmm_map_.find(addr) != fpmm_map_.end();
  }
  bool is_fpmm_visible_at(const std::string &addr, int64_t sort_key) const {
    if (!is_known_fpmm(addr)) {
      return false;
    }
    auto it = chunk_fpmm_visible_from_sort_.find(addr);
    if (it == chunk_fpmm_visible_from_sort_.end()) {
      return true;
    }
    return sort_key >= it->second;
  }
  bool is_token_known_visible_at(const std::string &token_id, int64_t sort_key) const {
    auto it = token_map_.find(token_id);
    if (it == token_map_.end()) {
      return false;
    }
    if (it->second.cond_idx == UNKNOWN_COND_IDX || it->second.token_idx == UNKNOWN_TOKEN_IDX) {
      return false;
    }
    auto vis_it = chunk_token_known_visible_from_sort_.find(token_id);
    if (vis_it == chunk_token_known_visible_from_sort_.end()) {
      return true;
    }
    return sort_key >= vis_it->second;
  }
  bool is_negrisk_cond_visible_at(uint32_t cond_idx, int64_t sort_key) const {
    if (negrisk_cond_idxs_.count(cond_idx) == 0) {
      return false;
    }
    auto vis_it = chunk_negrisk_visible_from_sort_.find(cond_idx);
    if (vis_it == chunk_negrisk_visible_from_sort_.end()) {
      return true;
    }
    return sort_key >= vis_it->second;
  }

  TransferClass classify_and_emit(int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
                                  int64_t block, const std::string &op,
                                  const std::string &from, const std::string &to,
                                  const std::string &token_id, int64_t amount,
                                  uint32_t cond_idx, uint8_t token_idx, Collateral collateral);
};

} // namespace stage2
