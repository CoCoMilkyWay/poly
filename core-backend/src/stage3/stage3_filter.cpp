#include "stage3.hpp"

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
// Expression parser and evaluator (no SQL, direct feature evaluation)
// ============================================================================

std::string trim_copy(const std::string& s) {
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

std::string to_lower(const std::string& s) {
  std::string result = s;
  for (char& c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

std::string normalize_token(const std::string& raw) {
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

std::optional<int8_t> resolve_tag_id(const std::string& raw_tag) {
  std::string normalized = normalize_token(raw_tag);
  if (normalized.empty()) return std::nullopt;
  
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

std::optional<FeatureField> parse_feature_field(const std::string& token) {
  std::string t = to_lower(token);
  std::string base;
  enum class Window { W10, W100, W1000 };
  std::optional<Window> window;
  
  if (t.ends_with("1k") || t.ends_with("1000")) {
    base = t.ends_with("1k") ? t.substr(0, t.size() - 2) : t.substr(0, t.size() - 4);
    window = Window::W1000;
  } else if (t.ends_with("100")) {
    base = t.substr(0, t.size() - 3);
    window = Window::W100;
  } else if (t.ends_with("10")) {
    base = t.substr(0, t.size() - 2);
    window = Window::W10;
  } else {
    return std::nullopt;
  }
  
  if (base.empty()) return std::nullopt;
  
  if (base == "tok") {
    if (*window == Window::W10) return FeatureField::TOKEN_AVG_10W;
    if (*window == Window::W100) return FeatureField::TOKEN_AVG_100W;
    return FeatureField::TOKEN_AVG_1000W;
  }
  if (base == "exp") {
    if (*window == Window::W10) return FeatureField::EXPOSURE_AVG_10W;
    if (*window == Window::W100) return FeatureField::EXPOSURE_AVG_100W;
    return FeatureField::EXPOSURE_AVG_1000W;
  }
  if (base == "vol") {
    if (*window == Window::W10) return FeatureField::VOLUME_10W;
    if (*window == Window::W100) return FeatureField::VOLUME_AVG_100W;
    return FeatureField::VOLUME_AVG_1000W;
  }
  if (base == "hp") {
    if (*window == Window::W10) return FeatureField::HP_AVG_10W;
    if (*window == Window::W100) return FeatureField::HP_AVG_100W;
    return FeatureField::HP_AVG_1000W;
  }
  if (base == "shp") {
    if (*window == Window::W10) return FeatureField::SHARPE_10W;
    if (*window == Window::W100) return FeatureField::SHARPE_100W;
    return FeatureField::SHARPE_1000W;
  }
  
  return std::nullopt;
}

double get_feature_value(const FeatureSlot* feat, FeatureField field) {
  if (!feat) return 0.0;
  
  switch (field) {
    case FeatureField::TOKEN_AVG_10W: return static_cast<double>(feat->token_avg_10w);
    case FeatureField::TOKEN_AVG_100W: return static_cast<double>(feat->token_avg_100w);
    case FeatureField::TOKEN_AVG_1000W: return static_cast<double>(feat->token_avg_1000w);
    case FeatureField::EXPOSURE_AVG_10W: return static_cast<double>(feat->exposure_avg_10w);
    case FeatureField::EXPOSURE_AVG_100W: return static_cast<double>(feat->exposure_avg_100w);
    case FeatureField::EXPOSURE_AVG_1000W: return static_cast<double>(feat->exposure_avg_1000w);
    case FeatureField::VOLUME_10W: return static_cast<double>(feat->volume_10w);
    case FeatureField::VOLUME_AVG_100W: return static_cast<double>(feat->volume_avg_100w);
    case FeatureField::VOLUME_AVG_1000W: return static_cast<double>(feat->volume_avg_1000w);
    case FeatureField::HP_AVG_10W: return static_cast<double>(feat->holding_period_avg_10w);
    case FeatureField::HP_AVG_100W: return static_cast<double>(feat->holding_period_avg_100w);
    case FeatureField::HP_AVG_1000W: return static_cast<double>(feat->holding_period_avg_1000w);
    case FeatureField::SHARPE_10W: return static_cast<double>(feat->sharpe_10w);
    case FeatureField::SHARPE_100W: return static_cast<double>(feat->sharpe_100w);
    case FeatureField::SHARPE_1000W: return static_cast<double>(feat->sharpe_1000w);
  }
  return 0.0;
}

// ============================================================================
// Simple expression evaluator
// Supports: number, tag.field, +, -, *, /, >, <, >=, <=, ==, !=, and, or, not, (, )
// ============================================================================

struct FieldRef {
  int8_t tag_id;
  FeatureField field;
};

class ExprEvaluator {
public:
  using GetFeatureFn = std::function<const FeatureSlot*(int8_t tag_id)>;
  
  ExprEvaluator(GetFeatureFn get_feature) : get_feature_(std::move(get_feature)) {}
  
  double eval_numeric(const std::string& expr) {
    pos_ = 0;
    expr_ = trim_copy(expr);
    double result = parse_additive();
    assert(pos_ == expr_.size());
    return result;
  }
  
  bool eval_bool(const std::string& expr) {
    pos_ = 0;
    expr_ = trim_copy(expr);
    bool result = parse_or();
    assert(pos_ == expr_.size());
    return result;
  }
  
private:
  GetFeatureFn get_feature_;
  std::string expr_;
  size_t pos_ = 0;
  
  char peek() const { return pos_ < expr_.size() ? expr_[pos_] : '\0'; }
  char get() { return pos_ < expr_.size() ? expr_[pos_++] : '\0'; }
  void skip_ws() { while (std::isspace(static_cast<unsigned char>(peek()))) ++pos_; }
  
  bool parse_or() {
    bool left = parse_and();
    skip_ws();
    while (pos_ + 2 <= expr_.size() && to_lower(expr_.substr(pos_, 2)) == "or") {
      pos_ += 2;
      skip_ws();
      left = left || parse_and();
      skip_ws();
    }
    return left;
  }
  
  bool parse_and() {
    bool left = parse_not();
    skip_ws();
    while (pos_ + 3 <= expr_.size() && to_lower(expr_.substr(pos_, 3)) == "and") {
      pos_ += 3;
      skip_ws();
      left = left && parse_not();
      skip_ws();
    }
    return left;
  }
  
  bool parse_not() {
    skip_ws();
    if (pos_ + 3 <= expr_.size() && to_lower(expr_.substr(pos_, 3)) == "not") {
      pos_ += 3;
      skip_ws();
      return !parse_not();
    }
    return parse_comparison();
  }
  
  bool parse_comparison() {
    skip_ws();
    if (peek() == '(') {
      ++pos_;
      bool result = parse_or();
      skip_ws();
      assert(peek() == ')');
      ++pos_;
      return result;
    }
    
    double left = parse_additive();
    skip_ws();
    
    std::string op;
    if (pos_ + 2 <= expr_.size()) {
      std::string two = expr_.substr(pos_, 2);
      if (two == ">=" || two == "<=" || two == "==" || two == "!=") {
        op = two;
        pos_ += 2;
      }
    }
    if (op.empty() && (peek() == '>' || peek() == '<' || peek() == '=')) {
      op = std::string(1, get());
    }
    
    if (op.empty()) {
      // No comparison operator - treat as boolean (non-zero = true)
      return left != 0.0;
    }
    
    skip_ws();
    double right = parse_additive();
    
    if (op == ">") return left > right;
    if (op == "<") return left < right;
    if (op == ">=") return left >= right;
    if (op == "<=") return left <= right;
    if (op == "==" || op == "=") return std::abs(left - right) < 1e-9;
    if (op == "!=") return std::abs(left - right) >= 1e-9;
    
    assert(false);
    return false;
  }
  
  double parse_additive() {
    double left = parse_multiplicative();
    skip_ws();
    while (peek() == '+' || peek() == '-') {
      char op = get();
      skip_ws();
      double right = parse_multiplicative();
      if (op == '+') left += right;
      else left -= right;
      skip_ws();
    }
    return left;
  }
  
  double parse_multiplicative() {
    double left = parse_unary();
    skip_ws();
    while (peek() == '*' || peek() == '/') {
      char op = get();
      skip_ws();
      double right = parse_unary();
      if (op == '*') left *= right;
      else left /= right;
      skip_ws();
    }
    return left;
  }
  
  double parse_unary() {
    skip_ws();
    if (peek() == '-') {
      ++pos_;
      return -parse_unary();
    }
    if (peek() == '+') {
      ++pos_;
      return parse_unary();
    }
    return parse_primary();
  }
  
  double parse_primary() {
    skip_ws();
    
    // Parentheses
    if (peek() == '(') {
      ++pos_;
      double result = parse_additive();
      skip_ws();
      assert(peek() == ')');
      ++pos_;
      return result;
    }
    
    // Number
    if (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.') {
      size_t start = pos_;
      while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.') {
        ++pos_;
      }
      return std::stod(expr_.substr(start, pos_ - start));
    }
    
    // Identifier (tag.field or field)
    if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_') {
      size_t start = pos_;
      while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        ++pos_;
      }
      std::string lhs = expr_.substr(start, pos_ - start);
      
      skip_ws();
      if (peek() == '.') {
        // tag.field format
        ++pos_;
        skip_ws();
        start = pos_;
        while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
          ++pos_;
        }
        std::string rhs = expr_.substr(start, pos_ - start);
        
        auto tag_id = resolve_tag_id(lhs);
        assert(tag_id.has_value());
        
        auto field = parse_feature_field(rhs);
        assert(field.has_value());
        
        // Sharpe only for all (-1)
        if (*field >= FeatureField::SHARPE_10W && *tag_id != -1) {
          return 0.0;
        }
        
        const FeatureSlot* feat = get_feature_(*tag_id);
        return get_feature_value(feat, *field);
      }
      
      // Just field name - assume all (-1)
      auto field = parse_feature_field(lhs);
      if (field.has_value()) {
        const FeatureSlot* feat = get_feature_(-1);
        return get_feature_value(feat, *field);
      }
      
      // Keywords
      if (to_lower(lhs) == "true") return 1.0;
      if (to_lower(lhs) == "false") return 0.0;
      
      assert(false); // Unknown identifier
      return 0.0;
    }
    
    assert(false);
    return 0.0;
  }
};

} // namespace

// ============================================================================
// stage3_query_filter
// ============================================================================

FilterResult stage3_query_filter(Stage3Runtime* rt, const FilterRequest& req) {
  FilterResult result{};
  result.anchor_bucket = req.anchor_bucket;
  
  assert(req.anchor_bucket >= 0);
  assert(req.limit > 0 && req.limit <= 1000);
  
  const int32_t anchor_bucket = static_cast<int32_t>(req.anchor_bucket);
  
  // Temporary storage for candidates
  struct Candidate {
    uint32_t user_idx;
    double sort_value;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(rt->header->user_count);
  
  // Scan all users
  for (uint64_t i = 0; i < rt->header->user_count; ++i) {
    uint32_t user_idx = static_cast<uint32_t>(i);
    UserBlock* user = &rt->users[user_idx];
    
    if (!(user->flags & 1)) continue; // Not occupied
    
    // Create feature getter for this user
    auto get_feature = [&](int8_t tag_id) -> const FeatureSlot* {
      return feature_find(rt, user_idx, anchor_bucket, tag_id);
    };
    
    // Check if user has any feature at anchor_bucket
    const FeatureSlot* global_feat = get_feature(-1);
    if (!global_feat) continue;
    
    // Evaluate filters
    ExprEvaluator evaluator(get_feature);
    bool pass_all = true;
    
    for (const auto& filter : req.filters) {
      if (filter.empty()) continue;
      try {
        if (!evaluator.eval_bool(filter)) {
          pass_all = false;
          break;
        }
      } catch (...) {
        pass_all = false;
        break;
      }
    }
    
    if (!pass_all) continue;
    
    // Evaluate sort expression
    double sort_value = 0.0;
    if (!req.sort_expr.empty()) {
      try {
        sort_value = evaluator.eval_numeric(req.sort_expr);
      } catch (...) {
        continue;
      }
    }
    
    candidates.push_back({user_idx, sort_value});
  }
  
  // Sort
  if (req.sort_asc) {
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                return a.sort_value < b.sort_value;
              });
  } else {
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                return a.sort_value > b.sort_value;
              });
  }
  
  // Limit results
  size_t limit = std::min(static_cast<size_t>(req.limit), candidates.size());
  result.users.reserve(limit);
  
  for (size_t i = 0; i < limit; ++i) {
    FilterUserRow row{};
    row.addr = rt->users[candidates[i].user_idx].addr;
    row.sort_value = candidates[i].sort_value;
    result.users.push_back(row);
  }
  
  return result;
}

} // namespace stage3
