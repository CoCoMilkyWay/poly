#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <cmath>
#include <unordered_set>

namespace stage3 {

int64_t StageSync::round_i64(double v) { return static_cast<int64_t>(std::llround(v)); }

void StageSync::load_cond_state_values(CondState &st,
                                       duckdb::MaterializedQueryResult &src,
                                       idx_t row_idx,
                                       int pos_col_begin,
                                       int cost_col_begin,
                                       int lp_col_begin,
                                       int realized_col,
                                       int event_count_col,
                                       int last_sort_key_col) {
  for (int j = 0; j < MAX_OUTCOMES; ++j) {
    st.positions[j] = static_cast<double>(src.GetValue(pos_col_begin + j, row_idx).GetValue<int64_t>());
    st.cost[j] = static_cast<double>(src.GetValue(cost_col_begin + j, row_idx).GetValue<int64_t>());
    st.last_price[j] = static_cast<double>(src.GetValue(lp_col_begin + j, row_idx).GetValue<int64_t>());
  }
  st.realized_pnl = static_cast<double>(src.GetValue(realized_col, row_idx).GetValue<int64_t>());
  st.event_count = src.GetValue(event_count_col, row_idx).GetValue<int64_t>();
  st.last_sort_key = src.GetValue(last_sort_key_col, row_idx).GetValue<int64_t>();
}

void StageSync::append_cond_state_values(duckdb::Appender &ap, const CondState &st) {
  for (int j = 0; j < MAX_OUTCOMES; ++j) {
    ap.Append(round_i64(st.positions[j]));
  }
  for (int j = 0; j < MAX_OUTCOMES; ++j) {
    ap.Append(round_i64(st.cost[j]));
  }
  for (int j = 0; j < MAX_OUTCOMES; ++j) {
    ap.Append(round_i64(st.last_price[j]));
  }
  ap.Append(round_i64(st.realized_pnl));
  ap.Append(st.event_count);
  ap.Append(st.last_sort_key);
}

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &wp = builder_.progress();
  const auto &bp = builder_.committed_progress();
  Stage2Data p;
  p.phase = wp.phase;
  p.running = wp.running;
  p.total_users = bp.total_users;
  p.total_events = bp.total_events;
  p.cond_tree = bp.cond_tree;
  p.token_tree = bp.token_tree;
  p.xfer_total = bp.xfer_stats.total;
  p.xfer_split_normal = bp.xfer_stats.split_normal;
  p.xfer_split_negrisk = bp.xfer_stats.split_negrisk;
  p.xfer_split_non_poly = bp.xfer_stats.split_non_poly;
  p.xfer_merge_normal = bp.xfer_stats.merge_normal;
  p.xfer_merge_negrisk = bp.xfer_stats.merge_negrisk;
  p.xfer_merge_non_poly = bp.xfer_stats.merge_non_poly;
  p.xfer_redemption = bp.xfer_stats.redemption;
  p.xfer_redemption_non_poly = bp.xfer_stats.redemption_non_poly;
  p.xfer_convert = bp.xfer_stats.convert;
  p.xfer_order_buy = bp.xfer_stats.order_buy;
  p.xfer_order_sell = bp.xfer_stats.order_sell;
  p.xfer_fpmm_buy = bp.xfer_stats.fpmm_buy;
  p.xfer_fpmm_sell = bp.xfer_stats.fpmm_sell;
  p.xfer_lp_add = bp.xfer_stats.fpmm_lp_add;
  p.xfer_lp_remove = bp.xfer_stats.fpmm_lp_remove;
  p.xfer_lp_return = bp.xfer_stats.fpmm_lp_return;
  p.xfer_transfer_in_negrisk = bp.xfer_stats.transfer_in_negrisk;
  p.xfer_transfer_in_other = bp.xfer_stats.transfer_in_other;
  p.xfer_transfer_in_non_poly = bp.xfer_stats.transfer_in_non_poly;
  p.xfer_transfer_out_negrisk = bp.xfer_stats.transfer_out_negrisk;
  p.xfer_transfer_out_other = bp.xfer_stats.transfer_out_other;
  p.xfer_transfer_out_non_poly = bp.xfer_stats.transfer_out_non_poly;
  p.xfer_internal_mint_negrisk = bp.xfer_stats.internal_mint_negrisk;
  p.xfer_internal_mint_fpmm = bp.xfer_stats.internal_mint_fpmm;
  p.xfer_internal_burn_negrisk = bp.xfer_stats.internal_burn_negrisk;
  p.xfer_internal_burn_fpmm = bp.xfer_stats.internal_burn_fpmm;
  p.xfer_internal_burn_convert = bp.xfer_stats.internal_burn_convert;
  p.xfer_internal_transfer_zero = bp.xfer_stats.internal_transfer_zero;
  p.xfer_internal_transfer_order = bp.xfer_stats.internal_transfer_order;
  p.xfer_internal_transfer_negrisk = bp.xfer_stats.internal_transfer_negrisk;
  p.xfer_internal_transfer_fpmm = bp.xfer_stats.internal_transfer_fpmm;
  p.xfer_internal_transfer_other = bp.xfer_stats.internal_transfer_other;
  p.split_sem_tree = bp.split_sem_tree;
  p.merge_sem_tree = bp.merge_sem_tree;
  p.convert_sem_tree = bp.convert_sem_tree;
  p.order_sem_tree = bp.order_sem_tree;
  p.event_by_collateral = bp.event_by_collateral;
  const auto s = status();
  if (s.behind_blocks == 0 && p.total_users > 0) {
    p.phase = 7;
  }
  return p;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (ioc_ == nullptr || stop_requested_) {
    return;
  }
  timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer_->async_wait([this](const boost::system::error_code &ec) {
    if (ec || stop_requested_) {
      return;
    }
    do_sync_tick();
  });
}

void StageSync::do_sync_tick() {
  Trace;
  static constexpr size_t kCommitHistoryWindow = 20;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (commit_history_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = commit_history_.front();
    const auto &last = commit_history_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };

  int64_t before_block = 0;
  {
    // 只在更新状态时持锁,避免 API 读取 status 被整段同步阻塞。
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    before_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
  }
  int64_t tick_head_block = builder_.cursor();
  std::string sync_trace_name =
      "s3/sync " + std::to_string(before_block + 1) + "-" + std::to_string(tick_head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      commit_history_.push_back({std::chrono::steady_clock::now(), after_block});
      if (commit_history_.size() > kCommitHistoryWindow) {
        commit_history_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    next_delay = (sync_.behind_blocks > 0) ? 0 : base_interval_seconds_;
  }
  schedule_sync(next_delay);
}

bool StageSync::process_chunk_locked() const {
  TraceN("s3/sync_chunk");
  int64_t current_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  auto source_conn = stage2_db_.create_connection();
  auto sink_conn = stage3_db_.create_connection();
  std::string user_hex = cursor_.user_hex;
  auto build_after_key_filter = [](int64_t sort_key,
                                   const std::string &user_hex_key,
                                   int32_t cond_idx,
                                   int32_t event_type,
                                   int32_t token_idx) {
    return "(sort_key > " + std::to_string(sort_key) + ") OR "
                                                       "(sort_key = " +
           std::to_string(sort_key) + " AND user_addr > from_hex('" + user_hex_key + "')) OR "
                                                                                     "(sort_key = " +
           std::to_string(sort_key) + " AND user_addr = from_hex('" + user_hex_key + "') "
                                                                                     "AND cond_idx > " +
           std::to_string(cond_idx) + ") OR "
                                      "(sort_key = " +
           std::to_string(sort_key) + " AND user_addr = from_hex('" + user_hex_key + "') "
                                                                                     "AND cond_idx = " +
           std::to_string(cond_idx) + " AND event_type > " + std::to_string(event_type) + ") OR "
                                                                                          "(sort_key = " +
           std::to_string(sort_key) + " AND user_addr = from_hex('" + user_hex_key + "') "
                                                                                     "AND cond_idx = " +
           std::to_string(cond_idx) + " AND event_type = " + std::to_string(event_type) +
           " AND token_idx > " + std::to_string(token_idx) + ")";
  };
  const std::string event_query_prefix =
      "SELECT lower(hex(user_addr)) AS user_hex, sort_key, cond_idx, event_type, token_idx, collateral, amount, price "
      "FROM user_event WHERE (";
  const std::string event_query_order =
      " ORDER BY sort_key, user_addr, cond_idx, event_type, token_idx";
  std::string after_cursor_filter =
      build_after_key_filter(cursor_.sort_key, user_hex, cursor_.cond_idx, cursor_.event_type, cursor_.token_idx);
  auto qr = source_conn->Query(event_query_prefix + after_cursor_filter + ") "
                                                                          "AND sort_key <= " +
                               std::to_string(head_sort_key) + event_query_order + " LIMIT " +
                               std::to_string(kStage3BatchEvents));
  assert(qr && !qr->HasError());
  if (qr->RowCount() == 0) {
    cursor_.sort_key = head_sort_key;
    cursor_.user_hex.clear();
    cursor_.cond_idx = kCursorSentinel;
    cursor_.event_type = kCursorSentinel;
    cursor_.token_idx = kCursorSentinel;
    auto tx = sink_conn->Query("BEGIN");
    assert(tx && !tx->HasError());
    save_cursor_locked(*sink_conn);
    auto cm = sink_conn->Query("COMMIT");
    assert(cm && !cm->HasError());
    return true;
  }

  std::vector<InputEvent> rows;
  rows.reserve(static_cast<size_t>(qr->RowCount()));
  std::unordered_set<std::string> touched_users;
  touched_users.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));

  std::unordered_map<PairKey, CondState, PairKeyHash> states;
  states.reserve(static_cast<size_t>(qr->RowCount() / 2 + 1));
  std::unordered_map<std::string, int64_t> user_event_inc;
  user_event_inc.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));
  std::unordered_map<std::string, int64_t> user_last_sk;
  user_last_sk.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_hex;
  int32_t prev_cond_idx = kCursorSentinel;
  int32_t prev_event_type = kCursorSentinel;
  int32_t prev_token_idx = kCursorSentinel;
  auto consume_rows = [&](duckdb::MaterializedQueryResult &result) {
    for (idx_t i = 0; i < result.RowCount(); ++i) {
      InputEvent row;
      row.user_hex = result.GetValue(0, i).GetValueUnsafe<std::string>();
      row.sort_key = result.GetValue(1, i).GetValue<int64_t>();
      row.cond_idx = result.GetValue(2, i).GetValue<int32_t>();
      row.event_type = result.GetValue(3, i).GetValue<int32_t>();
      row.token_idx = result.GetValue(4, i).GetValue<int32_t>();
      row.collateral = result.GetValue(5, i).GetValue<int32_t>();
      row.amount = result.GetValue(6, i).GetValue<int64_t>();
      row.price = result.GetValue(7, i).GetValue<int64_t>();
      if (has_prev_key) {
        assert(
            row.sort_key > prev_sort_key ||
            (row.sort_key == prev_sort_key &&
             (row.user_hex > prev_user_hex ||
              (row.user_hex == prev_user_hex &&
               (row.cond_idx > prev_cond_idx ||
                (row.cond_idx == prev_cond_idx &&
                 (row.event_type > prev_event_type ||
                  (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
      }
      has_prev_key = true;
      prev_sort_key = row.sort_key;
      prev_user_hex = row.user_hex;
      prev_cond_idx = row.cond_idx;
      prev_event_type = row.event_type;
      prev_token_idx = row.token_idx;
      rows.push_back(row);
      touched_users.insert(row.user_hex);
      user_event_inc[row.user_hex]++;
      user_last_sk[row.user_hex] = row.sort_key;
      if (row.cond_idx >= 0) {
        PairKey key{row.user_hex, row.cond_idx};
        if (states.find(key) == states.end()) {
          states.emplace(key, CondState{});
        }
      }
      cursor_.sort_key = row.sort_key;
      cursor_.user_hex = row.user_hex;
      cursor_.cond_idx = row.cond_idx;
      cursor_.event_type = row.event_type;
      cursor_.token_idx = row.token_idx;
      cursor_.processed_events++;
    }
  };
  consume_rows(*qr);
  if (qr->RowCount() == static_cast<idx_t>(kStage3BatchEvents)) {
    assert(!rows.empty());
    const InputEvent &last_row = rows.back();
    int64_t tail_upper_sort_key = (last_row.sort_key / SORT_KEY_SCALE) * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);
    std::string after_last_filter = build_after_key_filter(last_row.sort_key,
                                                           last_row.user_hex,
                                                           last_row.cond_idx,
                                                           last_row.event_type,
                                                           last_row.token_idx);
    auto tail_qr = source_conn->Query(event_query_prefix + after_last_filter + ") "
                                                                               "AND sort_key <= " +
                                      std::to_string(tail_upper_sort_key) + event_query_order);
    assert(tail_qr && !tail_qr->HasError());
    consume_rows(*tail_qr);
  }

  int32_t max_cond_idx = -1;
  for (const auto &row : rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }

  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<std::string, double> user_realized_cum;
  std::unordered_map<std::string, double> user_unrealized_cum;
  std::unordered_map<std::string, int64_t> user_active_cond_count;
  std::unordered_map<std::string, int32_t> user_token_count;
  user_realized_cum.reserve(touched_users.size() + 1);
  user_unrealized_cum.reserve(touched_users.size() + 1);
  user_active_cond_count.reserve(touched_users.size() + 1);
  user_token_count.reserve(touched_users.size() + 1);
  if (!touched_users.empty()) {
    sink_conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_s3_touched_users (user_addr BLOB)");
    sink_conn->Query("DELETE FROM tmp_s3_touched_users");
    {
      duckdb::Appender ap(*sink_conn, "tmp_s3_touched_users");
      for (const auto &uhex : touched_users) {
        std::string user_blob = hex_to_blob("0x" + uhex);
        ap.BeginRow();
        ap.Append(duckdb::Value::BLOB(
            reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
            user_blob.size()));
        ap.EndRow();
      }
      ap.Close();
    }
    auto sum_r = sink_conn->Query(
        "SELECT lower(hex(s.user_addr)) AS uh, s.total_realized_pnl, s.total_unrealized_pnl, s.active_conditions "
        "FROM s3_user_summary s "
        "JOIN tmp_s3_touched_users t ON s.user_addr = t.user_addr");
    assert(sum_r && !sum_r->HasError());
    for (idx_t i = 0; i < sum_r->RowCount(); ++i) {
      const std::string uh = sum_r->GetValue(0, i).GetValueUnsafe<std::string>();
      // Convert int64 from database to double
      user_realized_cum.emplace(uh, static_cast<double>(sum_r->GetValue(1, i).GetValue<int64_t>()));
      user_unrealized_cum.emplace(uh, static_cast<double>(sum_r->GetValue(2, i).GetValue<int64_t>()));
      user_active_cond_count.emplace(uh, sum_r->GetValue(3, i).GetValue<int64_t>());
    }
    for (const auto &uhex : touched_users) {
      if (!user_realized_cum.count(uhex)) {
        user_realized_cum.emplace(uhex, 0.0);
      }
      if (!user_unrealized_cum.count(uhex)) {
        user_unrealized_cum.emplace(uhex, 0.0);
      }
      if (!user_active_cond_count.count(uhex)) {
        user_active_cond_count.emplace(uhex, 0);
      }
    }

    auto tk_r = sink_conn->Query(
        "SELECT lower(hex(st.user_addr)) AS uh, "
        "SUM(CASE WHEN abs(st.pos_0) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END + "
                                         "    CASE WHEN abs(st.pos_1) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END + "
                                         "    CASE WHEN abs(st.pos_2) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END + "
                                         "    CASE WHEN abs(st.pos_3) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END + "
                                         "    CASE WHEN abs(st.pos_4) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END + "
                                         "    CASE WHEN abs(st.pos_5) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END + "
                                         "    CASE WHEN abs(st.pos_6) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END + "
                                         "    CASE WHEN abs(st.pos_7) >= " +
        std::to_string(kMinHoldingQty) + " THEN 1 ELSE 0 END) AS tk "
                                         "FROM s3_user_cond_state st "
                                         "JOIN tmp_s3_touched_users t ON st.user_addr = t.user_addr "
                                         "GROUP BY st.user_addr");
    assert(tk_r && !tk_r->HasError());
    for (idx_t i = 0; i < tk_r->RowCount(); ++i) {
      user_token_count.emplace(tk_r->GetValue(0, i).GetValueUnsafe<std::string>(),
                               tk_r->GetValue(1, i).GetValue<int32_t>());
    }
    for (const auto &uhex : touched_users) {
      if (!user_token_count.count(uhex)) {
        user_token_count.emplace(uhex, 0);
      }
    }
  }

  if (!states.empty()) {
    sink_conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_s3_keys (user_addr BLOB, cond_idx INTEGER)");
    sink_conn->Query("DELETE FROM tmp_s3_keys");
    {
      duckdb::Appender ap(*sink_conn, "tmp_s3_keys");
      for (const auto &[key, _] : states) {
        std::string user_blob = hex_to_blob("0x" + key.user_hex);
        ap.BeginRow();
        ap.Append(duckdb::Value::BLOB(
            reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
            user_blob.size()));
        ap.Append(key.cond_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    auto old = sink_conn->Query(
        "SELECT lower(hex(s.user_addr)) AS uh, s.cond_idx, "
        "s.pos_0,s.pos_1,s.pos_2,s.pos_3,s.pos_4,s.pos_5,s.pos_6,s.pos_7, "
        "s.cost_0,s.cost_1,s.cost_2,s.cost_3,s.cost_4,s.cost_5,s.cost_6,s.cost_7, "
        "s.lp_0,s.lp_1,s.lp_2,s.lp_3,s.lp_4,s.lp_5,s.lp_6,s.lp_7, "
        "s.realized_pnl, s.event_count, s.last_sort_key "
        "FROM s3_user_cond_state s "
        "JOIN tmp_s3_keys k "
        "ON s.user_addr = k.user_addr AND s.cond_idx = k.cond_idx");
    assert(old && !old->HasError());
    for (idx_t i = 0; i < old->RowCount(); ++i) {
      PairKey key{old->GetValue(0, i).GetValueUnsafe<std::string>(),
                  old->GetValue(1, i).GetValue<int32_t>()};
      auto it = states.find(key);
      assert(it != states.end());
      CondState &st = it->second;
      load_cond_state_values(st, *old, i, 2, 10, 18, 26, 27, 28);
      st.unrealized_pnl = compute_unrealized_pnl(st);
    }
  }

  std::vector<FactRow> fact_rows;
  fact_rows.reserve(rows.size());
  for (const auto &row : rows) {
    double realized_delta = 0.0;
    if (row.cond_idx >= 0) {
      PairKey key{row.user_hex, row.cond_idx};
      auto it = states.find(key);
      assert(it != states.end());
      double before_cond_unrealized = it->second.unrealized_pnl;
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      int before_active = has_any_position(it->second, cond.outcome_count) ? 1 : 0;
      int before_nonzero = count_effective_holdings(it->second, cond.outcome_count);
      realized_delta = apply_event_to_state(row, it->second);
      it->second.unrealized_pnl = compute_unrealized_pnl(it->second);
      user_unrealized_cum[row.user_hex] += (it->second.unrealized_pnl - before_cond_unrealized);
      int after_active = has_any_position(it->second, cond.outcome_count) ? 1 : 0;
      user_active_cond_count[row.user_hex] += (after_active - before_active);
      int after_nonzero = count_effective_holdings(it->second, cond.outcome_count);
      user_token_count[row.user_hex] += (after_nonzero - before_nonzero);
      it->second.event_count++;
      it->second.last_sort_key = row.sort_key;
    }
    double &realized_cum = user_realized_cum[row.user_hex];
    double &unrealized_cum = user_unrealized_cum[row.user_hex];
    realized_cum += realized_delta;
    int32_t token_count = user_token_count[row.user_hex];
    // Convert double to int64 for storage (round to nearest)
    fact_rows.push_back({
        row.user_hex,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        round_i64(realized_delta),
        round_i64(realized_cum),
        round_i64(unrealized_cum),
        token_count,
    });
  }
  assert(fact_rows.size() == rows.size());
  // Validate state after processing (allow small negative due to floating point)
  for (const auto &[key, st] : states) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    const auto &cond = conditions_[static_cast<size_t>(key.cond_idx)];
    for (int j = 0; j < cond.outcome_count; ++j) {
      assert(std::isfinite(st.positions[j]));
      assert(std::isfinite(st.cost[j]));
    }
  }

  {
    TraceN("s3/write");
    auto tx = sink_conn->Query("BEGIN");
    assert(tx && !tx->HasError());

    if (!states.empty()) {
      sink_conn->Query(
          "CREATE TEMP TABLE IF NOT EXISTS tmp_s3_state ("
          "user_addr BLOB, cond_idx INTEGER, "
          "pos_0 BIGINT, pos_1 BIGINT, pos_2 BIGINT, pos_3 BIGINT, pos_4 BIGINT, pos_5 BIGINT, pos_6 BIGINT, pos_7 BIGINT, "
          "cost_0 BIGINT, cost_1 BIGINT, cost_2 BIGINT, cost_3 BIGINT, cost_4 BIGINT, cost_5 BIGINT, cost_6 BIGINT, cost_7 BIGINT, "
          "lp_0 BIGINT, lp_1 BIGINT, lp_2 BIGINT, lp_3 BIGINT, lp_4 BIGINT, lp_5 BIGINT, lp_6 BIGINT, lp_7 BIGINT, "
          "realized_pnl BIGINT, event_count BIGINT, last_sort_key BIGINT)");
      sink_conn->Query("DELETE FROM tmp_s3_state");
      {
        duckdb::Appender ap(*sink_conn, "tmp_s3_state");
        for (const auto &[key, st] : states) {
          std::string user_blob = hex_to_blob("0x" + key.user_hex);
          ap.BeginRow();
          ap.Append(duckdb::Value::BLOB(
              reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
              user_blob.size()));
          ap.Append(key.cond_idx);
          append_cond_state_values(ap, st);
          ap.EndRow();
        }
        ap.Close();
      }
      auto up = sink_conn->Query(
          "INSERT INTO s3_user_cond_state ("
          "user_addr, cond_idx, "
          "pos_0,pos_1,pos_2,pos_3,pos_4,pos_5,pos_6,pos_7, "
          "cost_0,cost_1,cost_2,cost_3,cost_4,cost_5,cost_6,cost_7, "
          "lp_0,lp_1,lp_2,lp_3,lp_4,lp_5,lp_6,lp_7, "
          "realized_pnl,event_count,last_sort_key"
          ") "
          "SELECT "
          "user_addr, cond_idx, "
          "pos_0,pos_1,pos_2,pos_3,pos_4,pos_5,pos_6,pos_7, "
          "cost_0,cost_1,cost_2,cost_3,cost_4,cost_5,cost_6,cost_7, "
          "lp_0,lp_1,lp_2,lp_3,lp_4,lp_5,lp_6,lp_7, "
          "realized_pnl,event_count,last_sort_key "
          "FROM tmp_s3_state "
          "ON CONFLICT(user_addr, cond_idx) DO UPDATE SET "
          "pos_0=excluded.pos_0, pos_1=excluded.pos_1, pos_2=excluded.pos_2, pos_3=excluded.pos_3, "
          "pos_4=excluded.pos_4, pos_5=excluded.pos_5, pos_6=excluded.pos_6, pos_7=excluded.pos_7, "
          "cost_0=excluded.cost_0, cost_1=excluded.cost_1, cost_2=excluded.cost_2, cost_3=excluded.cost_3, "
          "cost_4=excluded.cost_4, cost_5=excluded.cost_5, cost_6=excluded.cost_6, cost_7=excluded.cost_7, "
          "lp_0=excluded.lp_0, lp_1=excluded.lp_1, lp_2=excluded.lp_2, lp_3=excluded.lp_3, "
          "lp_4=excluded.lp_4, lp_5=excluded.lp_5, lp_6=excluded.lp_6, lp_7=excluded.lp_7, "
          "realized_pnl=excluded.realized_pnl, event_count=excluded.event_count, last_sort_key=excluded.last_sort_key");
      assert(up && !up->HasError());
    }

    if (!fact_rows.empty()) {
      duckdb::Appender ap(*sink_conn, "s3_user_event_fact");
      for (const auto &fr : fact_rows) {
        std::string user_blob = hex_to_blob("0x" + fr.user_hex);
        ap.BeginRow();
        ap.Append(duckdb::Value::BLOB(
            reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
            user_blob.size()));
        ap.Append(fr.sort_key);
        ap.Append(fr.cond_idx);
        ap.Append(fr.token_idx);
        ap.Append(fr.event_type);
        ap.Append(fr.realized_delta);
        ap.Append(fr.realized_cum);
        ap.Append(fr.unrealized_pnl);
        ap.Append(fr.token_count);
        ap.EndRow();
      }
      ap.Close();
    }

    sink_conn->Query(
        "CREATE TEMP TABLE IF NOT EXISTS tmp_s3_users ("
        "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, "
        "rpnl BIGINT, upnl BIGINT, active_cnt BIGINT)");
    sink_conn->Query("DELETE FROM tmp_s3_users");
    {
      duckdb::Appender ap(*sink_conn, "tmp_s3_users");
      for (const auto &[uhex, inc] : user_event_inc) {
        std::string user_blob = hex_to_blob("0x" + uhex);
        ap.BeginRow();
        ap.Append(duckdb::Value::BLOB(
            reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
            user_blob.size()));
        ap.Append(inc);
        ap.Append(user_last_sk[uhex]);
        ap.Append(round_i64(user_realized_cum[uhex]));
        ap.Append(round_i64(user_unrealized_cum[uhex]));
        assert(user_active_cond_count[uhex] >= 0);
        ap.Append(user_active_cond_count[uhex]);
        ap.EndRow();
      }
      ap.Close();
    }
    auto su = sink_conn->Query(
        "INSERT INTO s3_user_summary ("
        "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_conditions, last_sort_key"
        ") "
        "SELECT user_addr, event_inc, rpnl, upnl, active_cnt, last_sort_key FROM tmp_s3_users "
        "ON CONFLICT(user_addr) DO UPDATE SET "
        "total_events=s3_user_summary.total_events + excluded.total_events, "
        "total_realized_pnl=excluded.total_realized_pnl, "
        "total_unrealized_pnl=excluded.total_unrealized_pnl, "
        "active_conditions=excluded.active_conditions, "
        "last_sort_key=GREATEST(s3_user_summary.last_sort_key, excluded.last_sort_key)");
    assert(su && !su->HasError());

    save_cursor_locked(*sink_conn);
    auto cm = sink_conn->Query("COMMIT");
    assert(cm && !cm->HasError());
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_cache_mu_);
    user_cache_ = UserQueryCache{};
  }

  return true;
}

} // namespace stage3
