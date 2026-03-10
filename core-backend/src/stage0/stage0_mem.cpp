#include "stage0_mem.hpp"

#include <algorithm>

namespace stage0 {

json memory_breakdown(const QuerySync &query, const TagSync &tag) {
  const json q = query.memory_breakdown();
  const json t = tag.memory_breakdown();
  const int64_t q_total = q.value("estimated_total_bytes", int64_t{0});
  const int64_t t_total = t.value("estimated_total_bytes", int64_t{0});

  std::vector<std::pair<std::string, int64_t>> rows;
  for (const auto &item : q.value("items", json::array())) {
    rows.push_back({std::string("query.") + item.value("name", ""), item.value("estimated_bytes", int64_t{0})});
  }
  for (const auto &item : t.value("items", json::array())) {
    rows.push_back({std::string("tag.") + item.value("name", ""), item.value("estimated_bytes", int64_t{0})});
  }
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

  json items = json::array();
  for (const auto &row : rows) {
    items.push_back({{"name", row.first}, {"estimated_bytes", row.second}});
  }
  return {
      {"estimated_total_bytes", q_total + t_total},
      {"items", std::move(items)},
  };
}

} // namespace stage0
