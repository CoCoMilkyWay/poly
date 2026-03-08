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
};

struct Result {
  int64_t anchor_bucket = 0;
  std::vector<UserRow> users;
};

} // namespace stage3::filter
