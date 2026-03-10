#include "stage0_tag_sync.hpp"

#include "../core/mem.hpp"
#include "../core/mem_json.hpp"

namespace stage0 {

json TagSync::memory_breakdown() const {
  int64_t tagger_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(tagger_mutex_);
    if (tagger_) {
      const json tagger_mem = tagger_->memory_breakdown();
      if (tagger_mem.contains("estimated_total_bytes")) {
        tagger_bytes = tagger_mem["estimated_total_bytes"].get<int64_t>();
      }
    }
  }

  int64_t sync_strings_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    sync_strings_bytes += core::mem::estimate_string_extra(sync_.tag_device);
    sync_strings_bytes += core::mem::estimate_string_extra(sync_.tag_model_path);
    sync_strings_bytes += core::mem::estimate_string_extra(sync_.tag_model_size_text);
  }

  core::mem::MemRows rows = {
      {"tagger_", tagger_bytes},
      {"status_strings", sync_strings_bytes},
  };
  core::mem::sort_mem_rows_desc(rows);
  return core::mem::build_memory_breakdown_json(rows, tagger_bytes + sync_strings_bytes);
}

} // namespace stage0
