#include "stage2_sync.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <vector>

#include "misc/profiler.hpp"

namespace stage2 {

StageSync::StageSync(Database &stage1_db, Database &stage2_db, int base_interval)
    : stage1_db_(stage1_db),
      builder_(stage1_db, stage2_db), base_interval_(base_interval) {
  builder_.init_schema();
  builder_.load_from_rb();
  sync_.last_block = builder_.cursor();
}

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  builder_.clear_stop();
  schedule_sync(1);
}

void StageSync::stop() {
  stop_requested_ = true;
  builder_.request_stop();
}

auto StageSync::status() const -> const StageSync::Status & { return sync_; }

EventBuilder &StageSync::builder() { return builder_; }

std::optional<StageSync::ChunkBoundary>
StageSync::next_transfer_chunk(const std::vector<Database::FeatherChunk> &chunks,
                               int64_t cursor, int64_t head_block) {
  if (head_block <= cursor) {
    return std::nullopt;
  }
  if (chunks.empty()) {
    return std::nullopt;
  }
  for (const auto &chunk : chunks) {
    if (chunk.start_block > head_block) {
      break;
    }
    if (chunk.end_block <= cursor) {
      continue;
    }
    int64_t clipped_end = std::min<int64_t>(chunk.end_block, head_block);
    return ChunkBoundary{chunk.start_block, clipped_end};
  }
  return std::nullopt;
}

int64_t StageSync::pending_transfer_chunks(const std::vector<Database::FeatherChunk> &chunks,
                                           int64_t cursor, int64_t head_block) {
  if (head_block <= cursor) {
    return 0;
  }
  if (chunks.empty()) {
    return 0;
  }
  int64_t count = 0;
  for (const auto &chunk : chunks) {
    if (chunk.start_block > head_block) {
      break;
    }
    if (chunk.end_block > cursor) {
      count += 1;
    }
  }
  return count;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (stop_requested_)
    return;
  auto timer = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer->async_wait([this, timer](const boost::system::error_code &ec) {
    if (ec || stop_requested_)
      return;
    // Wait for pending commit outside of do_sync trace zone so tracy shows main thread as idle.
    builder_.wait_for_pending_commit();
    do_sync();
  });
}

void StageSync::do_sync() {
  Trace;
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (commit_history_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = commit_history_.front();
    const auto &last = commit_history_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.cursor - first.cursor);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
  };
  int64_t head_block = stage1_db_.get_last_block();
  int64_t last_block = builder_.cursor();
  std::string sync_trace_name = "s2/sync " + std::to_string(last_block + 1) + "-" + std::to_string(head_block);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());
  std::vector<Database::FeatherChunk> transfer_chunks = stage1_db_.feather_chunks("transfer");
  int64_t behind_blocks = std::max<int64_t>(0, head_block - last_block);
  int64_t behind_chunks = pending_transfer_chunks(transfer_chunks, last_block, head_block);

  sync_.head_block = head_block;
  sync_.last_block = last_block;
  sync_.behind_blocks = behind_blocks;
  sync_.behind_chunks = behind_chunks;
  refresh_timing_metrics(behind_blocks);

  if (behind_chunks == 0) {
    sync_.syncing = false;
    schedule_sync(base_interval_);
    return;
  }

  sync_.syncing = true;

  auto next_chunk = next_transfer_chunk(transfer_chunks, last_block, head_block);
  assert(next_chunk.has_value());
  sync_trace_name = "s2/sync " + std::to_string(next_chunk->start) + "-" + std::to_string(next_chunk->end);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());
  int64_t target = next_chunk->end;
  assert(target > last_block);
  assert(target <= head_block);
  builder_.build_chunk(target);
  int64_t new_cursor = builder_.cursor();
  if (new_cursor > last_block) {
    commit_history_.push_back({std::chrono::steady_clock::now(), new_cursor});
    if (commit_history_.size() > kEtaWindowSize) {
      commit_history_.pop_front();
    }
  }
  sync_.last_block = new_cursor;
  int64_t updated_behind_blocks = std::max<int64_t>(0, head_block - new_cursor);
  sync_.behind_blocks = updated_behind_blocks;
  sync_.behind_chunks = pending_transfer_chunks(transfer_chunks, new_cursor, head_block);
  refresh_timing_metrics(updated_behind_blocks);

  sync_.syncing = false;
  schedule_sync(sync_.behind_chunks > 1 ? 0 : base_interval_);
}

} // namespace stage2
