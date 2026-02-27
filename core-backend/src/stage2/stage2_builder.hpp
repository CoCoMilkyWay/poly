#pragma once

#include "../core/ctf_helpers.hpp"
#include "../core/database.hpp"
#include "../core/keccak256.hpp"
#include "stage2_assert.hpp"
#include "stage2_models.hpp"
#include "stage2_types.hpp"
#include "stage2_utils.hpp"
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace stage2 {

class EventBuilder {
public:
  EventBuilder(Database &stage1_db, Database &stage2_db, int chunk_size);
  ~EventBuilder();

  void init_schema();

  void load_from_rb();

  int64_t cursor() const;

  bool build_chunk(int64_t target_block);

  const BuildProgress &progress() const;
  const BuildProgress &committed_progress() const;

private:
  Database &stage1_db_;
  Database &stage2_db_;
  int chunk_size_;
  BuildProgress progress_;
  BuildProgress committed_progress_;

  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<std::string, uint32_t> cond_map_;
  std::unordered_map<std::string, TokenInfo> token_map_;
  std::unordered_map<std::string, FPMMInfo> fpmm_map_;
  std::unordered_map<std::string, std::string> cond_to_market_; // condition_id -> market_id
  std::unordered_set<std::string> seen_users_;                  // 实时统计唯一用户
  std::unordered_set<std::string> seen_markets_;                // 统计唯一 NegRisk 市场
  std::unordered_set<uint32_t> fpmm_cond_idxs_;                 // Polymarket AMM 对应的 cond_idx
  std::unordered_set<uint32_t> negrisk_cond_idxs_;              // NegRisk 对应的 cond_idx
  std::unordered_map<uint32_t, uint8_t> cond_collateral_;       // cond_idx -> collateral_id
  std::unordered_map<std::string, uint8_t> collateral_addr_to_id_;
  std::unordered_map<uint8_t, std::string> collateral_id_to_addr_;
  uint8_t next_collateral_id_ = static_cast<uint8_t>(Collateral::WrappedUSDCe) + 1;

  // 按 TxKey (tx_hash) 索引，统一处理已知和未知 token
  std::unordered_map<TxKey, std::vector<SplitInfo>> tx_split_;
  std::unordered_map<TxKey, std::vector<MergeInfo>> tx_merge_;
  std::unordered_map<TxKey, std::vector<RedemptionInfo>> tx_redemption_;
  std::unordered_map<TxMarketKey, std::vector<ConvertInfo>> tx_convert_;
  std::unordered_map<TxTokenKey, std::vector<OrderInfo>> tx_order_;
  std::unordered_map<TxKey, std::vector<ConvertInfo *>> tx_convert_by_tx_;
  std::unordered_map<TxTokenKey, std::unordered_map<int64_t, std::vector<OrderInfo *>>> tx_order_by_amount_;
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
  std::vector<NewNegRiskMarket> new_neg_risk_markets_;
  std::vector<std::tuple<std::string, RawEvent>> new_events_;
  std::string current_transfer_context_;
  int64_t build_cursor_ = 0;

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
      if (conditions_[idx].outcome_count < outcome_cnt) {
        conditions_[idx].outcome_count = outcome_cnt;
        changed = true;
      }
      // 更新question_id（如果之前没有）
      if (!question_id.empty() && conditions_[idx].question_id.empty()) {
        conditions_[idx].question_id = question_id;
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

  void intern_token(const std::string &token_id, uint32_t cond_idx, uint8_t token_idx, TokenSource source) {
    std::string lower = to_lower(token_id);
    auto it = token_map_.find(lower);
    if (it != token_map_.end()) {
      TokenInfo &existing = it->second;
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
        return;
      }

      // Strong semantic sources should never disagree on the same token.
      if (!same_assignment && existing_rank >= 3 && new_rank >= 3) {
        stage2_assert(false, AssertLevel::L1, "Mapping", "TokenStrongSourceConflict");
      }
      return;
    }
    token_map_[lower] = {cond_idx, token_idx, source};
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

  void set_cond_collateral(uint32_t cond_idx, uint8_t coll_id) {
    auto it = cond_collateral_.find(cond_idx);
    if (it != cond_collateral_.end() && it->second == coll_id) {
      return;
    }
    cond_collateral_[cond_idx] = coll_id;
    new_cond_collaterals_.push_back({cond_idx, coll_id});
  }

  void intern_fpmm(const std::string &addr, uint32_t cond_idx, uint8_t collateral) {
    std::string lower = to_lower(addr);
    if (fpmm_map_.count(lower))
      return;
    fpmm_map_[lower] = {cond_idx, collateral};
    new_fpmms_.push_back({lower, cond_idx, collateral});
    fpmm_cond_idxs_.insert(cond_idx);
    set_cond_collateral(cond_idx, collateral);
  }

  void intern_condition_tokens(const std::string &lower_cid, const std::string &collateral_hex,
                               uint32_t cond_idx, TokenSource source) {
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
      intern_token(token_id, cond_idx, outcome, source);
    }
  }

  void intern_fpmm_tokens(const std::vector<std::string> &condition_ids,
                          const std::string &collateral_hex,
                          uint32_t primary_cond_idx) {
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

    // 必须与 FPMMFactory._recordCollectionIDsForAllConditions 一致：
    // 递归时按 conditionIds 的倒序处理（conditionsLeft-- 后取 conditionIds[conditionsLeft]）。
    std::function<void(int, const std::string &, int)> dfs =
        [&](int cond_pos, const std::string &parent_collection_id, int first_condition_outcome) {
          if (cond_pos < 0) {
            auto position_hash = ctf::get_position_id(collateral_bytes, parent_collection_id);
            std::string token_id = crypto::Keccak256::to_hex(position_hash);
            stage2_assert(first_condition_outcome >= 0 &&
                              first_condition_outcome <= std::numeric_limits<uint8_t>::max(),
                          AssertLevel::L1, "Mapping", "FPMMFirstOutcomeFitsU8");
            uint8_t token_idx = static_cast<uint8_t>(first_condition_outcome);
            intern_token(token_id, primary_cond_idx, token_idx, TokenSource::PolymarketFPMM);
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
    // 问题树状partition: total = polymarket + other
    // 优先检查 fpmm_cond_idxs_/negrisk_cond_idxs_ 来判断是否是 Polymarket
    ConditionTree ct{};
    for (size_t i = 0; i < conditions_.size(); ++i) {
      ct.total++;
      uint32_t idx = static_cast<uint32_t>(i);
      auto src = conditions_[i].source;
      bool has_fpmm = fpmm_cond_idxs_.count(idx) > 0;
      bool has_negrisk = negrisk_cond_idxs_.count(idx) > 0;
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
        // 没有 TokenReg 但有 FPMM（早期 Polymarket 或只创建了池子）
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

      bool has_fpmm = fpmm_cond_idxs_.count(info.cond_idx) > 0;
      bool has_token_reg = (src == TokenSource::PolymarketTokenReg);

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
        // 没有 TokenReg 但有 FPMM（早期 Polymarket 或只创建了池子）
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

    MarketTree mt = progress_.market_tree;
    mt.observed_total = 0;
    mt.observed_polymarket = 0;
    mt.observed_other = 0;
    mt.observed_negrisk = 0;
    mt.observed_resolved = 0;
    mt.observed_unresolved = 0;
    mt.observed_has_market_id = 0;
    mt.observed_no_market_id = 0;
    mt.observed_token_none = 0;
    mt.observed_token_partial = 0;
    mt.observed_token_full = 0;
    mt.observed_by_outcome_count.clear();
    mt.observed_by_source.clear();
    mt.observed_by_collateral.clear();

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

    for (size_t i = 0; i < conditions_.size(); ++i) {
      uint32_t idx = static_cast<uint32_t>(i);
      const auto &info = conditions_[i];
      mt.observed_total++;
      mt.observed_by_outcome_count[info.outcome_count]++;
      mt.observed_by_source[static_cast<int64_t>(info.source)]++;

      bool has_fpmm = fpmm_cond_idxs_.count(idx) > 0;
      bool has_token_reg = (info.source == ConditionSource::PolymarketTokenReg);
      bool is_poly = has_token_reg || has_fpmm;
      if (is_poly) {
        mt.observed_polymarket++;
      } else {
        mt.observed_other++;
      }
      if (negrisk_cond_idxs_.count(idx) > 0) {
        mt.observed_negrisk++;
      }

      bool resolved = !info.payout_numerators.empty();
      if (resolved) {
        mt.observed_resolved++;
      } else {
        mt.observed_unresolved++;
      }

      bool has_market_id = !info.question_id.empty() && cond_to_market_.count(info.question_id) > 0;
      if (has_market_id) {
        mt.observed_has_market_id++;
      } else {
        mt.observed_no_market_id++;
      }

      uint8_t coll = static_cast<uint8_t>(Collateral::Unknown);
      auto coll_it = cond_collateral_.find(idx);
      if (coll_it != cond_collateral_.end()) {
        coll = coll_it->second;
      }
      mt.observed_by_collateral[coll]++;

      int tokenized = 0;
      auto tm_it = cond_token_mask.find(idx);
      if (tm_it != cond_token_mask.end()) {
        tokenized = __builtin_popcount(static_cast<unsigned int>(tm_it->second));
      }
      if (tokenized == 0) {
        mt.observed_token_none++;
      } else if (tokenized >= info.outcome_count) {
        mt.observed_token_full++;
      } else {
        mt.observed_token_partial++;
      }
    }
    progress_.market_tree = mt;
  }

  void push_event(const std::string &user_addr, const RawEvent &evt) {
    std::string lower = to_lower(user_addr);
    new_events_.emplace_back(lower, evt);
    progress_.total_events++;

    // 实时更新用户数
    if (seen_users_.insert(lower).second) {
      progress_.total_users = seen_users_.size();
    }

    update_xfer_tree(evt);

    bump_event_counter(static_cast<EventType>(evt.type), 1);
  }

  void phase1_update_mappings(int64_t start, int64_t end);
  void phase2_build_semantic_index(int64_t start, int64_t end);
  void phase3_process_transfers(int64_t start, int64_t end);
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

  TransferClass classify_and_emit(int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
                                  int64_t block, const std::string &op,
                                  const std::string &from, const std::string &to,
                                  const std::string &token_id, int64_t amount,
                                  uint32_t cond_idx, uint8_t token_idx, Collateral collateral);
};

} // namespace stage2
