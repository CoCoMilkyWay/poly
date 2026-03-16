#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace stage3 {

// ============================================================================
// update_sharpe_on_event - 更新 Sharpe 样本
// ============================================================================

void update_sharpe_on_event(Stage3Runtime *rt, uint32_t user_idx, int64_t pnl, int64_t prev_pnl, int32_t bucket, int32_t block_offset) {
  UserBlock *user = &rt->users[user_idx];

  // Initialize anchor on first sharpe data (matches old behavior: cache.pnl_before_first_bucket = prev_account_pnl)
  if (user->sharpe_agg_head == NULL_IDX) {
    user->pnl_before_first_sharpe_bucket = prev_pnl;
  }

  SharpeAgg *agg = sharpe_agg_get_or_create(rt, user_idx, bucket);

  // Check if same block (only keep last sample per block)
  int32_t current_block = bucket * BLOCK_BUCKET_SIZE + block_offset;

  if (current_block != agg->last_block) {
    // New block - add new sample
    uint32_t sample_idx = sharpe_sample_alloc(rt);
    SharpeSample *sample = &rt->sharpe_sample_pool[sample_idx];
    sample->agg_idx = static_cast<uint32_t>(agg - rt->sharpe_agg_pool);
    sample->block_offset = block_offset;
    sample->pnl = pnl;
    sample->next = agg->sample_head;
    agg->sample_head = sample_idx;
    agg->sample_count++;
    agg->last_block = current_block;
  } else {
    // Same block - update existing sample
    if (agg->sample_head != NULL_IDX) {
      rt->sharpe_sample_pool[agg->sample_head].pnl = pnl;
    }
  }

  // Update aggregates
  agg->close_pnl = pnl;
  agg->min_pnl = std::min(agg->min_pnl, pnl);
  agg->max_pnl = std::max(agg->max_pnl, pnl);
}

// ============================================================================
// Sharpe calculation helpers
// ============================================================================

namespace {

struct SharpeWindow {
  float sharpe;
};

// Calculate Sharpe ratio for a window
// p0: left boundary anchor (pnl before window start)
// samples: (block, pnl) pairs within window, sorted by block
// start_block: window start block
// end_block: right boundary
// avg_exposure: average exposure in window
SharpeWindow calc_sharpe_window(
    int64_t p0,
    const std::vector<std::pair<int32_t, int64_t>> &samples,
    int64_t start_block,
    int64_t end_block,
    int64_t avg_exposure) {

  SharpeWindow result{};
  result.sharpe = 0.0f;

  if (samples.empty()) {
    return result;
  }

  assert(end_block >= start_block);

  long double min_pnl = static_cast<long double>(p0);
  int64_t tail_pnl = p0;
  for (const auto &sample : samples) {
    min_pnl = std::min(min_pnl, static_cast<long double>(sample.second));
    tail_pnl = sample.second;
  }

  const long double nav_base =
      std::max(static_cast<long double>(avg_exposure), std::fabs(min_pnl)) + 1.0L;

  double sum_r = 0.0;
  double sum_r2_dt = 0.0;
  int64_t prev_block = start_block - 1;
  int64_t prev_pnl = p0;

  auto append_sample = [&](int64_t block, int64_t pnl) {
    assert(block >= prev_block);
    if (block == prev_block) {
      prev_pnl = pnl;
      return;
    }
    const int64_t dt = block - prev_block;
    assert(dt > 0);
    const long double nav_prev = nav_base + static_cast<long double>(prev_pnl);
    const long double nav_curr = nav_base + static_cast<long double>(pnl);
    assert(nav_prev > 0.0L);
    assert(nav_curr > 0.0L);
    const double r = static_cast<double>((nav_curr - nav_prev) / nav_prev);
    sum_r += r;
    sum_r2_dt += (r * r) / static_cast<double>(dt);
    prev_block = block;
    prev_pnl = pnl;
  };

  for (const auto &sample : samples) {
    assert(sample.first >= start_block);
    assert(sample.first <= end_block);
    append_sample(sample.first, sample.second);
  }
  if (prev_block < end_block) {
    append_sample(end_block, tail_pnl);
  }

  const int64_t total_time = prev_block - (start_block - 1);
  if (total_time <= 0) {
    return result;
  }

  double T = static_cast<double>(total_time);
  const double mean_return = sum_r / T;
  const double var_return = sum_r2_dt / T - mean_return * mean_return;

  if (var_return <= 0) {
    return result;
  }

  double sigma = std::sqrt(var_return);
  double raw_sharpe = mean_return / sigma;

  // Normalize to 1000w block scale
  result.sharpe = static_cast<float>(raw_sharpe * std::sqrt(10000000.0));

  return result;
}

} // namespace

// ============================================================================
// calc_sharpe_for_feature - Calculate Sharpe ratios for feature slot
// Called when needed (lazy evaluation)
// ============================================================================

void calc_sharpe_for_feature(Stage3Runtime *rt, uint32_t user_idx, FeatureSlot *feat) {
  // Sharpe only for global aggregation (tag_id = -1)
  if (feat->tag_id != -1)
    return;

  const int32_t bucket = feat->bucket;

  // Helper to collect samples for a bucket
  auto collect_bucket_samples = [&](int32_t b) -> std::vector<std::pair<int32_t, int64_t>> {
    std::vector<std::pair<int32_t, int64_t>> samples;
    SharpeAgg *agg = sharpe_agg_find(rt, user_idx, b);
    if (!agg)
      return samples;

    uint32_t idx = agg->sample_head;
    while (idx != NULL_IDX) {
      SharpeSample *s = &rt->sharpe_sample_pool[idx];
      samples.push_back({b * BLOCK_BUCKET_SIZE + s->block_offset, s->pnl});
      idx = s->next;
    }
    std::sort(samples.begin(), samples.end());
    return samples;
  };

  // Get user's pnl anchor (faithful to old architecture)
  UserBlock *user = &rt->users[user_idx];

  // Helper to get p0 (pnl before window start)
  // First try to find the closest bucket before start_bucket,
  // then fall back to user's pnl_before_first_sharpe_bucket anchor
  auto get_p0 = [&](int32_t start_bucket) -> int64_t {
    for (int32_t b = start_bucket - 1; b >= 0 && b >= start_bucket - 100; --b) {
      SharpeAgg *agg = sharpe_agg_find(rt, user_idx, b);
      if (agg)
        return agg->close_pnl;
    }
    // Fall back to user-level anchor (matches old pnl_before_first_bucket)
    return user->pnl_before_first_sharpe_bucket;
  };

  // ========== 10w Sharpe (current bucket) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> samples = collect_bucket_samples(bucket);
    int64_t p0 = get_p0(bucket);
    int64_t start_block = static_cast<int64_t>(bucket) * BLOCK_BUCKET_SIZE;
    int64_t end_block = bucket_end_block(bucket);

    SharpeWindow win = calc_sharpe_window(p0, samples, start_block, end_block, feat->exposure_avg_10w);
    feat->sharpe_10w = win.sharpe;
  }

  // ========== 100w Sharpe (10 buckets) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> all_samples;
    int32_t start_bucket = std::max(0, bucket - 9);
    int64_t total_exp = 0;

    for (int32_t b = start_bucket; b <= bucket; ++b) {
      auto samples = collect_bucket_samples(b);
      all_samples.insert(all_samples.end(), samples.begin(), samples.end());

      FeatureSlot *f = feature_find(rt, user_idx, b, -1);
      if (f) {
        total_exp += f->exposure_avg_10w;
      }
      // Empty bucket contributes 0 to total_exp (faithfully matches old behavior)
    }

    std::sort(all_samples.begin(), all_samples.end());
    int64_t p0 = get_p0(start_bucket);
    int64_t start_block = static_cast<int64_t>(start_bucket) * BLOCK_BUCKET_SIZE;
    int64_t end_block = bucket_end_block(bucket);
    // Use fixed window denominator: min(10, bucket + 1)
    int32_t denom = std::min(10, bucket + 1);
    int64_t avg_exp = denom > 0 ? total_exp / denom : 0;

    SharpeWindow win = calc_sharpe_window(p0, all_samples, start_block, end_block, avg_exp);
    feat->sharpe_100w = win.sharpe;
  }

  // ========== 1000w Sharpe (100 buckets) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> all_samples;
    int32_t start_bucket = std::max(0, bucket - 99);
    int64_t total_exp = 0;

    for (int32_t b = start_bucket; b <= bucket; ++b) {
      auto samples = collect_bucket_samples(b);
      all_samples.insert(all_samples.end(), samples.begin(), samples.end());

      FeatureSlot *f = feature_find(rt, user_idx, b, -1);
      if (f) {
        total_exp += f->exposure_avg_10w;
      }
      // Empty bucket contributes 0 to total_exp (faithfully matches old behavior)
    }

    std::sort(all_samples.begin(), all_samples.end());
    int64_t p0 = get_p0(start_bucket);
    int64_t start_block = static_cast<int64_t>(start_bucket) * BLOCK_BUCKET_SIZE;
    int64_t end_block = bucket_end_block(bucket);
    // Use fixed window denominator: min(100, bucket + 1)
    int32_t denom = std::min(100, bucket + 1);
    int64_t avg_exp = denom > 0 ? total_exp / denom : 0;

    SharpeWindow win = calc_sharpe_window(p0, all_samples, start_block, end_block, avg_exp);
    feat->sharpe_1000w = win.sharpe;
  }
}

// ============================================================================
// sharpe_prune_old_buckets - 淘汰用户的旧 bucket
// 拆表头，重新连接链表
// ============================================================================

void sharpe_prune_old_buckets(Stage3Runtime *rt, uint32_t user_idx, int32_t min_bucket_to_keep) {
  UserBlock *user = &rt->users[user_idx];

  uint32_t *prev_ptr = &user->sharpe_agg_head;
  uint32_t idx = user->sharpe_agg_head;

  while (idx != NULL_IDX) {
    SharpeAgg *agg = &rt->sharpe_agg_pool[idx];
    uint32_t next_idx = agg->next;

    if (agg->bucket < min_bucket_to_keep) {
      // Before freeing, update the user's pnl anchor (matches old prune_user_sharpe_cache)
      user->pnl_before_first_sharpe_bucket = agg->close_pnl;

      // Free all samples in this agg
      uint32_t sample_idx = agg->sample_head;
      while (sample_idx != NULL_IDX) {
        uint32_t next_sample = rt->sharpe_sample_pool[sample_idx].next;
        sharpe_sample_free(rt, sample_idx);
        sample_idx = next_sample;
      }

      // Remove from linked list: connect prev to next
      *prev_ptr = next_idx;
      user->sharpe_agg_count--;

      // Keep runtime index consistent with list pruning.
      rt->sharpe_agg_index.map.erase(SharpeAggIndex::make_key(user_idx, agg->bucket));

      // Free the agg
      sharpe_agg_free(rt, idx);
    } else {
      // Keep this agg, move prev_ptr forward
      prev_ptr = &agg->next;
    }

    idx = next_idx;
  }
}

} // namespace stage3
