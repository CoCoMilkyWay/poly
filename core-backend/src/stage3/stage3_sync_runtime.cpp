#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTokenKeys = "tmp_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w BIGINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w BIGINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w BIGINT, "
    "realized_sum_10w BIGINT, realized_sq_sum_10w BIGINT, realized_count_10w BIGINT, realized_kll_10w BLOB, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "realized_sum_10w, realized_sq_sum_10w, realized_count_10w, realized_kll_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, updated_sort_key";
constexpr const char *kSqlColsUserSummaryState =
    "user_addr, total_events, total_realized_pnl, total_unrealized_pnl, active_tokens, last_sort_key";
constexpr const char *kSqlSetFeatureTensorStateUpsert =
    "last_sort_key_10w=excluded.last_sort_key_10w, "
    "last_block_10w=excluded.last_block_10w, "
    "last_exposure_10w=excluded.last_exposure_10w, "
    "last_holding_period_10w=excluded.last_holding_period_10w, "
    "last_token_count_10w=excluded.last_token_count_10w, "
    "time_weight_sum_10w=excluded.time_weight_sum_10w, "
    "token_count_tw_sum_10w=excluded.token_count_tw_sum_10w, "
    "exposure_tw_sum_10w=excluded.exposure_tw_sum_10w, "
    "volume_sum_10w=excluded.volume_sum_10w, "
    "holding_period_exp_tw_sum_10w=excluded.holding_period_exp_tw_sum_10w, "
    "realized_sum_10w=excluded.realized_sum_10w, "
    "realized_sq_sum_10w=excluded.realized_sq_sum_10w, "
    "realized_count_10w=excluded.realized_count_10w, "
    "realized_kll_10w=excluded.realized_kll_10w, "
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlOrderByEventSourceKey = " ORDER BY sort_key, user_addr, cond_idx, event_type, token_idx";
constexpr const char *kSqlSelectEventInputCols =
    "SELECT user_addr, sort_key, cond_idx, event_type, token_idx, collateral, amount, price ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w, f.realized_sum_10w, f.realized_sq_sum_10w, f.realized_count_10w, "
    "f.realized_kll_10w ";
} // namespace

int64_t StageSync::round_i64(double v) { return static_cast<int64_t>(std::llround(v)); }

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

int64_t StageSync::get_max_bucket() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  if (sync_cursor_.sort_key < 0) {
    return -1;
  }
  return sort_key_to_block_bucket(sync_cursor_.sort_key);
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
    before_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
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
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
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
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  auto source_conn = stage2_db_.create_connection();
  auto sink_conn = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_event_fact = kSqlTableEventFact;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto checked_query = [&](const std::string &sql) {
    auto q = sink_conn->Query(sql);
    assert(q && !q->HasError());
    return q;
  };
  auto prepare_tmp_table = [&](const char *tmp_table, const char *schema_cols) {
    (void)checked_query("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)checked_query("DELETE FROM " + std::string(tmp_table));
  };
  auto append_blob = [](duckdb::Appender &ap, const std::string &blob) {
    ap.Append(duckdb::Value::BLOB(
        reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
        blob.size()));
  };
  const std::string event_query_order_sql = kSqlOrderByEventSourceKey;
  auto qr = source_conn->Query(
      std::string(kSqlSelectEventInputCols) + "FROM user_event WHERE sort_key > " +
      std::to_string(sync_cursor_.sort_key) + " AND sort_key <= " +
      std::to_string(head_sort_key) + event_query_order_sql + " LIMIT " +
      std::to_string(kStage3BatchEvents));
  assert(qr && !qr->HasError());
  if (qr->RowCount() == 0) {
    sync_cursor_.sort_key = head_sort_key;
    (void)checked_query("BEGIN");
    save_cursor_locked(*sink_conn);
    (void)checked_query("COMMIT");
    return true;
  }

  std::vector<EventInput> event_inputs;
  event_inputs.reserve(static_cast<size_t>(qr->RowCount()));
  std::vector<std::string> user_blob_pool;
  user_blob_pool.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));
  std::unordered_map<std::string, uint32_t> user_blob_to_id;
  user_blob_to_id.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));
  auto intern_user_id = [&](const std::string &user_blob) {
    auto it = user_blob_to_id.find(user_blob);
    if (it != user_blob_to_id.end()) {
      return it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_pool.size());
    user_blob_pool.push_back(user_blob);
    user_blob_to_id.emplace(user_blob_pool.back(), user_id);
    return user_id;
  };
  auto compare_blob_unsigned = [](const std::string &lhs, const std::string &rhs) {
    const size_t n = std::min(lhs.size(), rhs.size());
    for (size_t i = 0; i < n; ++i) {
      const auto l = static_cast<uint8_t>(lhs[i]);
      const auto r = static_cast<uint8_t>(rhs[i]);
      if (l != r) {
        return (l < r) ? -1 : 1;
      }
    }
    if (lhs.size() == rhs.size()) {
      return 0;
    }
    return (lhs.size() < rhs.size()) ? -1 : 1;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  auto consume_event_inputs = [&](duckdb::MaterializedQueryResult &result) {
    for (idx_t i = 0; i < result.RowCount(); ++i) {
      const std::string user_blob = result.GetValue(0, i).GetValueUnsafe<std::string>();
      EventInput row;
      row.user_id = intern_user_id(user_blob);
      row.sort_key = result.GetValue(1, i).GetValue<int64_t>();
      row.cond_idx = result.GetValue(2, i).GetValue<int32_t>();
      row.event_type = result.GetValue(3, i).GetValue<int32_t>();
      row.token_idx = result.GetValue(4, i).GetValue<int32_t>();
      row.collateral = result.GetValue(5, i).GetValue<int32_t>();
      row.amount = result.GetValue(6, i).GetValue<int64_t>();
      row.price = result.GetValue(7, i).GetValue<int64_t>();
      if (has_prev_key) {
        const int user_cmp = compare_blob_unsigned(user_blob, prev_user_blob);
        assert(
            row.sort_key > prev_sort_key ||
            (row.sort_key == prev_sort_key &&
             (user_cmp > 0 ||
              (user_cmp == 0 &&
               (row.cond_idx > prev_cond_idx ||
                (row.cond_idx == prev_cond_idx &&
                 (row.event_type > prev_event_type ||
                  (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))))));
      }
      has_prev_key = true;
      prev_sort_key = row.sort_key;
      prev_user_blob = user_blob;
      prev_cond_idx = row.cond_idx;
      prev_event_type = row.event_type;
      prev_token_idx = row.token_idx;
      event_inputs.push_back(row);
    }
  };
  consume_event_inputs(*qr);

  if (qr->RowCount() == static_cast<idx_t>(kStage3BatchEvents)) {
    assert(!event_inputs.empty());
    const EventInput &last_row = event_inputs.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_inputs.size();
    while (cut > 0 && sort_key_to_block(event_inputs[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_inputs.resize(cut);
  }
  assert(!event_inputs.empty());

  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_states;
  token_states.reserve(static_cast<size_t>(event_inputs.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> user_event_increments;
  user_event_increments.reserve(static_cast<size_t>(event_inputs.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> user_last_sort_keys;
  user_last_sort_keys.reserve(static_cast<size_t>(event_inputs.size() / 10 + 1));
  for (const auto &row : event_inputs) {
    user_event_increments[row.user_id]++;
    user_last_sort_keys[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_states.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_inputs.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_inputs.size());

  int32_t max_cond_idx = -1;
  for (const auto &row : event_inputs) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }

  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> user_realized_totals;
  std::unordered_map<uint32_t, double> user_unrealized_totals;
  std::unordered_map<uint32_t, int32_t> user_active_token_counts;
  user_realized_totals.reserve(user_event_increments.size() + 1);
  user_unrealized_totals.reserve(user_event_increments.size() + 1);
  user_active_token_counts.reserve(user_event_increments.size() + 1);
  if (!user_event_increments.empty()) {
    prepare_tmp_table(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_conn, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : user_event_increments) {
        const std::string &user_blob = user_blob_pool[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = sink_conn->Query(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    assert(summary_seed_result && !summary_seed_result->HasError());
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto uid_it = user_blob_to_id.find(user_blob);
      assert(uid_it != user_blob_to_id.end());
      const uint32_t user_id = uid_it->second;
      user_realized_totals.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      user_unrealized_totals.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      user_active_token_counts.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : user_event_increments) {
      if (!user_realized_totals.count(user_id)) {
        user_realized_totals.emplace(user_id, 0.0);
      }
      if (!user_unrealized_totals.count(user_id)) {
        user_unrealized_totals.emplace(user_id, 0.0);
      }
      if (!user_active_token_counts.count(user_id)) {
        user_active_token_counts.emplace(user_id, 0);
      }
    }
  }

  if (!token_states.empty()) {
    prepare_tmp_table(kSqlTmpTokenKeys, kSqlTmpSchemaTokenKeys);
    {
      duckdb::Appender ap(*sink_conn, kSqlTmpTokenKeys);
      for (const auto &[key, _] : token_states) {
        const std::string &user_blob = user_blob_pool[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_token_state_with_token_keys =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTokenKeys) +
        " k ON s.user_addr = k.user_addr AND s.cond_idx = k.cond_idx AND s.token_idx = k.token_idx";
    auto token_state_result = sink_conn->Query(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_token_keys);
    assert(token_state_result && !token_state_result->HasError());
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto uid_it = user_blob_to_id.find(user_blob);
      assert(uid_it != user_blob_to_id.end());
      TokenKey key{uid_it->second,
                   token_state_result->GetValue(1, i).GetValue<int32_t>(),
                   token_state_result->GetValue(2, i).GetValue<int32_t>()};
      auto it = token_states.find(key);
      assert(it != token_states.end());
      TokenState &st = it->second;
      st.pos = static_cast<double>(token_state_result->GetValue(3, i).GetValue<int64_t>());
      st.cost = static_cast<double>(token_state_result->GetValue(4, i).GetValue<int64_t>());
      st.lp = static_cast<double>(token_state_result->GetValue(5, i).GetValue<int64_t>());
      st.entry_block = static_cast<double>(token_state_result->GetValue(6, i).GetValue<int64_t>());
    }
  }

  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_states;
  bucket_agg_states.reserve(static_cast<size_t>(event_inputs.size() / 4 + 1));
  for (const auto &row : event_inputs) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    AggKey key{row.user_id, sort_key_to_block_bucket(row.sort_key), tag_id};
    bucket_agg_states.try_emplace(key, BucketAggState{});
  }

  if (!bucket_agg_states.empty()) {
    prepare_tmp_table(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_conn, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_states) {
        const std::string &user_blob = user_blob_pool[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_feature_tensor_with_keys =
        "FROM " + table_feature_tensor + " f " +
        "JOIN " + std::string(kSqlTmpFeatureTensorKeys) +
        " k ON f.user_addr = k.user_addr AND f.block_bucket = k.block_bucket AND f.tag_id = k.tag_id";
    auto feature_state_result = sink_conn->Query(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    assert(feature_state_result && !feature_state_result->HasError());
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto uid_it = user_blob_to_id.find(user_blob);
      assert(uid_it != user_blob_to_id.end());
      AggKey key{
          uid_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto it = bucket_agg_states.find(key);
      assert(it != bucket_agg_states.end());
      BucketAggState &agg = it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_period = feature_state_result->GetValue(6, i).GetValue<int64_t>();
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = feature_state_result->GetValue(10, i).GetValue<int64_t>();
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_tw_sum = feature_state_result->GetValue(12, i).GetValue<int64_t>();
      agg.realized_sum = feature_state_result->GetValue(13, i).GetValue<int64_t>();
      agg.realized_sq_sum = feature_state_result->GetValue(14, i).GetValue<int64_t>();
      agg.event_count = feature_state_result->GetValue(15, i).GetValue<int64_t>();
      if (!feature_state_result->GetValue(16, i).IsNull()) {
        const std::string blob = feature_state_result->GetValue(16, i).GetValueUnsafe<std::string>();
        if (!blob.empty()) {
          agg.realized_kll = KLLcache::deserialize(reinterpret_cast<const uint8_t *>(blob.data()), blob.size());
        }
      }
      agg.has_tail = (agg.event_count > 0);
    }
  }

  std::vector<EventFact> event_facts;
  event_facts.reserve(event_inputs.size());
  for (const auto &row : event_inputs) {
    double realized_delta = 0.0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t holding_period = 0;
    int64_t volume = calc_volume_1e6(row);
    if (row.cond_idx >= 0) {
      assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
      tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      auto it = token_states.find(key);
      assert(it != token_states.end());
      TokenState &st = it->second;
      const double before_unrealized = calc_unrealized_pnl(st);
      const int before_holding = is_effective_holding(st.pos) ? 1 : 0;
      realized_delta = apply_event_input(row, st);
      const double after_unrealized = calc_unrealized_pnl(st);
      const int after_holding = is_effective_holding(st.pos) ? 1 : 0;
      user_unrealized_totals[row.user_id] += (after_unrealized - before_unrealized);
      user_active_token_counts[row.user_id] += (after_holding - before_holding);
      assert(user_active_token_counts[row.user_id] >= 0);

      const int64_t row_block = sort_key_to_block(row.sort_key);
      exposure = calc_exposure_1e6(st);
      holding_period = calc_holding_period_blocks(row_block, st);

      AggKey agg_key{row.user_id, sort_key_to_block_bucket(row.sort_key), tag_id};
      auto agg_it = bucket_agg_states.find(agg_key);
      assert(agg_it != bucket_agg_states.end());
      BucketAggState &agg = agg_it->second;
      update_tail_window(agg, agg_key.block_bucket, row_block, exposure, holding_period, user_active_token_counts[row.user_id]);
      feature_comp::accumulate_event_delta(agg, realized_delta, volume, row.sort_key);
    }
    double &realized_total = user_realized_totals[row.user_id];
    double &unrealized_total = user_unrealized_totals[row.user_id];
    realized_total += realized_delta;
    int32_t token_count = user_active_token_counts[row.user_id];
    event_facts.push_back({
        row.user_id,
        row.sort_key,
        row.cond_idx,
        row.token_idx,
        row.event_type,
        round_i64(realized_delta),
        round_i64(realized_total),
        round_i64(unrealized_total),
        token_count,
        tag_id,
        exposure,
        volume,
        holding_period,
    });
  }
  assert(event_facts.size() == event_inputs.size());

  {
    const auto no_extra = [](const auto &) { return int64_t{0}; };
    const int64_t event_inputs_bytes = core::mem::estimate_vector_plain(event_inputs);
    const int64_t user_blob_pool_bytes =
        core::mem::estimate_vector(user_blob_pool, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
    const int64_t user_index_bytes =
        core::mem::estimate_unordered_map(user_blob_to_id, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
        core::mem::estimate_unordered_map(user_event_increments, no_extra, no_extra) +
        core::mem::estimate_unordered_map(user_last_sort_keys, no_extra, no_extra) +
        core::mem::estimate_unordered_map(user_realized_totals, no_extra, no_extra) +
        core::mem::estimate_unordered_map(user_unrealized_totals, no_extra, no_extra) +
        core::mem::estimate_unordered_map(user_active_token_counts, no_extra, no_extra);
    const int64_t token_states_bytes = core::mem::estimate_unordered_map(token_states, no_extra, no_extra);
    const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_states, no_extra, no_extra);
    const int64_t event_facts_bytes = core::mem::estimate_vector_plain(event_facts);
    const int64_t total_working_set_bytes =
        event_inputs_bytes + user_blob_pool_bytes + user_index_bytes + token_states_bytes + bucket_agg_bytes + event_facts_bytes;
    {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_mem_probe_.event_inputs_bytes = event_inputs_bytes;
      runtime_mem_probe_.user_blob_pool_bytes = user_blob_pool_bytes;
      runtime_mem_probe_.user_index_bytes = user_index_bytes;
      runtime_mem_probe_.token_states_bytes = token_states_bytes;
      runtime_mem_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_mem_probe_.event_facts_bytes = event_facts_bytes;
      runtime_mem_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_mem_probe_.peak_working_set_bytes =
          std::max(runtime_mem_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_mem_probe_.row_count = static_cast<int64_t>(event_inputs.size());
      runtime_mem_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_states) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  {
    TraceN("s3/write");
    (void)checked_query("BEGIN");

    if (!token_states.empty()) {
      prepare_tmp_table(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
      {
        duckdb::Appender ap(*sink_conn, kSqlTmpTokenDirty);
        for (const auto &[key, _] : token_states) {
          const std::string &user_blob = user_blob_pool[key.user_id];
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.Append(key.cond_idx);
          ap.Append(key.token_idx);
          ap.EndRow();
        }
        ap.Close();
      }

      prepare_tmp_table(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
      {
        duckdb::Appender ap(*sink_conn, kSqlTmpTokenNew);
        for (const auto &[key, st] : token_states) {
          if (std::abs(st.pos) <= kPosEpsilon) {
            continue;
          }
          const std::string &user_blob = user_blob_pool[key.user_id];
          ap.BeginRow();
          ap.Append(duckdb::Value::BLOB(
              reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
              user_blob.size()));
          ap.Append(key.cond_idx);
          ap.Append(key.token_idx);
          ap.Append(round_i64(st.pos));
          ap.Append(round_i64(st.cost));
          ap.Append(round_i64(st.lp));
          ap.Append(round_i64(st.entry_block));
          ap.EndRow();
        }
        ap.Close();
      }
      (void)checked_query(
          "DELETE FROM " + table_token_state + " s "
                                               "WHERE EXISTS ("
                                               "  SELECT 1 FROM " +
          std::string(kSqlTmpTokenDirty) + " d "
                                           "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                           ")");
      (void)checked_query(
          "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                           "SELECT " +
          kSqlColsTokenState + " FROM " +
          std::string(kSqlTmpTokenNew));
    }

    if (!event_facts.empty()) {
      duckdb::Appender ap(*sink_conn, kSqlTableEventFact);
      for (const auto &fr : event_facts) {
        const std::string &user_blob = user_blob_pool[fr.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(fr.sort_key);
        ap.Append(fr.cond_idx);
        ap.Append(fr.token_idx);
        ap.Append(fr.event_type);
        ap.Append(fr.realized_delta);
        ap.Append(fr.realized_cum);
        ap.Append(fr.unrealized_pnl);
        ap.Append(fr.token_count);
        ap.Append(static_cast<int32_t>(fr.tag_id));
        ap.Append(fr.exposure);
        ap.Append(fr.volume);
        ap.Append(fr.holding_period);
        ap.EndRow();
      }
      ap.Close();
    }

    if (!bucket_agg_states.empty()) {
      prepare_tmp_table(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
      {
        duckdb::Appender ap(*sink_conn, kSqlTmpFeatureTensorState);
        for (const auto &[key, agg] : bucket_agg_states) {
          const std::string &user_blob = user_blob_pool[key.user_id];
          const std::vector<uint8_t> realized_kll_blob = agg.realized_kll.serialize();
          const int64_t tw = agg.time_weight_sum;
          const int64_t token_avg_10w =
              (tw > 0) ? round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
          const int64_t exposure_avg_10w =
              (tw > 0) ? round_i64(static_cast<double>(agg.exposure_tw_sum) / static_cast<double>(tw)) : 0;
          const int64_t holding_period_avg_10w =
              (tw > 0) ? round_i64(static_cast<double>(agg.holding_period_tw_sum) / static_cast<double>(tw)) : 0;
          const int64_t volume_10w = agg.volume_sum;
          double sharpe_10w = 0.0;
          if (agg.event_count > 1) {
            const double n = static_cast<double>(agg.event_count);
            const double mean = static_cast<double>(agg.realized_sum) / n;
            const double mean_sq = static_cast<double>(agg.realized_sq_sum) / n;
            const double variance = mean_sq - mean * mean;
            if (variance > 0.0) {
              sharpe_10w = mean / std::sqrt(variance);
            }
          }
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.Append(key.block_bucket);
          ap.Append(static_cast<int32_t>(key.tag_id));
          ap.Append(agg.last_sort_key);
          ap.Append(agg.last_block);
          ap.Append(agg.last_exposure);
          ap.Append(agg.last_holding_period);
          ap.Append(agg.last_token_count);
          ap.Append(agg.time_weight_sum);
          ap.Append(agg.token_count_tw_sum);
          ap.Append(agg.exposure_tw_sum);
          ap.Append(agg.volume_sum);
          ap.Append(agg.holding_period_tw_sum);
          ap.Append(agg.realized_sum);
          ap.Append(agg.realized_sq_sum);
          ap.Append(agg.event_count);
          ap.Append(duckdb::Value::BLOB(
              reinterpret_cast<duckdb::const_data_ptr_t>(realized_kll_blob.data()),
              realized_kll_blob.size()));
          ap.Append(token_avg_10w);
          ap.Append(exposure_avg_10w);
          ap.Append(volume_10w);
          ap.Append(holding_period_avg_10w);
          ap.Append(sharpe_10w);
          ap.Append(agg.last_sort_key);
          ap.EndRow();
        }
        ap.Close();
      }
      (void)checked_query(
          "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                      "SELECT " +
          kSqlColsFeatureTensorState + " FROM " +
          std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
    }

    prepare_tmp_table(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_conn, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : user_event_increments) {
        const std::string &user_blob = user_blob_pool[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(user_last_sort_keys[user_id]);
        ap.Append(round_i64(user_realized_totals[user_id]));
        ap.Append(round_i64(user_unrealized_totals[user_id]));
        assert(user_active_token_counts[user_id] >= 0);
        ap.Append(user_active_token_counts[user_id]);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)checked_query(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);

    save_cursor_locked(*sink_conn);
    (void)checked_query("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_cache_mu_);
    user_cache_ = UserQueryCache{};
  }

  return true;
}

} // namespace stage3
