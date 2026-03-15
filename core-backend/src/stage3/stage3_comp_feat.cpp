#include "stage3_comp_feat.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace stage3::feature_comp {
namespace {

int64_t narrow_i64(__int128 v) {
  assert(v >= static_cast<__int128>(std::numeric_limits<int64_t>::min()));
  assert(v <= static_cast<__int128>(std::numeric_limits<int64_t>::max()));
  return static_cast<int64_t>(v);
}

__int128 linear_series_i128(__int128 base, int64_t slope, int64_t len) {
  assert(base >= 0);
  assert(slope >= 0);
  assert(len >= 0);
  const __int128 L = static_cast<__int128>(len);
  return static_cast<__int128>(base) * L + static_cast<__int128>(slope) * L * (L - 1) / 2;
}

int64_t calc_rms_cap_1e6(int64_t exposure_before, int64_t exposure_after) {
  assert(exposure_before >= 0);
  assert(exposure_after >= 0);
  const long double before = static_cast<long double>(exposure_before);
  const long double after = static_cast<long double>(exposure_after);
  const long double rms = std::sqrt((before * before + after * after) / 2.0L);
  return round_i64(static_cast<double>(rms));
}

} // namespace

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
                        __int128 current_holding_exp,
                        int64_t current_token_count,
                        int64_t block_bucket_size) {
  assert(block_bucket >= 0);
  assert(block_bucket_size > 0);
  assert(current_exposure >= 0);
  assert(current_holding_exp >= 0);
  assert(current_token_count >= 0);
  if (current_exposure == 0) {
    assert(current_holding_exp == 0);
  }
  assert(current_block >= block_bucket * block_bucket_size);
  assert(current_block <= bucket_end_block(block_bucket, block_bucket_size));
  const int64_t end_block = bucket_end_block(block_bucket, block_bucket_size);
  if (agg.has_tail) {
    assert(agg.last_block >= block_bucket * block_bucket_size);
    assert(agg.last_block <= end_block);
    assert(agg.last_exposure >= 0);
    assert(agg.last_holding_exp >= 0);
    assert(agg.last_token_count >= 0);
    if (agg.last_exposure == 0) {
      assert(agg.last_holding_exp == 0);
    }

    const int64_t old_tail = std::max<int64_t>(0, end_block - agg.last_block);
    agg.exposure_tw_sum -= static_cast<__int128>(agg.last_exposure) * old_tail;
    agg.holding_period_exp_tw_sum -= linear_series_i128(agg.last_holding_exp, agg.last_exposure, old_tail);
    agg.token_count_tw_sum = narrow_i64(
        static_cast<__int128>(agg.token_count_tw_sum) - static_cast<__int128>(agg.last_token_count) * old_tail);
    agg.time_weight_sum = narrow_i64(static_cast<__int128>(agg.time_weight_sum) - old_tail);

    const int64_t delta_blocks = current_block - agg.last_block;
    assert(delta_blocks >= 0);
    agg.exposure_tw_sum += static_cast<__int128>(agg.last_exposure) * delta_blocks;
    agg.holding_period_exp_tw_sum += linear_series_i128(agg.last_holding_exp, agg.last_exposure, delta_blocks);
    agg.token_count_tw_sum = narrow_i64(
        static_cast<__int128>(agg.token_count_tw_sum) + static_cast<__int128>(agg.last_token_count) * delta_blocks);
    agg.time_weight_sum = narrow_i64(static_cast<__int128>(agg.time_weight_sum) + delta_blocks);
  }

  const int64_t new_tail = std::max<int64_t>(0, end_block - current_block);
  agg.exposure_tw_sum += static_cast<__int128>(current_exposure) * new_tail;
  agg.holding_period_exp_tw_sum += linear_series_i128(current_holding_exp, current_exposure, new_tail);
  agg.token_count_tw_sum = narrow_i64(
      static_cast<__int128>(agg.token_count_tw_sum) + static_cast<__int128>(current_token_count) * new_tail);
  agg.time_weight_sum = narrow_i64(static_cast<__int128>(agg.time_weight_sum) + new_tail);

  agg.last_block = current_block;
  agg.last_exposure = current_exposure;
  agg.last_holding_exp = current_holding_exp;
  agg.last_token_count = current_token_count;
  agg.has_tail = true;
}

void accumulate_sharpe_interval(BucketAggState &agg,
                                int64_t pnl_delta,
                                int64_t exposure_before,
                                int64_t exposure_after,
                                int64_t delta_t) {
  assert(delta_t > 0);
  constexpr int64_t kMinCap = 1000; // 0.001 USD
  const int64_t cap = calc_rms_cap_1e6(exposure_before, exposure_after);
  if (cap < kMinCap) {
    return;
  }

  const long double return_rate =
      static_cast<long double>(pnl_delta) / static_cast<long double>(cap);
  const int64_t return_rate_1e6 = round_i64(static_cast<double>(return_rate * 1e6L));
  agg.sharpe_sum_r = narrow_i64(static_cast<__int128>(agg.sharpe_sum_r) + return_rate_1e6);

  const long double sq_over_dt =
      static_cast<long double>(return_rate_1e6) * static_cast<long double>(return_rate_1e6) /
      static_cast<long double>(delta_t);
  agg.sharpe_sum_r2_over_dt += static_cast<__int128>(sq_over_dt + 0.5L);
  agg.sharpe_time_sum =
      narrow_i64(static_cast<__int128>(agg.sharpe_time_sum) + static_cast<__int128>(delta_t));
}

} // namespace stage3::feature_comp