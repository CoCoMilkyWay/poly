#include "stage3_sync.hpp"
namespace stage3 {

StageSync::StageSync(EventBuilder &builder, Database &stage2_db, Database &stage3_db)
    : builder_(builder), stage2_db_(stage2_db), stage3_db_(stage3_db) {
  init_schema();
  load_conditions();
  load_cursor();
  refresh_status_locked();
}

void StageSync::init_schema() const {
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_sync_cursor (
        id INTEGER PRIMARY KEY,
        sort_key BIGINT NOT NULL,
        user_addr BLOB NOT NULL,
        cond_idx INTEGER NOT NULL,
        event_type INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        processed_events BIGINT NOT NULL
      )
    )");
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_cond_state (
        user_addr BLOB NOT NULL,
        cond_idx INTEGER NOT NULL,
        pos_0 BIGINT NOT NULL,
        pos_1 BIGINT NOT NULL,
        pos_2 BIGINT NOT NULL,
        pos_3 BIGINT NOT NULL,
        pos_4 BIGINT NOT NULL,
        pos_5 BIGINT NOT NULL,
        pos_6 BIGINT NOT NULL,
        pos_7 BIGINT NOT NULL,
        cost_0 BIGINT NOT NULL,
        cost_1 BIGINT NOT NULL,
        cost_2 BIGINT NOT NULL,
        cost_3 BIGINT NOT NULL,
        cost_4 BIGINT NOT NULL,
        cost_5 BIGINT NOT NULL,
        cost_6 BIGINT NOT NULL,
        cost_7 BIGINT NOT NULL,
        lp_0 BIGINT NOT NULL DEFAULT 0,
        lp_1 BIGINT NOT NULL DEFAULT 0,
        lp_2 BIGINT NOT NULL DEFAULT 0,
        lp_3 BIGINT NOT NULL DEFAULT 0,
        lp_4 BIGINT NOT NULL DEFAULT 0,
        lp_5 BIGINT NOT NULL DEFAULT 0,
        lp_6 BIGINT NOT NULL DEFAULT 0,
        lp_7 BIGINT NOT NULL DEFAULT 0,
        realized_pnl BIGINT NOT NULL,
        event_count BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL,
        PRIMARY KEY (user_addr, cond_idx)
      )
    )");
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_summary (
        user_addr BLOB PRIMARY KEY,
        total_events BIGINT NOT NULL,
        total_realized_pnl BIGINT NOT NULL,
        total_unrealized_pnl BIGINT NOT NULL DEFAULT 0,
        active_conditions BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL
      )
    )");
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_event_fact (
        user_addr BLOB NOT NULL,
        sort_key BIGINT NOT NULL,
        cond_idx INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        event_type INTEGER NOT NULL,
        realized_delta BIGINT NOT NULL,
        realized_cum BIGINT NOT NULL,
        unrealized_pnl BIGINT NOT NULL DEFAULT 0,
        token_count INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
      )
    )");
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_cond_checkpoint (
        user_addr BLOB NOT NULL,
        checkpoint_sort_key BIGINT NOT NULL,
        cond_idx INTEGER NOT NULL,
        pos_0 BIGINT NOT NULL,
        pos_1 BIGINT NOT NULL,
        pos_2 BIGINT NOT NULL,
        pos_3 BIGINT NOT NULL,
        pos_4 BIGINT NOT NULL,
        pos_5 BIGINT NOT NULL,
        pos_6 BIGINT NOT NULL,
        pos_7 BIGINT NOT NULL,
        cost_0 BIGINT NOT NULL,
        cost_1 BIGINT NOT NULL,
        cost_2 BIGINT NOT NULL,
        cost_3 BIGINT NOT NULL,
        cost_4 BIGINT NOT NULL,
        cost_5 BIGINT NOT NULL,
        cost_6 BIGINT NOT NULL,
        cost_7 BIGINT NOT NULL,
        lp_0 BIGINT NOT NULL DEFAULT 0,
        lp_1 BIGINT NOT NULL DEFAULT 0,
        lp_2 BIGINT NOT NULL DEFAULT 0,
        lp_3 BIGINT NOT NULL DEFAULT 0,
        lp_4 BIGINT NOT NULL DEFAULT 0,
        lp_5 BIGINT NOT NULL DEFAULT 0,
        lp_6 BIGINT NOT NULL DEFAULT 0,
        lp_7 BIGINT NOT NULL DEFAULT 0,
        realized_pnl BIGINT NOT NULL,
        event_count BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL,
        PRIMARY KEY (user_addr, checkpoint_sort_key, cond_idx)
      )
    )");
    stage3_db_.execute(
        "CREATE INDEX IF NOT EXISTS idx_s3_fact_user_sk "
        "ON s3_user_event_fact(user_addr, sort_key)");
    stage3_db_.execute(
        "CREATE INDEX IF NOT EXISTS idx_s3_ckpt_user_sk "
        "ON s3_user_cond_checkpoint(user_addr, checkpoint_sort_key)");
    stage3_db_.execute(
        "CREATE INDEX IF NOT EXISTS idx_s3_summary_events "
        "ON s3_user_summary(total_events)");
    auto conn = stage3_db_.create_connection();
    auto r = conn->Query("SELECT COUNT(*) FROM s3_sync_cursor WHERE id=1");
    assert(r && !r->HasError());
    if (r->GetValue(0, 0).GetValue<int64_t>() == 0) {
      auto ins = conn->Query(
          "INSERT INTO s3_sync_cursor VALUES (1, -1, from_hex(''), " +
          std::to_string(kCursorSentinel) + ", " +
          std::to_string(kCursorSentinel) + ", " +
          std::to_string(kCursorSentinel) + ", 0)");
      assert(ins && !ins->HasError());
    }
  }

void StageSync::load_conditions() {
    conditions_.clear();

    auto conn = stage2_db_.create_connection();
    auto rc = conn->Query(
        "SELECT cond_idx, outcome_cnt, "
        "payout_0, payout_1, payout_2, payout_3, payout_4, payout_5, payout_6, payout_7, "
        "CASE WHEN question_id IS NULL THEN '' ELSE lower(hex(question_id)) END AS qid, source "
        "FROM rb_condition ORDER BY cond_idx");
    assert(rc && !rc->HasError());
    conditions_.resize(static_cast<size_t>(rc->RowCount()));
    for (idx_t i = 0; i < rc->RowCount(); ++i) {
      uint32_t idx = rc->GetValue(0, i).GetValue<uint32_t>();
      assert(idx == i);
      ConditionInfo info;
      info.outcome_count = rc->GetValue(1, i).GetValue<uint8_t>();
      info.payout_numerators.reserve(info.outcome_count);
      for (int j = 0; j < info.outcome_count; ++j) {
        auto v = rc->GetValue(2 + j, i);
        info.payout_numerators.push_back(v.IsNull() ? -1 : v.GetValue<int64_t>());
      }
      std::string qid = rc->GetValue(10, i).GetValueUnsafe<std::string>();
      if (!qid.empty()) {
        info.question_id = "0x" + qid;
      }
      info.source = static_cast<ConditionSource>(rc->GetValue(11, i).GetValue<int32_t>());
      conditions_[idx] = std::move(info);
    }
  }

void StageSync::load_cursor() {
    auto conn = stage3_db_.create_connection();
    auto r = conn->Query(
        "SELECT sort_key, lower(hex(user_addr)), cond_idx, event_type, token_idx, processed_events "
        "FROM s3_sync_cursor WHERE id=1");
    assert(r && !r->HasError() && r->RowCount() == 1);
    cursor_.sort_key = r->GetValue(0, 0).GetValue<int64_t>();
    cursor_.user_hex = r->GetValue(1, 0).GetValueUnsafe<std::string>();
    cursor_.cond_idx = r->GetValue(2, 0).GetValue<int32_t>();
    cursor_.event_type = r->GetValue(3, 0).GetValue<int32_t>();
    cursor_.token_idx = r->GetValue(4, 0).GetValue<int32_t>();
    cursor_.processed_events = r->GetValue(5, 0).GetValue<int64_t>();
    if (cursor_.user_hex.empty()) {
      cursor_.user_hex = "";
    }
  }

void StageSync::save_cursor_locked(duckdb::Connection &conn) const {
    std::string user_hex = cursor_.user_hex;
    auto q = conn.Query(
        "UPDATE s3_sync_cursor SET "
        "sort_key=" + std::to_string(cursor_.sort_key) +
        ", user_addr=from_hex('" + user_hex + "')" +
        ", cond_idx=" + std::to_string(cursor_.cond_idx) +
        ", event_type=" + std::to_string(cursor_.event_type) +
        ", token_idx=" + std::to_string(cursor_.token_idx) +
        ", processed_events=" + std::to_string(cursor_.processed_events) +
        " WHERE id=1");
    assert(q && !q->HasError());
  }

void StageSync::refresh_status_locked() const {
    sync_.head_block = builder_.cursor();
    sync_.last_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
    sync_.behind_blocks = std::max<int64_t>(0, sync_.head_block - sync_.last_block);
    sync_.behind_chunks = (sync_.behind_blocks + kStage3ChunkBlocks - 1) / kStage3ChunkBlocks;
  }

} // namespace stage3
