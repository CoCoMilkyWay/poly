#pragma once

#include "../core/ctf_helpers.hpp"
#include "../core/database.hpp"
#include "../core/keccak256.hpp"
#include "stage2_models.hpp"
#include "stage2_types.hpp"
#include "stage2_utils.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace stage2 {

class EventBuilder {
public:
  EventBuilder(Database &stage1_db, Database &stage2_db, int chunk_size);

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
  std::unordered_map<TxFPMMKey, std::vector<FPMMTradeInfo>> tx_fpmm_trade_;
  std::unordered_map<TxFPMMKey, std::vector<FPMMFundingInfo>> tx_fpmm_funding_;
  // Tx-level semantic bounds built from ordered semantic log_index sequence.
  std::unordered_map<TxKey, std::vector<TxOpBounds>> tx_op_bounds_;
  std::unordered_map<TxLogKey, uint32_t> tx_op_type_mask_;
  TransferStats chunk_xfer_stats_;          // 当前 chunk 的 transfer 统计
  ChunkLog chunk_log_;                      // 当前 chunk 的日志
  std::string log_dir_ = "data/stage2/log"; // 日志目录

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
  std::vector<NewFPMM> new_fpmms_;
  std::vector<NewCollateral> new_collaterals_;
  std::vector<NewCondCollateral> new_cond_collaterals_;
  std::vector<NewNegRiskMarket> new_neg_risk_markets_;
  std::vector<std::tuple<std::string, RawEvent>> new_events_;
  std::string current_transfer_context_;

  uint32_t intern_condition(const std::string &cond_id, uint8_t outcome_cnt,
                            ConditionSource source = ConditionSource::ConditionPrep,
                            const std::string &question_id = "") {
    std::string lower = to_lower(cond_id);
    auto it = cond_map_.find(lower);
    if (it != cond_map_.end()) {
      // 更新question_id（如果之前没有）
      if (!question_id.empty() && conditions_[it->second].question_id.empty()) {
        conditions_[it->second].question_id = question_id;
        // 确保更新被持久化
        bool found = false;
        for (auto &nc : new_conditions_) {
          if (nc.idx == it->second) {
            nc.info.question_id = question_id;
            found = true;
            break;
          }
        }
        if (!found) {
          // 条件在之前 chunk 创建，需要添加到 new_conditions_ 以持久化更新
          new_conditions_.push_back({it->second, cond_ids_[it->second], conditions_[it->second]});
        }
      }
      return it->second;
    }

    uint32_t idx = static_cast<uint32_t>(conditions_.size());
    ConditionInfo info;
    info.outcome_count = outcome_cnt;
    info.question_id = question_id;
    info.source = source;
    conditions_.push_back(info);
    cond_ids_.push_back(lower);
    cond_map_[lower] = idx;

    new_conditions_.push_back({idx, lower, info});
    progress_.total_conditions = conditions_.size();
    return idx;
  }

  void update_condition_payout(uint32_t idx, const std::vector<int64_t> &payouts) {
    if (idx < conditions_.size()) {
      conditions_[idx].payout_numerators = payouts;
      for (auto &nc : new_conditions_) {
        if (nc.idx == idx) {
          nc.info.payout_numerators = payouts;
          return;
        }
      }
      new_conditions_.push_back({idx, cond_ids_[idx], conditions_[idx]});
    }
  }

  void intern_token(const std::string &token_id, uint32_t cond_idx, uint8_t token_idx, TokenSource source) {
    std::string lower = to_lower(token_id);
    auto it = token_map_.find(lower);
    if (it != token_map_.end()) {
      // 已存在：不应该出现 TransferInferred 后又从其他来源发现的情况
      // 因为事件按时间顺序处理，FPMM/Split 等事件应该先于 Transfer
      assert(!(it->second.source == TokenSource::TransferInferred && source != TokenSource::TransferInferred));
      return;
    }
    token_map_[lower] = {cond_idx, token_idx, source};
    new_tokens_.push_back({lower, cond_idx, token_idx, source});
    progress_.total_tokens = token_map_.size();
  }

  uint8_t intern_collateral(const std::string &collateral_addr) {
    std::string lower = to_lower(collateral_addr);
    auto it = collateral_addr_to_id_.find(lower);
    if (it != collateral_addr_to_id_.end()) {
      return it->second;
    }
    assert(next_collateral_id_ != 0);
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
    uint8_t outcome_count = 2;
    if (cond_idx < conditions_.size()) {
      outcome_count = conditions_[cond_idx].outcome_count;
    }
    assert(outcome_count > 0 && outcome_count <= MAX_OUTCOMES);
    for (uint8_t outcome = 0; outcome < outcome_count; ++outcome) {
      assert(outcome < 31);
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
    assert(!condition_ids.empty());
    auto collateral_bytes = hex_to_blob(collateral_hex);

    std::vector<std::string> cond_bytes;
    std::vector<uint8_t> outcome_counts;
    cond_bytes.reserve(condition_ids.size());
    outcome_counts.reserve(condition_ids.size());
    for (const auto &cid : condition_ids) {
      std::string lower = to_lower(cid);
      auto it = cond_map_.find(lower);
      assert(it != cond_map_.end());
      cond_bytes.push_back(hex_to_blob(lower));
      outcome_counts.push_back(conditions_[it->second].outcome_count);
      assert(outcome_counts.back() > 0 && outcome_counts.back() <= MAX_OUTCOMES);
    }

    // 必须与 FPMMFactory._recordCollectionIDsForAllConditions 一致：
    // 递归时按 conditionIds 的倒序处理（conditionsLeft-- 后取 conditionIds[conditionsLeft]）。
    std::function<void(int, const std::string &, int)> dfs =
        [&](int cond_pos, const std::string &parent_collection_id, int first_condition_outcome) {
          if (cond_pos < 0) {
            auto position_hash = ctf::get_position_id(collateral_bytes, parent_collection_id);
            std::string token_id = crypto::Keccak256::to_hex(position_hash);
            assert(first_condition_outcome >= 0 && first_condition_outcome <= std::numeric_limits<uint8_t>::max());
            uint8_t token_idx = static_cast<uint8_t>(first_condition_outcome);
            intern_token(token_id, primary_cond_idx, token_idx, TokenSource::PolymarketFPMM);
            return;
          }

          uint8_t outcome_cnt = outcome_counts[cond_pos];
          for (uint8_t outcome = 0; outcome < outcome_cnt; ++outcome) {
            assert(outcome < 31);
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
        default:
          break;
        }
      }
    }
    // 恒等式验证
    assert(ct.total == ct.polymarket.total + ct.other.total);
    assert(ct.polymarket.total == ct.polymarket.token_reg.total + ct.polymarket.fpmm_poly);
    assert(ct.polymarket.token_reg.total ==
           ct.polymarket.token_reg.amm + ct.polymarket.token_reg.negrisk +
               ct.polymarket.token_reg.orderbook + ct.polymarket.token_reg.other);
    assert(ct.other.total == ct.other.prep + ct.other.fpmm_other + ct.other.split);
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
        default:
          break;
        }
      }
    }
    // 恒等式验证
    assert(tt.total == tt.polymarket.total + tt.other.total);
    assert(tt.polymarket.total == tt.polymarket.token_reg.total + tt.polymarket.fpmm_poly.total);
    assert(tt.polymarket.token_reg.total ==
           tt.polymarket.token_reg.amm + tt.polymarket.token_reg.negrisk +
               tt.polymarket.token_reg.orderbook + tt.polymarket.token_reg.other);
    {
      int64_t sum = 0;
      for (const auto &[k, v] : tt.polymarket.fpmm_poly.by_collateral)
        sum += v;
      assert(tt.polymarket.fpmm_poly.total == sum);
    }
    assert(tt.other.total == tt.other.fpmm_other + tt.other.split + tt.other.transfer_inferred);
    progress_.token_tree = tt;
  }

  void push_event(const std::string &user_addr, const RawEvent &evt) {
    std::string lower = to_lower(user_addr);
    new_events_.emplace_back(lower, evt);
    progress_.total_events++;

    // 实时更新用户数
    if (seen_users_.insert(lower).second) {
      progress_.total_users = seen_users_.size();
    }

    // 维护 Collateral 维度统计
    uint16_t coll_key = static_cast<uint16_t>(evt.type) * 256 + evt.collateral;
    progress_.event_by_collateral[coll_key]++;

    // 更新事件计数
    switch (evt.type) {
    case EventType::OrderBuy:
    case EventType::OrderSell:
      progress_.cnt_order++;
      break;
    case EventType::SplitNormal:
    case EventType::SplitNegRisk:
    case EventType::SplitNonPoly:
      progress_.cnt_split++;
      break;
    case EventType::MergeNormal:
    case EventType::MergeNegRisk:
    case EventType::MergeNonPoly:
      progress_.cnt_merge++;
      break;
    case EventType::Redemption:
    case EventType::RedemptionNonPoly:
      progress_.cnt_redemption++;
      break;
    case EventType::FPMMBuy:
    case EventType::FPMMSell:
      progress_.cnt_fpmm_trade++;
      break;
    case EventType::FPMMLPAdd:
    case EventType::FPMMLPRemove:
    case EventType::FPMMLPReturn:
      progress_.cnt_fpmm_funding++;
      break;
    case EventType::Convert:
      progress_.cnt_convert++;
      break;
    case EventType::TransferInNegRisk:
    case EventType::TransferInOther:
    case EventType::TransferInNonPoly:
    case EventType::TransferOutNegRisk:
    case EventType::TransferOutOther:
    case EventType::TransferOutNonPoly:
      progress_.cnt_transfer++;
      break;
    default:
      assert(false);
      break;
    }
  }

  void phase1_update_mappings(int64_t start, int64_t end);
  void phase2_build_semantic_index(int64_t start, int64_t end);
  void phase3_process_transfers(int64_t start, int64_t end);
  void commit_chunk(int64_t new_cursor);

  [[noreturn]] void fail_transfer_assert(const char *msg) const {
    std::cerr << "[Stage2][ASSERT] " << msg << std::endl;
    if (!current_transfer_context_.empty()) {
      std::cerr << "[Stage2][ASSERT] current transfer: " << current_transfer_context_ << std::endl;
    }
    assert(false && "stage2 transfer assertion");
    std::abort();
  }

  void assert_transfer(bool cond, const char *msg) const {
    if (!cond) {
      fail_transfer_assert(msg);
    }
  }

  bool is_protocol_contract(const std::string &addr) const {
    return addr == ZERO_ADDR || addr == CTF_EXCHANGE || addr == NEG_RISK_CTF_EXCHANGE ||
           addr == NEG_RISK_ADAPTER || addr == CONDITIONAL_TOKENS ||
           addr == NO_TOKEN_BURN_ADDRESS || fpmm_map_.count(addr) > 0;
  }

  TransferClass classify_and_emit(int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
                                  int64_t block, const std::string &op,
                                  const std::string &from, const std::string &to,
                                  const std::string &token_id, int64_t amount,
                                  uint32_t cond_idx, uint8_t token_idx, Collateral collateral);
};

} // namespace stage2
