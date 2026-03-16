#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace stage3 {

namespace {

// HUGEINT helpers
struct Int128 {
  int64_t lo;
  int64_t hi;
};

inline Int128 make_i128(int64_t lo, int64_t hi) {
  return {lo, hi};
}

inline Int128 add_i128(Int128 a, Int128 b) {
  uint64_t lo = static_cast<uint64_t>(a.lo) + static_cast<uint64_t>(b.lo);
  int64_t carry = (lo < static_cast<uint64_t>(a.lo)) ? 1 : 0;
  int64_t hi = a.hi + b.hi + carry;
  return {static_cast<int64_t>(lo), hi};
}

inline Int128 sub_i128(Int128 a, Int128 b) {
  uint64_t lo = static_cast<uint64_t>(a.lo) - static_cast<uint64_t>(b.lo);
  int64_t borrow = (a.lo < b.lo) ? 1 : 0;
  int64_t hi = a.hi - b.hi - borrow;
  return {static_cast<int64_t>(lo), hi};
}

inline Int128 mul_i64_i64(int64_t a, int64_t b) {
  // Simple multiplication for non-overflow cases
  __int128 result = static_cast<__int128>(a) * static_cast<__int128>(b);
  return {static_cast<int64_t>(result), static_cast<int64_t>(result >> 64)};
}

inline int64_t div_i128_i64(Int128 a, int64_t b) {
  assert(b != 0);
  __int128 num = (static_cast<__int128>(a.hi) << 64) | static_cast<uint64_t>(a.lo);
  return static_cast<int64_t>(num / b);
}

// Linear series: base * L + slope * L * (L-1) / 2
inline Int128 linear_series(Int128 base, int64_t slope, int64_t len) {
  assert(len >= 0);
  if (len == 0) return make_i128(0, 0);
  
  Int128 base_sum = mul_i64_i64(base.lo + base.hi * INT64_MAX, len);
  // Simplified: assume base.hi is small
  __int128 slope_sum = static_cast<__int128>(slope) * len * (len - 1) / 2;
  
  Int128 result = add_i128(base_sum, make_i128(static_cast<int64_t>(slope_sum), static_cast<int64_t>(slope_sum >> 64)));
  return result;
}

// Volume event check
inline bool is_volume_event(int32_t event_type) {
  return event_type == 1 || event_type == 2 ||   // Order Buy/Sell
         event_type == 3 || event_type == 4 ||   // FPMM Buy/Sell
         event_type == 17 || event_type == 18 || event_type == 19 || // TransferIn*
         event_type == 20 || event_type == 21 || event_type == 22 || // TransferOut*
         event_type == 14 || event_type == 15;   // Redemption*
}

} // namespace

// ============================================================================
// update_feature_on_event - 更新特征张量
// ============================================================================

void update_feature_on_event(Stage3Runtime* rt, uint32_t user_idx, const EventInput& evt, const EventRecord& rec) {
  // Only update features for valid condition events
  if (evt.cond_idx < 0) return;
  
  const int64_t current_block = sort_key_to_block(evt.sort_key);
  const int32_t bucket = block_to_bucket(current_block);
  const int8_t tag_id = rec.tag_id;
  
  // Update both specific tag and global (-1) aggregation
  int8_t tags_to_update[2] = {tag_id, -1};
  
  for (int8_t tag : tags_to_update) {
    FeatureSlot* feat = feature_get_or_create(rt, user_idx, bucket, tag);
    
    // ========== Node-A0: 增量续算锚点更新 ==========
    const int64_t delta_blocks = current_block - feat->last_block_10w;
    
    if (delta_blocks > 0 && feat->last_block_10w > 0) {
      // Accumulate time-weighted sums from last event to current
      feat->time_weight_sum_10w += delta_blocks;
      feat->token_count_tw_sum_10w += feat->last_token_count_10w * delta_blocks;
      
      // exposure_tw_sum (HUGEINT)
      Int128 exp_contrib = mul_i64_i64(feat->last_exposure_10w, delta_blocks);
      Int128 exp_sum = make_i128(feat->exposure_tw_sum_10w_lo, feat->exposure_tw_sum_10w_hi);
      exp_sum = add_i128(exp_sum, exp_contrib);
      feat->exposure_tw_sum_10w_lo = exp_sum.lo;
      feat->exposure_tw_sum_10w_hi = exp_sum.hi;
      
      // holding_period_exp_tw_sum (HUGEINT)
      // Linear series: last_holding_period * last_exposure for each block
      Int128 hp_base = make_i128(feat->last_holding_period_10w_lo, feat->last_holding_period_10w_hi);
      Int128 hp_contrib = linear_series(mul_i64_i64(hp_base.lo, feat->last_exposure_10w), feat->last_exposure_10w, delta_blocks);
      Int128 hp_sum = make_i128(feat->holding_period_exp_tw_sum_10w_lo, feat->holding_period_exp_tw_sum_10w_hi);
      hp_sum = add_i128(hp_sum, hp_contrib);
      feat->holding_period_exp_tw_sum_10w_lo = hp_sum.lo;
      feat->holding_period_exp_tw_sum_10w_hi = hp_sum.hi;
    }
    
    // Update last values
    feat->last_sort_key_10w = evt.sort_key;
    feat->last_block_10w = current_block;
    feat->last_exposure_10w = rec.exposure;
    feat->last_holding_period_10w_lo = rec.holding_period;
    feat->last_holding_period_10w_hi = 0;
    feat->last_token_count_10w = rec.token_count;
    
    // ========== Node-A: 原子统计 ==========
    if (is_volume_event(evt.event_type)) {
      feat->volume_sum_10w += rec.volume;
    }
    
    // ========== Node-B: 归一化输出 ==========
    if (feat->time_weight_sum_10w > 0) {
      feat->token_avg_10w = feat->token_count_tw_sum_10w / feat->time_weight_sum_10w;
      
      Int128 exp_sum = make_i128(feat->exposure_tw_sum_10w_lo, feat->exposure_tw_sum_10w_hi);
      feat->exposure_avg_10w = div_i128_i64(exp_sum, feat->time_weight_sum_10w);
      
      feat->volume_10w = feat->volume_sum_10w;
      
      Int128 hp_sum = make_i128(feat->holding_period_exp_tw_sum_10w_lo, feat->holding_period_exp_tw_sum_10w_hi);
      Int128 exp_total = make_i128(feat->exposure_tw_sum_10w_lo, feat->exposure_tw_sum_10w_hi);
      if (exp_total.lo != 0 || exp_total.hi != 0) {
        // holding_period_avg = hp_exp_tw_sum / exposure_tw_sum
        __int128 hp_num = (static_cast<__int128>(hp_sum.hi) << 64) | static_cast<uint64_t>(hp_sum.lo);
        __int128 exp_denom = (static_cast<__int128>(exp_total.hi) << 64) | static_cast<uint64_t>(exp_total.lo);
        if (exp_denom != 0) {
          feat->holding_period_avg_10w = static_cast<int64_t>(hp_num / exp_denom);
        }
      }
    }
    
    // ========== Node-C: 前缀缓存 ==========
    // Need previous bucket's prefix
    FeatureSlot* prev_feat = feature_find(rt, user_idx, bucket - 1, tag);
    if (prev_feat) {
      feat->ps_token_avg_10w = prev_feat->ps_token_avg_10w + feat->token_avg_10w;
      feat->ps_exposure_avg_10w = prev_feat->ps_exposure_avg_10w + feat->exposure_avg_10w;
      feat->ps_volume_10w = prev_feat->ps_volume_10w + feat->volume_10w;
      feat->ps_holding_period_avg_10w = prev_feat->ps_holding_period_avg_10w + feat->holding_period_avg_10w;
    } else {
      feat->ps_token_avg_10w = feat->token_avg_10w;
      feat->ps_exposure_avg_10w = feat->exposure_avg_10w;
      feat->ps_volume_10w = feat->volume_10w;
      feat->ps_holding_period_avg_10w = feat->holding_period_avg_10w;
    }
    
    // ========== Node-D: 窗口投影 ==========
    // 100w = 10 buckets, 1000w = 100 buckets
    const int32_t bucket_count = bucket + 1;
    
    // 100w window
    FeatureSlot* feat_10_back = feature_find(rt, user_idx, bucket - 10, tag);
    int64_t ps_10_tok = feat_10_back ? feat_10_back->ps_token_avg_10w : 0;
    int64_t ps_10_exp = feat_10_back ? feat_10_back->ps_exposure_avg_10w : 0;
    int64_t ps_10_vol = feat_10_back ? feat_10_back->ps_volume_10w : 0;
    int64_t ps_10_hp = feat_10_back ? feat_10_back->ps_holding_period_avg_10w : 0;
    int32_t win_100 = std::min(10, bucket_count);
    if (win_100 > 0) {
      feat->token_avg_100w = (feat->ps_token_avg_10w - ps_10_tok) / win_100;
      feat->exposure_avg_100w = (feat->ps_exposure_avg_10w - ps_10_exp) / win_100;
      feat->volume_avg_100w = (feat->ps_volume_10w - ps_10_vol) / win_100;
      feat->holding_period_avg_100w = (feat->ps_holding_period_avg_10w - ps_10_hp) / win_100;
    }
    
    // 1000w window
    FeatureSlot* feat_100_back = feature_find(rt, user_idx, bucket - 100, tag);
    int64_t ps_100_tok = feat_100_back ? feat_100_back->ps_token_avg_10w : 0;
    int64_t ps_100_exp = feat_100_back ? feat_100_back->ps_exposure_avg_10w : 0;
    int64_t ps_100_vol = feat_100_back ? feat_100_back->ps_volume_10w : 0;
    int64_t ps_100_hp = feat_100_back ? feat_100_back->ps_holding_period_avg_10w : 0;
    int32_t win_1000 = std::min(100, bucket_count);
    if (win_1000 > 0) {
      feat->token_avg_1000w = (feat->ps_token_avg_10w - ps_100_tok) / win_1000;
      feat->exposure_avg_1000w = (feat->ps_exposure_avg_10w - ps_100_exp) / win_1000;
      feat->volume_avg_1000w = (feat->ps_volume_10w - ps_100_vol) / win_1000;
      feat->holding_period_avg_1000w = (feat->ps_holding_period_avg_10w - ps_100_hp) / win_1000;
    }
    
    feat->updated_sort_key = evt.sort_key;
  }
  
  // Update head_bucket
  if (bucket > rt->header->head_bucket) {
    rt->header->head_bucket = bucket;
  }
}

} // namespace stage3
