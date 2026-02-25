#pragma once

#include "../core/ctf_helpers.hpp"
#include "../core/database.hpp"
#include "../core/keccak256.hpp"
#include "event_build_types.hpp"
#include "event_build_utils.hpp"
#include "types.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace stage2 {

class EventBuilder {
public:
  EventBuilder(Database &stage1_db, Database &stage2_db, int chunk_size)
      : stage1_db_(stage1_db), stage2_db_(stage2_db), chunk_size_(chunk_size) {}

  void init_schema() {
    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS stage2_cursor (
        key TEXT PRIMARY KEY,
        value BIGINT
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_condition (
        cond_idx    INTEGER PRIMARY KEY,
        cond_id     BLOB NOT NULL UNIQUE,
        outcome_cnt INTEGER NOT NULL,
        payout_0    BIGINT,
        payout_1    BIGINT,
        payout_2    BIGINT,
        payout_3    BIGINT,
        payout_4    BIGINT,
        payout_5    BIGINT,
        payout_6    BIGINT,
        payout_7    BIGINT,
        question_id BLOB,
        source      INTEGER NOT NULL DEFAULT 0
      )
    )");
    {
      auto conn = stage2_db_.create_connection();
      auto cols = conn->Query("PRAGMA table_info(rb_condition)");
      bool has_question_id = false, has_source = false;
      for (idx_t i = 0; i < cols->RowCount(); ++i) {
        std::string name = cols->GetValue(1, i).GetValueUnsafe<std::string>();
        if (name == "question_id")
          has_question_id = true;
        if (name == "source")
          has_source = true;
      }
      if (!has_question_id) {
        stage2_db_.execute("ALTER TABLE rb_condition ADD COLUMN question_id BLOB");
      }
      if (!has_source) {
        stage2_db_.execute("ALTER TABLE rb_condition ADD COLUMN source INTEGER NOT NULL DEFAULT 0");
      }
    }

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_token (
        token_id  BLOB PRIMARY KEY,
        cond_idx  INTEGER NOT NULL,
        is_yes    INTEGER NOT NULL,
        source    INTEGER NOT NULL DEFAULT 0
      )
    )");
    {
      auto conn = stage2_db_.create_connection();
      auto cols = conn->Query("PRAGMA table_info(rb_token)");
      bool has_source = false;
      for (idx_t i = 0; i < cols->RowCount(); ++i) {
        if (cols->GetValue(1, i).GetValueUnsafe<std::string>() == "source") {
          has_source = true;
          break;
        }
      }
      if (!has_source) {
        stage2_db_.execute("ALTER TABLE rb_token ADD COLUMN source INTEGER NOT NULL DEFAULT 0");
      }
    }

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_fpmm (
        fpmm_addr    BLOB PRIMARY KEY,
        cond_idx     INTEGER NOT NULL,
        collateral   INTEGER NOT NULL DEFAULT 1
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_collateral (
        coll_id         INTEGER PRIMARY KEY,
        collateral_addr BLOB NOT NULL UNIQUE
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_cond_collateral (
        cond_idx INTEGER PRIMARY KEY,
        coll_id  INTEGER NOT NULL
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_neg_risk_market (
        question_id BLOB PRIMARY KEY,
        market_id   BLOB NOT NULL
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS user_event (
        user_addr   BLOB NOT NULL,
        sort_key    BIGINT NOT NULL,
        cond_idx    INTEGER NOT NULL,
        event_type  INTEGER NOT NULL,
        token_idx   INTEGER NOT NULL,
        collateral  INTEGER NOT NULL DEFAULT 1,
        amount      BIGINT NOT NULL,
        price       BIGINT NOT NULL,
        PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
      )
    )");

    auto conn = stage2_db_.create_connection();
    auto r = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
    if (r->RowCount() == 0) {
      conn->Query("INSERT INTO stage2_cursor VALUES ('last_block', 0)");
    }
  }

  void load_from_rb() {
    auto conn = stage2_db_.create_connection();

    collateral_addr_to_id_.clear();
    collateral_id_to_addr_.clear();
    next_collateral_id_ = static_cast<uint8_t>(Collateral::WrappedUSDCe) + 1;
    // 预置已知抵押品，保持固定 ID 兼容历史数据
    collateral_addr_to_id_[ZERO_ADDR] = static_cast<uint8_t>(Collateral::Unknown);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::Unknown)] = ZERO_ADDR;
    collateral_addr_to_id_[USDC_NATIVE] = static_cast<uint8_t>(Collateral::USDC);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::USDC)] = USDC_NATIVE;
    collateral_addr_to_id_[USDC_E] = static_cast<uint8_t>(Collateral::USDCe);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::USDCe)] = USDC_E;
    collateral_addr_to_id_[WETH] = static_cast<uint8_t>(Collateral::WETH);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::WETH)] = WETH;
    collateral_addr_to_id_[DAI] = static_cast<uint8_t>(Collateral::DAI);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::DAI)] = DAI;
    collateral_addr_to_id_[WMATIC] = static_cast<uint8_t>(Collateral::WMATIC);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::WMATIC)] = WMATIC;
    collateral_addr_to_id_[USDT] = static_cast<uint8_t>(Collateral::USDT);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::USDT)] = USDT;
    collateral_addr_to_id_[WRAPPED_USDC_E] = static_cast<uint8_t>(Collateral::WrappedUSDCe);
    collateral_id_to_addr_[static_cast<uint8_t>(Collateral::WrappedUSDCe)] = WRAPPED_USDC_E;

    auto coll_r = conn->Query("SELECT coll_id, collateral_addr FROM rb_collateral");
    for (idx_t i = 0; i < coll_r->RowCount(); ++i) {
      uint8_t coll_id = static_cast<uint8_t>(coll_r->GetValue(0, i).GetValue<int32_t>());
      std::string addr = to_lower(blob_to_hex(coll_r->GetValue(1, i).GetValueUnsafe<std::string>()));
      collateral_addr_to_id_[addr] = coll_id;
      collateral_id_to_addr_[coll_id] = addr;
      if (coll_id >= next_collateral_id_) {
        next_collateral_id_ = coll_id + 1;
      }
    }

    auto cur = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
    progress_.cursor = cur->RowCount() > 0 ? cur->GetValue(0, 0).GetValue<int64_t>() : 0;

    // 从 user_event 表重新计算事件计数（保证数据一致性）
    auto cnt_r = conn->Query(R"(
      SELECT event_type, COUNT(*) as cnt FROM user_event GROUP BY event_type
    )");
    for (idx_t i = 0; i < cnt_r->RowCount(); ++i) {
      int type = cnt_r->GetValue(0, i).GetValue<int>();
      int64_t cnt = cnt_r->GetValue(1, i).GetValue<int64_t>();
      switch (static_cast<EventType>(type)) {
      case EventType::Buy:
      case EventType::Sell:
        progress_.cnt_order += cnt;
        break;
      case EventType::Split:
        progress_.cnt_split += cnt;
        break;
      case EventType::Merge:
        progress_.cnt_merge += cnt;
        break;
      case EventType::Redemption:
        progress_.cnt_redemption += cnt;
        break;
      case EventType::FPMMBuy:
      case EventType::FPMMSell:
        progress_.cnt_fpmm_trade += cnt;
        break;
      case EventType::FPMMLPAdd:
      case EventType::FPMMLPRemove:
        progress_.cnt_fpmm_funding += cnt;
        break;
      case EventType::Convert:
        progress_.cnt_convert += cnt;
        break;
      case EventType::TransferIn:
      case EventType::TransferOut:
        progress_.cnt_transfer += cnt;
        break;
      }
      progress_.total_events += cnt;
    }

    auto cond_r = conn->Query("SELECT cond_idx, cond_id, outcome_cnt, "
                              "payout_0, payout_1, payout_2, payout_3, "
                              "payout_4, payout_5, payout_6, payout_7, question_id, source FROM rb_condition ORDER BY cond_idx");
    for (idx_t i = 0; i < cond_r->RowCount(); ++i) {
      uint32_t idx = cond_r->GetValue(0, i).GetValue<uint32_t>();
      std::string cond_id = blob_to_hex(cond_r->GetValue(1, i).GetValueUnsafe<std::string>());
      ConditionInfo info;
      info.outcome_count = cond_r->GetValue(2, i).GetValue<uint8_t>();
      for (int j = 0; j < info.outcome_count; ++j) {
        auto v = cond_r->GetValue(3 + j, i);
        info.payout_numerators.push_back(v.IsNull() ? -1 : v.GetValue<int64_t>());
      }
      auto qid_v = cond_r->GetValue(11, i);
      if (!qid_v.IsNull()) {
        info.question_id = to_lower(blob_to_hex(qid_v.GetValueUnsafe<std::string>()));
      }
      info.source = static_cast<ConditionSource>(cond_r->GetValue(12, i).GetValue<int>());
      while (conditions_.size() <= idx) {
        conditions_.emplace_back();
        cond_ids_.emplace_back();
      }
      conditions_[idx] = info;
      cond_ids_[idx] = cond_id;
      cond_map_[to_lower(cond_id)] = idx;
    }

    auto token_r = conn->Query("SELECT token_id, cond_idx, is_yes, source FROM rb_token");
    for (idx_t i = 0; i < token_r->RowCount(); ++i) {
      std::string tid = blob_to_hex(token_r->GetValue(0, i).GetValueUnsafe<std::string>());
      TokenInfo info;
      int32_t db_cond_idx = token_r->GetValue(1, i).GetValue<int32_t>();
      info.cond_idx = (db_cond_idx == -1) ? UNKNOWN_COND_IDX : static_cast<uint32_t>(db_cond_idx);
      info.is_yes = token_r->GetValue(2, i).GetValue<uint8_t>();
      info.source = static_cast<TokenSource>(token_r->GetValue(3, i).GetValue<int>());
      token_map_[to_lower(tid)] = info;
    }

    auto cond_coll_r = conn->Query("SELECT cond_idx, coll_id FROM rb_cond_collateral");
    for (idx_t i = 0; i < cond_coll_r->RowCount(); ++i) {
      uint32_t cond_idx = cond_coll_r->GetValue(0, i).GetValue<uint32_t>();
      uint8_t coll_id = static_cast<uint8_t>(cond_coll_r->GetValue(1, i).GetValue<int32_t>());
      cond_collateral_[cond_idx] = coll_id;
    }

    auto fpmm_r = conn->Query("SELECT fpmm_addr, cond_idx, collateral FROM rb_fpmm");
    for (idx_t i = 0; i < fpmm_r->RowCount(); ++i) {
      std::string addr = blob_to_hex(fpmm_r->GetValue(0, i).GetValueUnsafe<std::string>());
      FPMMInfo info;
      info.cond_idx = fpmm_r->GetValue(1, i).GetValue<uint32_t>();
      info.collateral = static_cast<uint8_t>(fpmm_r->GetValue(2, i).GetValue<int32_t>());
      fpmm_map_[to_lower(addr)] = info;
      fpmm_cond_idxs_.insert(info.cond_idx);
      if (!cond_collateral_.count(info.cond_idx)) {
        cond_collateral_[info.cond_idx] = info.collateral;
      }
    }

    // 从 rb_neg_risk_market 表加载 question_id -> market_id 映射，并标记 NegRisk 条件
    auto nrm_r = conn->Query("SELECT question_id, market_id FROM rb_neg_risk_market");
    for (idx_t i = 0; i < nrm_r->RowCount(); ++i) {
      std::string question_id = to_lower(blob_to_hex(nrm_r->GetValue(0, i).GetValueUnsafe<std::string>()));
      std::string market_id = to_lower(blob_to_hex(nrm_r->GetValue(1, i).GetValueUnsafe<std::string>()));
      cond_to_market_[question_id] = market_id;
      seen_markets_.insert(market_id);

      // 计算 conditionId 并标记对应条件为 NegRisk
      auto oracle_bytes = hex_to_blob(NEG_RISK_ADAPTER);
      auto qid_bytes = hex_to_blob(question_id);
      std::string input(84, '\0');
      std::memcpy(input.data(), oracle_bytes.data(), std::min(size_t(20), oracle_bytes.size()));
      std::memcpy(input.data() + 20, qid_bytes.data(), std::min(size_t(32), qid_bytes.size()));
      input[83] = 2; // outcomeSlotCount = 2
      auto cond_hash = crypto::keccak256(input);
      std::string cond_id = to_lower(crypto::Keccak256::to_hex(cond_hash));

      auto it = cond_map_.find(cond_id);
      if (it != cond_map_.end()) {
        negrisk_cond_idxs_.insert(it->second);
      }
    }

    progress_.total_conditions = conditions_.size();
    progress_.total_tokens = token_map_.size();
    progress_.total_markets = seen_markets_.size();
    update_cond_type_stats();

    // 加载已知用户（恢复时从数据库）
    auto user_r = conn->Query("SELECT DISTINCT user_addr FROM user_event");
    for (idx_t i = 0; i < user_r->RowCount(); ++i) {
      std::string addr = blob_to_hex(user_r->GetValue(0, i).GetValueUnsafe<std::string>());
      seen_users_.insert(to_lower(addr));
    }
    progress_.total_users = seen_users_.size();

    progress_.event_by_collateral.clear();
    progress_.event_by_token.clear();

    // 恢复 event_by_collateral 统计
    auto evt_stats = conn->Query(
        "SELECT ue.event_type, "
        "CASE "
        "  WHEN ue.collateral = 0 AND cc.coll_id IS NOT NULL THEN cc.coll_id "
        "  WHEN ue.collateral = 0 AND nrm.question_id IS NOT NULL THEN " + std::to_string(static_cast<int>(Collateral::WrappedUSDCe)) + " "
        "  ELSE ue.collateral "
        "END AS effective_collateral, "
        "COUNT(*) "
        "FROM user_event ue "
        "LEFT JOIN rb_cond_collateral cc ON ue.cond_idx = cc.cond_idx "
        "LEFT JOIN rb_condition rc ON ue.cond_idx = rc.cond_idx "
        "LEFT JOIN rb_neg_risk_market nrm ON rc.question_id = nrm.question_id "
        "GROUP BY ue.event_type, effective_collateral");
    for (idx_t i = 0; i < evt_stats->RowCount(); ++i) {
      uint8_t event_type = evt_stats->GetValue(0, i).GetValue<uint8_t>();
      uint8_t collateral = evt_stats->GetValue(1, i).GetValue<uint8_t>();
      int64_t count = evt_stats->GetValue(2, i).GetValue<int64_t>();
      uint16_t key = static_cast<uint16_t>(event_type) * 256 + collateral;
      progress_.event_by_collateral[key] = count;
    }

    // 恢复 event_by_token 统计
    auto token_stats = conn->Query(
        "SELECT event_type, token_idx, COUNT(*) "
        "FROM user_event "
        "GROUP BY event_type, token_idx");
    for (idx_t i = 0; i < token_stats->RowCount(); ++i) {
      uint8_t event_type = token_stats->GetValue(0, i).GetValue<uint8_t>();
      uint8_t token_idx = token_stats->GetValue(1, i).GetValue<uint8_t>();
      int64_t count = token_stats->GetValue(2, i).GetValue<int64_t>();
      uint16_t key = static_cast<uint16_t>(event_type) * 256 + token_idx;
      progress_.event_by_token[key] = count;
    }
    auto evt_total = conn->Query("SELECT COUNT(*) FROM user_event");
    progress_.total_events = evt_total->RowCount() > 0 ? evt_total->GetValue(0, 0).GetValue<int64_t>() : 0;

    if (progress_.cursor > 0)
      progress_.phase = 3;

    std::cerr << "[Stage2] Restored: " << conditions_.size() << " conditions, "
              << token_map_.size() << " tokens, " << fpmm_map_.size() << " FPMMs" << std::endl;
  }

  int64_t cursor() const { return progress_.cursor; }

  bool build_chunk(int64_t target_block) {
    if (progress_.cursor >= target_block)
      return false;

    int64_t chunk_start = progress_.cursor;
    int64_t chunk_end = std::min(progress_.cursor + chunk_size_, target_block);
    // std::cerr << "[Stage2] Processing chunk: " << chunk_start << " -> " << chunk_end << std::endl;
    progress_.target = target_block;
    progress_.chunk_start = chunk_start;
    progress_.chunk_end = chunk_end;
    progress_.running = true;

    new_conditions_.clear();
    new_tokens_.clear();
    new_fpmms_.clear();
    new_collaterals_.clear();
    new_cond_collaterals_.clear();
    new_neg_risk_markets_.clear();
    new_events_.clear();

    tx_split_.clear();
    tx_merge_.clear();
    tx_redemption_.clear();
    tx_convert_.clear();
    tx_order_.clear();
    tx_fpmm_trade_.clear();
    tx_fpmm_funding_.clear();
    chunk_xfer_stats_ = {};

    // 开始 chunk log
    chunk_log_.begin(log_dir_, chunk_start, chunk_end);

    progress_.phase = 1;
    phase1_update_mappings(chunk_start, chunk_end);

    // 写入 log header（phase1 之后，此时 token_map 已更新）
    chunk_log_.write_header(token_map_.size(), fpmm_map_.size(), cond_map_.size());
    if (!token_map_.empty()) {
      auto it = token_map_.begin();
      chunk_log_.write_token_sample(it->first, it->second.cond_idx, it->second.is_yes);
    }

    progress_.phase = 2;
    phase2_build_semantic_index(chunk_start, chunk_end);

    progress_.phase = 3;
    phase3_process_transfers(chunk_start, chunk_end);

    // 验证 transfer 分类完整性
    chunk_xfer_stats_.verify();
    progress_.xfer_stats += chunk_xfer_stats_;

    commit_chunk(chunk_end);

    // 记录统计信息并结束 chunk log
    chunk_log_.set_xfer_stats(TransferStats::format_log(chunk_xfer_stats_, progress_.xfer_stats));
    chunk_log_.finish();

    progress_.cursor = chunk_end;
    progress_.running = false;
    return true;
  }

  const BuildProgress &progress() const { return progress_; }

private:
  Database &stage1_db_;
  Database &stage2_db_;
  int chunk_size_;
  BuildProgress progress_;

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
  std::unordered_map<TxTokenKey, OrderInfo> tx_order_;
  std::unordered_map<TxFPMMKey, std::vector<FPMMTradeInfo>> tx_fpmm_trade_;
  std::unordered_map<TxFPMMKey, std::vector<FPMMFundingInfo>> tx_fpmm_funding_;
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
    uint8_t is_yes;
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

  void intern_token(const std::string &token_id, uint32_t cond_idx, uint8_t is_yes, TokenSource source) {
    std::string lower = to_lower(token_id);
    auto it = token_map_.find(lower);
    if (it != token_map_.end()) {
      // 已存在：不应该出现 TransferInferred 后又从其他来源发现的情况
      // 因为事件按时间顺序处理，FPMM/Split 等事件应该先于 Transfer
      assert(!(it->second.source == TokenSource::TransferInferred && source != TokenSource::TransferInferred));
      return;
    }
    token_map_[lower] = {cond_idx, is_yes, source};
    new_tokens_.push_back({lower, cond_idx, is_yes, source});
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
    for (int index_set = 1; index_set <= 2; ++index_set) {
      auto collection_id = ctf::get_collection_id(cond_bytes, index_set);
      auto position_hash = ctf::get_position_id(collateral_bytes, collection_id);
      std::string token_id = crypto::Keccak256::to_hex(position_hash);
      intern_token(token_id, cond_idx, index_set == 1 ? 1 : 0, source);
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
            uint8_t is_yes = (first_condition_outcome == 0) ? 1 : 0;
            intern_token(token_id, primary_cond_idx, is_yes, TokenSource::PolymarketFPMM);
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
          ct.polymarket.token_reg.normal++;
        }
      } else if (has_fpmm) {
        // 没有 TokenReg 但有 FPMM（早期 Polymarket 或只创建了池子）
        ct.polymarket.total++;
        ct.polymarket.fpmm_only++;
      } else {
        // 既没有 TokenReg 也没有 FPMM → 其他协议
        ct.other.total++;
        switch (src) {
        case ConditionSource::ConditionPrep:
          ct.other.prep++;
          break;
        case ConditionSource::OtherFPMM:
          ct.other.other_fpmm++;
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
    assert(ct.polymarket.total == ct.polymarket.token_reg.total + ct.polymarket.fpmm_only);
    assert(ct.polymarket.token_reg.total == ct.polymarket.token_reg.amm + ct.polymarket.token_reg.negrisk + ct.polymarket.token_reg.normal);
    assert(ct.other.total == ct.other.prep + ct.other.other_fpmm + ct.other.split);
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
        tt.other.transfer_inferred_by_token[tid]++;
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
          tt.polymarket.token_reg.normal++;
        }
      } else if (has_fpmm) {
        // 没有 TokenReg 但有 FPMM（早期 Polymarket 或只创建了池子）
        tt.polymarket.total++;
        tt.polymarket.fpmm_only.total++;
        auto cit = cond_collateral_.find(info.cond_idx);
        uint8_t coll = cit != cond_collateral_.end() ? cit->second : static_cast<uint8_t>(Collateral::Unknown);
        tt.polymarket.fpmm_only.by_collateral[coll]++;
      } else {
        // 既没有 TokenReg 也没有 FPMM → 其他协议
        tt.other.total++;
        switch (src) {
        case TokenSource::OtherFPMM:
          tt.other.other_fpmm++;
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
    assert(tt.polymarket.total == tt.polymarket.token_reg.total + tt.polymarket.fpmm_only.total);
    assert(tt.polymarket.token_reg.total == tt.polymarket.token_reg.amm + tt.polymarket.token_reg.negrisk + tt.polymarket.token_reg.normal);
    {
      int64_t sum = 0;
      for (const auto &[k, v] : tt.polymarket.fpmm_only.by_collateral)
        sum += v;
      assert(tt.polymarket.fpmm_only.total == sum);
    }
    assert(tt.other.total == tt.other.other_fpmm + tt.other.split + tt.other.transfer_inferred);
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

    // 维护两套统计：Token 维度 + Collateral 维度
    uint16_t coll_key = static_cast<uint16_t>(evt.type) * 256 + evt.collateral;
    progress_.event_by_collateral[coll_key]++;
    uint16_t token_key = static_cast<uint16_t>(evt.type) * 256 + evt.token_idx;
    progress_.event_by_token[token_key]++;

    // 更新事件计数
    switch (evt.type) {
    case EventType::Buy:
    case EventType::Sell:
      progress_.cnt_order++;
      break;
    case EventType::Split:
      progress_.cnt_split++;
      break;
    case EventType::Merge:
      progress_.cnt_merge++;
      break;
    case EventType::Redemption:
      progress_.cnt_redemption++;
      break;
    case EventType::FPMMBuy:
    case EventType::FPMMSell:
      progress_.cnt_fpmm_trade++;
      break;
    case EventType::FPMMLPAdd:
    case EventType::FPMMLPRemove:
      progress_.cnt_fpmm_funding++;
      break;
    case EventType::Convert:
      progress_.cnt_convert++;
      break;
    case EventType::TransferIn:
    case EventType::TransferOut:
      progress_.cnt_transfer++;
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
