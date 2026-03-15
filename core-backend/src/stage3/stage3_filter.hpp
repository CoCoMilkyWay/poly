#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace stage3::filter {

struct Request {
  int64_t anchor_bucket;
  std::vector<std::string> filters;
  std::string sort_expr;
  bool sort_asc;
  int32_t limit;
};

struct UserRow {
  std::string addr;
  double sort_value = 0.0;
  int64_t month_avg_tok = 0;
  int64_t month_avg_exp = 0;
  int64_t month_avg_hp = 0;
  int64_t pnl = 0;
};

struct FilterStat {
  int64_t pass_count = 0;   // 满足该条件的用户数
  int64_t reject_count = 0; // 被该条件过滤掉的用户数
};

struct Result {
  int64_t anchor_bucket = 0;
  std::vector<UserRow> users;
  std::vector<FilterStat> filter_stats; // 每个过滤条件的统计
};

} // namespace stage3::filter
