#include "stage2_sync.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>

#include "misc/profiler.hpp"

namespace stage2 {

EventSync::EventSync(Database &stage1_db, Database &stage2_db, int base_interval)
    : stage1_db_(stage1_db), stage2_db_(stage2_db),
      builder_(stage1_db, stage2_db, kStage2ChunkBlocks),
      chunk_size_(kStage2ChunkBlocks), base_interval_(base_interval) {
  builder_.init_schema();
  builder_.load_from_rb();
  progress_.stage2_cursor = builder_.cursor();
}

void EventSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  schedule_sync(1);
}

void EventSync::stop() { stop_requested_ = true; }

const SyncProgress &EventSync::progress() const { return progress_; }

const BuildProgress &EventSync::build_progress() const { return builder_.progress(); }

EventBuilder &EventSync::builder() { return builder_; }

void EventSync::schedule_sync(int delay_seconds) {
  if (stop_requested_)
    return;
  auto timer = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer->async_wait([this, timer](const boost::system::error_code &ec) {
    if (ec || stop_requested_)
      return;
    do_sync();
  });
}

void EventSync::do_sync() {
  TraceN("s2/sync");
  auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
    if (chunk_interval_history_s_.empty()) {
      progress_.blocks_per_second = 0.0;
      progress_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    assert(chunk_interval_history_s_.size() == chunk_block_history_.size());
    double total_interval_s = 0.0;
    double total_blocks = 0.0;
    for (size_t i = 0; i < chunk_interval_history_s_.size(); ++i) {
      total_interval_s += chunk_interval_history_s_[i];
      total_blocks += static_cast<double>(chunk_block_history_[i]);
    }
    if (total_interval_s <= 0.0) {
      progress_.blocks_per_second = 0.0;
      progress_.eta_seconds = -1.0;
      return;
    }
    progress_.blocks_per_second = total_blocks / total_interval_s;
    double avg_interval_s = total_interval_s / static_cast<double>(chunk_interval_history_s_.size());
    int64_t remaining_chunks = (remaining_blocks + chunk_size_ - 1) / chunk_size_;
    progress_.eta_seconds = (remaining_blocks == 0) ? 0.0 : avg_interval_s * static_cast<double>(remaining_chunks);
  };
  int64_t stage1_last = stage1_db_.get_last_block();
  int64_t stage2_cursor = builder_.cursor();
  int64_t behind_blocks = std::max<int64_t>(0, stage1_last - stage2_cursor);
  int64_t behind_chunks = (behind_blocks + chunk_size_ - 1) / chunk_size_;

  progress_.stage1_last_block = stage1_last;
  progress_.stage2_cursor = stage2_cursor;
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

  int64_t target = stage1_last;
  builder_.build_chunk(target);
  progress_.phase = builder_.progress().phase;
  int64_t new_cursor = builder_.cursor();
  if (new_cursor > stage2_cursor) {
    auto chunk_done_at = std::chrono::steady_clock::now();
    if (last_chunk_done_at_.has_value()) {
      double interval_s = std::chrono::duration<double>(chunk_done_at - *last_chunk_done_at_).count();
      if (interval_s <= 0.0) {
        interval_s = 1e-6;
      }
      int64_t block_count = new_cursor - stage2_cursor;
      chunk_interval_history_s_.push_back(interval_s);
      chunk_block_history_.push_back(block_count);
      if (chunk_interval_history_s_.size() > kEtaWindowSize) {
        chunk_interval_history_s_.pop_front();
        chunk_block_history_.pop_front();
      }
    }
    last_chunk_done_at_ = chunk_done_at;
  }
  progress_.stage2_cursor = new_cursor;
  int64_t updated_behind_blocks = std::max<int64_t>(0, stage1_last - new_cursor);
  progress_.behind_blocks = updated_behind_blocks;
  progress_.behind_chunks = (updated_behind_blocks + chunk_size_ - 1) / chunk_size_;
  refresh_timing_metrics(updated_behind_blocks);

  progress_.syncing = false;
  schedule_sync(progress_.behind_chunks > 1 ? 0 : base_interval_);
}

} // namespace stage2
