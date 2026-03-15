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
  // 时间加权夏普统计：
  // return_sum = Σr_i (收益率和，1e6 标度)
  // return_tw_sum = Σ(r_i * Δt_i) (时间加权收益率和，1e6 标度)
  // return_sq_tw_sum = Σ(r_i² * Δt_i) (时间加权平方和，1e12 标度)
  // first_block = bucket 内首个事件 block
  int64_t return_sum = 0;
  __int128 return_tw_sum = 0;
  __int128 return_sq_tw_sum = 0;
  int64_t first_block = 0;
  __int128 exposure_tw_sum = 0;
  int64_t volume_sum = 0;
  __int128 holding_period_exp_tw_sum = 0;
  int64_t token_count_tw_sum = 0;
  int64_t time_weight_sum = 0;
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

// prev_block: 上一个事件的 block（用于计算 delta_t）
// 必须在调用 update_tail_window 之前保存 agg.last_block
void accumulate_event_delta(BucketAggState &agg, double realized_delta, int64_t exposure_before, int64_t volume, int64_t sort_key, int64_t current_block, int64_t prev_block);

} // namespace stage3::feature_comp