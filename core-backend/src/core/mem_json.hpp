#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace core::mem {

using json = nlohmann::json;
using MemRows = std::vector<std::pair<std::string, int64_t>>;

inline void sort_mem_rows_desc(MemRows &rows) {
  std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
}

inline json build_items_json(const MemRows &rows, size_t limit = std::numeric_limits<size_t>::max()) {
  json items = json::array();
  const size_t n = std::min(rows.size(), limit);
  for (size_t i = 0; i < n; ++i) {
    items.push_back({{"name", rows[i].first}, {"estimated_bytes", rows[i].second}});
  }
  return items;
}

inline json build_memory_breakdown_json(const MemRows &rows, int64_t estimated_total_bytes,
                                        size_t limit = std::numeric_limits<size_t>::max()) {
  return {
      {"estimated_total_bytes", estimated_total_bytes},
      {"items", build_items_json(rows, limit)},
  };
}

} // namespace core::mem
