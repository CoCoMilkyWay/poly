#include "stage1_sync.hpp"

#include "../core/mem.hpp"
#include "../core/mem_json.hpp"

namespace stage1 {
namespace {

int64_t estimate_json_value(const json &j) {
  if (j.is_string()) {
    return static_cast<int64_t>(sizeof(json)) + core::mem::estimate_string_extra(j.get_ref<const std::string &>());
  }
  if (j.is_array()) {
    int64_t bytes = static_cast<int64_t>(sizeof(json));
    for (const auto &item : j) {
      bytes += estimate_json_value(item);
    }
    return bytes;
  }
  if (j.is_object()) {
    int64_t bytes = static_cast<int64_t>(sizeof(json));
    for (auto it = j.begin(); it != j.end(); ++it) {
      bytes += core::mem::estimate_string_extra(it.key());
      bytes += estimate_json_value(it.value());
    }
    return bytes;
  }
  return static_cast<int64_t>(sizeof(json));
}

int64_t estimate_decoded_events_extra(const DecodedEvents &ev) {
  int64_t bytes = 0;
  bytes += core::mem::estimate_vector(ev.condition_resolution, [](const ConditionResolveEvent &e) {
    return core::mem::estimate_vector_plain(e.payout_numerators);
  });
  bytes += core::mem::estimate_vector(ev.split, [](const SplitMergeEvent &e) { return core::mem::estimate_vector_plain(e.partition); });
  bytes += core::mem::estimate_vector(ev.merge, [](const SplitMergeEvent &e) { return core::mem::estimate_vector_plain(e.partition); });
  bytes += core::mem::estimate_vector(ev.redemption, [](const RedemptionEvent &e) { return core::mem::estimate_vector_plain(e.index_sets); });
  bytes += core::mem::estimate_vector(ev.fpmm, [](const FpmmEvent &e) {
    return core::mem::estimate_string_extra(e.creation_layout) + core::mem::estimate_vector_plain(e.condition_ids);
  });
  bytes += core::mem::estimate_vector(ev.fpmm_funding, [](const FpmmFundingEvent &e) { return core::mem::estimate_vector_plain(e.amounts); });
  bytes += core::mem::estimate_vector(ev.order_filled, [](const OrderFilledEvent &e) { return core::mem::estimate_string_extra(e.exchange); });
  bytes += core::mem::estimate_vector(ev.token_map, [](const TokenMapEvent &e) { return core::mem::estimate_string_extra(e.exchange); });
  bytes += core::mem::estimate_vector(ev.neg_risk_market, [](const NegRiskMarketEvent &e) {
    return e.data.has_value() ? core::mem::estimate_string_extra(*e.data) : 0;
  });
  bytes += core::mem::estimate_vector(ev.neg_risk_question, [](const NegRiskQuestionEvent &e) {
    return e.data.has_value() ? core::mem::estimate_string_extra(*e.data) : 0;
  });
  bytes += core::mem::estimate_vector_plain(ev.transfer);
  bytes += core::mem::estimate_vector_plain(ev.condition_preparation);
  bytes += core::mem::estimate_vector_plain(ev.fpmm_trade);
  bytes += core::mem::estimate_vector_plain(ev.convert);
  return bytes;
}

} // namespace

json StageSync::memory_breakdown() const {
  const int64_t rpc_workers_bytes = core::mem::estimate_vector_plain(rpc_workers_);
  const int64_t decode_workers_bytes = core::mem::estimate_vector_plain(decode_workers_);
  int64_t decode_queue_bytes = static_cast<int64_t>(sizeof(decode_queue_));
  for (const auto &task : decode_queue_) {
    decode_queue_bytes += static_cast<int64_t>(sizeof(DecodeTask));
    if (task.shared_raw_logs) {
      decode_queue_bytes += static_cast<int64_t>(sizeof(std::vector<json>));
      decode_queue_bytes += static_cast<int64_t>(task.shared_raw_logs->capacity()) * static_cast<int64_t>(sizeof(json));
      for (const auto &row : *task.shared_raw_logs) {
        decode_queue_bytes += estimate_json_value(row);
      }
    }
  }
  int64_t buffered_batches_bytes = static_cast<int64_t>(sizeof(buffered_batches_));
  for (const auto &batch : buffered_batches_) {
    buffered_batches_bytes += static_cast<int64_t>(sizeof(BufferedBatch));
    buffered_batches_bytes += estimate_decoded_events_extra(batch.events);
  }
  int64_t ready_batches_bytes = static_cast<int64_t>(sizeof(ready_batches_));
  ready_batches_bytes += static_cast<int64_t>(ready_batches_.size()) *
                         (static_cast<int64_t>(sizeof(std::pair<const int64_t, BufferedBatch>)) + core::mem::kNodeOverheadBytes);
  for (const auto &[_, batch] : ready_batches_) {
    ready_batches_bytes += estimate_decoded_events_extra(batch.events);
  }
  const int64_t chunk_history_bytes = static_cast<int64_t>(sizeof(chunk_history_)) +
                                      static_cast<int64_t>(chunk_history_.size()) * static_cast<int64_t>(sizeof(ChunkRecord));
  const int64_t commit_history_bytes = static_cast<int64_t>(sizeof(commit_history_)) +
                                       static_cast<int64_t>(commit_history_.size()) * static_cast<int64_t>(sizeof(CommitRecord));

  core::mem::MemRows rows = {
      {"buffered_batches_", buffered_batches_bytes},
      {"ready_batches_", ready_batches_bytes},
      {"decode_queue_", decode_queue_bytes},
      {"rpc_workers_", rpc_workers_bytes},
      {"decode_workers_", decode_workers_bytes},
      {"chunk_history_", chunk_history_bytes},
      {"commit_history_", commit_history_bytes},
  };
  core::mem::sort_mem_rows_desc(rows);

  const int64_t total_bytes =
      rpc_workers_bytes + decode_workers_bytes + decode_queue_bytes + buffered_batches_bytes + ready_batches_bytes +
      chunk_history_bytes + commit_history_bytes;
  return core::mem::build_memory_breakdown_json(rows, total_bytes);
}

} // namespace stage1
