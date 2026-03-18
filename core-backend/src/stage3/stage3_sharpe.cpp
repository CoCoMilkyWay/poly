#include "stage3.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>

namespace stage3 {

// ============================================================================
// update_sharpe_on_event - 记录 Sharpe 点
// ============================================================================

void update_sharpe_on_event(Stage3Runtime *rt,
                            uint32_t user_idx,
                            int64_t exposure,
                            int64_t prev_exposure,
                            int64_t pnl,
                            int64_t prev_pnl,
                            int32_t bucket,
                            int32_t block_offset) {
  UserBlock *user = &rt->users[user_idx];
  if (user->sharpe_bucket_head == NULL_IDX) {
    user->pnl_before_first_sharpe_bucket = prev_pnl;
    user->exposure_before_first_sharpe_bucket = prev_exposure;
  }

  SharpeBucket *bucket_node = sharpe_bucket_get_or_create(rt, user_idx, bucket);
  uint32_t point_idx = sharpe_point_alloc(rt, user_idx);
  SharpePoint *point = &rt->sharpe_point_pool[point_idx];
  point->next = NULL_IDX;
  point->block_offset = block_offset;
  point->exposure = exposure;
  point->pnl = pnl;

  if (bucket_node->point_tail == NULL_IDX) {
    bucket_node->point_head = point_idx;
    bucket_node->point_tail = point_idx;
  } else {
    rt->sharpe_point_pool[bucket_node->point_tail].next = point_idx;
    bucket_node->point_tail = point_idx;
  }
  bucket_node->close_pnl = pnl;
  bucket_node->close_exposure = exposure;
}

// ============================================================================
// Sharpe calculation helpers
// ============================================================================

namespace {

constexpr size_t MIN_SHARPE_POINT_COUNT = 10;
constexpr long double SHARPE_RETURN_EPS = 1.0L;
constexpr long double SHARPE_FIT_EPS = 1.0L;

struct SharpePointView {
  int64_t block;
  int64_t exposure;
  int64_t pnl;
};

struct CollectedSharpePoints {
  int64_t p0 = 0;
  int64_t e0 = 0;
  std::vector<SharpePointView> points;
};

long double calc_pnl_shift(const std::vector<SharpePointView> &points, int64_t p0) {
  long double min_pnl0 = 0.0L;
  for (const auto &point : points) {
    min_pnl0 = std::min(min_pnl0, static_cast<long double>(point.pnl - p0));
  }
  return std::fabs(min_pnl0);
}

long double eval_quadratic(const std::array<long double, 3> &coef, long double x) {
  return coef[0] + coef[1] * x + coef[2] * x * x;
}

std::array<long double, 3> fit_quadratic(const std::vector<SharpePointView> &points, int64_t start_block, int64_t p0) {
  const long double shift = calc_pnl_shift(points, p0);

  long double sx0 = 0.0L;
  long double sx1 = 0.0L;
  long double sx2 = 0.0L;
  long double sx3 = 0.0L;
  long double sx4 = 0.0L;
  long double sy = 0.0L;
  long double sxy = 0.0L;
  long double sx2y = 0.0L;
  for (const auto &point : points) {
    const long double x = static_cast<long double>(point.block - start_block);
    const long double y = static_cast<long double>(point.pnl - p0) + shift;
    const long double x2 = x * x;
    sx0 += 1.0L;
    sx1 += x;
    sx2 += x2;
    sx3 += x2 * x;
    sx4 += x2 * x2;
    sy += y;
    sxy += x * y;
    sx2y += x2 * y;
  }

  std::array<std::array<long double, 4>, 3> mat{{
      {{sx0, sx1, sx2, sy}},
      {{sx1, sx2, sx3, sxy}},
      {{sx2, sx3, sx4, sx2y}},
  }};

  for (int32_t col = 0; col < 3; ++col) {
    int32_t pivot = col;
    for (int32_t row = col + 1; row < 3; ++row) {
      if (std::fabs(mat[row][col]) > std::fabs(mat[pivot][col])) {
        pivot = row;
      }
    }
    if (std::fabs(mat[pivot][col]) <= 1e-18L) {
      return {sx0 > 0.0L ? sy / sx0 : 0.0L, 0.0L, 0.0L};
    }
    if (pivot != col) {
      std::swap(mat[pivot], mat[col]);
    }
    const long double base = mat[col][col];
    for (int32_t k = col; k < 4; ++k) {
      mat[col][k] /= base;
    }
    for (int32_t row = 0; row < 3; ++row) {
      if (row == col) {
        continue;
      }
      const long double factor = mat[row][col];
      for (int32_t k = col; k < 4; ++k) {
        mat[row][k] -= factor * mat[col][k];
      }
    }
  }

  return {mat[0][3], mat[1][3], mat[2][3]};
}

long double calc_curve_penalty(const std::vector<SharpePointView> &points, int64_t start_block, int64_t p0) {
  if (points.empty()) {
    return 0.0L;
  }

  const long double shift = calc_pnl_shift(points, p0);
  const auto coef = fit_quadratic(points, start_block, p0);

  long double sum_penalty = 0.0L;
  for (const auto &point : points) {
    const long double x = static_cast<long double>(point.block - start_block);
    const long double y = static_cast<long double>(point.pnl - p0) + shift;
    const long double fit_y = eval_quadratic(coef, x);
    const long double denom = std::max(fit_y, SHARPE_FIT_EPS);
    const long double rho = std::fabs(y - fit_y) / denom;
    const long double rho2 = rho * rho;
    sum_penalty += rho2 / (1.0L + rho2);
  }
  return sum_penalty / static_cast<long double>(points.size());
}

CollectedSharpePoints collect_sharpe_points(Stage3Runtime *rt, uint32_t user_idx, int32_t start_bucket, int32_t end_bucket) {
  CollectedSharpePoints result{};
  UserBlock *user = &rt->users[user_idx];

  int32_t best_bucket = std::numeric_limits<int32_t>::min();
  result.p0 = user->pnl_before_first_sharpe_bucket;
  result.e0 = user->exposure_before_first_sharpe_bucket;

  uint32_t idx = user->sharpe_bucket_head;
  while (idx != NULL_IDX) {
    const SharpeBucket *bucket = &rt->sharpe_bucket_pool[idx];
    if (bucket->bucket < start_bucket && bucket->bucket > best_bucket) {
      best_bucket = bucket->bucket;
      result.p0 = bucket->close_pnl;
      result.e0 = bucket->close_exposure;
    }
    idx = bucket->next;
  }

  for (int32_t bucket = start_bucket; bucket <= end_bucket; ++bucket) {
    const SharpeBucket *node = sharpe_bucket_find(rt, user_idx, bucket);
    if (!node) {
      continue;
    }
    uint32_t point_idx = node->point_head;
    while (point_idx != NULL_IDX) {
      const SharpePoint *point = &rt->sharpe_point_pool[point_idx];
      result.points.push_back({
          static_cast<int64_t>(bucket) * BLOCK_BUCKET_SIZE + point->block_offset,
          point->exposure,
          point->pnl,
      });
      point_idx = point->next;
    }
  }

  return result;
}

float calc_sharpe_window(const CollectedSharpePoints &window, int64_t start_block, int64_t end_block, size_t min_trade_count) {
  if (end_block < start_block) {
    return 0.0f;
  }
  if (window.points.size() < MIN_SHARPE_POINT_COUNT) {
    return 0.0f;
  }

  const long double shift = calc_pnl_shift(window.points, window.p0);

  long double prev_pnl_plus = shift;
  long double prev_exposure = static_cast<long double>(window.e0);
  long double sum_r = 0.0L;
  long double sum_r2 = 0.0L;
  int64_t prev_block = start_block - 1;
  for (const auto &point : window.points) {
    assert(point.block >= start_block);
    assert(point.block <= end_block);
    assert(point.block >= prev_block);
    const long double curr_pnl_plus = static_cast<long double>(point.pnl - window.p0) + shift;
    const long double denom = std::max(std::fabs(prev_exposure), SHARPE_RETURN_EPS);
    const long double r = (curr_pnl_plus - prev_pnl_plus) / denom;
    sum_r += r;
    sum_r2 += r * r;
    prev_pnl_plus = curr_pnl_plus;
    prev_exposure = static_cast<long double>(point.exposure);
    prev_block = point.block;
  }

  const int64_t total_time = end_block - start_block + 1;
  assert(total_time > 0);
  const long double T = static_cast<long double>(total_time);
  const long double mu = sum_r / T;
  const long double sigma2 = sum_r2 / T - mu * mu;
  if (sigma2 <= 0.0L || !std::isfinite(static_cast<double>(sigma2))) {
    return 0.0f;
  }

  const long double s0 = mu / std::sqrt(sigma2);
  if (!std::isfinite(static_cast<double>(s0))) {
    return 0.0f;
  }

  const long double a = calc_curve_penalty(window.points, start_block, window.p0);
  if (!std::isfinite(static_cast<double>(a))) {
    return 0.0f;
  }

  // trade count linear decay adjustment
  long double trade_decay = 1.0L;
  if (window.points.size() < min_trade_count) {
    trade_decay = static_cast<long double>(window.points.size()) / static_cast<long double>(min_trade_count);
  }

  const long double sharpe = s0 * (1.0L - a) * trade_decay * std::sqrt(10000000.0L);
  if (!std::isfinite(static_cast<double>(sharpe))) {
    return 0.0f;
  }
  return static_cast<float>(sharpe);
}

} // namespace

// ============================================================================
// calc_sharpe_for_feature - Calculate Sharpe ratios for feature slot
// ============================================================================

void calc_sharpe_for_feature(Stage3Runtime *rt, uint32_t user_idx, FeatureSlot *feat, int32_t first_bucket) {
  if (feat->tag_id != -1) {
    return;
  }

  const int32_t bucket = feat->bucket;
  feat->sharpe_100w = 0.0f;
  feat->sharpe_1000w = 0.0f;
  if (first_bucket < 0 || first_bucket > bucket) {
    return;
  }

  const int64_t end_block = bucket_last_block(bucket);
  auto recalc_window = [&](int32_t bucket_span, size_t min_trade_count, float *out) {
    const int32_t start_bucket = std::max(first_bucket, bucket - bucket_span + 1);
    const CollectedSharpePoints window = collect_sharpe_points(rt, user_idx, start_bucket, bucket);
    const int64_t start_block = static_cast<int64_t>(start_bucket) * BLOCK_BUCKET_SIZE;
    *out = calc_sharpe_window(window, start_block, end_block, min_trade_count);
  };
  recalc_window(10, 10, &feat->sharpe_100w);
  recalc_window(100, 100, &feat->sharpe_1000w);
}

// ============================================================================
// sharpe_prune_old_buckets - 淘汰用户的旧 bucket
// ============================================================================

void sharpe_prune_old_buckets(Stage3Runtime *rt, uint32_t user_idx, int32_t min_bucket_to_keep) {
  UserBlock *user = &rt->users[user_idx];

  uint32_t *prev_ptr = &user->sharpe_bucket_head;
  uint32_t idx = user->sharpe_bucket_head;
  int32_t newest_pruned_bucket = std::numeric_limits<int32_t>::min();
  int64_t newest_pruned_close_pnl = user->pnl_before_first_sharpe_bucket;
  int64_t newest_pruned_close_exposure = user->exposure_before_first_sharpe_bucket;

  while (idx != NULL_IDX) {
    SharpeBucket *bucket = &rt->sharpe_bucket_pool[idx];
    uint32_t next_idx = bucket->next;

    if (bucket->bucket < min_bucket_to_keep) {
      if (bucket->bucket > newest_pruned_bucket) {
        newest_pruned_bucket = bucket->bucket;
        newest_pruned_close_pnl = bucket->close_pnl;
        newest_pruned_close_exposure = bucket->close_exposure;
      }

      uint32_t point_idx = bucket->point_head;
      while (point_idx != NULL_IDX) {
        const uint32_t next_point = rt->sharpe_point_pool[point_idx].next;
        sharpe_point_free(rt, point_idx);
        point_idx = next_point;
      }

      *prev_ptr = next_idx;
      sharpe_bucket_free(rt, idx);
    } else {
      prev_ptr = &bucket->next;
    }

    idx = next_idx;
  }

  if (newest_pruned_bucket != std::numeric_limits<int32_t>::min()) {
    user->pnl_before_first_sharpe_bucket = newest_pruned_close_pnl;
    user->exposure_before_first_sharpe_bucket = newest_pruned_close_exposure;
  }
}

} // namespace stage3
