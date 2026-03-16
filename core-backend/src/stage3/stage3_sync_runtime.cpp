#include "../core/mem.hpp"
#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace stage3 {
namespace {
constexpr const char *kSqlTmpTouchedUsers = "tmp_touched_users";
constexpr const char *kSqlTmpTouchedUserTags = "tmp_touched_user_tags";
constexpr const char *kSqlTmpTouchedTokenKeys = "tmp_touched_token_keys";
constexpr const char *kSqlTmpFeatureTensorKeys = "tmp_feature_tensor_keys";
constexpr const char *kSqlTmpSharpeCacheLoad = "tmp_sharpe_cache_load";
constexpr const char *kSqlTmpTokenDirty = "tmp_token_dirty";
constexpr const char *kSqlTmpTokenNew = "tmp_token_new";
constexpr const char *kSqlTmpAccountBucketPnlState = "tmp_account_bucket_pnl_state";
constexpr const char *kSqlTmpFeatureTensorState = "tmp_feature_tensor_state";
constexpr const char *kSqlTmpUserSummaryDelta = "tmp_user_summary_delta";
constexpr const char *kSqlTmpSchemaTouchedUsers = "user_addr BLOB";
constexpr const char *kSqlTmpSchemaTouchedUserTags = "user_addr BLOB, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaTouchedTokenKeys = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaFeatureTensorKeys = "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER";
constexpr const char *kSqlTmpSchemaSharpeCacheLoad = "user_addr BLOB, start_bucket BIGINT, end_bucket BIGINT";
constexpr const char *kSqlTmpSchemaTokenDirty = "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER";
constexpr const char *kSqlTmpSchemaTokenNew =
    "user_addr BLOB, cond_idx INTEGER, token_idx INTEGER, pos BIGINT, cost BIGINT, lp BIGINT, entry_block BIGINT";
constexpr const char *kSqlTmpSchemaAccountBucketPnlState =
    "user_addr BLOB, block_bucket BIGINT, samples_blob BLOB, close_pnl BIGINT, min_pnl BIGINT, updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaFeatureTensorState =
    "user_addr BLOB, block_bucket BIGINT, tag_id INTEGER, "
    "last_sort_key_10w BIGINT, last_block_10w BIGINT, last_exposure_10w BIGINT, "
    "last_holding_period_10w HUGEINT, last_token_count_10w BIGINT, "
    "time_weight_sum_10w BIGINT, token_count_tw_sum_10w BIGINT, exposure_tw_sum_10w HUGEINT, "
    "volume_sum_10w BIGINT, holding_period_exp_tw_sum_10w HUGEINT, "
    "token_avg_10w BIGINT, exposure_avg_10w BIGINT, volume_10w BIGINT, holding_period_avg_10w BIGINT, "
    "sharpe_10w DOUBLE, "
    "ps_token_avg_10w BIGINT, ps_exposure_avg_10w BIGINT, ps_volume_10w BIGINT, "
    "ps_holding_period_avg_10w BIGINT, "
    "token_avg_100w BIGINT, token_avg_1000w BIGINT, "
    "exposure_avg_100w BIGINT, exposure_avg_1000w BIGINT, "
    "volume_avg_100w BIGINT, volume_avg_1000w BIGINT, "
    "holding_period_avg_100w BIGINT, holding_period_avg_1000w BIGINT, "
    "sharpe_100w DOUBLE, sharpe_1000w DOUBLE, "
    "updated_sort_key BIGINT";
constexpr const char *kSqlTmpSchemaUserSummaryDelta =
    "user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT, rpnl BIGINT, upnl BIGINT, active_tokens BIGINT";
constexpr const char *kSqlColsTokenState = "user_addr, cond_idx, token_idx, pos, cost, lp, entry_block";
constexpr const char *kSqlColsAccountBucketPnlState =
    "user_addr, block_bucket, samples_blob, close_pnl, min_pnl, updated_sort_key";
constexpr const char *kSqlColsFeatureTensorState =
    "user_addr, block_bucket, tag_id, "
    "last_sort_key_10w, last_block_10w, last_exposure_10w, last_holding_period_10w, last_token_count_10w, "
    "time_weight_sum_10w, token_count_tw_sum_10w, exposure_tw_sum_10w, volume_sum_10w, holding_period_exp_tw_sum_10w, "
    "token_avg_10w, exposure_avg_10w, volume_10w, holding_period_avg_10w, sharpe_10w, "
    "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, "
    "token_avg_100w, token_avg_1000w, "
    "exposure_avg_100w, exposure_avg_1000w, "
    "volume_avg_100w, volume_avg_1000w, "
    "holding_period_avg_100w, holding_period_avg_1000w, "
    "sharpe_100w, sharpe_1000w, "
    "updated_sort_key";
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
    "token_avg_10w=excluded.token_avg_10w, "
    "exposure_avg_10w=excluded.exposure_avg_10w, "
    "volume_10w=excluded.volume_10w, "
    "holding_period_avg_10w=excluded.holding_period_avg_10w, "
    "sharpe_10w=excluded.sharpe_10w, "
    "ps_token_avg_10w=excluded.ps_token_avg_10w, "
    "ps_exposure_avg_10w=excluded.ps_exposure_avg_10w, "
    "ps_volume_10w=excluded.ps_volume_10w, "
    "ps_holding_period_avg_10w=excluded.ps_holding_period_avg_10w, "
    "token_avg_100w=excluded.token_avg_100w, "
    "token_avg_1000w=excluded.token_avg_1000w, "
    "exposure_avg_100w=excluded.exposure_avg_100w, "
    "exposure_avg_1000w=excluded.exposure_avg_1000w, "
    "volume_avg_100w=excluded.volume_avg_100w, "
    "volume_avg_1000w=excluded.volume_avg_1000w, "
    "holding_period_avg_100w=excluded.holding_period_avg_100w, "
    "holding_period_avg_1000w=excluded.holding_period_avg_1000w, "
    "sharpe_100w=excluded.sharpe_100w, "
    "sharpe_1000w=excluded.sharpe_1000w, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictFeatureTensorState = "ON CONFLICT(user_addr, block_bucket, tag_id) DO UPDATE SET ";
constexpr const char *kSqlOnConflictAccountBucketPnlState =
    "ON CONFLICT(user_addr, block_bucket) DO UPDATE SET "
    "samples_blob=excluded.samples_blob, "
    "close_pnl=excluded.close_pnl, "
    "min_pnl=excluded.min_pnl, "
    "updated_sort_key=excluded.updated_sort_key";
constexpr const char *kSqlOnConflictUserSummaryState = "ON CONFLICT(user_addr) DO UPDATE SET ";
constexpr const char *kSqlSelectSummarySeedCols =
    "SELECT s.user_addr, s.total_realized_pnl, s.total_unrealized_pnl, s.active_tokens ";
constexpr const char *kSqlSelectTokenStateCols =
    "SELECT s.user_addr, s.cond_idx, s.token_idx, s.pos, s.cost, s.lp, s.entry_block ";
constexpr const char *kSqlSelectFeatureTensorStateCols =
    "SELECT f.user_addr, f.block_bucket, f.tag_id, "
    "f.last_sort_key_10w, f.last_block_10w, f.last_exposure_10w, f.last_holding_period_10w, f.last_token_count_10w, "
    "f.time_weight_sum_10w, f.token_count_tw_sum_10w, f.exposure_tw_sum_10w, f.volume_sum_10w, "
    "f.holding_period_exp_tw_sum_10w ";

int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

duckdb::hugeint_t i128_to_hugeint(__int128 v) {
  duckdb::hugeint_t out;
  out.lower = static_cast<uint64_t>(v);
  out.upper = static_cast<int64_t>(v >> 64);
  return out;
}

__int128 hugeint_to_i128(const duckdb::hugeint_t &v) {
  return (static_cast<__int128>(v.upper) << 64) + static_cast<__int128>(v.lower);
}

long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

template <typename SampleVec>
void decode_account_bucket_samples_blob(const std::string &blob, SampleVec &out) {
  assert(blob.size() % 12 == 0);
  const size_t sample_count = blob.size() / 12;
  out.clear();
  out.reserve(sample_count);
  int32_t prev_block_offset = -1;
  for (size_t i = 0; i < sample_count; ++i) {
    const size_t off = i * 12;
    const int32_t block_offset = static_cast<int32_t>(core::rocks::detail::read_u32_be(blob, off));
    const int64_t pnl =
        core::rocks::detail::decode_i64_lex(core::rocks::detail::read_u64_be(blob, off + 4));
    assert(block_offset >= 0);
    assert(block_offset > prev_block_offset);
    out.push_back({block_offset, pnl});
    prev_block_offset = block_offset;
  }
}

template <typename SampleVec>
std::string encode_account_bucket_samples_blob(const SampleVec &samples) {
  std::string blob;
  blob.reserve(samples.size() * 12);
  int32_t prev_block_offset = -1;
  for (const auto &sample : samples) {
    assert(sample.block_offset >= 0);
    assert(sample.block_offset > prev_block_offset);
    core::rocks::detail::append_u32_be(blob, static_cast<uint32_t>(sample.block_offset));
    core::rocks::detail::append_u64_be(blob, core::rocks::detail::encode_i64_lex(sample.pnl));
    prev_block_offset = sample.block_offset;
  }
  return blob;
}

template <typename BucketDeque>
double calc_window_return_sharpe(const BucketDeque &buckets,
                                 int64_t pnl_before_first_bucket,
                                 int64_t start_bucket,
                                 int64_t end_bucket,
                                 int64_t avg_exposure_1e6,
                                 int64_t block_bucket_size) {
  assert(start_bucket >= 0);
  assert(end_bucket >= start_bucket);
  assert(avg_exposure_1e6 >= 0);
  assert(block_bucket_size > 0);
  const int64_t start_block = start_bucket * block_bucket_size;
  const int64_t end_block = (end_bucket + 1) * block_bucket_size - 1;
  assert(end_block >= start_block);

  const auto bucket_begin_it = std::lower_bound(
      buckets.begin(),
      buckets.end(),
      start_bucket,
      [](const auto &bucket, int64_t target_bucket) { return bucket.block_bucket < target_bucket; });
  const auto bucket_end_it = std::upper_bound(
      bucket_begin_it,
      buckets.end(),
      end_bucket,
      [](int64_t target_bucket, const auto &bucket) { return target_bucket < bucket.block_bucket; });
  int64_t anchor_pnl = pnl_before_first_bucket;
  if (bucket_begin_it != buckets.begin()) {
    anchor_pnl = std::prev(bucket_begin_it)->close_pnl;
  }

  // Rebase window PnL to the left boundary anchor so every interval starts from 0.
  const auto to_interval_pnl = [&](int64_t absolute_pnl) {
    return i64_narrow_checked(static_cast<__int128>(absolute_pnl) - static_cast<__int128>(anchor_pnl));
  };

  long double min_interval_pnl = 0.0L;
  long double max_interval_pnl = 0.0L;
  int64_t tail_interval_pnl = 0;
  for (auto bucket_it = bucket_begin_it; bucket_it != bucket_end_it; ++bucket_it) {
    for (const auto &sample : bucket_it->samples) {
      const int64_t interval_pnl = to_interval_pnl(sample.pnl);
      const long double interval_pnl_ld = static_cast<long double>(interval_pnl);
      min_interval_pnl = std::min(min_interval_pnl, interval_pnl_ld);
      max_interval_pnl = std::max(max_interval_pnl, interval_pnl_ld);
      tail_interval_pnl = interval_pnl;
    }
  }
  const long double interval_range = max_interval_pnl - min_interval_pnl;
  assert(interval_range >= 0.0L);
  assert(interval_range >= std::fabs(min_interval_pnl));

  const long double nav_base =
      static_cast<long double>(avg_exposure_1e6) + std::fabs(min_interval_pnl) + 1000000.0L;
  long double sum_r = 0.0L;
  long double sum_r2_over_dt = 0.0L;
  int64_t prev_block = start_block - 1;
  int64_t prev_interval_pnl = 0;

  auto append_sample = [&](int64_t block, int64_t interval_pnl) {
    assert(block >= prev_block);
    if (block == prev_block) {
      prev_interval_pnl = interval_pnl;
      return;
    }
    const int64_t delta_t = block - prev_block;
    assert(delta_t > 0);
    const long double prev_nav = nav_base + static_cast<long double>(prev_interval_pnl);
    const long double curr_nav = nav_base + static_cast<long double>(interval_pnl);
    assert(prev_nav > 0.0L);
    assert(curr_nav > 0.0L);
    const long double period_return = (curr_nav - prev_nav) / prev_nav;
    sum_r += period_return;
    sum_r2_over_dt += (period_return * period_return) / static_cast<long double>(delta_t);
    prev_block = block;
    prev_interval_pnl = interval_pnl;
  };

  for (auto bucket_it = bucket_begin_it; bucket_it != bucket_end_it; ++bucket_it) {
    const int64_t bucket_block_base = bucket_it->block_bucket * block_bucket_size;
    for (const auto &sample : bucket_it->samples) {
      const int64_t block = bucket_block_base + sample.block_offset;
      const int64_t interval_pnl = to_interval_pnl(sample.pnl);
      assert(block >= start_block);
      assert(block <= end_block);
      if (prev_block < block - 1) {
        append_sample(block - 1, prev_interval_pnl);
      }
      append_sample(block, interval_pnl);
    }
  }
  if (prev_block < end_block) {
    append_sample(end_block, tail_interval_pnl);
  }

  const int64_t total_t = prev_block - (start_block - 1);
  if (total_t <= 0) {
    return 0.0;
  }
  const long double T = static_cast<long double>(total_t);
  const long double mean = sum_r / T;
  const long double variance = sum_r2_over_dt / T - mean * mean;
  if (variance <= 0.0L) {
    return 0.0;
  }
  const long double raw_sharpe = mean / std::sqrt(variance);
  const long double normalize_factor = std::sqrt(10000000.0L);
  return static_cast<double>(raw_sharpe * normalize_factor);
}

int blob_unsigned_compare(const std::string &lhs, const std::string &rhs) {
  const size_t n = std::min(lhs.size(), rhs.size());
  for (size_t i = 0; i < n; ++i) {
    const auto left_byte = static_cast<uint8_t>(lhs[i]);
    const auto right_byte = static_cast<uint8_t>(rhs[i]);
    if (left_byte != right_byte) {
      return (left_byte < right_byte) ? -1 : 1;
    }
  }
  if (lhs.size() == rhs.size()) {
    return 0;
  }
  return (lhs.size() < rhs.size()) ? -1 : 1;
}

void append_blob(duckdb::Appender &ap, const std::string &blob) {
  ap.Append(duckdb::Value::BLOB(
      reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()),
      blob.size()));
}

struct PrefixSumHistoryRecord {
  int64_t block_bucket = 0;
  int64_t ps_token_avg = 0;
  int64_t ps_exposure_avg = 0;
  int64_t ps_volume = 0;
  int64_t ps_holding_period_avg = 0;
};

struct BlobTagKey {
  std::string user_blob;
  int32_t tag_id = -1;
  bool operator==(const BlobTagKey &o) const {
    return user_blob == o.user_blob && tag_id == o.tag_id;
  }
};

struct BlobTagKeyHasher {
  size_t operator()(const BlobTagKey &k) const {
    size_t h = std::hash<std::string>()(k.user_blob);
    h ^= std::hash<int32_t>()(k.tag_id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
  }
};

const PrefixSumHistoryRecord *find_prefix_sum_history_by_pair_key_le(
    const std::vector<PrefixSumHistoryRecord> &records, int64_t target_bucket) {
  // records 按 bucket 降序排列，二分查找第一个 bucket <= target 的记录
  const auto it = std::lower_bound(
      records.begin(),
      records.end(),
      target_bucket,
      [](const PrefixSumHistoryRecord &rec, int64_t target) { return rec.block_bucket > target; });
  return (it == records.end()) ? nullptr : &(*it);
}
} // namespace

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
  // 从数据库查询实际已提交的最大 bucket，而不是内存中的 sync_cursor_
  // 因为 sync_cursor_ 在处理开始时就更新了，但数据可能还没写入数据库
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COALESCE(MAX(block_bucket), -1) FROM " +
                       std::string(kSqlTableFeatureTensorState));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return -1;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

int64_t StageSync::get_bucket_user_count(int64_t bucket) const {
  if (bucket < 0) {
    return 0;
  }
  auto conn = stage3_db_.create_connection();
  auto r = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM " +
                       std::string(kSqlTableFeatureTensorState) +
                       " WHERE block_bucket = " + std::to_string(bucket));
  if (!r || r->HasError() || r->RowCount() == 0) {
    return 0;
  }
  return r->GetValue(0, 0).GetValue<int64_t>();
}

StageSync::Stage2Data StageSync::stage2_data() const {
  const auto &working_progress = builder_.progress();
  const auto &committed_progress = builder_.committed_progress();
  Stage2Data stage2_data;
  stage2_data.phase = working_progress.phase;
  stage2_data.running = working_progress.running;
  stage2_data.total_users = committed_progress.total_users;
  stage2_data.total_events = committed_progress.total_events;
  stage2_data.cond_tree = committed_progress.cond_tree;
  stage2_data.token_tree = committed_progress.token_tree;
  stage2_data.xfer_total = committed_progress.xfer_stats.total;
  stage2_data.xfer_split_normal = committed_progress.xfer_stats.split_normal;
  stage2_data.xfer_split_negrisk = committed_progress.xfer_stats.split_negrisk;
  stage2_data.xfer_split_non_poly = committed_progress.xfer_stats.split_non_poly;
  stage2_data.xfer_merge_normal = committed_progress.xfer_stats.merge_normal;
  stage2_data.xfer_merge_negrisk = committed_progress.xfer_stats.merge_negrisk;
  stage2_data.xfer_merge_non_poly = committed_progress.xfer_stats.merge_non_poly;
  stage2_data.xfer_redemption = committed_progress.xfer_stats.redemption;
  stage2_data.xfer_redemption_non_poly = committed_progress.xfer_stats.redemption_non_poly;
  stage2_data.xfer_convert = committed_progress.xfer_stats.convert;
  stage2_data.xfer_order_buy = committed_progress.xfer_stats.order_buy;
  stage2_data.xfer_order_sell = committed_progress.xfer_stats.order_sell;
  stage2_data.xfer_fpmm_buy = committed_progress.xfer_stats.fpmm_buy;
  stage2_data.xfer_fpmm_sell = committed_progress.xfer_stats.fpmm_sell;
  stage2_data.xfer_lp_add = committed_progress.xfer_stats.fpmm_lp_add;
  stage2_data.xfer_lp_remove = committed_progress.xfer_stats.fpmm_lp_remove;
  stage2_data.xfer_lp_return = committed_progress.xfer_stats.fpmm_lp_return;
  stage2_data.xfer_transfer_in_negrisk = committed_progress.xfer_stats.transfer_in_negrisk;
  stage2_data.xfer_transfer_in_other = committed_progress.xfer_stats.transfer_in_other;
  stage2_data.xfer_transfer_in_non_poly = committed_progress.xfer_stats.transfer_in_non_poly;
  stage2_data.xfer_transfer_out_negrisk = committed_progress.xfer_stats.transfer_out_negrisk;
  stage2_data.xfer_transfer_out_other = committed_progress.xfer_stats.transfer_out_other;
  stage2_data.xfer_transfer_out_non_poly = committed_progress.xfer_stats.transfer_out_non_poly;
  stage2_data.xfer_internal_mint_negrisk = committed_progress.xfer_stats.internal_mint_negrisk;
  stage2_data.xfer_internal_mint_fpmm = committed_progress.xfer_stats.internal_mint_fpmm;
  stage2_data.xfer_internal_burn_negrisk = committed_progress.xfer_stats.internal_burn_negrisk;
  stage2_data.xfer_internal_burn_fpmm = committed_progress.xfer_stats.internal_burn_fpmm;
  stage2_data.xfer_internal_burn_convert = committed_progress.xfer_stats.internal_burn_convert;
  stage2_data.xfer_internal_transfer_zero = committed_progress.xfer_stats.internal_transfer_zero;
  stage2_data.xfer_internal_transfer_order = committed_progress.xfer_stats.internal_transfer_order;
  stage2_data.xfer_internal_transfer_negrisk = committed_progress.xfer_stats.internal_transfer_negrisk;
  stage2_data.xfer_internal_transfer_fpmm = committed_progress.xfer_stats.internal_transfer_fpmm;
  stage2_data.xfer_internal_transfer_other = committed_progress.xfer_stats.internal_transfer_other;
  stage2_data.split_sem_tree = committed_progress.split_sem_tree;
  stage2_data.merge_sem_tree = committed_progress.merge_sem_tree;
  stage2_data.convert_sem_tree = committed_progress.convert_sem_tree;
  stage2_data.order_sem_tree = committed_progress.order_sem_tree;
  stage2_data.event_by_collateral = committed_progress.event_by_collateral;
  const auto sync_status = status();
  if (sync_status.behind_blocks == 0 && stage2_data.total_users > 0) {
    stage2_data.phase = 7;
  }
  return stage2_data;
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
  static constexpr int kStage2YieldDelaySeconds = 1;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (sync_commit_points_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = sync_commit_points_.front();
    const auto &last = sync_commit_points_.back();
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

  if (builder_.is_building() || builder_.has_pending_commit()) {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    refresh_timing_metrics(sync_.behind_blocks);
    sync_.syncing = false;
    schedule_sync(kStage2YieldDelaySeconds);
    return;
  }

  bool advanced = process_chunk_locked();

  int next_delay = base_interval_seconds_;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_status_locked();
    int64_t after_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
    if (advanced && after_block > before_block) {
      sync_commit_points_.push_back({std::chrono::steady_clock::now(), after_block});
      if (sync_commit_points_.size() > kCommitHistoryWindow) {
        sync_commit_points_.pop_front();
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
  if (builder_.is_building() || builder_.has_pending_commit()) {
    return false;
  }
  // L0: sync range / cursor
  int64_t current_block = (sync_cursor_.sort_key < 0) ? 0 : sync_cursor_.sort_key / SORT_KEY_SCALE;
  int64_t head_block = builder_.cursor();
  if (current_block >= head_block) {
    return false;
  }
  int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

  // L1: sink io helpers
  auto sink_connection = stage3_db_.create_connection();
  const std::string table_token_state = kSqlTableTokenState;
  const std::string table_user_summary = kSqlTableUserSummaryState;
  const std::string table_account_bucket_pnl = kSqlTableAccountBucketPnlState;
  const std::string table_feature_tensor = kSqlTableFeatureTensorState;
  const std::string sql_set_user_summary_upsert =
      "total_events=" + table_user_summary + ".total_events + excluded.total_events, "
                                             "total_realized_pnl=excluded.total_realized_pnl, "
                                             "total_unrealized_pnl=excluded.total_unrealized_pnl, "
                                             "active_tokens=excluded.active_tokens, "
                                             "last_sort_key=GREATEST(" +
      table_user_summary + ".last_sort_key, excluded.last_sort_key)";
  auto query_checked = [&](const std::string &sql) {
    auto query_result = sink_connection->Query(sql);
    assert(query_result && !query_result->HasError());
    return query_result;
  };
  auto tmp_table_reset = [&](const char *tmp_table, const char *schema_cols) {
    (void)query_checked("CREATE TEMP TABLE IF NOT EXISTS " + std::string(tmp_table) + " (" + schema_cols + ")");
    (void)query_checked("DELETE FROM " + std::string(tmp_table));
  };
  auto token_state_read_from_row = [](duckdb::MaterializedQueryResult &query_result, idx_t row_idx) {
    TokenState loaded_state;
    loaded_state.pos = static_cast<double>(query_result.GetValue(3, row_idx).GetValue<int64_t>());
    loaded_state.cost = static_cast<double>(query_result.GetValue(4, row_idx).GetValue<int64_t>());
    loaded_state.lp = static_cast<double>(query_result.GetValue(5, row_idx).GetValue<int64_t>());
    loaded_state.entry_block = static_cast<double>(query_result.GetValue(6, row_idx).GetValue<int64_t>());
    return loaded_state;
  };
  auto query_dense_feature_max_bucket = [&]() {
    auto r = query_checked("SELECT COALESCE(MAX(block_bucket), -1) FROM " + table_feature_tensor);
    return r->GetValue(0, 0).GetValue<int64_t>();
  };
  auto fill_dense_quiet_feature_bucket = [&](int64_t target_bucket) {
    assert(target_bucket >= 0);
    if (target_bucket == 0) {
      return;
    }
    struct QuietPrevRow {
      std::string user_blob;
      int32_t tag_id = -1;
      int64_t last_sort_key = 0;
      int64_t last_block = 0;
      int64_t last_exposure = 0;
      __int128 last_holding_exp = 0;
      int64_t last_token_count = 0;
      int64_t ps_token = 0;
      int64_t ps_exposure = 0;
      int64_t ps_volume = 0;
      int64_t ps_holding = 0;
      int64_t updated_sort_key = 0;
    };
    const int64_t prev_bucket = target_bucket - 1;
    auto prev_result = query_checked(
        "SELECT user_addr, tag_id, last_sort_key_10w, last_block_10w, last_exposure_10w, "
        "last_holding_period_10w, last_token_count_10w, "
        "ps_token_avg_10w, ps_exposure_avg_10w, ps_volume_10w, ps_holding_period_avg_10w, updated_sort_key "
        "FROM " +
        table_feature_tensor + " WHERE block_bucket = " + std::to_string(prev_bucket) +
        " ORDER BY user_addr, tag_id");
    if (prev_result->RowCount() == 0) {
      return;
    }

    std::vector<QuietPrevRow> prev_rows;
    prev_rows.reserve(prev_result->RowCount());
    std::vector<std::string> sharpe_users;
    sharpe_users.reserve(prev_result->RowCount() / 16 + 1);
    for (idx_t i = 0; i < prev_result->RowCount(); ++i) {
      QuietPrevRow row;
      row.user_blob = prev_result->GetValue(0, i).GetValueUnsafe<std::string>();
      row.tag_id = prev_result->GetValue(1, i).GetValue<int32_t>();
      row.last_sort_key = prev_result->GetValue(2, i).GetValue<int64_t>();
      row.last_block = prev_result->GetValue(3, i).GetValue<int64_t>();
      row.last_exposure = prev_result->GetValue(4, i).GetValue<int64_t>();
      row.last_holding_exp = hugeint_to_i128(prev_result->GetValue(5, i).GetValue<duckdb::hugeint_t>());
      row.last_token_count = prev_result->GetValue(6, i).GetValue<int64_t>();
      row.ps_token = prev_result->GetValue(7, i).GetValue<int64_t>();
      row.ps_exposure = prev_result->GetValue(8, i).GetValue<int64_t>();
      row.ps_volume = prev_result->GetValue(9, i).GetValue<int64_t>();
      row.ps_holding = prev_result->GetValue(10, i).GetValue<int64_t>();
      row.updated_sort_key = prev_result->GetValue(11, i).GetValue<int64_t>();
      prev_rows.push_back(row);
      if (row.tag_id == -1) {
        sharpe_users.push_back(row.user_blob);
      }
    }

    std::unordered_map<BlobTagKey, PrefixSumHistoryRecord, BlobTagKeyHasher> boundary_100_by_key;
    std::unordered_map<BlobTagKey, PrefixSumHistoryRecord, BlobTagKeyHasher> boundary_1000_by_key;
    auto load_boundary_map =
        [&](int64_t boundary_bucket,
            std::unordered_map<BlobTagKey, PrefixSumHistoryRecord, BlobTagKeyHasher> &out) {
          if (boundary_bucket < 0) {
            return;
          }
          auto boundary_result = query_checked(
              "SELECT user_addr, tag_id, ps_token_avg_10w, ps_exposure_avg_10w, "
              "ps_volume_10w, ps_holding_period_avg_10w "
              "FROM " +
              table_feature_tensor + " WHERE block_bucket = " + std::to_string(boundary_bucket));
          out.reserve(boundary_result->RowCount());
          for (idx_t i = 0; i < boundary_result->RowCount(); ++i) {
            BlobTagKey key{
                boundary_result->GetValue(0, i).GetValueUnsafe<std::string>(),
                boundary_result->GetValue(1, i).GetValue<int32_t>(),
            };
            out.emplace(
                std::move(key),
                PrefixSumHistoryRecord{
                    boundary_bucket,
                    boundary_result->GetValue(2, i).GetValue<int64_t>(),
                    boundary_result->GetValue(3, i).GetValue<int64_t>(),
                    boundary_result->GetValue(4, i).GetValue<int64_t>(),
                    boundary_result->GetValue(5, i).GetValue<int64_t>(),
                });
          }
        };
    load_boundary_map(target_bucket - 10, boundary_100_by_key);
    load_boundary_map(target_bucket - 100, boundary_1000_by_key);

    std::unordered_map<std::string, UserSharpeCacheState> quiet_sharpe_cache_by_user;
    quiet_sharpe_cache_by_user.reserve(sharpe_users.size());
    if (!sharpe_users.empty()) {
      tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
        for (const std::string &user_blob : sharpe_users) {
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.EndRow();
          quiet_sharpe_cache_by_user.try_emplace(user_blob, UserSharpeCacheState{});
        }
        ap.Close();
      }
      const int64_t start_bucket = std::max<int64_t>(0, target_bucket - 99);
      auto anchor_result = query_checked(
          "SELECT x.user_addr, x.close_pnl "
          "FROM ("
          "  SELECT s.user_addr, s.close_pnl, "
          "         ROW_NUMBER() OVER (PARTITION BY t.user_addr ORDER BY s.block_bucket DESC) AS rn "
          "  FROM " +
          table_account_bucket_pnl + " s "
                                     "  JOIN " +
          std::string(kSqlTmpTouchedUsers) +
          " t ON s.user_addr = t.user_addr "
          "  WHERE s.block_bucket < " +
          std::to_string(start_bucket) +
          ") x WHERE x.rn = 1");
      for (idx_t i = 0; i < anchor_result->RowCount(); ++i) {
        const std::string user_blob = anchor_result->GetValue(0, i).GetValueUnsafe<std::string>();
        quiet_sharpe_cache_by_user[user_blob].pnl_before_first_bucket =
            anchor_result->GetValue(1, i).GetValue<int64_t>();
      }
      auto history_result = query_checked(
          "SELECT s.user_addr, s.block_bucket, s.samples_blob, s.close_pnl, s.min_pnl, s.updated_sort_key "
          "FROM " +
          table_account_bucket_pnl + " s "
                                     "JOIN " +
          std::string(kSqlTmpTouchedUsers) +
          " t ON s.user_addr = t.user_addr "
          "WHERE s.block_bucket >= " +
          std::to_string(start_bucket) + " AND s.block_bucket <= " + std::to_string(target_bucket) +
          " ORDER BY s.user_addr, s.block_bucket");
      for (idx_t i = 0; i < history_result->RowCount(); ++i) {
        const std::string user_blob = history_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto cache_it = quiet_sharpe_cache_by_user.find(user_blob);
        assert(cache_it != quiet_sharpe_cache_by_user.end());
        UserSharpeCacheState &cache = cache_it->second;
        cache.buckets.push_back(AccountBucketPnlState{
            history_result->GetValue(1, i).GetValue<int64_t>(),
            {},
            history_result->GetValue(3, i).GetValue<int64_t>(),
            history_result->GetValue(4, i).GetValue<int64_t>(),
            history_result->GetValue(5, i).GetValue<int64_t>(),
        });
        decode_account_bucket_samples_blob(
            history_result->GetValue(2, i).GetValueUnsafe<std::string>(),
            cache.buckets.back().samples);
      }
    }

    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      const int64_t bucket_start_block = target_bucket * kBlockBucketSize;
      const int64_t denom_100 = std::min<int64_t>(10, target_bucket + 1);
      const int64_t denom_1000 = std::min<int64_t>(100, target_bucket + 1);
      for (const QuietPrevRow &row : prev_rows) {
        assert(row.last_exposure >= 0);
        assert(row.last_token_count >= 0);
        const int64_t delta_to_start = bucket_start_block - row.last_block;
        assert(delta_to_start >= 0);
        const __int128 start_holding_exp =
            feature_comp::advance_holding_exp(row.last_holding_exp, row.last_exposure, delta_to_start);
        const int64_t token_avg_10w = row.last_token_count;
        const int64_t exposure_avg_10w = row.last_exposure;
        const int64_t volume_10w = 0;
        const int64_t time_weight_sum = kBlockBucketSize;
        const int64_t token_count_tw_sum =
            i64_narrow_checked(static_cast<__int128>(token_avg_10w) * static_cast<__int128>(kBlockBucketSize));
        const __int128 exposure_tw_sum =
            static_cast<__int128>(exposure_avg_10w) * static_cast<__int128>(kBlockBucketSize);
        const __int128 holding_period_exp_tw_sum =
            feature_comp::linear_series_i128(start_holding_exp, exposure_avg_10w, kBlockBucketSize);
        const int64_t holding_period_avg_10w =
            (exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(holding_period_exp_tw_sum) /
                                                i128_to_long_double(exposure_tw_sum)))
                : 0;
        const int64_t ps_token_avg =
            i64_narrow_checked(static_cast<__int128>(row.ps_token) + static_cast<__int128>(token_avg_10w));
        const int64_t ps_exposure_avg =
            i64_narrow_checked(static_cast<__int128>(row.ps_exposure) + static_cast<__int128>(exposure_avg_10w));
        const int64_t ps_volume = row.ps_volume;
        const int64_t ps_holding_period_avg =
            i64_narrow_checked(static_cast<__int128>(row.ps_holding) + static_cast<__int128>(holding_period_avg_10w));

        const PrefixSumHistoryRecord kZeroBoundary{};
        const auto boundary_100_it = boundary_100_by_key.find(BlobTagKey{row.user_blob, row.tag_id});
        const auto boundary_1000_it = boundary_1000_by_key.find(BlobTagKey{row.user_blob, row.tag_id});
        const PrefixSumHistoryRecord &boundary_100 =
            (boundary_100_it != boundary_100_by_key.end()) ? boundary_100_it->second : kZeroBoundary;
        const PrefixSumHistoryRecord &boundary_1000 =
            (boundary_1000_it != boundary_1000_by_key.end()) ? boundary_1000_it->second : kZeroBoundary;

        const int64_t token_avg_100w = (denom_100 > 0)
                                           ? feature_comp::round_i64(static_cast<double>(
                                                 i64_narrow_checked(static_cast<__int128>(ps_token_avg) -
                                                                    static_cast<__int128>(boundary_100.ps_token_avg))) /
                                                                     static_cast<double>(denom_100))
                                           : 0;
        const int64_t token_avg_1000w = (denom_1000 > 0)
                                            ? feature_comp::round_i64(static_cast<double>(
                                                  i64_narrow_checked(static_cast<__int128>(ps_token_avg) -
                                                                     static_cast<__int128>(boundary_1000.ps_token_avg))) /
                                                                      static_cast<double>(denom_1000))
                                            : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0)
                ? feature_comp::round_i64(static_cast<double>(
                                              i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) -
                                                                 static_cast<__int128>(boundary_100.ps_exposure_avg))) /
                                          static_cast<double>(denom_100))
                : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0)
                ? feature_comp::round_i64(static_cast<double>(
                                              i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) -
                                                                 static_cast<__int128>(boundary_1000.ps_exposure_avg))) /
                                          static_cast<double>(denom_1000))
                : 0;
        const int64_t volume_avg_100w = (denom_100 > 0)
                                            ? feature_comp::round_i64(static_cast<double>(
                                                  i64_narrow_checked(static_cast<__int128>(ps_volume) -
                                                                     static_cast<__int128>(boundary_100.ps_volume))) /
                                                                      static_cast<double>(denom_100))
                                            : 0;
        const int64_t volume_avg_1000w = (denom_1000 > 0)
                                             ? feature_comp::round_i64(static_cast<double>(
                                                   i64_narrow_checked(static_cast<__int128>(ps_volume) -
                                                                      static_cast<__int128>(boundary_1000.ps_volume))) /
                                                                       static_cast<double>(denom_1000))
                                             : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0)
                ? feature_comp::round_i64(static_cast<double>(
                                              i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) -
                                                                 static_cast<__int128>(boundary_100.ps_holding_period_avg))) /
                                          static_cast<double>(denom_100))
                : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0)
                ? feature_comp::round_i64(static_cast<double>(
                                              i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) -
                                                                 static_cast<__int128>(boundary_1000.ps_holding_period_avg))) /
                                          static_cast<double>(denom_1000))
                : 0;

        double sharpe_10w = 0.0;
        double sharpe_100w = 0.0;
        double sharpe_1000w = 0.0;
        if (row.tag_id == -1) {
          auto cache_it = quiet_sharpe_cache_by_user.find(row.user_blob);
          assert(cache_it != quiet_sharpe_cache_by_user.end());
          const UserSharpeCacheState &cache = cache_it->second;
          sharpe_10w = calc_window_return_sharpe(
              cache.buckets, cache.pnl_before_first_bucket, target_bucket, target_bucket, exposure_avg_10w, kBlockBucketSize);
          sharpe_100w = calc_window_return_sharpe(
              cache.buckets,
              cache.pnl_before_first_bucket,
              std::max<int64_t>(0, target_bucket - 9),
              target_bucket,
              exposure_avg_100w,
              kBlockBucketSize);
          sharpe_1000w = calc_window_return_sharpe(
              cache.buckets,
              cache.pnl_before_first_bucket,
              std::max<int64_t>(0, target_bucket - 99),
              target_bucket,
              exposure_avg_1000w,
              kBlockBucketSize);
        }

        ap.BeginRow();
        append_blob(ap, row.user_blob);
        ap.Append(target_bucket);
        ap.Append(row.tag_id);
        ap.Append(row.last_sort_key);
        ap.Append(bucket_start_block);
        ap.Append(exposure_avg_10w);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(start_holding_exp)));
        ap.Append(token_avg_10w);
        ap.Append(time_weight_sum);
        ap.Append(token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(exposure_tw_sum)));
        ap.Append(volume_10w);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(holding_period_exp_tw_sum)));
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(row.updated_sort_key);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                  "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) +
        " ON CONFLICT(user_addr, block_bucket, tag_id) DO NOTHING");
  };
  int64_t dense_feature_max_bucket = query_dense_feature_max_bucket();
  // L2: source ingestion
  std::vector<core::rocks::Stage2UserEventRecord> source_event_rows = builder_.user_event_store().scan_by_sort_key(
      sync_cursor_.sort_key, head_sort_key, static_cast<size_t>(kStage3BatchEvents));
  if (source_event_rows.empty()) {
    (void)query_checked("BEGIN");
    const int64_t head_bucket = feature_comp::sort_key_to_block_bucket(head_sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    while (dense_feature_max_bucket < head_bucket) {
      fill_dense_quiet_feature_bucket(dense_feature_max_bucket + 1);
      ++dense_feature_max_bucket;
    }
    sync_cursor_.sort_key = head_sort_key;
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
    return true;
  }

  std::vector<EventInput> event_input_rows;
  event_input_rows.reserve(source_event_rows.size());
  std::vector<std::string> user_blob_by_user_id;
  user_blob_by_user_id.reserve(source_event_rows.size() / 10 + 1);
  std::unordered_map<std::string, uint32_t> user_id_by_user_blob;
  user_id_by_user_blob.reserve(source_event_rows.size() / 10 + 1);
  auto intern_user_id_from_blob = [&](const std::string &user_blob) {
    auto user_id_it = user_id_by_user_blob.find(user_blob);
    if (user_id_it != user_id_by_user_blob.end()) {
      return user_id_it->second;
    }
    const uint32_t user_id = static_cast<uint32_t>(user_blob_by_user_id.size());
    user_blob_by_user_id.push_back(user_blob);
    user_id_by_user_blob.emplace(user_blob_by_user_id.back(), user_id);
    return user_id;
  };
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  std::string prev_user_blob;
  int32_t prev_cond_idx = std::numeric_limits<int32_t>::min();
  int32_t prev_event_type = std::numeric_limits<int32_t>::min();
  int32_t prev_token_idx = std::numeric_limits<int32_t>::min();
  for (const auto &src : source_event_rows) {
    const std::string &user_blob = src.user_addr;
    stage2_assert(user_blob.size() == 20, AssertLevel::L0, "Data", "Stage2UserAddrLen20");
    EventInput row;
    row.user_id = intern_user_id_from_blob(user_blob);
    row.sort_key = src.sort_key;
    row.cond_idx = src.cond_idx;
    row.event_type = src.event_type;
    row.token_idx = src.token_idx;
    row.collateral = src.collateral;
    row.amount = src.amount;
    row.price = src.price;
    if (has_prev_key) {
      const int user_cmp = blob_unsigned_compare(user_blob, prev_user_blob);
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
    event_input_rows.push_back(row);
  }

  if (source_event_rows.size() == static_cast<size_t>(kStage3BatchEvents)) {
    assert(!event_input_rows.empty());
    const EventInput &last_row = event_input_rows.back();
    const int64_t last_block = sort_key_to_block(last_row.sort_key);
    size_t cut = event_input_rows.size();
    while (cut > 0 && sort_key_to_block(event_input_rows[cut - 1].sort_key) == last_block) {
      --cut;
    }
    // 不探测尾部,命中 LIMIT 时直接丢弃末尾 block,下个 chunk 再补。
    // 约束:单个 block 事件数必须小于 batch 上限,否则会一直丢空。
    assert(cut > 0);
    event_input_rows.resize(cut);
  }
  assert(!event_input_rows.empty());

  // L3: bootstrap runtime states
  std::unordered_map<TokenKey, TokenState, TokenKeyHash> token_state_by_token_key;
  token_state_by_token_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<uint32_t, int64_t> event_increment_by_user_id;
  event_increment_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  std::unordered_map<uint32_t, int64_t> last_sort_key_by_user_id;
  last_sort_key_by_user_id.reserve(static_cast<size_t>(event_input_rows.size() / 10 + 1));
  for (const auto &row : event_input_rows) {
    event_increment_by_user_id[row.user_id]++;
    last_sort_key_by_user_id[row.user_id] = row.sort_key;
    if (row.cond_idx >= 0) {
      TokenKey key{row.user_id, row.cond_idx, row.token_idx};
      token_state_by_token_key.try_emplace(key, TokenState{});
    }
  }
  const EventInput &last_row = event_input_rows.back();
  sync_cursor_.sort_key = last_row.sort_key;
  sync_cursor_.processed_events += static_cast<int64_t>(event_input_rows.size());

  int32_t max_cond_idx = -1;
  bool has_convert_event = false;
  for (const auto &row : event_input_rows) {
    if (row.cond_idx > max_cond_idx) {
      max_cond_idx = row.cond_idx;
    }
    if (row.event_type == static_cast<int32_t>(EventType::Convert) && row.cond_idx >= 0) {
      has_convert_event = true;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  if (has_convert_event) {
    bool needs_refresh = false;
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      if (static_cast<size_t>(row.cond_idx) >= cond_market_question_counts_.size() ||
          cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] < 2) {
        needs_refresh = true;
        break;
      }
    }
    if (needs_refresh) {
      const_cast<StageSync *>(this)->load_conditions();
    }
    for (const auto &row : event_input_rows) {
      if (row.event_type != static_cast<int32_t>(EventType::Convert) || row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_market_question_counts_.size());
      assert(cond_market_question_counts_[static_cast<size_t>(row.cond_idx)] >= 2);
    }
  }

  // L4: runtime pair states and bucket states
  // Use double for cumulative PnL tracking to match internal state
  std::unordered_map<uint32_t, double> realized_total_by_user_id;
  std::unordered_map<uint32_t, double> unrealized_total_by_user_id;
  std::unordered_map<uint32_t, int32_t> active_token_count_by_user_id;
  realized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  unrealized_total_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  active_token_count_by_user_id.reserve(event_increment_by_user_id.size() + 1);
  if (!event_increment_by_user_id.empty()) {
    tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
      for (const auto &[user_id, _] : event_increment_by_user_id) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_from_user_summary_with_touched_users =
        "FROM " + table_user_summary + " s " +
        "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
    auto summary_seed_result = query_checked(std::string(kSqlSelectSummarySeedCols) + sql_from_user_summary_with_touched_users);
    for (idx_t i = 0; i < summary_seed_result->RowCount(); ++i) {
      const std::string user_blob = summary_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      realized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(1, i).GetValue<int64_t>()));
      unrealized_total_by_user_id.emplace(user_id, static_cast<double>(summary_seed_result->GetValue(2, i).GetValue<int64_t>()));
      active_token_count_by_user_id.emplace(user_id, summary_seed_result->GetValue(3, i).GetValue<int32_t>());
    }
    for (const auto &[user_id, _] : event_increment_by_user_id) {
      if (!realized_total_by_user_id.count(user_id)) {
        realized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!unrealized_total_by_user_id.count(user_id)) {
        unrealized_total_by_user_id.emplace(user_id, 0.0);
      }
      if (!active_token_count_by_user_id.count(user_id)) {
        active_token_count_by_user_id.emplace(user_id, 0);
      }
    }
  }

  struct UserTagRuntimePairKey {
    uint32_t user_id = 0;
    int8_t tag_id = 13;
    bool operator==(const UserTagRuntimePairKey &o) const {
      return user_id == o.user_id && tag_id == o.tag_id;
    }
  };
  struct UserTagRuntimePairKeyHasher {
    size_t operator()(const UserTagRuntimePairKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(static_cast<int32_t>(k.tag_id)) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  struct UserTagRuntimePairState {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry_sum = 0;
  };
  std::unordered_map<UserTagRuntimePairKey, UserTagRuntimePairState, UserTagRuntimePairKeyHasher> runtime_pair_state_by_pair_key;
  runtime_pair_state_by_pair_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> zero_seed_runtime_pair_key_set;
  zero_seed_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));

  auto token_feature_contrib = [&](const TokenState &st) {
    struct TokenFeatureContrib {
      int64_t token_count = 0;
      int64_t exposure = 0;
      __int128 exposure_entry = 0;
    };
    TokenFeatureContrib c;
    c.token_count = is_effective_holding_i64(feature_comp::round_i64(st.pos)) ? 1 : 0;
    c.exposure = feature_comp::calc_exposure_1e6(
        feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
    const int64_t entry_block = feature_comp::round_i64(st.entry_block);
    c.exposure_entry = (c.exposure > 0) ? static_cast<__int128>(c.exposure) * entry_block : 0;
    return c;
  };

  auto apply_runtime_contrib_delta = [&](uint32_t user_id, int8_t tag_id, int64_t token_delta,
                                         int64_t exposure_delta, __int128 exposure_entry_delta) {
    UserTagRuntimePairKey key{user_id, tag_id};
    auto [runtime_state_it, _] = runtime_pair_state_by_pair_key.try_emplace(key, UserTagRuntimePairState{});
    UserTagRuntimePairState &state = runtime_state_it->second;
    state.token_count = i64_narrow_checked(static_cast<__int128>(state.token_count) + token_delta);
    state.exposure = i64_narrow_checked(static_cast<__int128>(state.exposure) + exposure_delta);
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  if (!token_state_by_token_key.empty()) {
    tmp_table_reset(kSqlTmpTouchedTokenKeys, kSqlTmpSchemaTouchedTokenKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedTokenKeys);
      for (const auto &[key, _] : token_state_by_token_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }

    const std::string sql_from_token_state_with_touched_tokens =
        "FROM " + table_token_state + " s " +
        "JOIN " + std::string(kSqlTmpTouchedTokenKeys) +
        " t ON s.user_addr = t.user_addr AND s.cond_idx = t.cond_idx AND s.token_idx = t.token_idx";
    auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_tokens);
    for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
      const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
      const int32_t token_idx = token_state_result->GetValue(2, i).GetValue<int32_t>();
      TokenKey key{user_id, cond_idx, token_idx};
      auto token_state_it = token_state_by_token_key.find(key);
      assert(token_state_it != token_state_by_token_key.end());
      token_state_it->second = token_state_read_from_row(*token_state_result, i);
    }
  }

  std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> touched_runtime_pair_key_set;
  touched_runtime_pair_key_set.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key;
  bucket_agg_state_by_agg_key.reserve(static_cast<size_t>(event_input_rows.size() / 2 + 1));
  for (const auto &row : event_input_rows) {
    if (row.cond_idx < 0) {
      continue;
    }
    assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
    const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(row.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, tag_id}, BucketAggState{});
    bucket_agg_state_by_agg_key.try_emplace(AggKey{row.user_id, block_bucket, -1}, BucketAggState{});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, tag_id});
    touched_runtime_pair_key_set.insert(UserTagRuntimePairKey{row.user_id, -1});
  }

  std::unordered_map<uint32_t, int64_t> min_dirty_account_bucket_by_user;
  min_dirty_account_bucket_by_user.reserve(bucket_agg_state_by_agg_key.size() / 2 + 1);
  for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
    if (key.tag_id != -1) {
      continue;
    }
    auto [it, inserted] = min_dirty_account_bucket_by_user.emplace(key.user_id, key.block_bucket);
    if (!inserted) {
      it->second = std::min(it->second, key.block_bucket);
    }
  }
  if (!min_dirty_account_bucket_by_user.empty()) {
    struct SharpeCacheLoadSpec {
      uint32_t user_id = 0;
      int64_t start_bucket = 0;
      int64_t end_bucket = -1;
    };
    std::vector<SharpeCacheLoadSpec> sharpe_cache_load_specs;
    sharpe_cache_load_specs.reserve(min_dirty_account_bucket_by_user.size());
    for (const auto &[user_id, min_dirty_bucket] : min_dirty_account_bucket_by_user) {
      const std::string &user_blob = user_blob_by_user_id[user_id];
      const int64_t start_bucket = std::max<int64_t>(0, min_dirty_bucket - 99);
      auto cache_it = sharpe_cache_by_user_blob_.find(user_blob);
      if (cache_it == sharpe_cache_by_user_blob_.end() ||
          cache_it->second.buckets.empty() ||
          cache_it->second.buckets.front().block_bucket > start_bucket) {
        sharpe_cache_load_specs.push_back({user_id, start_bucket, min_dirty_bucket - 1});
      }
    }
    if (!sharpe_cache_load_specs.empty()) {
      tmp_table_reset(kSqlTmpSharpeCacheLoad, kSqlTmpSchemaSharpeCacheLoad);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpSharpeCacheLoad);
        for (const auto &spec : sharpe_cache_load_specs) {
          ap.BeginRow();
          append_blob(ap, user_blob_by_user_id[spec.user_id]);
          ap.Append(spec.start_bucket);
          ap.Append(spec.end_bucket);
          ap.EndRow();
        }
        ap.Close();
      }

      std::unordered_map<std::string, int64_t> anchor_pnl_by_user;
      anchor_pnl_by_user.reserve(sharpe_cache_load_specs.size());
      auto anchor_result = query_checked(
          "SELECT x.user_addr, x.close_pnl "
          "FROM ("
          "  SELECT s.user_addr, s.close_pnl, "
          "         ROW_NUMBER() OVER (PARTITION BY r.user_addr ORDER BY s.block_bucket DESC) AS rn "
          "  FROM " +
          table_account_bucket_pnl + " s "
                                     "  JOIN " +
          std::string(kSqlTmpSharpeCacheLoad) +
          " r ON s.user_addr = r.user_addr "
          "  WHERE s.block_bucket < r.start_bucket"
          ") x WHERE x.rn = 1");
      for (idx_t i = 0; i < anchor_result->RowCount(); ++i) {
        const std::string user_blob = anchor_result->GetValue(0, i).GetValueUnsafe<std::string>();
        anchor_pnl_by_user.emplace(user_blob, anchor_result->GetValue(1, i).GetValue<int64_t>());
      }

      for (const auto &spec : sharpe_cache_load_specs) {
        const std::string &user_blob = user_blob_by_user_id[spec.user_id];
        auto &cache = sharpe_cache_by_user_blob_[user_blob];
        cache = UserSharpeCacheState{};
        auto anchor_it = anchor_pnl_by_user.find(user_blob);
        if (anchor_it != anchor_pnl_by_user.end()) {
          cache.pnl_before_first_bucket = anchor_it->second;
        }
      }

      auto history_result = query_checked(
          "SELECT s.user_addr, s.block_bucket, s.samples_blob, s.close_pnl, s.min_pnl, s.updated_sort_key "
          "FROM " +
          table_account_bucket_pnl + " s "
                                     "JOIN " +
          std::string(kSqlTmpSharpeCacheLoad) +
          " r ON s.user_addr = r.user_addr "
          "WHERE s.block_bucket >= r.start_bucket AND s.block_bucket <= r.end_bucket "
          "ORDER BY s.user_addr, s.block_bucket");
      for (idx_t i = 0; i < history_result->RowCount(); ++i) {
        const std::string user_blob = history_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto cache_it = sharpe_cache_by_user_blob_.find(user_blob);
        assert(cache_it != sharpe_cache_by_user_blob_.end());
        UserSharpeCacheState &cache = cache_it->second;
        cache.buckets.push_back(AccountBucketPnlState{
            history_result->GetValue(1, i).GetValue<int64_t>(),
            {},
            history_result->GetValue(3, i).GetValue<int64_t>(),
            history_result->GetValue(4, i).GetValue<int64_t>(),
            history_result->GetValue(5, i).GetValue<int64_t>(),
        });
        decode_account_bucket_samples_blob(
            history_result->GetValue(2, i).GetValueUnsafe<std::string>(),
            cache.buckets.back().samples);
      }
    }
  }

  if (!touched_runtime_pair_key_set.empty()) {
    tmp_table_reset(kSqlTmpTouchedUserTags, kSqlTmpSchemaTouchedUserTags);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUserTags);
      for (const auto &key : touched_runtime_pair_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }
    const std::string sql_runtime_seed =
        "SELECT x.user_addr, x.tag_id, x.last_block_10w, x.last_exposure_10w, "
        "x.last_holding_period_10w, x.last_token_count_10w "
        "FROM ("
        "  SELECT f.user_addr, f.tag_id, f.last_block_10w, f.last_exposure_10w, "
        "         f.last_holding_period_10w, f.last_token_count_10w, "
        "         ROW_NUMBER() OVER (PARTITION BY f.user_addr, f.tag_id ORDER BY f.block_bucket DESC) AS rn "
        "  FROM " +
        table_feature_tensor + " f "
                               "  JOIN " +
        std::string(kSqlTmpTouchedUserTags) +
        " t ON f.user_addr = t.user_addr AND f.tag_id = t.tag_id"
        ") x WHERE x.rn = 1";
    auto runtime_seed_result = query_checked(sql_runtime_seed);
    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> seeded_runtime_pair_key_set;
    seeded_runtime_pair_key_set.reserve(touched_runtime_pair_key_set.size() + 1);
    for (idx_t i = 0; i < runtime_seed_result->RowCount(); ++i) {
      const std::string user_blob = runtime_seed_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const uint32_t user_id = user_id_it->second;
      const int8_t tag_id = static_cast<int8_t>(runtime_seed_result->GetValue(1, i).GetValue<int32_t>());
      seeded_runtime_pair_key_set.insert(UserTagRuntimePairKey{user_id, tag_id});
      const int64_t last_block = runtime_seed_result->GetValue(2, i).GetValue<int64_t>();
      const int64_t last_exposure = runtime_seed_result->GetValue(3, i).GetValue<int64_t>();
      const __int128 last_holding_exp = hugeint_to_i128(runtime_seed_result->GetValue(4, i).GetValue<duckdb::hugeint_t>());
      const int64_t last_token_count = runtime_seed_result->GetValue(5, i).GetValue<int64_t>();
      assert(last_exposure >= 0);
      assert(last_token_count >= 0);
      __int128 exposure_entry_sum = 0;
      if (last_exposure > 0) {
        const __int128 computed = static_cast<__int128>(last_block) * last_exposure - last_holding_exp;
        assert(computed >= 0);
        exposure_entry_sum = computed;
      } else {
        assert(last_holding_exp == 0);
      }
      UserTagRuntimePairState state;
      state.token_count = last_token_count;
      state.exposure = last_exposure;
      state.exposure_entry_sum = exposure_entry_sum;
      runtime_pair_state_by_pair_key[UserTagRuntimePairKey{user_id, tag_id}] = state;
    }

    std::unordered_set<UserTagRuntimePairKey, UserTagRuntimePairKeyHasher> runtime_seed_missing_pair_key_set;
    runtime_seed_missing_pair_key_set.reserve(touched_runtime_pair_key_set.size());
    for (const auto &pair_key : touched_runtime_pair_key_set) {
      if (!seeded_runtime_pair_key_set.count(pair_key)) {
        runtime_seed_missing_pair_key_set.insert(pair_key);
      }
    }
    if (!runtime_seed_missing_pair_key_set.empty()) {
      std::unordered_set<uint32_t> runtime_seed_scan_users;
      runtime_seed_scan_users.reserve(runtime_seed_missing_pair_key_set.size());
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        if (pair_key.tag_id == -1) {
          runtime_seed_scan_users.insert(pair_key.user_id);
        }
      }
      if (!runtime_seed_scan_users.empty()) {
        tmp_table_reset(kSqlTmpTouchedUsers, kSqlTmpSchemaTouchedUsers);
        {
          duckdb::Appender ap(*sink_connection, kSqlTmpTouchedUsers);
          for (uint32_t user_id : runtime_seed_scan_users) {
            const std::string &user_blob = user_blob_by_user_id[user_id];
            ap.BeginRow();
            append_blob(ap, user_blob);
            ap.EndRow();
          }
          ap.Close();
        }
        const std::string sql_from_token_state_with_touched_users =
            "FROM " + table_token_state + " s " +
            "JOIN " + std::string(kSqlTmpTouchedUsers) + " t ON s.user_addr = t.user_addr";
        auto token_state_result = query_checked(std::string(kSqlSelectTokenStateCols) + sql_from_token_state_with_touched_users);
        for (idx_t i = 0; i < token_state_result->RowCount(); ++i) {
          const std::string user_blob = token_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
          auto user_id_it = user_id_by_user_blob.find(user_blob);
          if (user_id_it == user_id_by_user_blob.end()) {
            continue;
          }
          const uint32_t user_id = user_id_it->second;
          const int32_t cond_idx = token_state_result->GetValue(1, i).GetValue<int32_t>();
          assert(cond_idx >= 0);
          assert(static_cast<size_t>(cond_idx) < cond_tag_ids_.size());
          const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(cond_idx)];
          const TokenState loaded_state = token_state_read_from_row(*token_state_result, i);
          const auto contrib = token_feature_contrib(loaded_state);
          if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, tag_id})) {
            apply_runtime_contrib_delta(user_id, tag_id, contrib.token_count, contrib.exposure, contrib.exposure_entry);
          }
          if (runtime_seed_missing_pair_key_set.count(UserTagRuntimePairKey{user_id, -1})) {
            apply_runtime_contrib_delta(user_id, -1, contrib.token_count, contrib.exposure, contrib.exposure_entry);
          }
        }
      }
      for (const auto &pair_key : runtime_seed_missing_pair_key_set) {
        if (runtime_pair_state_by_pair_key.count(pair_key)) {
          continue;
        }
        runtime_pair_state_by_pair_key.try_emplace(pair_key, UserTagRuntimePairState{});
        zero_seed_runtime_pair_key_set.insert(pair_key);
      }
    }
  }

  if (!zero_seed_runtime_pair_key_set.empty()) {
    for (const auto &[token_key, st] : token_state_by_token_key) {
      assert(token_key.cond_idx >= 0);
      assert(static_cast<size_t>(token_key.cond_idx) < cond_tag_ids_.size());
      const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(token_key.cond_idx)];
      const auto contrib = token_feature_contrib(st);
      if (contrib.token_count == 0 && contrib.exposure == 0 && contrib.exposure_entry == 0) {
        continue;
      }
      assert(!zero_seed_runtime_pair_key_set.count(UserTagRuntimePairKey{token_key.user_id, tag_id}));
      assert(!zero_seed_runtime_pair_key_set.count(UserTagRuntimePairKey{token_key.user_id, -1}));
    }
  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
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
    auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
    for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
      const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      AggKey key{
          user_id_it->second,
          feature_state_result->GetValue(1, i).GetValue<int64_t>(),
          static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
      };
      auto bucket_agg_it = bucket_agg_state_by_agg_key.find(key);
      assert(bucket_agg_it != bucket_agg_state_by_agg_key.end());
      BucketAggState &agg = bucket_agg_it->second;
      agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
      agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
      agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
      agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
      agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
      agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
      agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
      agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
      agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
      agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
      agg.has_tail = (agg.time_weight_sum > 0);
    }
  }
  std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher>
      prefix_sum_history_by_pair_key;

  // L5: event stream apply + per-bucket dense feature flush
  std::vector<EventFact> event_fact_rows;
  event_fact_rows.reserve(event_input_rows.size());
  std::unordered_set<TokenKey, TokenKeyHash> dirty_token_key_set;
  dirty_token_key_set.reserve(token_state_by_token_key.size());
  struct AccountBucketKey {
    uint32_t user_id = 0;
    int64_t block_bucket = 0;
    bool operator==(const AccountBucketKey &o) const {
      return user_id == o.user_id && block_bucket == o.block_bucket;
    }
  };
  struct AccountBucketKeyHasher {
    size_t operator()(const AccountBucketKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int64_t>()(k.block_bucket) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };
  std::unordered_set<AccountBucketKey, AccountBucketKeyHasher> dirty_account_bucket_key_set;
  dirty_account_bucket_key_set.reserve(event_input_rows.size() / 10 + 1);
  auto token_state_rounded_equal = [&](const TokenState &lhs, const TokenState &rhs) {
    return feature_comp::round_i64(lhs.pos) == feature_comp::round_i64(rhs.pos) &&
           feature_comp::round_i64(lhs.cost) == feature_comp::round_i64(rhs.cost) &&
           feature_comp::round_i64(lhs.lp) == feature_comp::round_i64(rhs.lp) &&
           feature_comp::round_i64(lhs.entry_block) == feature_comp::round_i64(rhs.entry_block);
  };
  auto prune_user_sharpe_cache = [&](UserSharpeCacheState &cache, int64_t min_bucket_to_keep) {
    while (!cache.buckets.empty() && cache.buckets.front().block_bucket < min_bucket_to_keep) {
      cache.pnl_before_first_bucket = cache.buckets.front().close_pnl;
      cache.buckets.pop_front();
    }
  };
  auto update_account_bucket_cache =
      [&](uint32_t user_id,
          int64_t sort_key,
          int64_t prev_account_pnl,
          int64_t pnl,
          std::unordered_set<AccountBucketKey, AccountBucketKeyHasher> &dirty_bucket_keys) {
        const std::string &user_blob = user_blob_by_user_id[user_id];
        auto [cache_it, inserted] = sharpe_cache_by_user_blob_.try_emplace(user_blob, UserSharpeCacheState{});
        (void)inserted;
        UserSharpeCacheState &cache = cache_it->second;
        if (cache.buckets.empty()) {
          cache.pnl_before_first_bucket = prev_account_pnl;
        }
        const int64_t row_block = sort_key_to_block(sort_key);
        const int64_t block_bucket =
            feature_comp::sort_key_to_block_bucket(sort_key, SORT_KEY_SCALE, kBlockBucketSize);
        int64_t prev_pnl = cache.pnl_before_first_bucket;
        if (!cache.buckets.empty()) {
          prev_pnl = cache.buckets.back().close_pnl;
        }
        bool created_new_bucket = false;
        if (cache.buckets.empty() || cache.buckets.back().block_bucket < block_bucket) {
          if (pnl == prev_pnl) {
            return;
          }
          cache.buckets.push_back(AccountBucketPnlState{block_bucket, {}, prev_pnl, pnl, sort_key});
          created_new_bucket = true;
        } else {
          assert(cache.buckets.back().block_bucket == block_bucket);
        }
        AccountBucketPnlState &bucket_state = cache.buckets.back();
        const int32_t block_offset = static_cast<int32_t>(row_block - block_bucket * kBlockBucketSize);
        assert(block_offset >= 0);
        assert(block_offset < kBlockBucketSize);
        if (!bucket_state.samples.empty() && bucket_state.samples.back().block_offset == block_offset) {
          if (bucket_state.samples.back().pnl == pnl) {
            return;
          }
          bucket_state.samples.back().pnl = pnl;
          bucket_state.min_pnl = bucket_state.samples.front().pnl;
          for (const auto &sample : bucket_state.samples) {
            bucket_state.min_pnl = std::min(bucket_state.min_pnl, sample.pnl);
          }
        } else {
          if (!created_new_bucket && bucket_state.close_pnl == pnl) {
            return;
          }
          assert(bucket_state.samples.empty() || bucket_state.samples.back().block_offset < block_offset);
          bucket_state.samples.push_back({block_offset, pnl});
          bucket_state.min_pnl = std::min(bucket_state.min_pnl, pnl);
        }
        bucket_state.close_pnl = pnl;
        bucket_state.updated_sort_key = sort_key;
        dirty_bucket_keys.insert(AccountBucketKey{user_id, block_bucket});
      };
  struct BucketRange {
    int64_t block_bucket = 0;
    size_t begin_idx = 0;
    size_t end_idx = 0;
  };
  std::vector<BucketRange> bucket_ranges;
  bucket_ranges.reserve(event_input_rows.size() / 1000 + 1);
  for (size_t i = 0; i < event_input_rows.size();) {
    const int64_t block_bucket =
        feature_comp::sort_key_to_block_bucket(event_input_rows[i].sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    size_t j = i + 1;
    while (j < event_input_rows.size() &&
           feature_comp::sort_key_to_block_bucket(event_input_rows[j].sort_key, SORT_KEY_SCALE, kBlockBucketSize) == block_bucket) {
      ++j;
    }
    bucket_ranges.push_back(BucketRange{block_bucket, i, j});
    i = j;
  }

  {
    static uint64_t memory_probe_counter = 0;
    ++memory_probe_counter;
    const bool should_sample_memory = (memory_probe_counter % 16 == 1);
    if (should_sample_memory) {
      const auto no_extra = [](const auto &) { return int64_t{0}; };
      const auto sharpe_cache_value_extra = [](const UserSharpeCacheState &cache) {
        int64_t extra = 0;
        extra += static_cast<int64_t>(cache.buckets.size()) * static_cast<int64_t>(sizeof(AccountBucketPnlState));
        for (const auto &bucket : cache.buckets) {
          extra += core::mem::estimate_vector_plain(bucket.samples);
        }
        return extra;
      };
      const int64_t event_input_rows_bytes = core::mem::estimate_vector_plain(event_input_rows);
      const int64_t user_blob_by_user_id_bytes =
          core::mem::estimate_vector(user_blob_by_user_id, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
      const int64_t user_index_bytes =
          core::mem::estimate_unordered_map(
              user_id_by_user_blob, [](const std::string &k) { return core::mem::estimate_string_extra(k); }, no_extra) +
          core::mem::estimate_unordered_map(event_increment_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(last_sort_key_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(realized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(unrealized_total_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(active_token_count_by_user_id, no_extra, no_extra) +
          core::mem::estimate_unordered_map(runtime_pair_state_by_pair_key, no_extra, no_extra);
      const int64_t token_state_by_token_key_bytes = core::mem::estimate_unordered_map(token_state_by_token_key, no_extra, no_extra);
      const int64_t bucket_agg_bytes = core::mem::estimate_unordered_map(bucket_agg_state_by_agg_key, no_extra, no_extra);
      const int64_t event_fact_rows_bytes = core::mem::estimate_vector_plain(event_fact_rows);
      const int64_t sharpe_cache_bytes =
          core::mem::estimate_unordered_map(
              sharpe_cache_by_user_blob_,
              [](const std::string &k) { return core::mem::estimate_string_extra(k); },
              sharpe_cache_value_extra);
      int64_t sharpe_cache_buckets = 0;
      int64_t sharpe_cache_samples = 0;
      for (const auto &[_, cache] : sharpe_cache_by_user_blob_) {
        sharpe_cache_buckets += static_cast<int64_t>(cache.buckets.size());
        for (const auto &bucket : cache.buckets) {
          sharpe_cache_samples += static_cast<int64_t>(bucket.samples.size());
        }
      }
      const int64_t total_working_set_bytes =
          event_input_rows_bytes + user_blob_by_user_id_bytes + user_index_bytes + token_state_by_token_key_bytes +
          bucket_agg_bytes + event_fact_rows_bytes;
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.event_inputs_bytes = event_input_rows_bytes;
      runtime_memory_probe_.user_blob_pool_bytes = user_blob_by_user_id_bytes;
      runtime_memory_probe_.user_index_bytes = user_index_bytes;
      runtime_memory_probe_.token_states_bytes = token_state_by_token_key_bytes;
      runtime_memory_probe_.bucket_agg_bytes = bucket_agg_bytes;
      runtime_memory_probe_.event_facts_bytes = event_fact_rows_bytes;
      runtime_memory_probe_.sharpe_cache_bytes = sharpe_cache_bytes;
      runtime_memory_probe_.sharpe_cache_users = static_cast<int64_t>(sharpe_cache_by_user_blob_.size());
      runtime_memory_probe_.sharpe_cache_buckets = sharpe_cache_buckets;
      runtime_memory_probe_.sharpe_cache_samples = sharpe_cache_samples;
      runtime_memory_probe_.total_working_set_bytes = total_working_set_bytes;
      runtime_memory_probe_.peak_working_set_bytes =
          std::max(runtime_memory_probe_.peak_working_set_bytes, total_working_set_bytes);
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    } else {
      std::lock_guard<std::mutex> lock(sync_mu_);
      runtime_memory_probe_.sharpe_cache_users = static_cast<int64_t>(sharpe_cache_by_user_blob_.size());
      runtime_memory_probe_.row_count = static_cast<int64_t>(event_input_rows.size());
      runtime_memory_probe_.max_cond_idx = max_cond_idx;
    }
  }

  for (const auto &[key, st] : token_state_by_token_key) {
    assert(key.cond_idx >= 0);
    assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
    assert(std::isfinite(st.pos));
    assert(std::isfinite(st.cost));
    assert(std::isfinite(st.lp));
    assert(std::isfinite(st.entry_block));
  }

  bool write_tx_open = false;
  {
    TraceN("s3/wr_open");
    (void)query_checked("BEGIN");
    write_tx_open = true;
  }

  auto flush_account_bucket_rows =
      [&](const std::unordered_set<AccountBucketKey, AccountBucketKeyHasher> &dirty_bucket_keys) {
        if (dirty_bucket_keys.empty()) {
          return;
        }
        TraceN("s3/wr_duck_bucket_pnl");
        tmp_table_reset(kSqlTmpAccountBucketPnlState, kSqlTmpSchemaAccountBucketPnlState);
        {
          duckdb::Appender ap(*sink_connection, kSqlTmpAccountBucketPnlState);
          for (const auto &key : dirty_bucket_keys) {
            const std::string &user_blob = user_blob_by_user_id[key.user_id];
            auto cache_it = sharpe_cache_by_user_blob_.find(user_blob);
            assert(cache_it != sharpe_cache_by_user_blob_.end());
            const UserSharpeCacheState &cache = cache_it->second;
            const auto bucket_it = std::lower_bound(
                cache.buckets.begin(),
                cache.buckets.end(),
                key.block_bucket,
                [](const AccountBucketPnlState &row, int64_t block_bucket) { return row.block_bucket < block_bucket; });
            assert(bucket_it != cache.buckets.end());
            assert(bucket_it->block_bucket == key.block_bucket);
            assert(!bucket_it->samples.empty());
            const std::string samples_blob = encode_account_bucket_samples_blob(bucket_it->samples);
            ap.BeginRow();
            append_blob(ap, user_blob);
            ap.Append(key.block_bucket);
            append_blob(ap, samples_blob);
            ap.Append(bucket_it->close_pnl);
            ap.Append(bucket_it->min_pnl);
            ap.Append(bucket_it->updated_sort_key);
            ap.EndRow();
          }
          ap.Close();
        }
        (void)query_checked(
            "INSERT INTO " + table_account_bucket_pnl + " (" + kSqlColsAccountBucketPnlState + ") "
                                                                             "SELECT " +
            kSqlColsAccountBucketPnlState + " FROM " +
            std::string(kSqlTmpAccountBucketPnlState) + " " + std::string(kSqlOnConflictAccountBucketPnlState));
      };

  auto flush_feature_rows = [&](std::unordered_map<AggKey, BucketAggState, AggKeyHash> &bucket_agg_state_by_agg_key_local) {
    if (bucket_agg_state_by_agg_key_local.empty()) {
      return;
    }
    tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
      for (const auto &[key, _] : bucket_agg_state_by_agg_key_local) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.EndRow();
      }
      ap.Close();
    }

    std::unordered_map<UserTagRuntimePairKey, std::vector<PrefixSumHistoryRecord>, UserTagRuntimePairKeyHasher>
        prefix_sum_history_by_pair_key;
    prefix_sum_history_by_pair_key.reserve(bucket_agg_state_by_agg_key_local.size());
    const std::string sql_prefix_sum_history_by_pair_key =
        "SELECT f.user_addr, f.tag_id, f.block_bucket, "
        "f.ps_token_avg_10w, f.ps_exposure_avg_10w, f.ps_volume_10w, "
        "f.ps_holding_period_avg_10w "
        "FROM (SELECT user_addr, tag_id, MIN(block_bucket) - 100 AS min_boundary "
        "      FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " GROUP BY user_addr, tag_id) k "
                                                "JOIN " +
        table_feature_tensor + " f "
                               "ON f.user_addr = k.user_addr AND f.tag_id = k.tag_id "
                               "AND f.block_bucket >= k.min_boundary "
                               "AND NOT EXISTS ("
                               "  SELECT 1 FROM " +
        std::string(kSqlTmpFeatureTensorKeys) + " cur "
                                                "  WHERE cur.user_addr = f.user_addr AND cur.tag_id = f.tag_id "
                                                "  AND cur.block_bucket = f.block_bucket"
                                                ") "
                                                "ORDER BY f.user_addr, f.tag_id, f.block_bucket DESC";
    auto prefix_sum_result = query_checked(sql_prefix_sum_history_by_pair_key);
    for (idx_t i = 0; i < prefix_sum_result->RowCount(); ++i) {
      const std::string user_blob = prefix_sum_result->GetValue(0, i).GetValueUnsafe<std::string>();
      auto user_id_it = user_id_by_user_blob.find(user_blob);
      assert(user_id_it != user_id_by_user_blob.end());
      const int8_t tag_id = static_cast<int8_t>(prefix_sum_result->GetValue(1, i).GetValue<int32_t>());
      UserTagRuntimePairKey pair_key{user_id_it->second, tag_id};
      auto [ps_it, _] = prefix_sum_history_by_pair_key.try_emplace(pair_key, std::vector<PrefixSumHistoryRecord>{});
      ps_it->second.push_back({
          prefix_sum_result->GetValue(2, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(3, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(4, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(5, i).GetValue<int64_t>(),
          prefix_sum_result->GetValue(6, i).GetValue<int64_t>(),
      });
    }

    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key_local.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key_local) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      for (const AggKey &key : sorted_feature_keys) {
        const BucketAggState &agg = bucket_agg_state_by_agg_key_local.at(key);
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        if (ps_history_it != prefix_sum_history_by_pair_key.end()) {
          const PrefixSumHistoryRecord *prev_rec =
              find_prefix_sum_history_by_pair_key_le(ps_history_it->second, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
          }
        }
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);

        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0;
            int64_t exposure = 0;
            int64_t volume = 0;
            int64_t holding = 0;
          };
          BoundaryPrefixSum result{};
          auto history_it = prefix_sum_history_by_pair_key.find(pair_key);
          if (history_it == prefix_sum_history_by_pair_key.end()) {
            return result;
          }
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(history_it->second, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
          }
          return result;
        };
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 =
            i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 =
            i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);

        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);
        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0)
                ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100))
                : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0)
                ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000))
                : 0;

        double sharpe_10w = 0.0;
        double sharpe_100w = 0.0;
        double sharpe_1000w = 0.0;
        if (key.tag_id == -1) {
          auto cache_it = sharpe_cache_by_user_blob_.find(user_blob);
          assert(cache_it != sharpe_cache_by_user_blob_.end());
          const UserSharpeCacheState &cache = cache_it->second;
          sharpe_10w = calc_window_return_sharpe(
              cache.buckets, cache.pnl_before_first_bucket, key.block_bucket, key.block_bucket, exposure_avg_10w, kBlockBucketSize);
          sharpe_100w = calc_window_return_sharpe(
              cache.buckets,
              cache.pnl_before_first_bucket,
              std::max<int64_t>(0, key.block_bucket - 9),
              key.block_bucket,
              exposure_avg_100w,
              kBlockBucketSize);
          sharpe_1000w = calc_window_return_sharpe(
              cache.buckets,
              cache.pnl_before_first_bucket,
              std::max<int64_t>(0, key.block_bucket - 99),
              key.block_bucket,
              exposure_avg_1000w,
              kBlockBucketSize);
        }

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) +
        std::string(kSqlSetFeatureTensorStateUpsert));
  };

  for (const BucketRange &bucket_range : bucket_ranges) {
    while (dense_feature_max_bucket < bucket_range.block_bucket) {
      fill_dense_quiet_feature_bucket(dense_feature_max_bucket + 1);
      ++dense_feature_max_bucket;
    }

    std::unordered_map<AggKey, BucketAggState, AggKeyHash> bucket_agg_state_by_agg_key_local;
    bucket_agg_state_by_agg_key_local.reserve((bucket_range.end_idx - bucket_range.begin_idx) * 2 + 1);
    for (size_t idx = bucket_range.begin_idx; idx < bucket_range.end_idx; ++idx) {
      const auto &row = event_input_rows[idx];
      if (row.cond_idx < 0) {
        continue;
      }
      assert(static_cast<size_t>(row.cond_idx) < cond_tag_ids_.size());
      const int8_t tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];
      bucket_agg_state_by_agg_key_local.try_emplace(AggKey{row.user_id, bucket_range.block_bucket, tag_id}, BucketAggState{});
      bucket_agg_state_by_agg_key_local.try_emplace(AggKey{row.user_id, bucket_range.block_bucket, -1}, BucketAggState{});
    }
    if (!bucket_agg_state_by_agg_key_local.empty()) {
      tmp_table_reset(kSqlTmpFeatureTensorKeys, kSqlTmpSchemaFeatureTensorKeys);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorKeys);
        for (const auto &[key, _] : bucket_agg_state_by_agg_key_local) {
          const std::string &user_blob = user_blob_by_user_id[key.user_id];
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
      auto feature_state_result = query_checked(std::string(kSqlSelectFeatureTensorStateCols) + sql_from_feature_tensor_with_keys);
      for (idx_t i = 0; i < feature_state_result->RowCount(); ++i) {
        const std::string user_blob = feature_state_result->GetValue(0, i).GetValueUnsafe<std::string>();
        auto user_id_it = user_id_by_user_blob.find(user_blob);
        assert(user_id_it != user_id_by_user_blob.end());
        AggKey key{
            user_id_it->second,
            feature_state_result->GetValue(1, i).GetValue<int64_t>(),
            static_cast<int8_t>(feature_state_result->GetValue(2, i).GetValue<int32_t>()),
        };
        BucketAggState &agg = bucket_agg_state_by_agg_key_local.at(key);
        agg.last_sort_key = feature_state_result->GetValue(3, i).GetValue<int64_t>();
        agg.last_block = feature_state_result->GetValue(4, i).GetValue<int64_t>();
        agg.last_exposure = feature_state_result->GetValue(5, i).GetValue<int64_t>();
        agg.last_holding_exp = hugeint_to_i128(feature_state_result->GetValue(6, i).GetValue<duckdb::hugeint_t>());
        agg.last_token_count = feature_state_result->GetValue(7, i).GetValue<int64_t>();
        agg.time_weight_sum = feature_state_result->GetValue(8, i).GetValue<int64_t>();
        agg.token_count_tw_sum = feature_state_result->GetValue(9, i).GetValue<int64_t>();
        agg.exposure_tw_sum = hugeint_to_i128(feature_state_result->GetValue(10, i).GetValue<duckdb::hugeint_t>());
        agg.volume_sum = feature_state_result->GetValue(11, i).GetValue<int64_t>();
        agg.holding_period_exp_tw_sum = hugeint_to_i128(feature_state_result->GetValue(12, i).GetValue<duckdb::hugeint_t>());
        agg.has_tail = (agg.time_weight_sum > 0);
      }
      const int64_t bucket_start_block = bucket_range.block_bucket * kBlockBucketSize;
      for (auto &[_, agg] : bucket_agg_state_by_agg_key_local) {
        if (agg.has_tail) {
          continue;
        }
        agg.last_block = bucket_start_block;
        agg.last_exposure = 0;
        agg.last_holding_exp = 0;
        agg.last_token_count = 0;
        agg.time_weight_sum = kBlockBucketSize;
        agg.token_count_tw_sum = 0;
        agg.exposure_tw_sum = 0;
        agg.volume_sum = 0;
        agg.holding_period_exp_tw_sum = 0;
        agg.has_tail = true;
      }
    }

    std::unordered_set<AccountBucketKey, AccountBucketKeyHasher> bucket_dirty_account_bucket_key_set;
    bucket_dirty_account_bucket_key_set.reserve(bucket_range.end_idx - bucket_range.begin_idx + 1);
    for (size_t idx = bucket_range.begin_idx; idx < bucket_range.end_idx; ++idx) {
      const auto &row = event_input_rows[idx];
      auto realized_total_it = realized_total_by_user_id.find(row.user_id);
      auto unrealized_total_it = unrealized_total_by_user_id.find(row.user_id);
      auto active_token_count_it = active_token_count_by_user_id.find(row.user_id);
      assert(realized_total_it != realized_total_by_user_id.end());
      assert(unrealized_total_it != unrealized_total_by_user_id.end());
      assert(active_token_count_it != active_token_count_by_user_id.end());
      const int64_t prev_realized_total_i64 = feature_comp::round_i64(realized_total_it->second);
      const int64_t prev_unrealized_total_i64 = feature_comp::round_i64(unrealized_total_it->second);
      const int64_t prev_account_pnl_i64 =
          i64_narrow_checked(static_cast<__int128>(prev_realized_total_i64) + static_cast<__int128>(prev_unrealized_total_i64));

      double realized_delta = 0.0;
      int8_t tag_id = 13;
      int64_t exposure = 0;
      int64_t holding_period = 0;
      const int64_t volume = calc_volume_1e6(row);
      if (row.cond_idx >= 0) {
        assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
        const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
        assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
        tag_id = cond_tag_ids_[static_cast<size_t>(row.cond_idx)];

        TokenKey key{row.user_id, row.cond_idx, row.token_idx};
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        TokenState &st = token_state_it->second;
        const TokenState before_state = st;
        const auto before_contrib = token_feature_contrib(st);
        const double before_unrealized = feature_comp::calc_unrealized_pnl(
            feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
        const int before_holding = static_cast<int>(before_contrib.token_count);
        realized_delta = apply_event_input(row, st);
        const auto after_contrib = token_feature_contrib(st);
        const double after_unrealized = feature_comp::calc_unrealized_pnl(
            feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
        const int after_holding = static_cast<int>(after_contrib.token_count);
        unrealized_total_it->second += (after_unrealized - before_unrealized);
        active_token_count_it->second += (after_holding - before_holding);
        assert(active_token_count_it->second >= 0);

        const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
        const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
        const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;
        apply_runtime_contrib_delta(row.user_id, tag_id, token_count_delta, exposure_delta, exposure_entry_delta);
        apply_runtime_contrib_delta(row.user_id, -1, token_count_delta, exposure_delta, exposure_entry_delta);
        if (!token_state_rounded_equal(before_state, st)) {
          dirty_token_key_set.insert(key);
        }

        const int64_t row_block = sort_key_to_block(row.sort_key);
        exposure = after_contrib.exposure;
        holding_period = feature_comp::calc_holding_period_blocks(
            row_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
        auto apply_feature_bucket = [&](int8_t agg_tag_id) {
          UserTagRuntimePairKey rt_key{row.user_id, agg_tag_id};
          auto [runtime_it, _] = runtime_pair_state_by_pair_key.try_emplace(rt_key, UserTagRuntimePairState{});
          UserTagRuntimePairState &rt = runtime_it->second;
          assert(rt.token_count >= 0);
          assert(rt.exposure >= 0);
          __int128 holding_exp = 0;
          if (rt.exposure > 0) {
            const __int128 computed = static_cast<__int128>(row_block) * rt.exposure - rt.exposure_entry_sum;
            assert(computed >= 0);
            holding_exp = computed;
          } else {
            assert(rt.exposure_entry_sum == 0);
          }

          AggKey agg_key{row.user_id, bucket_range.block_bucket, agg_tag_id};
          BucketAggState &agg = bucket_agg_state_by_agg_key_local.at(agg_key);
          feature_comp::update_tail_window(
              agg, agg_key.block_bucket, row_block, rt.exposure, holding_exp, rt.token_count, kBlockBucketSize);
          agg.volume_sum += volume;
          agg.last_sort_key = row.sort_key;
        };

        apply_feature_bucket(tag_id);
        apply_feature_bucket(-1);
      }

      double &realized_total = realized_total_it->second;
      double &unrealized_total = unrealized_total_it->second;
      realized_total += realized_delta;
      const int64_t realized_total_i64 = feature_comp::round_i64(realized_total);
      const int64_t unrealized_total_i64 = feature_comp::round_i64(unrealized_total);
      const int64_t account_pnl_i64 =
          i64_narrow_checked(static_cast<__int128>(realized_total_i64) + static_cast<__int128>(unrealized_total_i64));
      update_account_bucket_cache(row.user_id, row.sort_key, prev_account_pnl_i64, account_pnl_i64, bucket_dirty_account_bucket_key_set);
      event_fact_rows.push_back({
          row.user_id,
          row.sort_key,
          row.cond_idx,
          row.token_idx,
          row.event_type,
          feature_comp::round_i64(realized_delta),
          realized_total_i64,
          unrealized_total_i64,
          active_token_count_it->second,
          tag_id,
          exposure,
          volume,
          holding_period,
      });
    }
    flush_account_bucket_rows(bucket_dirty_account_bucket_key_set);
    flush_feature_rows(bucket_agg_state_by_agg_key_local);
  }
  assert(event_fact_rows.size() == event_input_rows.size());
  bucket_agg_state_by_agg_key.clear();
  dirty_account_bucket_key_set.clear();

  {
    TraceN("s3/wr_open");
    if (!write_tx_open) {
      (void)query_checked("BEGIN");
      write_tx_open = true;
    }
  }

  if (!dirty_token_key_set.empty()) {
    TraceN("s3/wr_duck_tok");
    tmp_table_reset(kSqlTmpTokenDirty, kSqlTmpSchemaTokenDirty);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenDirty);
      for (const auto &key : dirty_token_key_set) {
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.EndRow();
      }
      ap.Close();
    }
    tmp_table_reset(kSqlTmpTokenNew, kSqlTmpSchemaTokenNew);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpTokenNew);
      for (const auto &key : dirty_token_key_set) {
        auto token_state_it = token_state_by_token_key.find(key);
        assert(token_state_it != token_state_by_token_key.end());
        const TokenState &st = token_state_it->second;
        if (std::abs(st.pos) <= kPosEpsilon) {
          continue;
        }
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.cond_idx);
        ap.Append(key.token_idx);
        ap.Append(feature_comp::round_i64(st.pos));
        ap.Append(feature_comp::round_i64(st.cost));
        ap.Append(feature_comp::round_i64(st.lp));
        ap.Append(feature_comp::round_i64(st.entry_block));
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "DELETE FROM " + table_token_state + " s "
                                             "WHERE EXISTS ("
                                             "  SELECT 1 FROM " +
        std::string(kSqlTmpTokenDirty) + " d "
                                         "  WHERE s.user_addr = d.user_addr AND s.cond_idx = d.cond_idx AND s.token_idx = d.token_idx"
                                         ")");
    (void)query_checked(
        "INSERT INTO " + table_token_state + " (" + kSqlColsTokenState + ") "
                                                                         "SELECT " +
        kSqlColsTokenState + " FROM " +
        std::string(kSqlTmpTokenNew));
  }

  if (!event_fact_rows.empty()) {
    TraceN("s3/wr_rock_evt");
    std::vector<core::rocks::Stage3EventFactRecord> event_fact_records;
    event_fact_records.reserve(event_fact_rows.size());
    for (const auto &event_fact_row : event_fact_rows) {
      const std::string &user_blob = user_blob_by_user_id[event_fact_row.user_id];
      event_fact_records.push_back({
          user_blob,
          event_fact_row.sort_key,
          event_fact_row.cond_idx,
          event_fact_row.event_type,
          event_fact_row.token_idx,
          event_fact_row.realized_delta,
          event_fact_row.realized_cum,
          event_fact_row.unrealized_pnl,
          event_fact_row.token_count,
          static_cast<int32_t>(event_fact_row.tag_id),
          event_fact_row.exposure,
          event_fact_row.volume,
          event_fact_row.holding_period,
      });
    }
    event_fact_store_->write_events(event_fact_records);
  }

  {
    TraceN("s3/wr_duck_bucket_pnl");
    if (!dirty_account_bucket_key_set.empty()) {
      tmp_table_reset(kSqlTmpAccountBucketPnlState, kSqlTmpSchemaAccountBucketPnlState);
      {
        duckdb::Appender ap(*sink_connection, kSqlTmpAccountBucketPnlState);
        for (const auto &key : dirty_account_bucket_key_set) {
          const std::string &user_blob = user_blob_by_user_id[key.user_id];
          auto cache_it = sharpe_cache_by_user_blob_.find(user_blob);
          assert(cache_it != sharpe_cache_by_user_blob_.end());
          const UserSharpeCacheState &cache = cache_it->second;
          const auto bucket_it = std::lower_bound(
              cache.buckets.begin(),
              cache.buckets.end(),
              key.block_bucket,
              [](const AccountBucketPnlState &row, int64_t block_bucket) { return row.block_bucket < block_bucket; });
          assert(bucket_it != cache.buckets.end());
          assert(bucket_it->block_bucket == key.block_bucket);
          assert(!bucket_it->samples.empty());
          const std::string samples_blob = encode_account_bucket_samples_blob(bucket_it->samples);
          ap.BeginRow();
          append_blob(ap, user_blob);
          ap.Append(key.block_bucket);
          append_blob(ap, samples_blob);
          ap.Append(bucket_it->close_pnl);
          ap.Append(bucket_it->min_pnl);
          ap.Append(bucket_it->updated_sort_key);
          ap.EndRow();
        }
        ap.Close();
      }
      (void)query_checked(
          "INSERT INTO " + table_account_bucket_pnl + " (" + kSqlColsAccountBucketPnlState + ") "
                                                                           "SELECT " +
          kSqlColsAccountBucketPnlState + " FROM " +
          std::string(kSqlTmpAccountBucketPnlState) + " " + std::string(kSqlOnConflictAccountBucketPnlState));
    }

  }

  if (!bucket_agg_state_by_agg_key.empty()) {
    TraceN("s3/wr_duck_feat");
    tmp_table_reset(kSqlTmpFeatureTensorState, kSqlTmpSchemaFeatureTensorState);
    {
      // 当前 bucket 输出（包含前缀和，用于 batch 内依赖传递）
      struct BucketPrefixOutput {
        int64_t block_bucket = 0;
        int64_t token_avg_10w = 0;
        int64_t exposure_avg_10w = 0;
        int64_t volume_10w = 0;
        int64_t holding_period_avg_10w = 0;
        // 前缀和（累计到当前 bucket）
        int64_t ps_token_avg = 0;
        int64_t ps_exposure_avg = 0;
        int64_t ps_volume = 0;
        int64_t ps_holding_period_avg = 0;
      };
      std::vector<AggKey> sorted_feature_keys;
      sorted_feature_keys.reserve(bucket_agg_state_by_agg_key.size());
      for (const auto &[key, _] : bucket_agg_state_by_agg_key) {
        sorted_feature_keys.push_back(key);
      }
      std::sort(sorted_feature_keys.begin(), sorted_feature_keys.end(), [](const AggKey &a, const AggKey &b) {
        if (a.user_id != b.user_id) {
          return a.user_id < b.user_id;
        }
        if (a.tag_id != b.tag_id) {
          return a.tag_id < b.tag_id;
        }
        return a.block_bucket < b.block_bucket;
      });
      std::unordered_map<UserTagRuntimePairKey, std::vector<BucketPrefixOutput>, UserTagRuntimePairKeyHasher>
          prefix_outputs_by_pair;
      prefix_outputs_by_pair.reserve(sorted_feature_keys.size());

      duckdb::Appender ap(*sink_connection, kSqlTmpFeatureTensorState);
      for (const AggKey &key : sorted_feature_keys) {
        auto agg_it_raw = bucket_agg_state_by_agg_key.find(key);
        assert(agg_it_raw != bucket_agg_state_by_agg_key.end());
        const BucketAggState &agg = agg_it_raw->second;
        const std::string &user_blob = user_blob_by_user_id[key.user_id];
        const int64_t tw = agg.time_weight_sum;
        const int64_t token_avg_10w =
            (tw > 0) ? feature_comp::round_i64(static_cast<double>(agg.token_count_tw_sum) / static_cast<double>(tw)) : 0;
        const int64_t exposure_avg_10w =
            (tw > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.exposure_tw_sum) /
                                                static_cast<long double>(tw)))
                : 0;
        const int64_t holding_period_avg_10w =
            (agg.exposure_tw_sum > 0)
                ? feature_comp::round_i64(static_cast<double>(i128_to_long_double(agg.holding_period_exp_tw_sum) /
                                                i128_to_long_double(agg.exposure_tw_sum)))
                : 0;
        const int64_t volume_10w = agg.volume_sum;

        UserTagRuntimePairKey pair_key{key.user_id, key.tag_id};
        auto [prefix_outputs_it, _] = prefix_outputs_by_pair.try_emplace(pair_key, std::vector<BucketPrefixOutput>{});
        std::vector<BucketPrefixOutput> &prefix_outputs = prefix_outputs_it->second;

        // 获取 DB 中该 (user, tag) 的前缀和历史
        const std::vector<PrefixSumHistoryRecord> empty_ps_history;
        auto ps_history_it = prefix_sum_history_by_pair_key.find(pair_key);
        const std::vector<PrefixSumHistoryRecord> &ps_history =
            (ps_history_it != prefix_sum_history_by_pair_key.end()) ? ps_history_it->second : empty_ps_history;

        // Step 1: 计算当前 bucket 的前缀和
        // 起点：前一个 bucket 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        int64_t prev_ps_token = 0, prev_ps_exposure = 0, prev_ps_volume = 0, prev_ps_holding = 0;
        if (!prefix_outputs.empty()) {
          // batch 内有更早的 bucket，使用最后一个（按 bucket 升序排列）
          const BucketPrefixOutput &prev = prefix_outputs.back();
          prev_ps_token = prev.ps_token_avg;
          prev_ps_exposure = prev.ps_exposure_avg;
          prev_ps_volume = prev.ps_volume;
          prev_ps_holding = prev.ps_holding_period_avg;
        } else {
          // 从 DB 历史获取前一个 bucket 的前缀和
          const PrefixSumHistoryRecord *prev_rec = find_prefix_sum_history_by_pair_key_le(ps_history, key.block_bucket - 1);
          if (prev_rec) {
            prev_ps_token = prev_rec->ps_token_avg;
            prev_ps_exposure = prev_rec->ps_exposure_avg;
            prev_ps_volume = prev_rec->ps_volume;
            prev_ps_holding = prev_rec->ps_holding_period_avg;
          }
        }

        // 当前 bucket 的前缀和 = 前一个 bucket 的前缀和 + 当前 bucket 的值
        const int64_t ps_token_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_token) + token_avg_10w);
        const int64_t ps_exposure_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_exposure) + exposure_avg_10w);
        const int64_t ps_volume = i64_narrow_checked(static_cast<__int128>(prev_ps_volume) + volume_10w);
        const int64_t ps_holding_period_avg = i64_narrow_checked(static_cast<__int128>(prev_ps_holding) + holding_period_avg_10w);

        // Step 2: 用前缀和差分计算窗口和
        // 辅助函数：获取 bucket <= target 的前缀和（优先从 batch 内获取，否则从 DB 历史获取）
        auto get_boundary_ps = [&](int64_t target_bucket) {
          struct BoundaryPrefixSum {
            int64_t token = 0, exposure = 0, volume = 0, holding = 0;
          };
          BoundaryPrefixSum result{};
          // 先从 batch 内找（prefix_outputs 按 block_bucket 升序）
          const auto batch_it = std::upper_bound(
              prefix_outputs.begin(),
              prefix_outputs.end(),
              target_bucket,
              [](int64_t target, const BucketPrefixOutput &row) {
                return target < row.block_bucket;
              });
          if (batch_it != prefix_outputs.begin()) {
            const auto &row = *std::prev(batch_it);
            result.token = row.ps_token_avg;
            result.exposure = row.ps_exposure_avg;
            result.volume = row.ps_volume;
            result.holding = row.ps_holding_period_avg;
            return result;
          }
          // 从 DB 历史找
          const PrefixSumHistoryRecord *rec = find_prefix_sum_history_by_pair_key_le(ps_history, target_bucket);
          if (rec) {
            result.token = rec->ps_token_avg;
            result.exposure = rec->ps_exposure_avg;
            result.volume = rec->ps_volume;
            result.holding = rec->ps_holding_period_avg;
          }
          return result;
        };

        // 100w 窗口边界：bucket - 10（窗口是 [bucket-9, bucket]）
        const auto boundary_100 = get_boundary_ps(key.block_bucket - 10);
        // 1000w 窗口边界：bucket - 100（窗口是 [bucket-99, bucket]）
        const auto boundary_1000 = get_boundary_ps(key.block_bucket - 100);

        // 窗口和 = 当前前缀和 - 边界前缀和
        const int64_t token_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_100.token);
        const int64_t token_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_token_avg) - boundary_1000.token);
        const int64_t exposure_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_100.exposure);
        const int64_t exposure_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_exposure_avg) - boundary_1000.exposure);
        const int64_t volume_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_100.volume);
        const int64_t volume_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_volume) - boundary_1000.volume);
        const int64_t holding_period_sum_100 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_100.holding);
        const int64_t holding_period_sum_1000 = i64_narrow_checked(static_cast<__int128>(ps_holding_period_avg) - boundary_1000.holding);

        // Step 3: 计算窗口平均值
        const int64_t denom_100 = std::min<int64_t>(10, key.block_bucket + 1);
        const int64_t denom_1000 = std::min<int64_t>(100, key.block_bucket + 1);

        const int64_t token_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t token_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(token_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t exposure_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t exposure_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(exposure_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t volume_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t volume_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(volume_sum_1000) / static_cast<double>(denom_1000)) : 0;
        const int64_t holding_period_avg_100w =
            (denom_100 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_100) / static_cast<double>(denom_100)) : 0;
        const int64_t holding_period_avg_1000w =
            (denom_1000 > 0) ? feature_comp::round_i64(static_cast<double>(holding_period_sum_1000) / static_cast<double>(denom_1000)) : 0;

        double sharpe_10w = 0.0;
        double sharpe_100w = 0.0;
        double sharpe_1000w = 0.0;
        if (key.tag_id == -1) {
          auto cache_it = sharpe_cache_by_user_blob_.find(user_blob);
          assert(cache_it != sharpe_cache_by_user_blob_.end());
          const UserSharpeCacheState &cache = cache_it->second;
          sharpe_10w = calc_window_return_sharpe(
              cache.buckets, cache.pnl_before_first_bucket, key.block_bucket, key.block_bucket, exposure_avg_10w, kBlockBucketSize);
          sharpe_100w = calc_window_return_sharpe(
              cache.buckets,
              cache.pnl_before_first_bucket,
              std::max<int64_t>(0, key.block_bucket - 9),
              key.block_bucket,
              exposure_avg_100w,
              kBlockBucketSize);
          sharpe_1000w = calc_window_return_sharpe(
              cache.buckets,
              cache.pnl_before_first_bucket,
              std::max<int64_t>(0, key.block_bucket - 99),
              key.block_bucket,
              exposure_avg_1000w,
              kBlockBucketSize);
        }

        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(key.block_bucket);
        ap.Append(static_cast<int32_t>(key.tag_id));
        ap.Append(agg.last_sort_key);
        ap.Append(agg.last_block);
        ap.Append(agg.last_exposure);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.last_holding_exp)));
        ap.Append(agg.last_token_count);
        ap.Append(agg.time_weight_sum);
        ap.Append(agg.token_count_tw_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.exposure_tw_sum)));
        ap.Append(agg.volume_sum);
        ap.Append(duckdb::Value::HUGEINT(i128_to_hugeint(agg.holding_period_exp_tw_sum)));
        ap.Append(token_avg_10w);
        ap.Append(exposure_avg_10w);
        ap.Append(volume_10w);
        ap.Append(holding_period_avg_10w);
        ap.Append(sharpe_10w);
        // Node-C: 前缀和
        ap.Append(ps_token_avg);
        ap.Append(ps_exposure_avg);
        ap.Append(ps_volume);
        ap.Append(ps_holding_period_avg);
        // Node-D: 窗口投影
        ap.Append(token_avg_100w);
        ap.Append(token_avg_1000w);
        ap.Append(exposure_avg_100w);
        ap.Append(exposure_avg_1000w);
        ap.Append(volume_avg_100w);
        ap.Append(volume_avg_1000w);
        ap.Append(holding_period_avg_100w);
        ap.Append(holding_period_avg_1000w);
        ap.Append(sharpe_100w);
        ap.Append(sharpe_1000w);
        ap.Append(agg.last_sort_key);
        ap.EndRow();

        // 保存当前 bucket 的输出（包含前缀和，用于 batch 内后续 bucket）
        prefix_outputs.push_back(BucketPrefixOutput{
            key.block_bucket,
            token_avg_10w,
            exposure_avg_10w,
            volume_10w,
            holding_period_avg_10w,
            ps_token_avg,
            ps_exposure_avg,
            ps_volume,
            ps_holding_period_avg,
        });
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_feature_tensor + " (" + kSqlColsFeatureTensorState + ") "
                                                                                    "SELECT " +
        kSqlColsFeatureTensorState + " FROM " +
        std::string(kSqlTmpFeatureTensorState) + " " + std::string(kSqlOnConflictFeatureTensorState) + std::string(kSqlSetFeatureTensorStateUpsert));
  }

  {
    const int64_t committed_bucket =
        feature_comp::sort_key_to_block_bucket(sync_cursor_.sort_key, SORT_KEY_SCALE, kBlockBucketSize);
    const int64_t min_bucket_to_keep = std::max<int64_t>(0, committed_bucket - 99);
    if (min_bucket_to_keep > last_pruned_account_bucket_before_) {
      (void)query_checked(
          "DELETE FROM " + table_account_bucket_pnl + " WHERE block_bucket < " + std::to_string(min_bucket_to_keep));
      last_pruned_account_bucket_before_ = min_bucket_to_keep;
    }
    std::vector<std::string> cache_users_to_erase;
    cache_users_to_erase.reserve(sharpe_cache_by_user_blob_.size());
    for (auto &[user_blob, cache] : sharpe_cache_by_user_blob_) {
      prune_user_sharpe_cache(cache, min_bucket_to_keep);
      if (cache.buckets.empty()) {
        cache_users_to_erase.push_back(user_blob);
      }
    }
    for (const std::string &user_blob : cache_users_to_erase) {
      sharpe_cache_by_user_blob_.erase(user_blob);
    }
    const auto sharpe_cache_value_extra = [](const UserSharpeCacheState &cache) {
      int64_t extra = 0;
      extra += static_cast<int64_t>(cache.buckets.size()) * static_cast<int64_t>(sizeof(AccountBucketPnlState));
      for (const auto &bucket : cache.buckets) {
        extra += core::mem::estimate_vector_plain(bucket.samples);
      }
      return extra;
    };
    int64_t sharpe_cache_buckets = 0;
    int64_t sharpe_cache_samples = 0;
    for (const auto &[_, cache] : sharpe_cache_by_user_blob_) {
      sharpe_cache_buckets += static_cast<int64_t>(cache.buckets.size());
      for (const auto &bucket : cache.buckets) {
        sharpe_cache_samples += static_cast<int64_t>(bucket.samples.size());
      }
    }
    const int64_t sharpe_cache_bytes =
        core::mem::estimate_unordered_map(
            sharpe_cache_by_user_blob_,
            [](const std::string &k) { return core::mem::estimate_string_extra(k); },
            sharpe_cache_value_extra);
    std::lock_guard<std::mutex> lock(sync_mu_);
    runtime_memory_probe_.sharpe_cache_bytes = sharpe_cache_bytes;
    runtime_memory_probe_.sharpe_cache_users = static_cast<int64_t>(sharpe_cache_by_user_blob_.size());
    runtime_memory_probe_.sharpe_cache_buckets = sharpe_cache_buckets;
    runtime_memory_probe_.sharpe_cache_samples = sharpe_cache_samples;
  }

  {
    TraceN("s3/wr_duck_user");
    tmp_table_reset(kSqlTmpUserSummaryDelta, kSqlTmpSchemaUserSummaryDelta);
    {
      duckdb::Appender ap(*sink_connection, kSqlTmpUserSummaryDelta);
      for (const auto &[user_id, inc] : event_increment_by_user_id) {
        auto sort_key_it = last_sort_key_by_user_id.find(user_id);
        auto realized_total_it = realized_total_by_user_id.find(user_id);
        auto unrealized_total_it = unrealized_total_by_user_id.find(user_id);
        auto active_token_count_it = active_token_count_by_user_id.find(user_id);
        assert(sort_key_it != last_sort_key_by_user_id.end());
        assert(realized_total_it != realized_total_by_user_id.end());
        assert(unrealized_total_it != unrealized_total_by_user_id.end());
        assert(active_token_count_it != active_token_count_by_user_id.end());
        const std::string &user_blob = user_blob_by_user_id[user_id];
        ap.BeginRow();
        append_blob(ap, user_blob);
        ap.Append(inc);
        ap.Append(sort_key_it->second);
        ap.Append(feature_comp::round_i64(realized_total_it->second));
        ap.Append(feature_comp::round_i64(unrealized_total_it->second));
        assert(active_token_count_it->second >= 0);
        ap.Append(active_token_count_it->second);
        ap.EndRow();
      }
      ap.Close();
    }
    (void)query_checked(
        "INSERT INTO " + table_user_summary + " (" + kSqlColsUserSummaryState + ") "
                                                                                "SELECT user_addr, event_inc, rpnl, upnl, active_tokens, last_sort_key FROM " +
        std::string(kSqlTmpUserSummaryDelta) + " " + std::string(kSqlOnConflictUserSummaryState) + sql_set_user_summary_upsert);
  }

  {
    TraceN("s3/wr_duck_commit");
    save_cursor_locked(*sink_connection);
    (void)query_checked("COMMIT");
  }

  {
    std::lock_guard<std::mutex> cache_lock(user_query_cache_mu_);
    user_query_cache_state_ = UserQueryCacheState{};
  }

  return true;
}

} // namespace stage3
