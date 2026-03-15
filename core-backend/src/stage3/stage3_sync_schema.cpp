#include "stage3_sync.hpp"
#include <fstream>
namespace stage3 {

StageSync::StageSync(EventBuilder &builder, Database &stage0_db, Database &stage2_db, Database &stage3_db,
                     int base_interval_seconds)
    : builder_(builder), stage0_db_(stage0_db), stage2_db_(stage2_db), stage3_db_(stage3_db),
      event_fact_store_(std::make_unique<core::rocks::Stage3EventFactStore>(
          stage3_db_.data_dir() + "/event_fact.rocks")),
      base_interval_seconds_(base_interval_seconds) {
  assert(base_interval_seconds_ > 0);
  init_schema();
  load_tag_mapping();
  load_conditions();
  load_cursor();
  refresh_status_locked();
}

json StageSync::stage2_rocksdb_memory_breakdown() const {
  return builder_.rocksdb_memory_breakdown();
}

json StageSync::stage3_rocksdb_memory_breakdown() const {
  const core::rocks::MemoryStats stats = event_fact_store_->memory_stats();
  return {
      {"name", "stage3_event_fact"},
      {"engine", "rocksdb"},
      {"path", event_fact_store_->db_path()},
      {"memtables_bytes", stats.memtables_bytes},
      {"table_readers_bytes", stats.table_readers_bytes},
      {"block_cache_bytes", stats.block_cache_bytes},
      {"block_cache_pinned_bytes", stats.block_cache_pinned_bytes},
      {"estimated_total_bytes", stats.estimated_total_bytes()},
  };
}

void StageSync::load_tag_mapping() {
  auto trim = [](const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
      return std::string();
    }
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  };

  std::ifstream f("core-backend/src/stage0/TAG.md");
  assert(f.is_open());

  tag_to_industry_id_.clear();
  int8_t current_id = -1;
  int8_t next_id = 0;
  std::string line;
  while (std::getline(f, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    if (line.rfind("## ", 0) == 0) {
      const std::string level1 = trim(line.substr(3));
      assert(!level1.empty());
      assert(next_id <= 12);
      current_id = next_id;
      ++next_id;
      const std::string key = normalize_tag_key(level1);
      assert(!key.empty());
      tag_to_industry_id_[key] = current_id;
      continue;
    }
    if (line.rfind("- ", 0) == 0) {
      assert(current_id >= 0);
      std::string rest = line.substr(2);
      size_t hash_pos = rest.find('#');
      if (hash_pos != std::string::npos) {
        rest = rest.substr(0, hash_pos);
      }
      const std::string level2 = trim(rest);
      assert(!level2.empty());
      const std::string key = normalize_tag_key(level2);
      assert(!key.empty());
      tag_to_industry_id_[key] = current_id;
    }
  }
  assert(next_id == 13);
  assert(!tag_to_industry_id_.empty());
}

void StageSync::init_schema() const {
  const std::string table_cursor_state = kSqlTableCursorState;
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_account_bucket_pnl = kSqlTableAccountBucketPnlState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;

  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_cursor_state + R"( (
        id INTEGER PRIMARY KEY,
        sort_key BIGINT NOT NULL,
        processed_events BIGINT NOT NULL
      )
    )");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_token_state + R"( (
        user_addr BLOB NOT NULL,
        cond_idx INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        pos BIGINT NOT NULL,
        cost BIGINT NOT NULL,
        lp BIGINT NOT NULL,
        entry_block BIGINT NOT NULL,
        PRIMARY KEY (user_addr, cond_idx, token_idx)
      )
    )");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_user_summary + R"( (
        user_addr BLOB PRIMARY KEY,
        total_events BIGINT NOT NULL,
        total_realized_pnl BIGINT NOT NULL,
        total_unrealized_pnl BIGINT NOT NULL,
        active_tokens BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL
      )
    )");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_account_bucket_pnl + R"( (
        user_addr BLOB NOT NULL,
        block_bucket BIGINT NOT NULL,
        samples_blob BLOB NOT NULL,
        close_pnl BIGINT NOT NULL,
        min_pnl BIGINT NOT NULL,
        updated_sort_key BIGINT NOT NULL,
        PRIMARY KEY (user_addr, block_bucket)
      )
    )");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_feature_tensor + R"( (
        user_addr BLOB NOT NULL,
        block_bucket BIGINT NOT NULL,
        tag_id INTEGER NOT NULL,
        last_sort_key_10w BIGINT NOT NULL DEFAULT 0,
        last_block_10w BIGINT NOT NULL DEFAULT 0,
        last_exposure_10w BIGINT NOT NULL DEFAULT 0,
        last_holding_period_10w HUGEINT NOT NULL DEFAULT 0,
        last_token_count_10w BIGINT NOT NULL DEFAULT 0,
        time_weight_sum_10w BIGINT NOT NULL DEFAULT 0,
        token_count_tw_sum_10w BIGINT NOT NULL DEFAULT 0,
        exposure_tw_sum_10w HUGEINT NOT NULL DEFAULT 0,
        volume_sum_10w BIGINT NOT NULL DEFAULT 0,
        holding_period_exp_tw_sum_10w HUGEINT NOT NULL DEFAULT 0,
        token_avg_10w BIGINT NOT NULL DEFAULT 0,
        exposure_avg_10w BIGINT NOT NULL DEFAULT 0,
        volume_10w BIGINT NOT NULL DEFAULT 0,
        holding_period_avg_10w BIGINT NOT NULL DEFAULT 0,
        sharpe_10w DOUBLE NOT NULL DEFAULT 0,
        ps_token_avg_10w BIGINT NOT NULL DEFAULT 0,
        ps_exposure_avg_10w BIGINT NOT NULL DEFAULT 0,
        ps_volume_10w BIGINT NOT NULL DEFAULT 0,
        ps_holding_period_avg_10w BIGINT NOT NULL DEFAULT 0,
        token_avg_100w BIGINT NOT NULL DEFAULT 0,
        token_avg_1000w BIGINT NOT NULL DEFAULT 0,
        exposure_avg_100w BIGINT NOT NULL DEFAULT 0,
        exposure_avg_1000w BIGINT NOT NULL DEFAULT 0,
        volume_avg_100w BIGINT NOT NULL DEFAULT 0,
        volume_avg_1000w BIGINT NOT NULL DEFAULT 0,
        holding_period_avg_100w BIGINT NOT NULL DEFAULT 0,
        holding_period_avg_1000w BIGINT NOT NULL DEFAULT 0,
        sharpe_100w DOUBLE NOT NULL DEFAULT 0,
        sharpe_1000w DOUBLE NOT NULL DEFAULT 0,
        updated_sort_key BIGINT NOT NULL DEFAULT 0,
        PRIMARY KEY (user_addr, block_bucket, tag_id)
      )
    )");
  // 仅创建非PK前缀的有用索引 (PK自带B-tree可覆盖前缀查询)
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexUserSummaryEvents) +
      " ON " + table_user_summary + "(total_events)");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexAccountBucketPnlBlockBucket) +
      " ON " + table_account_bucket_pnl + "(block_bucket)");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexFeatureTensorBucketTagUser) +
      " ON " + table_feature_tensor + "(block_bucket, tag_id, user_addr)");
  auto conn = stage3_db_.create_connection();
  auto cursor_count_result = conn->Query("SELECT COUNT(*) FROM " + table_cursor_state + " WHERE id=1");
  assert(cursor_count_result && !cursor_count_result->HasError());
  if (cursor_count_result->GetValue(0, 0).GetValue<int64_t>() == 0) {
    auto ins = conn->Query(
        "INSERT INTO " + table_cursor_state + " VALUES (1, -1, 0)");
    assert(ins && !ins->HasError());
  }
}

void StageSync::load_conditions() {
  conditions_.clear();
  cond_tag_ids_.clear();
  cond_market_question_counts_.clear();

  auto stage2_conn = stage2_db_.create_connection();
  auto condition_result = stage2_conn->Query(
      "SELECT cond_idx, lower(hex(cond_id)) AS cond_hex, outcome_cnt, "
      "payout_0, payout_1, payout_2, payout_3, payout_4, payout_5, payout_6, payout_7, "
      "CASE WHEN question_id IS NULL THEN '' ELSE lower(hex(question_id)) END AS qid, source "
      "FROM rb_condition ORDER BY cond_idx");
  assert(condition_result && !condition_result->HasError());
  conditions_.resize(static_cast<size_t>(condition_result->RowCount()));
  cond_tag_ids_.assign(static_cast<size_t>(condition_result->RowCount()), 13);
  cond_market_question_counts_.assign(static_cast<size_t>(condition_result->RowCount()), 0);
  std::unordered_map<std::string, uint32_t> cond_hex_to_index;
  cond_hex_to_index.reserve(static_cast<size_t>(condition_result->RowCount()) + 1);
  for (idx_t i = 0; i < condition_result->RowCount(); ++i) {
    uint32_t idx = condition_result->GetValue(0, i).GetValue<uint32_t>();
    assert(idx == i);
    std::string cond_hex = condition_result->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_hex_to_index.emplace(cond_hex, idx);
    ConditionInfo info;
    info.outcome_count = condition_result->GetValue(2, i).GetValue<uint8_t>();
    info.payout_numerators.reserve(info.outcome_count);
    for (int j = 0; j < info.outcome_count; ++j) {
      auto payout_value = condition_result->GetValue(3 + j, i);
      info.payout_numerators.push_back(payout_value.IsNull() ? -1 : payout_value.GetValue<int64_t>());
    }
    std::string question_hex = condition_result->GetValue(11, i).GetValueUnsafe<std::string>();
    if (!question_hex.empty()) {
      info.question_id = "0x" + question_hex;
    }
    info.source = static_cast<ConditionSource>(condition_result->GetValue(12, i).GetValue<int32_t>());
    conditions_[idx] = std::move(info);
  }

  std::unordered_map<std::string, std::string> question_to_market;
  std::unordered_map<std::string, int32_t> market_question_counts;
  auto neg_risk_market_result = stage2_conn->Query(
      "SELECT lower(hex(question_id)) AS qid_hex, lower(hex(market_id)) AS market_hex "
      "FROM rb_neg_risk_market");
  assert(neg_risk_market_result && !neg_risk_market_result->HasError());
  question_to_market.reserve(static_cast<size_t>(neg_risk_market_result->RowCount()) + 1);
  market_question_counts.reserve(static_cast<size_t>(neg_risk_market_result->RowCount()) + 1);
  for (idx_t i = 0; i < neg_risk_market_result->RowCount(); ++i) {
    const std::string question_hex = neg_risk_market_result->GetValue(0, i).GetValueUnsafe<std::string>();
    const std::string market_hex = neg_risk_market_result->GetValue(1, i).GetValueUnsafe<std::string>();
    auto [market_it, inserted] = question_to_market.emplace(question_hex, market_hex);
    assert(inserted || market_it->second == market_hex);
    market_question_counts[market_hex]++;
  }

  auto stage0_conn = stage0_db_.create_connection();
  auto tags = stage0_conn->Query(
      "SELECT lower(hex(condition_id)) AS cond_hex, coalesce(tag_name, 'Unknown') AS tag_name "
      "FROM pm_condition_scan_class");
  assert(tags && !tags->HasError());
  for (idx_t i = 0; i < tags->RowCount(); ++i) {
    std::string cond_hex = tags->GetValue(0, i).GetValueUnsafe<std::string>();
    auto cond_it = cond_hex_to_index.find(cond_hex);
    if (cond_it == cond_hex_to_index.end()) {
      continue;
    }
    std::string tag_name = tags->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_tag_ids_[cond_it->second] = tag_name_to_id(tag_name);
  }

  for (size_t i = 0; i < conditions_.size(); ++i) {
    const auto &info = conditions_[i];
    if (info.question_id.empty()) {
      continue;
    }
    assert(info.question_id.rfind("0x", 0) == 0);
    const std::string question_hex = info.question_id.substr(2);
    auto question_market_it = question_to_market.find(question_hex);
    if (question_market_it == question_to_market.end()) {
      continue;
    }
    auto market_question_count_it = market_question_counts.find(question_market_it->second);
    assert(market_question_count_it != market_question_counts.end());
    assert(market_question_count_it->second > 0);
    cond_market_question_counts_[i] = static_cast<uint16_t>(market_question_count_it->second);
  }
}

void StageSync::load_cursor() {
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query(
      "SELECT sort_key, processed_events FROM " +
      std::string(kSqlTableCursorState) + " WHERE id=1");
  assert(r && !r->HasError() && r->RowCount() == 1);
  sync_cursor_.sort_key = r->GetValue(0, 0).GetValue<int64_t>();
  sync_cursor_.processed_events = r->GetValue(1, 0).GetValue<int64_t>();
}

void StageSync::save_cursor_locked(duckdb::Connection &conn) const {
  auto q = conn.Query(
      "UPDATE " + std::string(kSqlTableCursorState) + " SET "
                                                      "sort_key=" +
      std::to_string(sync_cursor_.sort_key) +
      ", processed_events=" + std::to_string(sync_cursor_.processed_events) +
      " WHERE id=1");
  assert(q && !q->HasError());
}

void StageSync::refresh_status_locked() const {
  sync_.head_block = builder_.cursor();
  sync_.last_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  sync_.behind_blocks = std::max<int64_t>(0, sync_.head_block - sync_.last_block);
  sync_.behind_chunks = (sync_.behind_blocks == 0) ? 0 : 1;
}

} // namespace stage3
