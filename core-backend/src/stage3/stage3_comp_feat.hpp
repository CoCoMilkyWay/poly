#pragma once

#include "../stage2/stage2_types.hpp"

#include <cstdint>

namespace stage3::feature_comp {

struct TokenSnapshot {
  double pos = 0.0;
  double cost = 0.0;
  double lp = 0.0;
  double entry_block = 0.0;
};

struct BucketAggState {
  __int128 exposure_tw_sum = 0;
  int64_t volume_sum = 0;
  __int128 holding_period_exp_tw_sum = 0;
  int64_t token_count_tw_sum = 0;
  int64_t time_weight_sum = 0;
  // Sharpe 原子统计：
  // r_i = Δpnl_i / RMS(exp_{i-1}, exp_i)
  // sum_r = Σr_i (1e6 标度)
  // sum_r2_over_dt = Σ(r_i² / Δt_i) (1e12 标度)
  // time_sum = ΣΔt_i
  int64_t sharpe_sum_r = 0;
  __int128 sharpe_sum_r2_over_dt = 0;
  int64_t sharpe_time_sum = 0;
  // Sharpe 续算锚点：
  // prev_* 是上一个已完成 block 采样点
  // pending_* 是当前未完成 block 的最新采样点
  // pending block / exposure 复用 last_block / last_exposure
  int64_t sharpe_prev_block = 0;
  int64_t sharpe_prev_pnl = 0;
  int64_t sharpe_prev_exposure = 0;
  int64_t sharpe_pending_pnl = 0;
  int64_t last_sort_key = 0;
  int64_t last_block = 0;
  int64_t last_exposure = 0;
  __int128 last_holding_exp = 0;
  int64_t last_token_count = 0;
  bool has_tail = false;
};

int64_t round_i64(double v);
int64_t sort_key_to_block(int64_t sort_key, int64_t sort_key_scale);
int64_t sort_key_to_block_bucket(int64_t sort_key, int64_t sort_key_scale, int64_t block_bucket_size);
int64_t bucket_end_block(int64_t block_bucket, int64_t block_bucket_size);

bool is_volume_event(stage2::EventType ty);
double calc_unrealized_pnl(const TokenSnapshot &st, double pos_epsilon);
int64_t calc_exposure_1e6(const TokenSnapshot &st, double pos_epsilon);
int64_t calc_holding_period_blocks(int64_t current_block, const TokenSnapshot &st, double pos_epsilon);
int64_t calc_volume_1e6(stage2::EventType ty, int64_t amount, int64_t price_1e6);

void update_tail_window(BucketAggState &agg,
                        int64_t block_bucket,
                        int64_t current_block,
                        int64_t current_exposure,
                        __int128 current_holding_exp,
                        int64_t current_token_count,
                        int64_t block_bucket_size);

void accumulate_sharpe_interval(BucketAggState &agg,
                                int64_t pnl_delta,
                                int64_t exposure_before,
                                int64_t exposure_after,
                                int64_t delta_t);

} // namespace stage3::feature_comp