#include "stage3.hpp"

#include "misc/profiler.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace stage3 {

namespace {

// ============================================================================
// Structured filter evaluator
// ============================================================================

std::string trim_copy(const std::string &s) {
  size_t begin = 0;
  while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  size_t end = s.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(begin, end - begin);
}

// Use shared utilities from stage3.hpp
using stage3::normalize_key;
using stage3::to_lower_str;

// Industry tag name to ID mapping
const std::unordered_map<std::string, int8_t> TAG_NAME_TO_ID = {
    {"crypto_price", 0},
    {"crypto_market", 1},
    {"sports_basketball", 2},
    {"sports_football", 3},
    {"sports_soccer", 4},
    {"sports_individual", 5},
    {"politics_us", 6},
    {"politics_world", 7},
    {"economy_finance", 8},
    {"tech", 9},
    {"entertainment", 10},
    {"weather", 11},
    {"society", 12},
    {"unknown", 13},
    {"all", -1},
};

std::optional<int8_t> resolve_tag_id(const std::string &raw_tag) {
  std::string normalized = normalize_key(raw_tag);
  if (normalized.empty())
    return std::nullopt;

  auto it = TAG_NAME_TO_ID.find(normalized);
  if (it != TAG_NAME_TO_ID.end()) {
    return it->second;
  }
  return std::nullopt;
}

// Feature field enumeration
enum class FeatureField {
  TOKEN_AVG_10W,
  TOKEN_AVG_100W,
  TOKEN_AVG_1000W,
  EXPOSURE_AVG_10W,
  EXPOSURE_AVG_100W,
  EXPOSURE_AVG_1000W,
  VOLUME_10W,
  VOLUME_AVG_100W,
  VOLUME_AVG_1000W,
  HP_AVG_10W,
  HP_AVG_100W,
  HP_AVG_1000W,
  SHARPE_10W,
  SHARPE_100W,
  SHARPE_1000W,
};

std::optional<FeatureField> parse_feature_field(const std::string &feature, const std::string &window) {
  std::string base = to_lower_str(feature);
  std::string normalized_window = to_lower_str(window);

  enum class WindowKind { W10,
                          W100,
                          W1000 };
  std::optional<WindowKind> window_kind;

  if (normalized_window == "10") {
    window_kind = WindowKind::W10;
  } else if (normalized_window == "100") {
    window_kind = WindowKind::W100;
  } else if (normalized_window == "1k" || normalized_window == "1000") {
    window_kind = WindowKind::W1000;
  } else {
    return std::nullopt;
  }

  if (base.empty()) {
    return std::nullopt;
  }

  if (base == "tok") {
    if (*window_kind == WindowKind::W10)
      return FeatureField::TOKEN_AVG_10W;
    if (*window_kind == WindowKind::W100)
      return FeatureField::TOKEN_AVG_100W;
    return FeatureField::TOKEN_AVG_1000W;
  }
  if (base == "exp") {
    if (*window_kind == WindowKind::W10)
      return FeatureField::EXPOSURE_AVG_10W;
    if (*window_kind == WindowKind::W100)
      return FeatureField::EXPOSURE_AVG_100W;
    return FeatureField::EXPOSURE_AVG_1000W;
  }
  if (base == "vol") {
    if (*window_kind == WindowKind::W10)
      return FeatureField::VOLUME_10W;
    if (*window_kind == WindowKind::W100)
      return FeatureField::VOLUME_AVG_100W;
    return FeatureField::VOLUME_AVG_1000W;
  }
  if (base == "hp") {
    if (*window_kind == WindowKind::W10)
      return FeatureField::HP_AVG_10W;
    if (*window_kind == WindowKind::W100)
      return FeatureField::HP_AVG_100W;
    return FeatureField::HP_AVG_1000W;
  }
  if (base == "shp") {
    if (*window_kind == WindowKind::W10)
      return FeatureField::SHARPE_10W;
    if (*window_kind == WindowKind::W100)
      return FeatureField::SHARPE_100W;
    return FeatureField::SHARPE_1000W;
  }

  return std::nullopt;
}

// Returns 0.0 if feature slot is missing.
double get_feature_value(const FeatureSlot *feat, FeatureField field) {
  if (!feat)
    return 0.0;

  switch (field) {
  case FeatureField::TOKEN_AVG_10W:
    return static_cast<double>(feat->token_avg_10w);
  case FeatureField::TOKEN_AVG_100W:
    return static_cast<double>(feat->token_avg_100w);
  case FeatureField::TOKEN_AVG_1000W:
    return static_cast<double>(feat->token_avg_1000w);
  case FeatureField::EXPOSURE_AVG_10W:
    return static_cast<double>(feat->exposure_avg_10w);
  case FeatureField::EXPOSURE_AVG_100W:
    return static_cast<double>(feat->exposure_avg_100w);
  case FeatureField::EXPOSURE_AVG_1000W:
    return static_cast<double>(feat->exposure_avg_1000w);
  case FeatureField::VOLUME_10W:
    return static_cast<double>(feat->volume_10w);
  case FeatureField::VOLUME_AVG_100W:
    return static_cast<double>(feat->volume_avg_100w);
  case FeatureField::VOLUME_AVG_1000W:
    return static_cast<double>(feat->volume_avg_1000w);
  case FeatureField::HP_AVG_10W:
    return static_cast<double>(feat->holding_period_avg_10w);
  case FeatureField::HP_AVG_100W:
    return static_cast<double>(feat->holding_period_avg_100w);
  case FeatureField::HP_AVG_1000W:
    return static_cast<double>(feat->holding_period_avg_1000w);
  case FeatureField::SHARPE_10W:
    return static_cast<double>(feat->sharpe_10w);
  case FeatureField::SHARPE_100W:
    return static_cast<double>(feat->sharpe_100w);
  case FeatureField::SHARPE_1000W:
    return static_cast<double>(feat->sharpe_1000w);
  }
  return 0.0;
}

bool is_sharpe_field(FeatureField field) {
  return field == FeatureField::SHARPE_10W || field == FeatureField::SHARPE_100W ||
         field == FeatureField::SHARPE_1000W;
}

enum class ArithmeticOp {
  NONE,
  MUL,
  DIV,
};

enum class CompareOp {
  GT,
  GE,
  LT,
  LE,
  EQ,
  NE,
};

struct CompiledMetricRef {
  int8_t tag_id = -1;
  FeatureField field = FeatureField::TOKEN_AVG_10W;
};

struct CompiledValueExpr {
  CompiledMetricRef lhs;
  ArithmeticOp arith_op = ArithmeticOp::NONE;
  CompiledMetricRef rhs;
};

struct CompiledFilterItem {
  std::string id;
  CompiledValueExpr expr;
  CompareOp compare_op = CompareOp::GT;
  double compare_value = 0.0;
};

struct CompiledFilterPlan {
  std::vector<CompiledFilterItem> items;
  CompiledValueExpr sort_expr;
  bool sort_asc = false;
};

using GetFeatureFn = std::function<const FeatureSlot *(int8_t tag_id)>;

CompiledMetricRef compile_metric_ref(const FilterMetricRef &ref) {
  assert(!ref.industry.empty());
  assert(!ref.feature.empty());
  assert(!ref.window.empty());

  auto tag_id = resolve_tag_id(ref.industry);
  assert(tag_id.has_value());

  auto field = parse_feature_field(ref.feature, ref.window);
  assert(field.has_value());
  assert(!is_sharpe_field(*field) || *tag_id == -1);

  CompiledMetricRef out;
  out.tag_id = *tag_id;
  out.field = *field;
  return out;
}

ArithmeticOp compile_arith_op(const std::string &raw_op) {
  const std::string op = trim_copy(raw_op);
  if (op.empty()) {
    return ArithmeticOp::NONE;
  }
  if (op == "*") {
    return ArithmeticOp::MUL;
  }
  if (op == "/") {
    return ArithmeticOp::DIV;
  }
  assert(false);
  return ArithmeticOp::NONE;
}

CompareOp compile_compare_op(const std::string &raw_op) {
  const std::string op = trim_copy(raw_op);
  if (op == ">") {
    return CompareOp::GT;
  }
  if (op == ">=") {
    return CompareOp::GE;
  }
  if (op == "<") {
    return CompareOp::LT;
  }
  if (op == "<=") {
    return CompareOp::LE;
  }
  if (op == "==" || op == "=") {
    return CompareOp::EQ;
  }
  if (op == "!=") {
    return CompareOp::NE;
  }
  assert(false);
  return CompareOp::GT;
}

CompiledValueExpr compile_value_expr(const FilterValueExpr &expr) {
  CompiledValueExpr out;
  out.lhs = compile_metric_ref(expr.lhs);
  out.arith_op = compile_arith_op(expr.arith_op);
  if (out.arith_op == ArithmeticOp::NONE) {
    out.rhs = out.lhs;
    return out;
  }
  out.rhs = compile_metric_ref(expr.rhs);
  return out;
}

CompiledFilterItem compile_filter_item(const FilterItem &item) {
  assert(!item.id.empty());

  CompiledFilterItem out;
  out.id = item.id;
  out.expr = compile_value_expr(item.expr);
  out.compare_op = compile_compare_op(item.compare_op);
  out.compare_value = item.compare_value;
  assert(std::isfinite(out.compare_value));
  return out;
}

CompiledFilterPlan compile_filter_plan(const FilterRequest &req) {
  CompiledFilterPlan out;
  out.items.reserve(req.items.size());
  for (const auto &item : req.items) {
    out.items.push_back(compile_filter_item(item));
  }
  out.sort_expr = compile_value_expr(req.sort.expr);
  out.sort_asc = req.sort.asc;
  return out;
}

double eval_metric_ref(const GetFeatureFn &get_feature, const CompiledMetricRef &ref) {
  const FeatureSlot *feat = get_feature(ref.tag_id);
  return get_feature_value(feat, ref.field);
}

double eval_value_expr(const GetFeatureFn &get_feature, const CompiledValueExpr &expr) {
  const double left = eval_metric_ref(get_feature, expr.lhs);
  if (expr.arith_op == ArithmeticOp::NONE) {
    return left;
  }

  const double right = eval_metric_ref(get_feature, expr.rhs);
  if (expr.arith_op == ArithmeticOp::MUL) {
    return left * right;
  }
  if (expr.arith_op == ArithmeticOp::DIV) {
    return left / right;
  }

  assert(false);
  return 0.0;
}

bool eval_compare(double left, CompareOp op, double right) {
  if (op == CompareOp::GT) {
    return left > right;
  }
  if (op == CompareOp::GE) {
    return left >= right;
  }
  if (op == CompareOp::LT) {
    return left < right;
  }
  if (op == CompareOp::LE) {
    return left <= right;
  }
  if (op == CompareOp::EQ) {
    return std::abs(left - right) < 1e-9;
  }
  if (op == CompareOp::NE) {
    return std::abs(left - right) >= 1e-9;
  }

  assert(false);
  return false;
}

bool eval_filter_item(const GetFeatureFn &get_feature, const CompiledFilterItem &item) {
  return eval_compare(eval_value_expr(get_feature, item.expr), item.compare_op, item.compare_value);
}

} // namespace

// ============================================================================
// stage3_query_filter
// ============================================================================

FilterResult stage3_query_filter(Stage3Runtime *rt, const FilterRequest &req) {
  TraceN("s3/filter_core");
  FilterResult result{};
  result.anchor_bucket = req.anchor_bucket;

  assert(req.anchor_bucket >= 0);
  assert(req.limit > 0 && req.limit <= 1000);

  const int32_t anchor_bucket = static_cast<int32_t>(req.anchor_bucket);
  const CompiledFilterPlan plan = compile_filter_plan(req);

  // Temporary storage for candidates
  struct Candidate {
    uint32_t user_idx;
    double sort_value;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(rt->header->user_count);

  std::vector<int64_t> item_before_counts(plan.items.size(), 0);
  std::vector<int64_t> item_filtered_counts(plan.items.size(), 0);

  // Scan all users
  for (uint64_t i = 0; i < rt->header->user_count; ++i) {
    uint32_t user_idx = static_cast<uint32_t>(i);
    UserBlock *user = &rt->users[user_idx];

    if (!(user->flags & 1))
      continue; // Not occupied

    std::array<int32_t, FEATURE_TAG_SLOT_COUNT> feature_first_buckets{};
    std::array<int32_t, FEATURE_TAG_SLOT_COUNT> feature_latest_buckets{};
    init_feature_timelines(rt, user_idx, feature_first_buckets, feature_latest_buckets);

    std::array<const FeatureSlot *, FEATURE_TAG_SLOT_COUNT> feature_cache{};
    std::array<bool, FEATURE_TAG_SLOT_COUNT> feature_loaded{};
    auto get_feature = [&](int8_t tag_id) -> const FeatureSlot * {
      const size_t slot = tag_slot(tag_id);
      if (!feature_loaded[slot]) {
        feature_loaded[slot] = true;
        const int32_t first_bucket = feature_first_buckets[slot];
        const int32_t latest_bucket = feature_latest_buckets[slot];
        const FeatureSlot *feat = nullptr;
        if (first_bucket >= 0 && anchor_bucket >= first_bucket) {
          feat = feature_find(rt, user_idx, std::min(anchor_bucket, latest_bucket), tag_id);
          assert(feat != nullptr);
        }
        feature_cache[slot] = feat;
      }
      return feature_cache[slot];
    };

    // Check if user has any feature at anchor_bucket
    const FeatureSlot *global_feat = get_feature(-1);
    if (!global_feat)
      continue;

    ++result.scanned_user_count;
    bool pass_all = true;

    for (size_t fi = 0; fi < plan.items.size(); ++fi) {
      ++item_before_counts[fi];
      const bool pass = eval_filter_item(get_feature, plan.items[fi]);
      if (!pass) {
        ++item_filtered_counts[fi];
        pass_all = false;
        break;
      }
    }

    if (!pass_all)
      continue;

    const double sort_value = eval_value_expr(get_feature, plan.sort_expr);
    candidates.push_back({user_idx, sort_value});
  }

  result.matched_user_count = static_cast<int64_t>(candidates.size());

  // Sort
  if (plan.sort_asc) {
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                return a.sort_value < b.sort_value;
              });
  } else {
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                return a.sort_value > b.sort_value;
              });
  }

  // Limit results
  size_t limit = std::min(static_cast<size_t>(req.limit), candidates.size());
  result.users.reserve(limit);

  for (size_t i = 0; i < limit; ++i) {
    FilterUserRow row{};
    row.user_idx = candidates[i].user_idx;
    row.sort_value = candidates[i].sort_value;
    result.users.push_back(row);
  }

  result.item_stats.reserve(plan.items.size());
  for (size_t fi = 0; fi < plan.items.size(); ++fi) {
    FilterItemStat stat{};
    stat.id = plan.items[fi].id;
    stat.before_count = item_before_counts[fi];
    stat.filtered_count = item_filtered_counts[fi];
    stat.remaining_count = item_before_counts[fi] - item_filtered_counts[fi];
    result.item_stats.push_back(stat);
  }

  return result;
}

} // namespace stage3
