#include "stage2_sync.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>

#include "misc/profiler.hpp"

namespace stage2 {

StageSync::StageSync(Database &stage1_db, Database &stage2_db, int base_interval)
    : stage1_db_(stage1_db), stage2_db_(stage2_db),
      builder_(stage1_db, stage2_db, kStage2ChunkBlocks),
      chunk_size_(kStage2ChunkBlocks), base_interval_(base_interval) {
  builder_.init_schema();
  builder_.load_from_rb();
  progress_.last_block = builder_.cursor();
}

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  schedule_sync(1);
}

void StageSync::stop() { stop_requested_ = true; }

const SyncProgress &StageSync::status() const { return progress_; }

const SyncProgress &StageSync::progress() const { return progress_; }

const BuildProgress &StageSync::build_progress() const { return builder_.progress(); }

EventBuilder &StageSync::builder() { return builder_; }

void StageSync::schedule_sync(int delay_seconds) {
  if (stop_requested_)
    return;
  auto timer = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer->async_wait([this, timer](const boost::system::error_code &ec) {
    if (ec || stop_requested_)
      return;
    do_sync();
  });
}

void StageSync::do_sync() {
  TraceN("s2/sync");
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (commit_history_.size() < 2) {
      progress_.blocks_per_second = 0.0;
      progress_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = commit_history_.front();
    const auto &last = commit_history_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      progress_.blocks_per_second = 0.0;
      progress_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.cursor - first.cursor);
    if (committed_blocks == 0) {
      progress_.blocks_per_second = 0.0;
      progress_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    progress_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    progress_.eta_seconds =
        (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / progress_.blocks_per_second;
  };
  int64_t head_block = stage1_db_.get_last_block();
  int64_t last_block = builder_.cursor();
  int64_t behind_blocks = std::max<int64_t>(0, head_block - last_block);
  int64_t behind_chunks = (behind_blocks + chunk_size_ - 1) / chunk_size_;

  progress_.head_block = head_block;
  progress_.last_block = last_block;
  progress_.behind_blocks = behind_blocks;
  progress_.behind_chunks = behind_chunks;
  refresh_timing_metrics(behind_blocks);

  if (behind_chunks == 0) {
    progress_.syncing = false;
    schedule_sync(base_interval_);
    return;
  }

  progress_.syncing = true;
  progress_.chunks_per_rebuild = 1;

  int64_t target = head_block;
  builder_.build_chunk(target);
  progress_.phase = builder_.progress().phase;
  int64_t new_cursor = builder_.cursor();
  if (new_cursor > last_block) {
    commit_history_.push_back({std::chrono::steady_clock::now(), new_cursor});
    if (commit_history_.size() > kEtaWindowSize) {
      commit_history_.pop_front();
    }
  }
  progress_.last_block = new_cursor;
  int64_t updated_behind_blocks = std::max<int64_t>(0, head_block - new_cursor);
  progress_.behind_blocks = updated_behind_blocks;
  progress_.behind_chunks = (updated_behind_blocks + chunk_size_ - 1) / chunk_size_;
  refresh_timing_metrics(updated_behind_blocks);

  progress_.syncing = false;
  schedule_sync(progress_.behind_chunks > 1 ? 0 : base_interval_);
}

} // namespace stage2
