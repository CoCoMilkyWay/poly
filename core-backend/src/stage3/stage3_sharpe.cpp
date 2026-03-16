#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

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
    uint32_t sample_idx = sharpe_sample_alloc(rt, user_idx);
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

  if (end_block < start_block) {
    return result;
  }

  long double min_x = 0.0L;
  int64_t tail_pnl = p0;
  for (const auto &sample : samples) {
    min_x = std::min(min_x, static_cast<long double>(sample.second - p0));
    tail_pnl = sample.second;
  }
  min_x = std::min(min_x, static_cast<long double>(tail_pnl - p0));

  const long double nav_base =
      static_cast<long double>(avg_exposure) + std::fabs(min_x) + 1.0L;
  if (nav_base <= 0.0L) {
    return result;
  }

  long double sum_r = 0.0L;
  long double sum_r2 = 0.0L;
  int64_t prev_block = start_block - 1;
  long double prev_x = 0.0L;
  long double prev_x_before_last_block = 0.0L;
  long double last_r = 0.0L;
  bool has_last_r = false;
  bool invalid = false;

  auto append_sample = [&](int64_t block, int64_t pnl) {
    if (block < prev_block) {
      invalid = true;
      return;
    }
    const long double curr_x = static_cast<long double>(pnl - p0);
    if (block == prev_block) {
      if (!has_last_r) {
        invalid = true;
        return;
      }
      const long double nav_prev = nav_base + prev_x_before_last_block;
      const long double nav_curr = nav_base + curr_x;
      if (nav_prev <= 0.0L || nav_curr <= 0.0L) {
        invalid = true;
        return;
      }
      const long double r = (nav_curr - nav_prev) / nav_prev;
      sum_r += r - last_r;
      sum_r2 += r * r - last_r * last_r;
      prev_x = curr_x;
      last_r = r;
      return;
    }
    const long double nav_prev = nav_base + prev_x;
    const long double nav_curr = nav_base + curr_x;
    if (nav_prev <= 0.0L || nav_curr <= 0.0L) {
      invalid = true;
      return;
    }
    const long double r = (nav_curr - nav_prev) / nav_prev;
    sum_r += r;
    sum_r2 += r * r;
    prev_x_before_last_block = prev_x;
    prev_block = block;
    prev_x = curr_x;
    last_r = r;
    has_last_r = true;
  };

  for (const auto &sample : samples) {
    if (sample.first < start_block) {
      continue;
    }
    if (sample.first > end_block) {
      break;
    }
    append_sample(sample.first, sample.second);
    if (invalid) {
      return result;
    }
  }
  if (prev_block < end_block) {
    append_sample(end_block, tail_pnl);
  }
  if (invalid) {
    return result;
  }

  const int64_t total_time = end_block - start_block + 1;
  if (total_time <= 0) {
    return result;
  }

  const long double T = static_cast<long double>(total_time);
  const long double mean_return = sum_r / T;
  // The window is a per-block return distribution: unsampled blocks contribute zero return,
  // sampled blocks contribute one discrete jump return at that block.
  const long double var_return = sum_r2 / T - mean_return * mean_return;

  if (var_return <= 0.0L || !std::isfinite(static_cast<double>(var_return))) {
    return result;
  }

  const long double sigma = std::sqrt(var_return);
  if (sigma <= 0.0L || !std::isfinite(static_cast<double>(sigma))) {
    return result;
  }
  const long double raw_sharpe = mean_return / sigma;
  if (!std::isfinite(static_cast<double>(raw_sharpe))) {
    return result;
  }

  // Normalize to 1000w block scale
  result.sharpe = static_cast<float>(raw_sharpe * std::sqrt(10000000.0L));

  return result;
}

} // namespace

// ============================================================================
// calc_sharpe_for_feature - Calculate Sharpe ratios for feature slot
// Called when needed (lazy evaluation)
// ============================================================================

void calc_sharpe_for_feature(Stage3Runtime *rt, uint32_t user_idx, FeatureSlot *feat, int32_t first_bucket) {
  // Sharpe only for global aggregation (tag_id = -1)
  if (feat->tag_id != -1)
    return;

  const int32_t bucket = feat->bucket;
  if (first_bucket < 0 || first_bucket > bucket) {
    feat->sharpe_10w = 0.0f;
    feat->sharpe_100w = 0.0f;
    feat->sharpe_1000w = 0.0f;
    return;
  }

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
  auto get_p0 = [&](int32_t start_bucket) -> int64_t {
    int32_t best_bucket = std::numeric_limits<int32_t>::min();
    int64_t best_pnl = user->pnl_before_first_sharpe_bucket;
    uint32_t idx = user->sharpe_agg_head;
    while (idx != NULL_IDX) {
      SharpeAgg *agg = &rt->sharpe_agg_pool[idx];
      if (agg->bucket < start_bucket && agg->bucket > best_bucket) {
        best_bucket = agg->bucket;
        best_pnl = agg->close_pnl;
      }
      idx = agg->next;
    }
    return best_pnl;
  };

  // ========== 10w Sharpe (current bucket) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> samples = collect_bucket_samples(bucket);
    int64_t p0 = get_p0(bucket);
    int64_t start_block = static_cast<int64_t>(bucket) * BLOCK_BUCKET_SIZE;
    int64_t end_block = bucket_last_block(bucket);

    SharpeWindow win = calc_sharpe_window(p0, samples, start_block, end_block, feat->exposure_avg_10w);
    feat->sharpe_10w = win.sharpe;
  }

  // ========== 100w Sharpe (10 buckets) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> all_samples;
    int32_t start_bucket = std::max(first_bucket, bucket - 9);

    for (int32_t b = start_bucket; b <= bucket; ++b) {
      auto samples = collect_bucket_samples(b);
      all_samples.insert(all_samples.end(), samples.begin(), samples.end());
    }

    std::sort(all_samples.begin(), all_samples.end());
    int64_t p0 = get_p0(start_bucket);
    int64_t start_block = static_cast<int64_t>(start_bucket) * BLOCK_BUCKET_SIZE;
    int64_t end_block = bucket_last_block(bucket);

    SharpeWindow win =
        calc_sharpe_window(p0, all_samples, start_block, end_block, feat->exposure_avg_100w);
    feat->sharpe_100w = win.sharpe;
  }

  // ========== 1000w Sharpe (100 buckets) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> all_samples;
    int32_t start_bucket = std::max(first_bucket, bucket - 99);

    for (int32_t b = start_bucket; b <= bucket; ++b) {
      auto samples = collect_bucket_samples(b);
      all_samples.insert(all_samples.end(), samples.begin(), samples.end());
    }

    std::sort(all_samples.begin(), all_samples.end());
    int64_t p0 = get_p0(start_bucket);
    int64_t start_block = static_cast<int64_t>(start_bucket) * BLOCK_BUCKET_SIZE;
    int64_t end_block = bucket_last_block(bucket);

    SharpeWindow win =
        calc_sharpe_window(p0, all_samples, start_block, end_block, feat->exposure_avg_1000w);
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
  int32_t newest_pruned_bucket = std::numeric_limits<int32_t>::min();
  int64_t newest_pruned_close_pnl = user->pnl_before_first_sharpe_bucket;

  while (idx != NULL_IDX) {
    SharpeAgg *agg = &rt->sharpe_agg_pool[idx];
    uint32_t next_idx = agg->next;

    if (agg->bucket < min_bucket_to_keep) {
      if (agg->bucket > newest_pruned_bucket) {
        newest_pruned_bucket = agg->bucket;
        newest_pruned_close_pnl = agg->close_pnl;
      }

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
      rt->sharpe_agg_index[user_shard(rt, user_idx)].map.erase(SharpeAggIndex::make_key(user_idx, agg->bucket));

      // Free the agg
      sharpe_agg_free(rt, idx);
    } else {
      // Keep this agg, move prev_ptr forward
      prev_ptr = &agg->next;
    }

    idx = next_idx;
  }

  if (newest_pruned_bucket != std::numeric_limits<int32_t>::min()) {
    user->pnl_before_first_sharpe_bucket = newest_pruned_close_pnl;
  }
}

} // namespace stage3
