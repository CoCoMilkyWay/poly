#include "stage3_sync.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "../core/rocks_store.hpp"
#include "../stage2/stage2_types.hpp"
#include "../stage2/stage2_utils.hpp"
#include "misc/profiler.hpp"

namespace stage3 {
namespace {

constexpr int64_t kFilterBlockBucketSize = 100000; // keep in sync with StageSync::kBlockBucketSize

bool expr_contains_sharpe(const std::string &expr) {
  const std::string expr_lower = to_lower(expr);
  return expr_lower.find("shp") != std::string::npos;
}

bool is_sharpe_feature_token(const std::string &token) {
  const std::string token_lower = to_lower(token);
  return token_lower == "shp" || token_lower == "shp10" || token_lower == "shp100" || token_lower == "shp1k" ||
         token_lower == "shp1000";
}

std::optional<int64_t> parse_sharpe_window(const std::string &expr) {
  const std::string expr_lower = to_lower(expr);
  if (expr_lower.find("shp10") != std::string::npos || expr_lower.find("all.shp10") != std::string::npos) {
    return 10LL * kFilterBlockBucketSize;
  }
  if (expr_lower.find("shp100") != std::string::npos || expr_lower.find("all.shp100") != std::string::npos) {
    return 100LL * kFilterBlockBucketSize;
  }
  if (expr_lower.find("shp1k") != std::string::npos || expr_lower.find("shp1000") != std::string::npos ||
      expr_lower.find("all.shp1k") != std::string::npos || expr_lower.find("all.shp1000") != std::string::npos) {
    return 1000LL * kFilterBlockBucketSize;
  }
  if (expr_lower.find("shp") != std::string::npos) {
    return 10LL * kFilterBlockBucketSize;
  }
  return std::nullopt;
}

bool eval_sharpe_filter_expr(const std::string &filter_expr, double sharpe) {
  const std::string expr_lower = to_lower(filter_expr);
  const size_t op_pos = expr_lower.find_first_of("><=!");
  if (op_pos == std::string::npos) {
    return true;
  }
  size_t value_start = op_pos;
  while (value_start < expr_lower.size() &&
         (expr_lower[value_start] == '=' || expr_lower[value_start] == '!' || expr_lower[value_start] == '<' ||
          expr_lower[value_start] == '>')) {
    ++value_start;
  }
  const std::string op = expr_lower.substr(op_pos, value_start - op_pos);
  const double threshold = std::stod(expr_lower.substr(value_start));
  if (op == ">") {
    return sharpe > threshold;
  }
  if (op == ">=") {
    return sharpe >= threshold;
  }
  if (op == "<") {
    return sharpe < threshold;
  }
  if (op == "<=") {
    return sharpe <= threshold;
  }
  if (op == "=" || op == "==") {
    return std::abs(sharpe - threshold) < 1e-9;
  }
  if (op == "!=") {
    return std::abs(sharpe - threshold) >= 1e-9;
  }
  return false;
}

std::optional<int64_t> find_sharpe_window_from_exprs(const std::string &sort_expr,
                                                     const std::vector<std::string> &filters) {
  if (const auto window_blocks = parse_sharpe_window(sort_expr); window_blocks.has_value()) {
    return window_blocks;
  }
  for (const auto &f : filters) {
    if (const auto window_blocks = parse_sharpe_window(f); window_blocks.has_value()) {
      return window_blocks;
    }
  }
  return std::nullopt;
}

double calc_account_sharpe(const std::vector<core::rocks::Stage3EventFactRecord> &events, int64_t window_blocks) {
  if (events.empty()) {
    return 0.0;
  }

  constexpr double kEps = 1.0;
  constexpr int64_t kMinSamples = 2;

  struct BlockValue {
    int64_t realized_cum = 0;
    int64_t unrealized_pnl = 0;
    int64_t account_exposure = 0;
  };

  std::map<int64_t, BlockValue> block_values;
  for (const auto &evt : events) {
    const int64_t block = evt.sort_key / stage2::SORT_KEY_SCALE;
    auto &bv = block_values[block];
    bv.realized_cum = evt.realized_cum;
    bv.unrealized_pnl = evt.unrealized_pnl;
    bv.account_exposure = evt.account_exposure;
  }

  if (block_values.empty()) {
    return 0.0;
  }
  const int64_t anchor_block = block_values.rbegin()->first;
  const int64_t window_start_block = std::max<int64_t>(0, anchor_block - window_blocks + 1);

  std::vector<std::pair<int64_t, double>> nav_sequence;
  int64_t start_block = -1;
  int64_t start_realized = 0;
  int64_t start_unrealized = 0;
  int64_t start_exposure = 0;

  for (const auto &[block, bv] : block_values) {
    if (block < window_start_block) {
      start_block = block;
      start_realized = bv.realized_cum;
      start_unrealized = bv.unrealized_pnl;
      start_exposure = bv.account_exposure;
      continue;
    }

    const int64_t P_t = bv.realized_cum + bv.unrealized_pnl;
    int64_t E_start = bv.account_exposure;
    int64_t P_start = P_t;
    if (start_block >= 0) {
      E_start = start_exposure;
      P_start = start_realized + start_unrealized;
    }
    const double V_t = std::max(kEps, static_cast<double>(E_start) / 1e6 + static_cast<double>(P_t - P_start) / 1e6);
    nav_sequence.push_back({block, V_t});
  }

  if (nav_sequence.size() < kMinSamples) {
    return 0.0;
  }

  std::vector<double> returns;
  std::vector<int64_t> deltas;
  for (size_t i = 1; i < nav_sequence.size(); ++i) {
    const double V_i = nav_sequence[i].second;
    const double V_prev = nav_sequence[i - 1].second;
    if (V_prev <= 0.0 || V_i <= 0.0) {
      continue;
    }
    const double r_i = std::log(V_i) - std::log(V_prev);
    const int64_t delta_t = nav_sequence[i].first - nav_sequence[i - 1].first;
    if (delta_t > 0) {
      returns.push_back(r_i);
      deltas.push_back(delta_t);
    }
  }

  if (returns.size() < kMinSamples) {
    return 0.0;
  }

  int64_t total_time = 0;
  double sum_r = 0.0;
  for (size_t i = 0; i < returns.size(); ++i) {
    total_time += deltas[i];
    sum_r += returns[i];
  }

  if (total_time <= 0) {
    return 0.0;
  }

  const double r_bar = sum_r / static_cast<double>(total_time);

  double sum_sq_tw = 0.0;
  for (size_t i = 0; i < returns.size(); ++i) {
    const double diff = returns[i] - r_bar;
    sum_sq_tw += diff * diff * static_cast<double>(deltas[i]);
  }

  const double variance = sum_sq_tw / static_cast<double>(total_time);
  if (variance <= 0.0) {
    return 0.0;
  }

  const double sigma = std::sqrt(variance);
  return r_bar / sigma;
}

struct FieldBinding {
  int32_t tag_id = -1;
  std::string column;
  std::string alias;
};

std::string trim_copy(const std::string &s) {
  size_t begin = 0;
  while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
    ++begin;
  }
  size_t end = s.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
    --end;
  }
  return s.substr(begin, end - begin);
}

std::string normalize_token(const std::string &raw) {
  std::string out;
  out.reserve(raw.size());
  bool prev_sep = false;
  for (char c : to_lower(raw)) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (keep) {
      out.push_back(c);
      prev_sep = false;
      continue;
    }
    if (!prev_sep && !out.empty()) {
      out.push_back('_');
      prev_sep = true;
    }
  }
  while (!out.empty() && out.back() == '_') {
    out.pop_back();
  }
  return out;
}

std::optional<std::string> feature_token_to_column(const std::string &feature_token) {
  const std::string token = to_lower(feature_token);
  std::string base;
  enum class WindowKind {
    W10,
    W100,
    W1000,
  };
  std::optional<WindowKind> window;

  if (token.ends_with("1k")) {
    base = token.substr(0, token.size() - 2);
    window = WindowKind::W1000;
  } else if (token.ends_with("1000")) {
    base = token.substr(0, token.size() - 4);
    window = WindowKind::W1000;
  } else if (token.ends_with("100")) {
    base = token.substr(0, token.size() - 3);
    window = WindowKind::W100;
  } else if (token.ends_with("10")) {
    base = token.substr(0, token.size() - 2);
    window = WindowKind::W10;
  } else {
    return std::nullopt;
  }
  if (base.empty()) {
    return std::nullopt;
  }

  if (base == "tok") {
    return (window == WindowKind::W10) ? "token_avg_10w"
                                       : (window == WindowKind::W100 ? "token_avg_100w" : "token_avg_1000w");
  }
  if (base == "exp") {
    return (window == WindowKind::W10) ? "exposure_avg_10w"
                                       : (window == WindowKind::W100 ? "exposure_avg_100w"
                                                                     : "exposure_avg_1000w");
  }
  if (base == "vol") {
    return (window == WindowKind::W10) ? "volume_10w"
                                       : (window == WindowKind::W100 ? "volume_avg_100w" : "volume_avg_1000w");
  }
  if (base == "hp") {
    return (window == WindowKind::W10) ? "holding_period_avg_10w"
                                       : (window == WindowKind::W100 ? "holding_period_avg_100w"
                                                                     : "holding_period_avg_1000w");
  }
  if (base == "shp") {
    return (window == WindowKind::W10) ? "sharpe_10w"
                                       : (window == WindowKind::W100 ? "sharpe_100w" : "sharpe_1000w");
  }
  return std::nullopt;
}

std::optional<int32_t> resolve_industry_tag_id(const std::unordered_map<std::string, int8_t> &tag_to_industry_id,
                                               const std::string &raw_tag) {
  const std::string normalized = normalize_token(raw_tag);
  if (normalized.empty()) {
    return std::nullopt;
  }
  if (normalized == "all") {
    return -1;
  }
  if (normalized == "unknown") {
    return 13;
  }
  auto it = tag_to_industry_id.find(normalized);
  if (it == tag_to_industry_id.end()) {
    return std::nullopt;
  }
  return static_cast<int32_t>(it->second);
}

class ExprTranslator {
public:
  using ResolveTagFn = std::function<std::optional<int32_t>(const std::string &)>;

  ExprTranslator(ResolveTagFn resolve_tag,
                 std::unordered_map<std::string, FieldBinding> &binding_map,
                 std::vector<FieldBinding> &bindings)
      : resolve_tag_(std::move(resolve_tag)),
        binding_map_(binding_map),
        bindings_(bindings) {}

  std::string translate(const std::string &expr) {
    std::string sql;
    sql.reserve(expr.size() * 2 + 16);
    size_t i = 0;
    while (i < expr.size()) {
      const char ch = expr[i];
      if (std::isspace(static_cast<unsigned char>(ch)) != 0) {
        sql.push_back(' ');
        ++i;
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(ch)) != 0 ||
          (ch == '.' && i + 1 < expr.size() && std::isdigit(static_cast<unsigned char>(expr[i + 1])) != 0)) {
        size_t start = i;
        bool seen_dot = false;
        if (ch == '.') {
          seen_dot = true;
          ++i;
        }
        while (i < expr.size()) {
          const char c = expr[i];
          if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            ++i;
            continue;
          }
          if (c == '.' && !seen_dot) {
            seen_dot = true;
            ++i;
            continue;
          }
          break;
        }
        sql += expr.substr(start, i - start);
        continue;
      }
      if (std::isalpha(static_cast<unsigned char>(ch)) != 0 || ch == '_') {
        const size_t token_start = i;
        ++i;
        while (i < expr.size()) {
          const char c = expr[i];
          if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
            ++i;
            continue;
          }
          break;
        }
        const std::string lhs = expr.substr(token_start, i - token_start);
        size_t dot_pos = i;
        while (dot_pos < expr.size() && std::isspace(static_cast<unsigned char>(expr[dot_pos])) != 0) {
          ++dot_pos;
        }
        if (dot_pos < expr.size() && expr[dot_pos] == '.') {
          size_t rhs_start = dot_pos + 1;
          while (rhs_start < expr.size() &&
                 std::isspace(static_cast<unsigned char>(expr[rhs_start])) != 0) {
            ++rhs_start;
          }
          if (rhs_start >= expr.size() ||
              (std::isalpha(static_cast<unsigned char>(expr[rhs_start])) == 0 &&
               expr[rhs_start] != '_')) {
            throw std::invalid_argument("stage3-filter: missing feature token after '.'");
          }
          size_t rhs_end = rhs_start + 1;
          while (rhs_end < expr.size()) {
            const char c = expr[rhs_end];
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
              ++rhs_end;
              continue;
            }
            break;
          }
          const std::string rhs = expr.substr(rhs_start, rhs_end - rhs_start);
          const std::optional<int32_t> tag_id = resolve_tag_(lhs);
          if (!tag_id.has_value()) {
            throw std::invalid_argument("stage3-filter: unknown industry '" + lhs + "'");
          }
          const std::optional<std::string> column = feature_token_to_column(rhs);
          if (!column.has_value()) {
            throw std::invalid_argument("stage3-filter: unknown feature token '" + rhs + "'");
          }
          if (is_sharpe_feature_token(rhs)) {
            if (*tag_id != -1) {
              throw std::invalid_argument("stage3-filter: Sharpe ratio (shp) is only supported for ALL industry, not for '" + lhs + "'");
            }
            throw std::invalid_argument("stage3-filter: Sharpe ratio (shp) must be computed exactly and cannot be used in SQL expressions");
          }
          sql += register_field(*tag_id, *column);
          i = rhs_end;
          continue;
        }
        const std::string kw = to_lower(lhs);
        if (kw == "and") {
          sql += " AND ";
          continue;
        }
        if (kw == "or") {
          sql += " OR ";
          continue;
        }
        if (kw == "not") {
          sql += " NOT ";
          continue;
        }
        if (kw == "true") {
          sql += "TRUE";
          continue;
        }
        if (kw == "false") {
          sql += "FALSE";
          continue;
        }
        throw std::invalid_argument("stage3-filter: unsupported token '" + lhs + "'");
      }
      if (ch == '(' || ch == ')' || ch == '+' || ch == '-' || ch == '*' || ch == '/') {
        sql.push_back(ch);
        ++i;
        continue;
      }
      if (ch == '>' || ch == '<' || ch == '=' || ch == '!') {
        if (i + 1 < expr.size()) {
          const std::string two = expr.substr(i, 2);
          if (two == ">=" || two == "<=" || two == "!=") {
            sql += two;
            i += 2;
            continue;
          }
          if (two == "==") {
            sql.push_back('=');
            i += 2;
            continue;
          }
        }
        if (ch == '!') {
          throw std::invalid_argument("stage3-filter: unsupported operator '!'");
        }
        sql.push_back(ch);
        ++i;
        continue;
      }
      throw std::invalid_argument("stage3-filter: unsupported character in expression");
    }

    return trim_copy(sql);
  }

private:
  std::string register_field(int32_t tag_id, const std::string &column) {
    const std::string key = std::to_string(tag_id) + ":" + column;
    auto it = binding_map_.find(key);
    if (it != binding_map_.end()) {
      return it->second.alias;
    }
    FieldBinding binding;
    binding.tag_id = tag_id;
    binding.column = column;
    binding.alias = "f" + std::to_string(bindings_.size());
    binding_map_.emplace(key, binding);
    bindings_.push_back(std::move(binding));
    return bindings_.back().alias;
  }

  ResolveTagFn resolve_tag_;
  std::unordered_map<std::string, FieldBinding> &binding_map_;
  std::vector<FieldBinding> &bindings_;
};

} // namespace

filter::Result StageSync::filter_users_by_features(const filter::Request &req) const {
  TraceN("s3/filter");
  filter::Result result;

  assert(req.anchor_bucket >= 0);
  assert(req.limit > 0);
  assert(req.limit <= 1000);

  const int64_t limit = req.limit;
  const int64_t anchor_bucket = req.anchor_bucket;

  auto conn = stage3_db_.create_connection();
  result.anchor_bucket = anchor_bucket;

  const bool sort_has_sharpe = expr_contains_sharpe(req.sort_expr);
  bool any_filter_has_sharpe = false;
  for (const auto &f : req.filters) {
    if (expr_contains_sharpe(f)) {
      any_filter_has_sharpe = true;
      break;
    }
  }

  if (sort_has_sharpe || any_filter_has_sharpe) {
    const std::optional<int64_t> window_blocks = find_sharpe_window_from_exprs(req.sort_expr, req.filters);
    if (!window_blocks.has_value()) {
      throw std::invalid_argument("stage3-filter: unable to determine Sharpe window from expressions");
    }

    const int64_t anchor_block = anchor_bucket * kBlockBucketSize + (kBlockBucketSize - 1);
    const int64_t sort_key_start = std::max<int64_t>(0, (anchor_block - *window_blocks + 1)) * stage2::SORT_KEY_SCALE;
    const int64_t sort_key_end = anchor_block * stage2::SORT_KEY_SCALE + (stage2::SORT_KEY_SCALE - 1);

    std::vector<filter::UserRow> candidate_users;
    auto append_candidate_users = [&candidate_users](const auto &r) {
      if (r && !r->HasError()) {
        for (idx_t i = 0; i < r->RowCount(); ++i) {
          std::string addr_hex = r->GetValue(0, i).template GetValueUnsafe<std::string>();
          candidate_users.push_back({
              "0x" + addr_hex,
              0.0,
              0,
              0,
              0,
              r->GetValue(1, i).template GetValue<int64_t>(),
          });
        }
      }
    };
    if (!any_filter_has_sharpe) {
      std::string sql = "SELECT lower(hex(user_addr)) AS addr, "
                        "COALESCE(total_realized_pnl, 0) + COALESCE(total_unrealized_pnl, 0) AS pnl "
                        "FROM " +
                        std::string(kSqlTableUserSummaryState) + " "
                                                                 "LIMIT 10000";
      auto r = conn->Query(sql);
      append_candidate_users(r);
    } else {
      auto resolve_tag = [this](const std::string &raw_tag) -> std::optional<int32_t> {
        return resolve_industry_tag_id(tag_to_industry_id_, raw_tag);
      };
      std::unordered_map<std::string, FieldBinding> binding_map;
      std::vector<FieldBinding> bindings;
      ExprTranslator translator(resolve_tag, binding_map, bindings);
      std::vector<std::string> non_sharpe_filters;
      for (const auto &raw_filter : req.filters) {
        const std::string filter_expr = trim_copy(raw_filter);
        if (filter_expr.empty()) {
          continue;
        }
        if (!expr_contains_sharpe(filter_expr)) {
          try {
            non_sharpe_filters.push_back(translator.translate(filter_expr));
          } catch (...) {
          }
        }
      }
      std::string sql = "SELECT lower(hex(user_addr)) AS addr, "
                        "COALESCE(total_realized_pnl, 0) + COALESCE(total_unrealized_pnl, 0) AS pnl "
                        "FROM " +
                        std::string(kSqlTableUserSummaryState) + " s";
      if (!non_sharpe_filters.empty()) {
        sql += " JOIN " + std::string(kSqlTableFeatureTensorState) + " f ON s.user_addr = f.user_addr "
                                                                     "WHERE f.block_bucket = " +
               std::to_string(anchor_bucket);
        for (size_t i = 0; i < non_sharpe_filters.size(); ++i) {
          sql += " AND (" + non_sharpe_filters[i] + ")";
        }
        sql += " GROUP BY s.user_addr";
      }
      sql += " LIMIT 10000";
      auto r = conn->Query(sql);
      append_candidate_users(r);
    }

    std::vector<std::pair<filter::UserRow, double>> users_with_sharpe;
    users_with_sharpe.reserve(candidate_users.size());
    std::vector<std::string> sharpe_filters;
    if (any_filter_has_sharpe) {
      sharpe_filters.reserve(req.filters.size());
      for (const auto &raw_filter : req.filters) {
        const std::string filter_expr = trim_copy(raw_filter);
        if (!filter_expr.empty() && expr_contains_sharpe(filter_expr)) {
          sharpe_filters.push_back(filter_expr);
        }
      }
    }

    for (const auto &user : candidate_users) {
      const std::string user_addr_lower = user.addr.substr(2);
      const std::string user_blob = hex_to_blob(user_addr_lower);
      auto events = event_fact_store_->scan_by_user_range(user_blob, sort_key_start, sort_key_end);
      const double sharpe = calc_account_sharpe(events, *window_blocks);
      filter::UserRow user_copy = user;
      user_copy.sort_value = sharpe;
      users_with_sharpe.push_back({user_copy, sharpe});
    }

    std::vector<filter::UserRow> filtered_users;
    if (any_filter_has_sharpe) {
      for (const auto &[user, sharpe] : users_with_sharpe) {
        bool passes = true;
        for (const auto &filter_expr : sharpe_filters) {
          if (!eval_sharpe_filter_expr(filter_expr, sharpe)) {
            passes = false;
            break;
          }
        }
        if (passes) {
          filtered_users.push_back(user);
        }
      }
    } else {
      for (const auto &[user, _] : users_with_sharpe) {
        filtered_users.push_back(user);
      }
    }

    std::sort(filtered_users.begin(), filtered_users.end(),
              [req](const filter::UserRow &a, const filter::UserRow &b) {
                return req.sort_asc ? a.sort_value < b.sort_value : a.sort_value > b.sort_value;
              });

    if (static_cast<int64_t>(filtered_users.size()) > limit) {
      filtered_users.resize(static_cast<size_t>(limit));
    }

    result.users = std::move(filtered_users);
    return result;
  }

  auto resolve_tag = [this](const std::string &raw_tag) -> std::optional<int32_t> {
    return resolve_industry_tag_id(tag_to_industry_id_, raw_tag);
  };

  std::unordered_map<std::string, FieldBinding> binding_map;
  std::vector<FieldBinding> bindings;
  ExprTranslator translator(resolve_tag, binding_map, bindings);

  const std::string sort_expr = trim_copy(req.sort_expr);
  if (sort_expr.empty()) {
    throw std::invalid_argument("stage3-filter: sort_expr is required");
  }
  const std::string sort_sql = translator.translate(sort_expr);

  std::vector<std::string> where_parts;
  where_parts.reserve(req.filters.size());
  for (const auto &raw_filter : req.filters) {
    const std::string filter_expr = trim_copy(raw_filter);
    if (filter_expr.empty()) {
      continue;
    }
    where_parts.push_back(translator.translate(filter_expr));
  }

  std::vector<int32_t> used_tag_ids;
  used_tag_ids.reserve(bindings.size());
  for (const auto &binding : bindings) {
    used_tag_ids.push_back(binding.tag_id);
  }
  used_tag_ids.push_back(-1);
  std::sort(used_tag_ids.begin(), used_tag_ids.end());
  used_tag_ids.erase(std::unique(used_tag_ids.begin(), used_tag_ids.end()), used_tag_ids.end());

  std::string sql = "WITH user_features AS (SELECT user_addr";
  sql += ", MAX(CASE WHEN tag_id = -1 THEN token_avg_100w END) AS month_avg_tok";
  sql += ", MAX(CASE WHEN tag_id = -1 THEN exposure_avg_100w END) AS month_avg_exp";
  sql += ", MAX(CASE WHEN tag_id = -1 THEN holding_period_avg_100w END) AS month_avg_hp";
  for (const auto &binding : bindings) {
    sql += ", MAX(CASE WHEN tag_id = " + std::to_string(binding.tag_id) +
           " THEN " + binding.column + " END) AS " + binding.alias;
  }
  sql += " FROM " + std::string(kSqlTableFeatureTensorState) +
         " WHERE block_bucket = " + std::to_string(anchor_bucket);
  if (!used_tag_ids.empty()) {
    sql += " AND tag_id IN (";
    for (size_t i = 0; i < used_tag_ids.size(); ++i) {
      if (i > 0) {
        sql += ",";
      }
      sql += std::to_string(used_tag_ids[i]);
    }
    sql += ")";
  }
  sql +=
      " GROUP BY user_addr)";
  sql += " SELECT lower(hex(f.user_addr)) AS addr, CAST((" + sort_sql +
         ") AS DOUBLE) AS sort_value, "
         "COALESCE(f.month_avg_tok, 0) AS month_avg_tok, "
         "COALESCE(f.month_avg_exp, 0) AS month_avg_exp, "
         "COALESCE(f.month_avg_hp, 0) AS month_avg_hp, "
         "COALESCE(s.total_realized_pnl, 0) + COALESCE(s.total_unrealized_pnl, 0) AS pnl "
         "FROM user_features f LEFT JOIN " +
         std::string(kSqlTableUserSummaryState) +
         " s ON f.user_addr = s.user_addr";
  if (!where_parts.empty()) {
    sql += " WHERE ";
    for (size_t i = 0; i < where_parts.size(); ++i) {
      if (i > 0) {
        sql += " AND ";
      }
      sql += "(" + where_parts[i] + ")";
    }
  }
  sql += " ORDER BY sort_value ";
  sql += req.sort_asc ? "ASC" : "DESC";
  sql += " NULLS LAST LIMIT " + std::to_string(limit);

  auto r = conn->Query(sql);
  if (!r || r->HasError()) {
    throw std::runtime_error("stage3-filter: query execution failed");
  }

  result.users.reserve(static_cast<size_t>(r->RowCount()));
  for (idx_t i = 0; i < r->RowCount(); ++i) {
    std::string addr_hex = r->GetValue(0, i).GetValueUnsafe<std::string>();
    const auto sort_val = r->GetValue(1, i);
    result.users.push_back({
        "0x" + addr_hex,
        sort_val.IsNull() ? 0.0 : sort_val.GetValue<double>(),
        r->GetValue(2, i).GetValue<int64_t>(),
        r->GetValue(3, i).GetValue<int64_t>(),
        r->GetValue(4, i).GetValue<int64_t>(),
        r->GetValue(5, i).GetValue<int64_t>(),
    });
  }
  return result;
}

} // namespace stage3
