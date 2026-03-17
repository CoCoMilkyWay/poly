#include "stage3_sync.hpp"

#include "misc/profiler.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <thread>

namespace stage3 {

class StageSync::QueryPauseGuard {
public:
  explicit QueryPauseGuard(const StageSync &owner)
      : owner_(owner), query_lock_(owner.query_mu_) {
    StageSync &state_owner = const_cast<StageSync &>(owner_);
    state_owner.pause_requested_.store(true);
    while (true) {
      uint8_t expected = kRtStateIdle;
      if (state_owner.rt_state_.compare_exchange_weak(expected, kRtStateQuerying)) {
        break;
      }
      std::this_thread::yield();
    }
  }

  ~QueryPauseGuard() {
    StageSync &state_owner = const_cast<StageSync &>(owner_);
    const uint8_t prev = state_owner.rt_state_.exchange(kRtStateIdle);
    assert(prev == kRtStateQuerying);
    state_owner.pause_requested_.store(false);
  }

private:
  const StageSync &owner_;
  std::unique_lock<std::mutex> query_lock_;
};

StageSync::StageSync(stage2::EventBuilder &builder,
                     Database &stage0_db,
                     Database &stage2_db,
                     Database &stage3_db,
                     int base_interval_seconds)
    : builder_(builder),
      stage0_db_(stage0_db),
      stage2_db_(stage2_db),
      stage3_db_(stage3_db),
      base_interval_seconds_(base_interval_seconds) {
  assert(base_interval_seconds_ > 0);
  const std::filesystem::path mmap_dir = std::filesystem::path(stage3_db_.data_dir()) / "mmap";
  std::filesystem::create_directories(mmap_dir);
  rt_ = stage3_open(mmap_dir.c_str());
  load_tag_mapping();
  refresh_conditions_if_needed();
  std::lock_guard<std::mutex> lock(sync_mu_);
  refresh_status_locked();
}

StageSync::~StageSync() {
  stop_requested_ = true;
  pause_requested_.store(true);
  std::lock_guard<std::mutex> query_lock(query_mu_);
  while (rt_state_.load() == kRtStateSyncing) {
    std::this_thread::yield();
  }
  assert(rt_state_.load() != kRtStateQuerying);
  if (rt_ != nullptr) {
    stage3_sync(rt_);
    stage3_close(rt_);
    rt_ = nullptr;
  }
}

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  pause_requested_.store(false);
  rt_state_.store(kRtStateIdle);
  schedule_sync(1);
}

void StageSync::stop() {
  stop_requested_ = true;
  pause_requested_.store(true);
}

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(sync_mu_);
  return sync_;
}

int64_t StageSync::get_max_bucket() const {
  return cached_max_bucket_.load(std::memory_order_relaxed);
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  (void)bucket; // bucket parameter ignored, we always return cached value for max_bucket
  return cached_bucket_user_count_.load(std::memory_order_relaxed);
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working = builder_.progress();
  const auto &committed = builder_.committed_progress();

  Stage2Data out;
  out.phase = working.phase;
  out.running = working.running;
  out.total_users = committed.total_users;
  out.total_events = committed.total_events;
  out.cond_tree = committed.cond_tree;
  out.token_tree = committed.token_tree;
  out.xfer_total = committed.xfer_stats.total;
  out.xfer_split_normal = committed.xfer_stats.split_normal;
  out.xfer_split_negrisk = committed.xfer_stats.split_negrisk;
  out.xfer_split_non_poly = committed.xfer_stats.split_non_poly;
  out.xfer_merge_normal = committed.xfer_stats.merge_normal;
  out.xfer_merge_negrisk = committed.xfer_stats.merge_negrisk;
  out.xfer_merge_non_poly = committed.xfer_stats.merge_non_poly;
  out.xfer_redemption = committed.xfer_stats.redemption;
  out.xfer_redemption_non_poly = committed.xfer_stats.redemption_non_poly;
  out.xfer_convert = committed.xfer_stats.convert;
  out.xfer_order_buy = committed.xfer_stats.order_buy;
  out.xfer_order_sell = committed.xfer_stats.order_sell;
  out.xfer_fpmm_buy = committed.xfer_stats.fpmm_buy;
  out.xfer_fpmm_sell = committed.xfer_stats.fpmm_sell;
  out.xfer_lp_add = committed.xfer_stats.fpmm_lp_add;
  out.xfer_lp_remove = committed.xfer_stats.fpmm_lp_remove;
  out.xfer_lp_return = committed.xfer_stats.fpmm_lp_return;
  out.xfer_transfer_in_negrisk = committed.xfer_stats.transfer_in_negrisk;
  out.xfer_transfer_in_other = committed.xfer_stats.transfer_in_other;
  out.xfer_transfer_in_non_poly = committed.xfer_stats.transfer_in_non_poly;
  out.xfer_transfer_out_negrisk = committed.xfer_stats.transfer_out_negrisk;
  out.xfer_transfer_out_other = committed.xfer_stats.transfer_out_other;
  out.xfer_transfer_out_non_poly = committed.xfer_stats.transfer_out_non_poly;
  out.xfer_internal_mint_negrisk = committed.xfer_stats.internal_mint_negrisk;
  out.xfer_internal_mint_fpmm = committed.xfer_stats.internal_mint_fpmm;
  out.xfer_internal_burn_negrisk = committed.xfer_stats.internal_burn_negrisk;
  out.xfer_internal_burn_fpmm = committed.xfer_stats.internal_burn_fpmm;
  out.xfer_internal_burn_convert = committed.xfer_stats.internal_burn_convert;
  out.xfer_internal_transfer_zero = committed.xfer_stats.internal_transfer_zero;
  out.xfer_internal_transfer_order = committed.xfer_stats.internal_transfer_order;
  out.xfer_internal_transfer_negrisk = committed.xfer_stats.internal_transfer_negrisk;
  out.xfer_internal_transfer_fpmm = committed.xfer_stats.internal_transfer_fpmm;
  out.xfer_internal_transfer_other = committed.xfer_stats.internal_transfer_other;
  out.split_sem_tree = committed.split_sem_tree;
  out.merge_sem_tree = committed.merge_sem_tree;
  out.convert_sem_tree = committed.convert_sem_tree;
  out.order_sem_tree = committed.order_sem_tree;
  out.event_by_collateral = committed.event_by_collateral;

  const auto s = status();
  if (s.behind_blocks == 0 && out.total_users > 0) {
    out.phase = 7;
  }
  return out;
}

json StageSync::memory_breakdown() const {
  QueryPauseGuard guard(*this);
  const int64_t token_used = static_cast<int64_t>(token_pool_used_total(rt_->header) * sizeof(TokenSlot));
  const int64_t feature_used = static_cast<int64_t>(feature_pool_used_total(rt_->header) * sizeof(FeatureSlot));
  const int64_t sharpe_bucket_used =
      static_cast<int64_t>(sharpe_bucket_pool_used_total(rt_->header) * sizeof(SharpeBucket));
  const int64_t sharpe_point_used =
      static_cast<int64_t>(sharpe_point_pool_used_total(rt_->header) * sizeof(SharpePoint));
  const int64_t users_used = static_cast<int64_t>(rt_->header->user_count * sizeof(UserBlock));
  const int64_t events_used = static_cast<int64_t>(rt_->header->events_log_tail);
  const int64_t index_used = static_cast<int64_t>(rt_->header->user_count * sizeof(UserIndexEntry));
  const int64_t estimated_total =
      token_used + feature_used + sharpe_bucket_used + sharpe_point_used + users_used + events_used + index_used;
  return {
      {"name", "stage3_mmap"},
      {"token_pool_used_bytes", token_used},
      {"feature_pool_used_bytes", feature_used},
      {"sharpe_bucket_pool_used_bytes", sharpe_bucket_used},
      {"sharpe_point_pool_used_bytes", sharpe_point_used},
      {"users_used_bytes", users_used},
      {"events_log_used_bytes", events_used},
      {"user_index_used_bytes", index_used},
      {"persistent_bytes", estimated_total},
      {"estimated_total_bytes", estimated_total},
      {"estimated_peak_candidate_bytes", estimated_total},
  };
}

json StageSync::stage2_rocksdb_memory_breakdown() const {
  return builder_.rocksdb_memory_breakdown();
}

json StageSync::stage3_rocksdb_memory_breakdown() const {
  QueryPauseGuard guard(*this);
  return {
      {"name", "stage3_mmap"},
      {"engine", "mmap"},
      {"path", stage3_db_.data_dir() + "/mmap"},
      {"memtables_bytes", 0},
      {"table_readers_bytes", 0},
      {"block_cache_bytes", 0},
      {"block_cache_pinned_bytes", 0},
      {"estimated_total_bytes", static_cast<int64_t>(rt_->header->events_log_tail)},
  };
}

std::vector<StageSync::UserSummaryRow> StageSync::get_users_sorted(int64_t limit) const {
  TraceN("s3/users");
  QueryPauseGuard guard(*this);
  const int64_t actual_limit = std::max<int64_t>(1, limit);
  rt_->rank_cache.rebuild_if_needed(rt_);
  const size_t n = std::min(rt_->rank_cache.by_events.size(), static_cast<size_t>(actual_limit));
  std::vector<UserSummaryRow> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const uint32_t user_idx = rt_->rank_cache.by_events[i].user_idx;
    const UserBlock &u = rt_->users[user_idx];
    out.push_back({
        format_address(u.addr),
        u.total_events,
        u.total_realized_pnl,
        u.total_unrealized_pnl,
    });
  }
  return out;
}

std::vector<StageSync::TimelineRow> StageSync::get_user_timeline(const std::string &addr) const {
  TraceN("s3/timeline");
  QueryPauseGuard guard(*this);
  const std::string normalized = normalize_addr(addr);
  if (normalized.empty()) {
    return {};
  }
  const Address20 user_addr = parse_address(normalized);
  UserQueryCache *cache = stage3_get_user_query_cache(rt_, user_addr);
  if (cache == nullptr || cache->timeline.empty()) {
    return {};
  }

  std::vector<TimelineRow> out;
  out.resize(cache->timeline.size());
  for (size_t i = 0; i < cache->timeline.size(); ++i) {
    const EventRecord &rec = cache->timeline[i];
    out[i] = {
        rec.sort_key,
        rec.cond_idx,
        rec.token_idx,
        static_cast<uint8_t>(rec.event_type),
        rec.amount,
        rec.price_1e6,
        rec.realized_cum,
        rec.unrealized_pnl,
        rec.token_count,
    };
  }
  return out;
}

std::vector<StageSync::PositionRow> StageSync::get_positions_at(const std::string &addr, int64_t sort_key) const {
  TraceN("s3/positions");
  QueryPauseGuard guard(*this);
  const std::string normalized = normalize_addr(addr);
  if (normalized.empty()) {
    return {};
  }
  const Address20 user_addr = parse_address(normalized);
  const PositionsResult positions = stage3_query_positions(rt_, user_addr, sort_key);
  std::vector<PositionRow> out;
  out.reserve(positions.positions.size());
  for (const auto &row : positions.positions) {
    uint8_t outcome_count = 0;
    const ConditionMeta *cond = stage3_get_condition(rt_, row.cond_idx);
    if (cond != nullptr) {
      outcome_count = cond->outcome_count;
    }
    out.push_back({
        static_cast<uint32_t>(row.cond_idx),
        static_cast<uint8_t>(row.token_idx),
        outcome_count,
        row.qty,
        row.cost,
        row.lp,
        row.entry_block,
    });
  }
  return out;
}

filter::Result StageSync::filter_users_by_features(const filter::Request &req) const {
  TraceN("s3/filter");
  QueryPauseGuard guard(*this);
  const FilterResult r = stage3_query_filter(rt_, req);

  filter::Result out;
  out.anchor_bucket = r.anchor_bucket;
  out.scanned_user_count = r.scanned_user_count;
  out.matched_user_count = r.matched_user_count;
  out.item_stats = r.item_stats;
  out.users.reserve(r.users.size());
  for (const auto &row : r.users) {
    filter::UserRow u{};
    const UserBlock *user = &rt_->users[row.user_idx];
    u.addr = format_address(user->addr);
    u.sort_value = row.sort_value;
    u.month_avg_tok = row.month_avg_tok;
    u.month_avg_exp = row.month_avg_exp;
    u.month_avg_hp = row.month_avg_hp;
    u.pnl = row.pnl;
    out.users.push_back(u);
  }
  return out;
}

std::string StageSync::normalize_addr(const std::string &addr) {
  std::string lower = to_lower_str(addr);
  if (lower.size() != 42 || !lower.starts_with("0x")) {
    return {};
  }
  for (size_t i = 2; i < lower.size(); ++i) {
    const char c = lower[i];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) {
      return {};
    }
  }
  return lower;
}

std::string StageSync::normalize_tag_key(const std::string &raw) {
  return normalize_key(raw);
}

int8_t StageSync::tag_name_to_id(const std::string &tag_name) const {
  const std::string key = normalize_tag_key(tag_name);
  auto it = tag_to_industry_id_.find(key);
  if (it != tag_to_industry_id_.end()) {
    return it->second;
  }
  return 13;
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

void StageSync::load_conditions() {
  auto stage2_conn = stage2_db_.create_connection();
  auto cond_result = stage2_conn->Query(
      "SELECT cond_idx, lower(hex(cond_id)) AS cond_hex, outcome_cnt, "
      "payout_0, payout_1, payout_2, payout_3, payout_4, payout_5, payout_6, payout_7, "
      "CASE WHEN question_id IS NULL THEN '' ELSE lower(hex(question_id)) END AS qid "
      "FROM rb_condition ORDER BY cond_idx");
  assert(cond_result && !cond_result->HasError());

  std::unordered_map<std::string, int32_t> cond_hex_to_idx;
  cond_hex_to_idx.reserve(static_cast<size_t>(cond_result->RowCount()) + 1);
  std::unordered_map<int32_t, int8_t> cond_tag_id;
  cond_tag_id.reserve(static_cast<size_t>(cond_result->RowCount()) + 1);

  for (idx_t i = 0; i < cond_result->RowCount(); ++i) {
    const int32_t cond_idx = cond_result->GetValue(0, i).GetValue<int32_t>();
    const std::string cond_hex = cond_result->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_hex_to_idx.emplace(cond_hex, cond_idx);
    cond_tag_id.emplace(cond_idx, 13);
  }

  auto stage0_conn = stage0_db_.create_connection();
  auto tags = stage0_conn->Query(
      "SELECT lower(hex(condition_id)) AS cond_hex, coalesce(tag_name, 'Unknown') AS tag_name "
      "FROM pm_condition_scan_class");
  assert(tags && !tags->HasError());
  for (idx_t i = 0; i < tags->RowCount(); ++i) {
    const std::string cond_hex = tags->GetValue(0, i).GetValueUnsafe<std::string>();
    auto cond_it = cond_hex_to_idx.find(cond_hex);
    if (cond_it == cond_hex_to_idx.end()) {
      continue;
    }
    const std::string tag_name = tags->GetValue(1, i).GetValueUnsafe<std::string>();
    cond_tag_id[cond_it->second] = tag_name_to_id(tag_name);
  }

  auto market_result = stage2_conn->Query(
      "SELECT lower(hex(question_id)) AS qid_hex, lower(hex(market_id)) AS market_hex "
      "FROM rb_neg_risk_market");
  assert(market_result && !market_result->HasError());
  std::unordered_map<std::string, std::string> question_to_market;
  std::unordered_map<std::string, uint16_t> market_question_count;
  question_to_market.reserve(static_cast<size_t>(market_result->RowCount()) + 1);
  market_question_count.reserve(static_cast<size_t>(market_result->RowCount()) + 1);
  for (idx_t i = 0; i < market_result->RowCount(); ++i) {
    const std::string qid = market_result->GetValue(0, i).GetValueUnsafe<std::string>();
    const std::string mid = market_result->GetValue(1, i).GetValueUnsafe<std::string>();
    question_to_market.emplace(qid, mid);
    market_question_count[mid]++;
  }

  for (idx_t i = 0; i < cond_result->RowCount(); ++i) {
    const int32_t cond_idx = cond_result->GetValue(0, i).GetValue<int32_t>();
    assert(cond_idx >= 0);
    assert(static_cast<size_t>(cond_idx) < MAX_CONDITIONS);
    const int32_t outcome_count_i32 = cond_result->GetValue(2, i).GetValue<int32_t>();
    assert(outcome_count_i32 > 0);
    assert(outcome_count_i32 <= stage2::MAX_OUTCOMES);
    const uint8_t outcome_count = static_cast<uint8_t>(outcome_count_i32);

    int64_t payout_buf[OUTCOME_MAX] = {};
    for (uint8_t j = 0; j < outcome_count; ++j) {
      const auto payout_value = cond_result->GetValue(3 + j, i);
      payout_buf[j] = payout_value.IsNull() ? -1 : payout_value.GetValue<int64_t>();
    }

    uint16_t question_count = 0;
    const std::string qid = cond_result->GetValue(11, i).GetValueUnsafe<std::string>();
    if (!qid.empty()) {
      auto qit = question_to_market.find(qid);
      if (qit != question_to_market.end()) {
        auto mit = market_question_count.find(qit->second);
        assert(mit != market_question_count.end());
        question_count = mit->second;
      }
    }

    const int8_t tag_id = cond_tag_id[cond_idx];
    stage3_set_condition(rt_, cond_idx, outcome_count, tag_id, payout_buf, question_count);
    stage3_mark_condition_valid(rt_, cond_idx);
  }
}

void StageSync::refresh_conditions_if_needed() {
  auto conn = stage2_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(cond_idx), -1) FROM rb_condition");
  assert(r && !r->HasError() && r->RowCount() == 1);
  const int64_t max_cond_idx = r->GetValue(0, 0).GetValue<int64_t>();

  bool needs_full_refresh = (max_cond_idx > loaded_max_cond_idx_);

  // Check if pending Convert events need question_count (faithful to old architecture)
  // Old code: for each Convert event, assert cond_market_question_counts_[cond_idx] >= 2
  if (!needs_full_refresh) {
    const int64_t cursor = rt_->header->cursor_sort_key;
    const int64_t head_sort_key = builder_.cursor() * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);
    auto pending_events = builder_.user_event_store().scan_by_sort_key(cursor, head_sort_key, 10000);

    for (const auto &evt : pending_events) {
      if (evt.event_type != EVT_CONVERT || evt.cond_idx < 0)
        continue;

      const size_t cond_idx = static_cast<size_t>(evt.cond_idx);
      if (cond_idx >= rt_->cond_market_question_counts.size() ||
          rt_->cond_market_question_counts[cond_idx] < 2) {
        needs_full_refresh = true;
        break;
      }
    }
  }

  if (!needs_full_refresh) {
    return;
  }

  load_conditions();
  loaded_max_cond_idx_ = max_cond_idx;
}

void StageSync::refresh_status_locked() {
  const int64_t head_block = builder_.cursor();
  const QueryStatus q = stage3_query_status(rt_, head_block);
  sync_.last_block = q.last_block;
  sync_.head_block = q.head_block;
  sync_.behind_blocks = q.behind_blocks;
  sync_.behind_chunks = q.behind_chunks;

  // Update cached bucket info for lightweight status API
  const int64_t max_bucket = (rt_->header->cursor_sort_key < 0) ? -1 : rt_->header->head_bucket;
  cached_max_bucket_.store(max_bucket, std::memory_order_relaxed);
  if (max_bucket >= 0 && max_bucket < static_cast<int64_t>(rt_->global_feature_user_counts.size())) {
    cached_bucket_user_count_.store(rt_->global_feature_user_counts[static_cast<size_t>(max_bucket)],
                                    std::memory_order_relaxed);
  } else {
    cached_bucket_user_count_.store(0, std::memory_order_relaxed);
  }
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
  TraceN("s3/runloop_tick");
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
    const double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    const int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };
  auto yield_sync = [&](int delay_seconds) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(delay_seconds);
  };

  int64_t before_block = 0;
  {
    TraceN("s3/runloop/enter");
    std::lock_guard<std::mutex> lock(sync_mu_);
    sync_.syncing = true;
    refresh_status_locked();
    before_block = sync_.last_block;
  }

  if (builder_.is_building() || builder_.has_pending_commit()) {
    {
      TraceN("s3/runloop/yield_stage2");
      yield_sync(kStage2YieldDelaySeconds);
    }
    TraceFrameN("s3_sync_loop");
    return;
  }

  if (pause_requested_.load()) {
    {
      TraceN("s3/runloop/yield_query");
      yield_sync(kPauseRetryDelaySeconds);
    }
    TraceFrameN("s3_sync_loop");
    return;
  }

  uint8_t expected_state = kRtStateIdle;
  if (!rt_state_.compare_exchange_strong(expected_state, kRtStateSyncing)) {
    {
      TraceN("s3/runloop/yield_query");
      yield_sync(kPauseRetryDelaySeconds);
    }
    TraceFrameN("s3_sync_loop");
    return;
  }

  size_t processed = 0;
  {
    TraceN("s3/runloop/refresh_conditions");
    refresh_conditions_if_needed();
  }

  const int64_t head_block = builder_.cursor();
  processed = stage3_sync_tick(rt_, builder_.user_event_store(), head_block, kStage3BatchEvents);
  if (processed > 0) {
    {
      TraceN("s3/runloop/prune");
      stage3_post_sync_prune(rt_);
    }
  }
  const uint8_t prev_state = rt_state_.exchange(kRtStateIdle);
  assert(prev_state == kRtStateSyncing);

  {
    TraceN("s3/runloop/msync");
    if (!pause_requested_.load()) {
      stage3_sync(rt_);
    }
  }

  {
    TraceN("s3/runloop/commit");
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    const int64_t after_block = sync_.last_block;
    if (after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
      }
    }
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    const int next_delay = pause_requested_.load() ? kPauseRetryDelaySeconds
                                                   : ((sync_.behind_blocks > 0) ? 0 : base_interval_seconds_);
    schedule_sync(next_delay);
  }
  TraceFrameN("s3_sync_loop");
}

} // namespace stage3
