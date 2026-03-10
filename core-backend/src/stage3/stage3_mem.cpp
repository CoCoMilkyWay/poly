#include "stage3_sync.hpp"

#include "../core/mem.hpp"
#include "../core/mem_json.hpp"

namespace stage3 {

json StageSync::memory_breakdown() const {
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
  const int64_t stage3_peak_candidate_bytes = stage3_persistent_total + probe_copy.peak_working_set_bytes;

  core::mem::MemRows stage3_rows = {
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
  core::mem::sort_mem_rows_desc(stage3_rows);
  json out = core::mem::build_memory_breakdown_json(stage3_rows, probe_copy.total_working_set_bytes + stage3_persistent_total, 20);
  out["persistent_bytes"] = stage3_persistent_total;
  out["peak_working_set_bytes"] = probe_copy.peak_working_set_bytes;
  out["estimated_peak_candidate_bytes"] = stage3_peak_candidate_bytes;
  return out;
}

} // namespace stage3
