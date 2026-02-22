#pragma once

#include <atomic>
#include <chrono>

#include <boost/asio.hpp>

#include "../core/database.hpp"
#include "event_build.hpp"

namespace asio = boost::asio;

namespace stage2 {

struct SyncProgress {
  bool syncing = false;
  int64_t stage1_last_block = 0;
  int64_t stage2_cursor = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  int64_t chunks_per_rebuild = 0;
  int phase = 0;
};

class EventSync {
public:
  EventSync(Database &stage1_db, Database &stage2_db, int chunk_size, int base_interval = 30)
      : stage1_db_(stage1_db), stage2_db_(stage2_db),
        builder_(stage1_db, stage2_db, chunk_size),
        chunk_size_(chunk_size), base_interval_(base_interval) {
    builder_.init_schema();
    builder_.load_from_rb();
    progress_.stage2_cursor = builder_.cursor();
  }

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    schedule_sync(1);
  }

  void stop() { stop_requested_ = true; }

  const SyncProgress &progress() const { return progress_; }
  const BuildProgress &build_progress() const { return builder_.progress(); }

  EventBuilder &builder() { return builder_; }

private:
  void schedule_sync(int delay_seconds) {
    if (stop_requested_) return;
    auto timer = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
    timer->async_wait([this, timer](const boost::system::error_code &ec) {
      if (ec || stop_requested_) return;
      do_sync();
    });
  }

  void do_sync() {
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

  Database &stage1_db_;
  Database &stage2_db_;
  EventBuilder builder_;
  asio::io_context *ioc_ = nullptr;
  int chunk_size_;
  int base_interval_;
  SyncProgress progress_;
  std::atomic<bool> stop_requested_{false};
};

} // namespace stage2
