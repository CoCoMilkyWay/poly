#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

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

struct CarryState {
  int64_t exposure = 0;
  __int128 holding_exp = 0;
  int64_t token_count = 0;
};

CarryState carry_state_for_bucket_start(const FeatureSlot *prev_feat, int32_t bucket) {
  CarryState carry{};
  if (prev_feat == nullptr) {
    return carry;
  }

  carry.exposure = prev_feat->last_exposure_10w;
  carry.token_count = prev_feat->last_token_count_10w;
  if (carry.exposure == 0) {
    return carry;
  }

  const __int128 last_holding_exp =
      i128_from_parts(prev_feat->last_holding_period_10w_lo, prev_feat->last_holding_period_10w_hi);
  const int64_t bucket_start = static_cast<int64_t>(bucket) * BLOCK_BUCKET_SIZE;
  assert(bucket_start >= prev_feat->last_block_10w);
  carry.holding_exp =
      last_holding_exp + static_cast<__int128>(carry.exposure) * (bucket_start - prev_feat->last_block_10w);
  return carry;
}

CarryState advance_carry_one_bucket(const CarryState &carry) {
  CarryState next = carry;
  if (next.exposure > 0) {
    next.holding_exp += static_cast<__int128>(next.exposure) * BLOCK_BUCKET_SIZE;
  }
  return next;
}

void seed_feature_full_bucket_from_carry(FeatureSlot *feat,
                                         int32_t bucket,
                                         const CarryState &carry) {
  const int64_t bucket_start = static_cast<int64_t>(bucket) * BLOCK_BUCKET_SIZE;
  const int64_t bucket_len = BLOCK_BUCKET_SIZE;
  feat->last_block_10w = bucket_start;
  feat->last_exposure_10w = carry.exposure;
  i128_to_parts(carry.holding_exp, feat->last_holding_period_10w_lo, feat->last_holding_period_10w_hi);
  feat->last_token_count_10w = carry.token_count;
  feat->time_weight_sum_10w = bucket_len;
  feat->token_count_tw_sum_10w =
      i64_narrow_checked(static_cast<__int128>(carry.token_count) * bucket_len);
  i128_to_parts(static_cast<__int128>(carry.exposure) * bucket_len,
                feat->exposure_tw_sum_10w_lo,
                feat->exposure_tw_sum_10w_hi);
  i128_to_parts(linear_series_i128(carry.holding_exp, carry.exposure, bucket_len),
                feat->holding_period_exp_tw_sum_10w_lo,
                feat->holding_period_exp_tw_sum_10w_hi);
  feat->volume_sum_10w = 0;
}

inline uint32_t feature_idx_from_ptr(Stage3Runtime *rt, const FeatureSlot *feat) {
  assert(feat != nullptr);
  return static_cast<uint32_t>(feat - rt->feature_pool);
}

void refresh_feature_outputs(Stage3Runtime *rt,
                             uint32_t user_idx,
                             int8_t tag,
                             FeatureSlot *feat,
                             const FeatureSlot *prev_feat,
                             int32_t first_bucket) {
  assert(first_bucket >= 0);
  assert(feat->bucket >= first_bucket);
  if (feat->time_weight_sum_10w > 0) {
    const long double tw = static_cast<long double>(feat->time_weight_sum_10w);
    feat->token_avg_10w = round_i64(static_cast<long double>(feat->token_count_tw_sum_10w) / tw);

    const __int128 exposure_tw_sum =
        i128_from_parts(feat->exposure_tw_sum_10w_lo, feat->exposure_tw_sum_10w_hi);
    feat->exposure_avg_10w = round_i64(i128_to_long_double(exposure_tw_sum) / tw);
    feat->volume_10w = feat->volume_sum_10w;

    if (exposure_tw_sum > 0) {
      const __int128 holding_tw_sum =
          i128_from_parts(feat->holding_period_exp_tw_sum_10w_lo, feat->holding_period_exp_tw_sum_10w_hi);
      feat->holding_period_avg_10w =
          round_i64(i128_to_long_double(holding_tw_sum) / i128_to_long_double(exposure_tw_sum));
    } else {
      feat->holding_period_avg_10w = 0;
    }
  } else {
    feat->token_avg_10w = 0;
    feat->exposure_avg_10w = 0;
    feat->volume_10w = 0;
    feat->holding_period_avg_10w = 0;
  }

  if (prev_feat) {
    feat->ps_token_avg_10w =
        i64_narrow_checked(static_cast<__int128>(prev_feat->ps_token_avg_10w) + feat->token_avg_10w);
    feat->ps_exposure_avg_10w =
        i64_narrow_checked(static_cast<__int128>(prev_feat->ps_exposure_avg_10w) + feat->exposure_avg_10w);
    feat->ps_volume_10w =
        i64_narrow_checked(static_cast<__int128>(prev_feat->ps_volume_10w) + feat->volume_10w);
    feat->ps_holding_period_avg_10w =
        i64_narrow_checked(static_cast<__int128>(prev_feat->ps_holding_period_avg_10w) + feat->holding_period_avg_10w);
  } else {
    feat->ps_token_avg_10w = feat->token_avg_10w;
    feat->ps_exposure_avg_10w = feat->exposure_avg_10w;
    feat->ps_volume_10w = feat->volume_10w;
    feat->ps_holding_period_avg_10w = feat->holding_period_avg_10w;
  }

  const int32_t bucket_count = feat->bucket - first_bucket + 1;
  assert(bucket_count > 0);
  const int32_t win_100 = std::min(10, bucket_count);
  const int32_t win_1000 = std::min(100, bucket_count);

  FeatureSlot *feat_10_back = (bucket_count > 10) ? feature_find(rt, user_idx, feat->bucket - 10, tag) : nullptr;
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

  FeatureSlot *feat_100_back =
      (bucket_count > 100) ? feature_find(rt, user_idx, feat->bucket - 100, tag) : nullptr;
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
}

FeatureSlot *prepare_feature_bucket(Stage3Runtime *rt,
                                    uint32_t user_idx,
                                    int32_t bucket,
                                    int8_t tag,
                                    int32_t &first_bucket,
                                    int32_t &latest_bucket,
                                    uint32_t &latest_idx) {
  assert(bucket >= 0);
  assert(latest_bucket <= bucket || latest_bucket < 0);

  if (latest_idx != NULL_IDX && latest_bucket == bucket) {
    return &rt->feature_pool[latest_idx];
  }

  const FeatureSlot *prev_feat =
      (latest_idx != NULL_IDX) ? &rt->feature_pool[latest_idx] : nullptr;
  CarryState carry =
      prev_feat ? carry_state_for_bucket_start(prev_feat, latest_bucket + 1) : CarryState{};
  const int32_t start_bucket = (latest_bucket >= 0) ? (latest_bucket + 1) : bucket;

  for (int32_t materialized_bucket = start_bucket; materialized_bucket <= bucket; ++materialized_bucket) {
    FeatureSlot *feat = feature_get_or_create(rt, user_idx, materialized_bucket, tag);
    if (feat->time_weight_sum_10w == 0) {
      seed_feature_full_bucket_from_carry(feat, materialized_bucket, carry);
    }
    if (first_bucket < 0) {
      first_bucket = materialized_bucket;
    }
    refresh_feature_outputs(rt, user_idx, tag, feat, prev_feat, first_bucket);
    latest_bucket = materialized_bucket;
    latest_idx = feature_idx_from_ptr(rt, feat);
    prev_feat = feat;
    carry = advance_carry_one_bucket(carry);
  }

  assert(latest_idx != NULL_IDX);
  assert(latest_bucket == bucket);
  return &rt->feature_pool[latest_idx];
}

} // namespace

// ============================================================================
// update_feature_on_event - 更新特征张量
// ============================================================================

void init_feature_timelines(Stage3Runtime *rt,
                            uint32_t user_idx,
                            std::array<int32_t, FEATURE_TAG_SLOT_COUNT> &first_buckets,
                            std::array<int32_t, FEATURE_TAG_SLOT_COUNT> &latest_buckets,
                            std::array<uint32_t, FEATURE_TAG_SLOT_COUNT> *latest_indices) {
  first_buckets.fill(-1);
  latest_buckets.fill(-1);
  if (latest_indices != nullptr) {
    latest_indices->fill(NULL_IDX);
  }
  if (user_idx >= rt->header->user_count) {
    return;
  }

  uint32_t idx = rt->users[user_idx].feature_head;
  while (idx != NULL_IDX) {
    FeatureSlot *feat = &rt->feature_pool[idx];
    if (feat->flags & 1) {
      const size_t slot = tag_slot(feat->tag_id);
      if (first_buckets[slot] < 0 || feat->bucket < first_buckets[slot]) {
        first_buckets[slot] = feat->bucket;
      }
      if (feat->bucket > latest_buckets[slot]) {
        latest_buckets[slot] = feat->bucket;
        if (latest_indices != nullptr) {
          (*latest_indices)[slot] = idx;
        }
      }
    }
    idx = feat->next;
  }
}

void update_feature_on_event(Stage3Runtime *rt,
                             uint32_t user_idx,
                             int64_t current_block,
                             int32_t bucket,
                             const EventInput &evt,
                             const EventRecord &rec,
                             const FeatureRuntimeState &tag_state,
                             const FeatureRuntimeState &global_state,
                             std::array<int32_t, FEATURE_TAG_SLOT_COUNT> &first_buckets,
                             std::array<int32_t, FEATURE_TAG_SLOT_COUNT> &latest_buckets,
                             std::array<uint32_t, FEATURE_TAG_SLOT_COUNT> &latest_indices) {
  // Only update features for valid condition events
  if (evt.cond_idx < 0)
    return;

  const int8_t tag_id = rec.tag_id;

  const auto update_single_tag = [&](int8_t tag, const FeatureRuntimeState &state) {
    const size_t slot = tag_slot(tag);
    FeatureSlot *feat = prepare_feature_bucket(
        rt, user_idx, bucket, tag, first_buckets[slot], latest_buckets[slot], latest_indices[slot]);
    assert(state.exposure >= 0);
    assert(state.token_count >= 0);
    assert(first_buckets[slot] >= 0);
    assert(latest_buckets[slot] == bucket);

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

    if (is_volume_event(evt.event_type)) {
      feat->volume_sum_10w = i64_narrow_checked(static_cast<__int128>(feat->volume_sum_10w) + rec.volume);
    }
    const FeatureSlot *prev_feat =
        (bucket > first_buckets[slot]) ? feature_find(rt, user_idx, bucket - 1, tag) : nullptr;
    refresh_feature_outputs(rt, user_idx, tag, feat, prev_feat, first_buckets[slot]);
  };

  update_single_tag(tag_id, tag_state);
  update_single_tag(-1, global_state);
}

void prepare_feature_buckets_for_mask(Stage3Runtime *rt,
                                      uint32_t user_idx,
                                      int32_t bucket,
                                      uint16_t tag_mask,
                                      std::array<int32_t, FEATURE_TAG_SLOT_COUNT> &first_buckets,
                                      std::array<int32_t, FEATURE_TAG_SLOT_COUNT> &latest_buckets,
                                      std::array<uint32_t, FEATURE_TAG_SLOT_COUNT> &latest_indices) {
  for (size_t slot = 0; slot < FEATURE_TAG_SLOT_COUNT; ++slot) {
    if ((tag_mask & (static_cast<uint16_t>(1u << slot))) == 0) {
      continue;
    }
    const int8_t tag_id = static_cast<int8_t>(static_cast<int>(slot) - 1);
    prepare_feature_bucket(
        rt, user_idx, bucket, tag_id, first_buckets[slot], latest_buckets[slot], latest_indices[slot]);
  }
}

} // namespace stage3
