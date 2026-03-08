#include "stage3_sync.hpp"

#include <cassert>
#include <cctype>
#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "misc/profiler.hpp"

namespace stage3 {
namespace {

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

  auto resolve_tag = [this](const std::string &raw_tag) -> std::optional<int32_t> {
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
    auto it = tag_to_industry_id_.find(normalized);
    if (it == tag_to_industry_id_.end()) {
      return std::nullopt;
    }
    return static_cast<int32_t>(it->second);
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

  std::string sql = "WITH user_features AS (SELECT user_addr";
  for (const auto &binding : bindings) {
    sql += ", MAX(CASE WHEN tag_id = " + std::to_string(binding.tag_id) +
           " THEN " + binding.column + " END) AS " + binding.alias;
  }
  sql += " FROM " + std::string(kSqlTableFeatureTensorState) +
         " WHERE block_bucket = " + std::to_string(anchor_bucket) +
         " GROUP BY user_addr)";
  sql += " SELECT lower(hex(user_addr)) AS addr, CAST((" + sort_sql +
         ") AS DOUBLE) AS sort_value FROM user_features";
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
    });
  }
  return result;
}

} // namespace stage3
