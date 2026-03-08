#include "stage3_comp_feat.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>

namespace stage3::feature_comp {

int64_t round_i64(double v) { return static_cast<int64_t>(std::llround(v)); }

int64_t sort_key_to_block(int64_t sort_key, int64_t sort_key_scale) {
  assert(sort_key >= 0);
  assert(sort_key_scale > 0);
  return sort_key / sort_key_scale;
}

int64_t sort_key_to_block_bucket(int64_t sort_key, int64_t sort_key_scale, int64_t block_bucket_size) {
  assert(block_bucket_size > 0);
  return sort_key_to_block(sort_key, sort_key_scale) / block_bucket_size;
}

int64_t bucket_end_block(int64_t block_bucket, int64_t block_bucket_size) {
  assert(block_bucket >= 0);
  assert(block_bucket_size > 0);
  return (block_bucket + 1) * block_bucket_size;
}

bool is_volume_event(stage2::EventType ty) {
  return ty == stage2::EventType::OrderBuy || ty == stage2::EventType::OrderSell ||
         ty == stage2::EventType::FPMMBuy || ty == stage2::EventType::FPMMSell ||
         ty == stage2::EventType::TransferInNegRisk || ty == stage2::EventType::TransferInOther ||
         ty == stage2::EventType::TransferInNonPoly || ty == stage2::EventType::TransferOutNegRisk ||
         ty == stage2::EventType::TransferOutOther || ty == stage2::EventType::TransferOutNonPoly ||
         ty == stage2::EventType::Redemption || ty == stage2::EventType::RedemptionNonPoly;
}

double calc_unrealized_pnl(const TokenSnapshot &st, double pos_epsilon) {
  if (std::abs(st.pos) <= pos_epsilon || st.lp <= 0.0) {
    return 0.0;
  }
  const double mtm = st.pos * st.lp / 1e6;
  return mtm - st.cost;
}

int64_t calc_exposure_1e6(const TokenSnapshot &st, double pos_epsilon) {
  if (std::abs(st.pos) <= pos_epsilon || st.lp <= 0.0) {
    return 0;
  }
  return round_i64(std::abs(st.pos) * st.lp / 1e6);
}

int64_t calc_holding_period_blocks(int64_t current_block, const TokenSnapshot &st, double pos_epsilon) {
  if (std::abs(st.pos) <= pos_epsilon || st.entry_block <= 0.0) {
    return 0;
  }
  const double hp = std::max(0.0, static_cast<double>(current_block) - st.entry_block);
  return round_i64(hp);
}

int64_t calc_volume_1e6(stage2::EventType ty, int64_t amount, int64_t price_1e6) {
  if (!is_volume_event(ty)) {
    return 0;
  }
  const double qty = std::abs(static_cast<double>(amount));
  const double px = std::abs(static_cast<double>(price_1e6)) / 1e6;
  return round_i64(qty * px);
}

void update_tail_window(BucketAggState &agg,
                        int64_t block_bucket,
                        int64_t current_block,
                        int64_t current_exposure,
                        int64_t current_holding_period,
                        int64_t current_token_count,
                        int64_t block_bucket_size) {
  assert(block_bucket >= 0);
  assert(block_bucket_size > 0);
  assert(current_block >= block_bucket * block_bucket_size);
  assert(current_block <= bucket_end_block(block_bucket, block_bucket_size));
  const int64_t end_block = bucket_end_block(block_bucket, block_bucket_size);
  if (agg.has_tail) {
    assert(agg.last_block >= block_bucket * block_bucket_size);
    assert(agg.last_block <= end_block);

    const int64_t old_tail = std::max<int64_t>(0, end_block - agg.last_block);
    agg.exposure_tw_sum -= agg.last_exposure * old_tail;
    agg.holding_period_tw_sum -= agg.last_holding_period * old_tail;
    agg.token_count_tw_sum -= agg.last_token_count * old_tail;
    agg.time_weight_sum -= old_tail;

    const int64_t delta_blocks = current_block - agg.last_block;
    assert(delta_blocks >= 0);
    agg.exposure_tw_sum += agg.last_exposure * delta_blocks;
    agg.holding_period_tw_sum += agg.last_holding_period * delta_blocks;
    agg.token_count_tw_sum += agg.last_token_count * delta_blocks;
    agg.time_weight_sum += delta_blocks;
  }

  const int64_t new_tail = std::max<int64_t>(0, end_block - current_block);
  agg.exposure_tw_sum += current_exposure * new_tail;
  agg.holding_period_tw_sum += current_holding_period * new_tail;
  agg.token_count_tw_sum += current_token_count * new_tail;
  agg.time_weight_sum += new_tail;

  agg.last_block = current_block;
  agg.last_exposure = current_exposure;
  agg.last_holding_period = current_holding_period;
  agg.last_token_count = current_token_count;
  agg.has_tail = true;
}

void accumulate_event_delta(BucketAggState &agg, double realized_delta, int64_t volume, int64_t sort_key) {
  agg.realized_sum += round_i64(realized_delta);
  {
    const long double sq = static_cast<long double>(realized_delta) * static_cast<long double>(realized_delta);
    const long double max_i64 = static_cast<long double>(std::numeric_limits<int64_t>::max());
    const int64_t sq_i64 = (sq >= max_i64) ? std::numeric_limits<int64_t>::max()
                                           : static_cast<int64_t>(std::llround(sq));
    if (sq_i64 > 0 && agg.realized_sq_sum > std::numeric_limits<int64_t>::max() - sq_i64) {
      agg.realized_sq_sum = std::numeric_limits<int64_t>::max();
    } else {
      agg.realized_sq_sum += sq_i64;
    }
  }
  static thread_local std::vector<float> kll_one(1, 0.0f);
  kll_one[0] = static_cast<float>(realized_delta);
  agg.realized_kll.addBatch(kll_one);
  agg.event_count += 1;
  agg.volume_sum += volume;
  agg.last_sort_key = sort_key;
}

} // namespace stage3::feature_comp