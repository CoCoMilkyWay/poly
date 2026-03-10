#include "stage0_query_sync.hpp"

#include "../core/mem.hpp"
#include "../core/mem_json.hpp"

#include <deque>

namespace stage0 {

json QuerySync::memory_breakdown() const {
  const int64_t known_condition_ids_bytes = core::mem::estimate_unordered_set(
      known_condition_ids_, [](const std::string &s) { return core::mem::estimate_string_extra(s); });
  const int64_t commit_history_bytes =
      static_cast<int64_t>(sizeof(commit_history_)) + static_cast<int64_t>(commit_history_.size()) *
                                                           static_cast<int64_t>(sizeof(CommitRecord));

  core::mem::MemRows rows = {
      {"known_condition_ids_", known_condition_ids_bytes},
      {"commit_history_", commit_history_bytes},
  };
  core::mem::sort_mem_rows_desc(rows);
  return core::mem::build_memory_breakdown_json(rows, known_condition_ids_bytes + commit_history_bytes);
}

} // namespace stage0
