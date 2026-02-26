#include "stage2_builder.hpp"

#include <algorithm>
#include <cstring>

namespace stage2 {

EventBuilder::EventBuilder(Database &stage1_db, Database &stage2_db, int chunk_size)
    : stage1_db_(stage1_db), stage2_db_(stage2_db), chunk_size_(chunk_size) {}

void EventBuilder::init_schema() {
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

void EventBuilder::load_from_rb() {
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
      "  WHEN ue.collateral = 0 AND nrm.question_id IS NOT NULL THEN " +
      std::to_string(static_cast<int>(Collateral::WrappedUSDCe)) + " "
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

int64_t EventBuilder::cursor() const { return progress_.cursor; }

bool EventBuilder::build_chunk(int64_t target_block) {
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

const BuildProgress &EventBuilder::progress() const { return progress_; }

} // namespace stage2
