#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace stage3 {

// ============================================================================
// update_sharpe_on_event - 更新 Sharpe 样本
// ============================================================================

void update_sharpe_on_event(Stage3Runtime* rt, uint32_t user_idx, int64_t pnl, int32_t bucket, int32_t block_offset) {
  SharpeAgg* agg = sharpe_agg_get_or_create(rt, user_idx, bucket);
  
  // Check if same block (only keep last sample per block)
  int32_t current_block = bucket * BLOCK_BUCKET_SIZE + block_offset;
  
  if (current_block != agg->last_block) {
    // New block - add new sample
    uint32_t sample_idx = sharpe_sample_alloc(rt);
    SharpeSample* sample = &rt->sharpe_sample_pool[sample_idx];
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
  int64_t min_interval_pnl;
  int64_t max_interval_pnl;
  int64_t range;
  double nav_base;
  double mean_return;
  double var_return;
  float sharpe;
};

// Calculate Sharpe ratio for a window
// p0: left boundary anchor (pnl before window start)
// samples: (block, pnl) pairs within window, sorted by block
// end_block: right boundary
// avg_exposure: average exposure in window
SharpeWindow calc_sharpe_window(
    int64_t p0,
    const std::vector<std::pair<int32_t, int64_t>>& samples,
    int32_t end_block,
    int64_t avg_exposure) {
  
  SharpeWindow result{};
  result.sharpe = 0.0f;
  
  if (samples.empty()) {
    return result;
  }
  
  // Rebase: x_i = pnl_i - p0
  std::vector<std::pair<int32_t, int64_t>> rebased;
  rebased.reserve(samples.size() + 2);
  
  // Left boundary at x_0 = 0
  rebased.push_back({samples.front().first, 0});
  
  for (const auto& s : samples) {
    rebased.push_back({s.first, s.second - p0});
  }
  
  // Find min/max
  result.min_interval_pnl = 0;
  result.max_interval_pnl = 0;
  for (const auto& s : rebased) {
    result.min_interval_pnl = std::min(result.min_interval_pnl, s.second);
    result.max_interval_pnl = std::max(result.max_interval_pnl, s.second);
  }
  result.range = result.max_interval_pnl - result.min_interval_pnl;
  
  // nav_base = avg_exposure + abs(min_interval_pnl) + 1 USD (1e6 scale)
  result.nav_base = static_cast<double>(avg_exposure) + 
                    static_cast<double>(std::abs(result.min_interval_pnl)) + 
                    1e6; // 1 USD
  
  if (result.nav_base <= 0) {
    return result;
  }
  
  // Calculate returns
  double sum_r = 0.0;
  double sum_r2_dt = 0.0;
  int64_t total_time = 0;
  
  for (size_t i = 1; i < rebased.size(); ++i) {
    int32_t prev_block = rebased[i-1].first;
    int32_t curr_block = rebased[i].first;
    int64_t dt = curr_block - prev_block;
    if (dt <= 0) continue;
    
    double nav_prev = result.nav_base + static_cast<double>(rebased[i-1].second);
    double nav_curr = result.nav_base + static_cast<double>(rebased[i].second);
    
    if (nav_prev <= 0) continue;
    
    double r = (nav_curr - nav_prev) / nav_prev;
    sum_r += r;
    sum_r2_dt += r * r / static_cast<double>(dt);
    total_time += dt;
  }
  
  if (total_time <= 0) {
    return result;
  }
  
  double T = static_cast<double>(total_time);
  result.mean_return = sum_r / T;
  result.var_return = sum_r2_dt / T - result.mean_return * result.mean_return;
  
  if (result.var_return <= 0) {
    return result;
  }
  
  double sigma = std::sqrt(result.var_return);
  double raw_sharpe = result.mean_return / sigma;
  
  // Normalize to 1000w block scale
  result.sharpe = static_cast<float>(raw_sharpe * std::sqrt(10000000.0));
  
  return result;
}

} // namespace

// ============================================================================
// calc_sharpe_for_feature - Calculate Sharpe ratios for feature slot
// Called when needed (lazy evaluation)
// ============================================================================

void calc_sharpe_for_feature(Stage3Runtime* rt, uint32_t user_idx, FeatureSlot* feat) {
  // Sharpe only for global aggregation (tag_id = -1)
  if (feat->tag_id != -1) return;
  
  const int32_t bucket = feat->bucket;
  
  // Helper to collect samples for a bucket
  auto collect_bucket_samples = [&](int32_t b) -> std::vector<std::pair<int32_t, int64_t>> {
    std::vector<std::pair<int32_t, int64_t>> samples;
    SharpeAgg* agg = sharpe_agg_find(rt, user_idx, b);
    if (!agg) return samples;
    
    uint32_t idx = agg->sample_head;
    while (idx != NULL_IDX) {
      SharpeSample* s = &rt->sharpe_sample_pool[idx];
      samples.push_back({b * BLOCK_BUCKET_SIZE + s->block_offset, s->pnl});
      idx = s->next;
    }
    std::sort(samples.begin(), samples.end());
    return samples;
  };
  
  // Helper to get p0 (pnl before window start)
  auto get_p0 = [&](int32_t start_bucket) -> int64_t {
    for (int32_t b = start_bucket - 1; b >= 0 && b >= start_bucket - 100; --b) {
      SharpeAgg* agg = sharpe_agg_find(rt, user_idx, b);
      if (agg) return agg->close_pnl;
    }
    return 0;
  };
  
  // ========== 10w Sharpe (current bucket) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> samples = collect_bucket_samples(bucket);
    int64_t p0 = get_p0(bucket);
    int32_t end_block = bucket_end_block(bucket);
    
    SharpeWindow win = calc_sharpe_window(p0, samples, end_block, feat->exposure_avg_10w);
    feat->sharpe_10w = win.sharpe;
  }
  
  // ========== 100w Sharpe (10 buckets) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> all_samples;
    int32_t start_bucket = std::max(0, bucket - 9);
    int64_t total_exp = 0;
    int32_t bucket_count = 0;
    
    for (int32_t b = start_bucket; b <= bucket; ++b) {
      auto samples = collect_bucket_samples(b);
      all_samples.insert(all_samples.end(), samples.begin(), samples.end());
      
      FeatureSlot* f = feature_find(rt, user_idx, b, -1);
      if (f) {
        total_exp += f->exposure_avg_10w;
        bucket_count++;
      }
    }
    
    std::sort(all_samples.begin(), all_samples.end());
    int64_t p0 = get_p0(start_bucket);
    int32_t end_block = bucket_end_block(bucket);
    int64_t avg_exp = bucket_count > 0 ? total_exp / bucket_count : 0;
    
    SharpeWindow win = calc_sharpe_window(p0, all_samples, end_block, avg_exp);
    feat->sharpe_100w = win.sharpe;
  }
  
  // ========== 1000w Sharpe (100 buckets) ==========
  {
    std::vector<std::pair<int32_t, int64_t>> all_samples;
    int32_t start_bucket = std::max(0, bucket - 99);
    int64_t total_exp = 0;
    int32_t bucket_count = 0;
    
    for (int32_t b = start_bucket; b <= bucket; ++b) {
      auto samples = collect_bucket_samples(b);
      all_samples.insert(all_samples.end(), samples.begin(), samples.end());
      
      FeatureSlot* f = feature_find(rt, user_idx, b, -1);
      if (f) {
        total_exp += f->exposure_avg_10w;
        bucket_count++;
      }
    }
    
    std::sort(all_samples.begin(), all_samples.end());
    int64_t p0 = get_p0(start_bucket);
    int32_t end_block = bucket_end_block(bucket);
    int64_t avg_exp = bucket_count > 0 ? total_exp / bucket_count : 0;
    
    SharpeWindow win = calc_sharpe_window(p0, all_samples, end_block, avg_exp);
    feat->sharpe_1000w = win.sharpe;
  }
}

} // namespace stage3
