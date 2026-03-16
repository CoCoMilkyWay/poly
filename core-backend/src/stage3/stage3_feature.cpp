#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace stage3 {

namespace {

inline int64_t i64_narrow_checked(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

inline __int128 i128_from_parts(int64_t lo, int64_t hi) {
  return (static_cast<__int128>(hi) << 64) | static_cast<uint64_t>(lo);
}

inline void i128_to_parts(__int128 v, int64_t &lo, int64_t &hi) {
  lo = static_cast<int64_t>(static_cast<uint64_t>(v));
  hi = static_cast<int64_t>(v >> 64);
}

inline long double i128_to_long_double(__int128 v) {
  constexpr long double kTwoPow64 = 18446744073709551616.0L;
  const int64_t hi = static_cast<int64_t>(v >> 64);
  const uint64_t lo = static_cast<uint64_t>(v);
  return static_cast<long double>(hi) * kTwoPow64 + static_cast<long double>(lo);
}

inline int64_t round_i64(long double v) {
  return static_cast<int64_t>(std::llround(v));
}

inline __int128 linear_series_i128(__int128 base, int64_t slope, int64_t len) {
  assert(base >= 0);
  assert(slope >= 0);
  assert(len >= 0);
  const __int128 L = static_cast<__int128>(len);
  return base * L + static_cast<__int128>(slope) * L * (L - 1) / 2;
}

inline bool is_volume_event(int32_t event_type) {
  return event_type == EVT_ORDER_BUY ||
         event_type == EVT_ORDER_SELL ||
         event_type == EVT_FPMM_BUY ||
         event_type == EVT_FPMM_SELL ||
         event_type == EVT_REDEMPTION ||
         event_type == EVT_REDEMPTION_NON_POLY ||
         event_type == EVT_TRANSFER_IN_NEGRISK ||
         event_type == EVT_TRANSFER_IN_OTHER ||
         event_type == EVT_TRANSFER_IN_NON_POLY ||
         event_type == EVT_TRANSFER_OUT_NEGRISK ||
         event_type == EVT_TRANSFER_OUT_OTHER ||
         event_type == EVT_TRANSFER_OUT_NON_POLY;
}

FeatureSlot *feature_find_le(Stage3Runtime *rt, uint32_t user_idx, int32_t target_bucket, int8_t tag_id) {
  if (target_bucket < 0) {
    return nullptr;
  }
  FeatureSlot *best = nullptr;
  uint32_t idx = rt->users[user_idx].feature_head;
  while (idx != NULL_IDX) {
    FeatureSlot *feat = &rt->feature_pool[idx];
    if ((feat->flags & 1) && feat->tag_id == tag_id && feat->bucket <= target_bucket) {
      if (!best || feat->bucket > best->bucket) {
        best = feat;
      }
    }
    idx = feat->next;
  }
  return best;
}

void update_tail_window(FeatureSlot *feat,
                        int32_t block_bucket,
                        int64_t current_block,
                        int64_t current_exposure,
                        __int128 current_holding_exp,
                        int64_t current_token_count) {
  assert(block_bucket >= 0);
  assert(current_exposure >= 0);
  assert(current_holding_exp >= 0);
  assert(current_token_count >= 0);
  if (current_exposure == 0) {
    assert(current_holding_exp == 0);
  }

  const int64_t bucket_start = static_cast<int64_t>(block_bucket) * BLOCK_BUCKET_SIZE;
  const int64_t bucket_end = bucket_end_block(block_bucket);
  assert(current_block >= bucket_start);
  assert(current_block <= bucket_end);

  __int128 exposure_tw_sum = i128_from_parts(feat->exposure_tw_sum_10w_lo, feat->exposure_tw_sum_10w_hi);
  __int128 holding_period_exp_tw_sum = i128_from_parts(feat->holding_period_exp_tw_sum_10w_lo,
                                                       feat->holding_period_exp_tw_sum_10w_hi);
  __int128 token_count_tw_sum = feat->token_count_tw_sum_10w;
  __int128 time_weight_sum = feat->time_weight_sum_10w;

  const bool has_tail = feat->time_weight_sum_10w > 0;
  if (has_tail) {
    assert(feat->last_block_10w >= bucket_start);
    assert(feat->last_block_10w <= bucket_end);
    assert(feat->last_exposure_10w >= 0);
    assert(feat->last_token_count_10w >= 0);

    const __int128 last_holding_exp = i128_from_parts(feat->last_holding_period_10w_lo,
                                                      feat->last_holding_period_10w_hi);
    if (feat->last_exposure_10w == 0) {
      assert(last_holding_exp == 0);
    }

    const int64_t old_tail = std::max<int64_t>(0, bucket_end - feat->last_block_10w);
    exposure_tw_sum -= static_cast<__int128>(feat->last_exposure_10w) * old_tail;
    holding_period_exp_tw_sum -= linear_series_i128(last_holding_exp, feat->last_exposure_10w, old_tail);
    token_count_tw_sum -= static_cast<__int128>(feat->last_token_count_10w) * old_tail;
    time_weight_sum -= old_tail;

    const int64_t delta_blocks = current_block - feat->last_block_10w;
    assert(delta_blocks >= 0);
    exposure_tw_sum += static_cast<__int128>(feat->last_exposure_10w) * delta_blocks;
    holding_period_exp_tw_sum += linear_series_i128(last_holding_exp, feat->last_exposure_10w, delta_blocks);
    token_count_tw_sum += static_cast<__int128>(feat->last_token_count_10w) * delta_blocks;
    time_weight_sum += delta_blocks;
  }

  const int64_t new_tail = std::max<int64_t>(0, bucket_end - current_block);
  exposure_tw_sum += static_cast<__int128>(current_exposure) * new_tail;
  holding_period_exp_tw_sum += linear_series_i128(current_holding_exp, current_exposure, new_tail);
  token_count_tw_sum += static_cast<__int128>(current_token_count) * new_tail;
  time_weight_sum += new_tail;

  feat->time_weight_sum_10w = i64_narrow_checked(time_weight_sum);
  feat->token_count_tw_sum_10w = i64_narrow_checked(token_count_tw_sum);
  i128_to_parts(exposure_tw_sum, feat->exposure_tw_sum_10w_lo, feat->exposure_tw_sum_10w_hi);
  i128_to_parts(holding_period_exp_tw_sum,
                feat->holding_period_exp_tw_sum_10w_lo,
                feat->holding_period_exp_tw_sum_10w_hi);
  feat->last_block_10w = current_block;
  feat->last_exposure_10w = current_exposure;
  i128_to_parts(current_holding_exp, feat->last_holding_period_10w_lo, feat->last_holding_period_10w_hi);
  feat->last_token_count_10w = current_token_count;
}

} // namespace

// ============================================================================
// update_feature_on_event - 更新特征张量
// ============================================================================

void update_feature_on_event(Stage3Runtime *rt,
                             uint32_t user_idx,
                             const EventInput &evt,
                             const EventRecord &rec,
                             const FeatureRuntimeState &tag_state,
                             const FeatureRuntimeState &global_state) {
  // Only update features for valid condition events
  if (evt.cond_idx < 0)
    return;

  const int64_t current_block = sort_key_to_block(evt.sort_key);
  const int32_t bucket = block_to_bucket(current_block);
  const int8_t tag_id = rec.tag_id;

  const auto update_single_tag = [&](int8_t tag, const FeatureRuntimeState &state) {
    FeatureSlot *feat = feature_get_or_create(rt, user_idx, bucket, tag);
    assert(state.exposure >= 0);
    assert(state.token_count >= 0);

    __int128 holding_exp = 0;
    if (state.exposure > 0) {
      const __int128 computed =
          static_cast<__int128>(current_block) * state.exposure - state.exposure_entry_sum;
      assert(computed >= 0);
      holding_exp = computed;
    } else {
      assert(state.exposure_entry_sum == 0);
    }

    update_tail_window(feat, bucket, current_block, state.exposure, holding_exp, state.token_count);
    feat->last_sort_key_10w = evt.sort_key;

    if (is_volume_event(evt.event_type)) {
      feat->volume_sum_10w = i64_narrow_checked(static_cast<__int128>(feat->volume_sum_10w) + rec.volume);
    }

    if (feat->time_weight_sum_10w > 0) {
      const long double tw = static_cast<long double>(feat->time_weight_sum_10w);
      feat->token_avg_10w = round_i64(static_cast<long double>(feat->token_count_tw_sum_10w) / tw);

      const __int128 exposure_tw_sum = i128_from_parts(feat->exposure_tw_sum_10w_lo, feat->exposure_tw_sum_10w_hi);
      feat->exposure_avg_10w = round_i64(i128_to_long_double(exposure_tw_sum) / tw);
      feat->volume_10w = feat->volume_sum_10w;

      if (exposure_tw_sum > 0) {
        const __int128 holding_tw_sum =
            i128_from_parts(feat->holding_period_exp_tw_sum_10w_lo, feat->holding_period_exp_tw_sum_10w_hi);
        feat->holding_period_avg_10w = round_i64(i128_to_long_double(holding_tw_sum) / i128_to_long_double(exposure_tw_sum));
      } else {
        feat->holding_period_avg_10w = 0;
      }
    } else {
      feat->token_avg_10w = 0;
      feat->exposure_avg_10w = 0;
      feat->volume_10w = 0;
      feat->holding_period_avg_10w = 0;
    }

    FeatureSlot *prev_feat = feature_find_le(rt, user_idx, bucket - 1, tag);
    if (prev_feat) {
      feat->ps_token_avg_10w = i64_narrow_checked(static_cast<__int128>(prev_feat->ps_token_avg_10w) + feat->token_avg_10w);
      feat->ps_exposure_avg_10w = i64_narrow_checked(static_cast<__int128>(prev_feat->ps_exposure_avg_10w) + feat->exposure_avg_10w);
      feat->ps_volume_10w = i64_narrow_checked(static_cast<__int128>(prev_feat->ps_volume_10w) + feat->volume_10w);
      feat->ps_holding_period_avg_10w =
          i64_narrow_checked(static_cast<__int128>(prev_feat->ps_holding_period_avg_10w) + feat->holding_period_avg_10w);
    } else {
      feat->ps_token_avg_10w = feat->token_avg_10w;
      feat->ps_exposure_avg_10w = feat->exposure_avg_10w;
      feat->ps_volume_10w = feat->volume_10w;
      feat->ps_holding_period_avg_10w = feat->holding_period_avg_10w;
    }

    const int32_t bucket_count = bucket + 1;
    const int32_t win_100 = std::min(10, bucket_count);
    const int32_t win_1000 = std::min(100, bucket_count);

    FeatureSlot *feat_10_back = feature_find_le(rt, user_idx, bucket - 10, tag);
    const int64_t ps_10_tok = feat_10_back ? feat_10_back->ps_token_avg_10w : 0;
    const int64_t ps_10_exp = feat_10_back ? feat_10_back->ps_exposure_avg_10w : 0;
    const int64_t ps_10_vol = feat_10_back ? feat_10_back->ps_volume_10w : 0;
    const int64_t ps_10_hp = feat_10_back ? feat_10_back->ps_holding_period_avg_10w : 0;

    if (win_100 > 0) {
      feat->token_avg_100w = round_i64(static_cast<long double>(feat->ps_token_avg_10w - ps_10_tok) / win_100);
      feat->exposure_avg_100w = round_i64(static_cast<long double>(feat->ps_exposure_avg_10w - ps_10_exp) / win_100);
      feat->volume_avg_100w = round_i64(static_cast<long double>(feat->ps_volume_10w - ps_10_vol) / win_100);
      feat->holding_period_avg_100w =
          round_i64(static_cast<long double>(feat->ps_holding_period_avg_10w - ps_10_hp) / win_100);
    } else {
      feat->token_avg_100w = 0;
      feat->exposure_avg_100w = 0;
      feat->volume_avg_100w = 0;
      feat->holding_period_avg_100w = 0;
    }

    FeatureSlot *feat_100_back = feature_find_le(rt, user_idx, bucket - 100, tag);
    const int64_t ps_100_tok = feat_100_back ? feat_100_back->ps_token_avg_10w : 0;
    const int64_t ps_100_exp = feat_100_back ? feat_100_back->ps_exposure_avg_10w : 0;
    const int64_t ps_100_vol = feat_100_back ? feat_100_back->ps_volume_10w : 0;
    const int64_t ps_100_hp = feat_100_back ? feat_100_back->ps_holding_period_avg_10w : 0;

    if (win_1000 > 0) {
      feat->token_avg_1000w = round_i64(static_cast<long double>(feat->ps_token_avg_10w - ps_100_tok) / win_1000);
      feat->exposure_avg_1000w = round_i64(static_cast<long double>(feat->ps_exposure_avg_10w - ps_100_exp) / win_1000);
      feat->volume_avg_1000w = round_i64(static_cast<long double>(feat->ps_volume_10w - ps_100_vol) / win_1000);
      feat->holding_period_avg_1000w =
          round_i64(static_cast<long double>(feat->ps_holding_period_avg_10w - ps_100_hp) / win_1000);
    } else {
      feat->token_avg_1000w = 0;
      feat->exposure_avg_1000w = 0;
      feat->volume_avg_1000w = 0;
      feat->holding_period_avg_1000w = 0;
    }

    feat->updated_sort_key = evt.sort_key;
  };

  update_single_tag(tag_id, tag_state);
  update_single_tag(-1, global_state);

  // Update head_bucket
  if (bucket > rt->header->head_bucket) {
    rt->header->head_bucket = bucket;
  }
}

} // namespace stage3
