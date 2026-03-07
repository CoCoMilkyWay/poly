#include "stage3_sync.hpp"
namespace stage3 {

StageSync::StageSync(EventBuilder &builder, Database &stage0_db, Database &stage2_db, Database &stage3_db,
                     int base_interval_seconds)
    : builder_(builder), stage0_db_(stage0_db), stage2_db_(stage2_db), stage3_db_(stage3_db),
      base_interval_seconds_(base_interval_seconds) {
  assert(base_interval_seconds_ > 0);
  init_schema();
  load_conditions();
  load_cursor();
  refresh_status_locked();
}

void StageSync::init_schema() const {
  const std::string table_sync_cursor = kSqlTableSyncCursorState;
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_event_fact = kSqlTableEventFact;
  const std::string table_block_agg = kSqlTableBlockAggState;

  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_sync_cursor + R"( (
        id INTEGER PRIMARY KEY,
        sort_key BIGINT NOT NULL,
        user_addr BLOB NOT NULL,
        cond_idx INTEGER NOT NULL,
        event_type INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        processed_events BIGINT NOT NULL
      )
    ))");
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
    ))");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_user_summary + R"( (
        user_addr BLOB PRIMARY KEY,
        total_events BIGINT NOT NULL,
        total_realized_pnl BIGINT NOT NULL,
        total_unrealized_pnl BIGINT NOT NULL,
        active_tokens BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL
      )
    ))");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_event_fact + R"( (
        user_addr BLOB NOT NULL,
        sort_key BIGINT NOT NULL,
        cond_idx INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        event_type INTEGER NOT NULL,
        realized_delta BIGINT NOT NULL,
        realized_cum BIGINT NOT NULL,
        unrealized_pnl BIGINT NOT NULL,
        token_count INTEGER NOT NULL,
        tag_id INTEGER NOT NULL,
        exposure BIGINT NOT NULL,
        volume BIGINT NOT NULL,
        holding_period BIGINT NOT NULL,
        PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
      )
    ))");
  stage3_db_.execute("CREATE TABLE IF NOT EXISTS " + table_block_agg + R"( (
        user_addr BLOB NOT NULL,
        block_bucket BIGINT NOT NULL,
        tag_id INTEGER NOT NULL,
        realized_sum BIGINT NOT NULL,
        realized_kll BLOB,
        event_count BIGINT NOT NULL,
        exposure_tw_sum BIGINT NOT NULL,
        volume_sum BIGINT NOT NULL,
        holding_period_tw_sum BIGINT NOT NULL,
        token_count_tw_sum BIGINT NOT NULL,
        time_weight_sum BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL,
        PRIMARY KEY (user_addr, block_bucket, tag_id)
      )
    ))");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexEventFactUserSk) +
      " ON " + table_event_fact + "(user_addr, sort_key)");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexEventFactUserTagSk) +
      " ON " + table_event_fact + "(user_addr, tag_id, sort_key, cond_idx, event_type, token_idx)");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexUserSummaryEvents) +
      " ON " + table_user_summary + "(total_events)");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexTokenStateUser) +
      " ON " + table_token_state + "(user_addr)");
  stage3_db_.execute(
      "CREATE INDEX IF NOT EXISTS " + std::string(kSqlIndexBlockAggStateUserBucket) +
      " ON " + table_block_agg + "(user_addr, block_bucket, tag_id)");
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(*) FROM " + table_sync_cursor + " WHERE id=1");
  assert(r && !r->HasError());
  if (r->GetValue(0, 0).GetValue<int64_t>() == 0) {
    auto ins = conn->Query(
        "INSERT INTO " + table_sync_cursor + " VALUES (1, -1, from_hex(''), " +
        std::to_string(kSyncCursorSentinel) + ", " +
        std::to_string(kSyncCursorSentinel) + ", " +
        std::to_string(kSyncCursorSentinel) + ", 0)");
    assert(ins && !ins->HasError());
  }
}

void StageSync::load_conditions() {
  conditions_.clear();
  cond_tag_ids_.clear();

  auto conn = stage2_db_.create_connection();
  auto rc = conn->Query(
      "SELECT cond_idx, lower(hex(cond_id)) AS cond_hex, outcome_cnt, "
      "payout_0, payout_1, payout_2, payout_3, payout_4, payout_5, payout_6, payout_7, "
      "CASE WHEN question_id IS NULL THEN '' ELSE lower(hex(question_id)) END AS qid, source "
      "FROM rb_condition ORDER BY cond_idx");
  assert(rc && !rc->HasError());
  conditions_.resize(static_cast<size_t>(rc->RowCount()));
  cond_tag_ids_.assign(static_cast<size_t>(rc->RowCount()), 13);
  std::unordered_map<std::string, uint32_t> cond_hex_to_idx;
  cond_hex_to_idx.reserve(static_cast<size_t>(rc->RowCount()) + 1);
  for (idx_t i = 0; i < rc->RowCount(); ++i) {
    uint32_t idx = rc->GetValue(0, i).GetValue<uint32_t>();
    assert(idx == i);
    std::string cond_hex = rc->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_hex_to_idx.emplace(cond_hex, idx);
    ConditionInfo info;
    info.outcome_count = rc->GetValue(2, i).GetValue<uint8_t>();
    info.payout_numerators.reserve(info.outcome_count);
    for (int j = 0; j < info.outcome_count; ++j) {
      auto v = rc->GetValue(3 + j, i);
      info.payout_numerators.push_back(v.IsNull() ? -1 : v.GetValue<int64_t>());
    }
    std::string qid = rc->GetValue(11, i).GetValueUnsafe<std::string>();
    if (!qid.empty()) {
      info.question_id = "0x" + qid;
    }
    info.source = static_cast<ConditionSource>(rc->GetValue(12, i).GetValue<int32_t>());
    conditions_[idx] = std::move(info);
  }

  auto stage0_conn = stage0_db_.create_connection();
  auto tags = stage0_conn->Query(
      "SELECT lower(hex(condition_id)) AS cond_hex, coalesce(tag_name, 'Unknown') AS tag_name "
      "FROM pm_condition_scan_class");
  assert(tags && !tags->HasError());
  for (idx_t i = 0; i < tags->RowCount(); ++i) {
    std::string cond_hex = tags->GetValue(0, i).GetValueUnsafe<std::string>();
    auto it = cond_hex_to_idx.find(cond_hex);
    if (it == cond_hex_to_idx.end()) {
      continue;
    }
    std::string tag_name = tags->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_tag_ids_[it->second] = tag_name_to_id(tag_name);
  }
}

void StageSync::load_cursor() {
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query(
      "SELECT sort_key, lower(hex(user_addr)), cond_idx, event_type, token_idx, processed_events "
      "FROM " +
      std::string(kSqlTableSyncCursorState) + " WHERE id=1");
  assert(r && !r->HasError() && r->RowCount() == 1);
  sync_cursor_.sort_key = r->GetValue(0, 0).GetValue<int64_t>();
  sync_cursor_.user_hex = r->GetValue(1, 0).GetValueUnsafe<std::string>();
  sync_cursor_.cond_idx = r->GetValue(2, 0).GetValue<int32_t>();
  sync_cursor_.event_type = r->GetValue(3, 0).GetValue<int32_t>();
  sync_cursor_.token_idx = r->GetValue(4, 0).GetValue<int32_t>();
  sync_cursor_.processed_events = r->GetValue(5, 0).GetValue<int64_t>();
}

void StageSync::save_cursor_locked(duckdb::Connection &conn) const {
  std::string user_hex = sync_cursor_.user_hex;
  auto q = conn.Query(
      "UPDATE " + std::string(kSqlTableSyncCursorState) + " SET "
                                                          "sort_key=" +
      std::to_string(sync_cursor_.sort_key) +
      ", user_addr=from_hex('" + user_hex + "')" +
      ", cond_idx=" + std::to_string(sync_cursor_.cond_idx) +
      ", event_type=" + std::to_string(sync_cursor_.event_type) +
      ", token_idx=" + std::to_string(sync_cursor_.token_idx) +
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
