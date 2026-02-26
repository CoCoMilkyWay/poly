#include "stage2_sync.hpp"

#include <algorithm>
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
  int64_t stage1_last = stage1_db_.get_last_block();
  int64_t stage2_cursor = builder_.cursor();
  int64_t behind_blocks = stage1_last - stage2_cursor;
  int64_t behind_chunks = (behind_blocks + chunk_size_ - 1) / chunk_size_;

  progress_.stage1_last_block = stage1_last;
  progress_.stage2_cursor = stage2_cursor;
  progress_.behind_blocks = behind_blocks;
  progress_.behind_chunks = behind_chunks;

  if (behind_chunks == 0) {
    progress_.syncing = false;
    schedule_sync(base_interval_);
    return;
  }

  progress_.syncing = true;
  progress_.chunks_per_rebuild = 1;

  int64_t target = std::min(builder_.cursor() + chunk_size_, stage1_last);
  builder_.build_chunk(target);
  progress_.phase = builder_.progress().phase;
  progress_.stage2_cursor = builder_.cursor();

  progress_.syncing = false;
  schedule_sync(behind_chunks > 1 ? 0 : base_interval_);
}

} // namespace stage2
