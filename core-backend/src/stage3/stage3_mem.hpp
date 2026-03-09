#pragma once

#include "../core/mem.hpp"
#include "stage3_sync.hpp"

namespace stage3 {

inline json StageSync::memory_breakdown() const {
  auto cond_extra = [](const ConditionInfo &info) {
    return static_cast<int64_t>(info.question_id.capacity()) +
           static_cast<int64_t>(info.payout_numerators.capacity()) * static_cast<int64_t>(sizeof(int64_t));
  };
  const int64_t conditions_bytes = core::mem::estimate_vector(conditions_, cond_extra);
  const int64_t cond_tag_ids_bytes = core::mem::estimate_vector_plain(cond_tag_ids_);
  const int64_t tag_to_industry_bytes =
      core::mem::estimate_unordered_map(tag_to_industry_id_, [](const std::string &k) { return core::mem::estimate_string_extra(k); },
                                        [](const int8_t &) { return int64_t{0}; });

  int64_t cache_addr_bytes = 0;
  int64_t cache_timeline_bytes = 0;
  int64_t cache_snapshots_bytes = 0;
  {
    std::lock_guard<std::mutex> lock(user_cache_mu_);
    cache_addr_bytes = core::mem::estimate_string_extra(user_cache_.addr_lower);
    cache_timeline_bytes = core::mem::estimate_vector_plain(user_cache_.timeline);
    cache_snapshots_bytes = core::mem::estimate_vector(user_cache_.snapshots, [](const UserQueryCache::Snapshot &s) {
      return core::mem::estimate_vector_plain(s.positions);
    });
  }

  RuntimeMemProbe probe_copy;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    probe_copy = runtime_mem_probe_;
  }
  const int64_t stage3_persistent_total = conditions_bytes + cond_tag_ids_bytes + tag_to_industry_bytes + cache_addr_bytes +
                                          cache_timeline_bytes + cache_snapshots_bytes;

  std::vector<std::pair<std::string, int64_t>> stage3_rows = {
      {"event_inputs", probe_copy.event_inputs_bytes},
      {"user_blob_pool", probe_copy.user_blob_pool_bytes},
      {"user_index_maps", probe_copy.user_index_bytes},
      {"token_states", probe_copy.token_states_bytes},
      {"bucket_agg", probe_copy.bucket_agg_bytes},
      {"event_facts", probe_copy.event_facts_bytes},
      {"conditions", conditions_bytes},
      {"cond_tag_ids", cond_tag_ids_bytes},
      {"tag_to_industry", tag_to_industry_bytes},
      {"user_cache_addr", cache_addr_bytes},
      {"user_cache_timeline", cache_timeline_bytes},
      {"user_cache_snapshots", cache_snapshots_bytes},
  };
  std::sort(stage3_rows.begin(), stage3_rows.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

  json stage3_top = json::array();
  const size_t top_n = std::min<size_t>(stage3_rows.size(), 20);
  for (size_t i = 0; i < top_n; ++i) {
    stage3_top.push_back({{"name", stage3_rows[i].first}, {"estimated_bytes", stage3_rows[i].second}});
  }

  json stage3_runtime = {
      {"estimated_total_bytes", probe_copy.total_working_set_bytes + stage3_persistent_total},
      {"working_set_bytes", probe_copy.total_working_set_bytes},
      {"persistent_bytes", stage3_persistent_total},
      {"peak_working_set_bytes", probe_copy.peak_working_set_bytes},
      {"row_count", probe_copy.row_count},
      {"max_cond_idx", probe_copy.max_cond_idx},
      {"top_containers", stage3_top},
  };

  json result = {
      {"stage2_builder", builder_.memory_breakdown()},
      {"stage3_runtime", stage3_runtime},
  };
  return result;
}

} // namespace stage3
