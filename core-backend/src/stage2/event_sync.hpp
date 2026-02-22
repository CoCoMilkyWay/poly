#pragma once

#include <chrono>
#include <iostream>

#include <boost/asio.hpp>

#include "../core/database.hpp"
#include "event_build.hpp"

namespace asio = boost::asio;

namespace stage2 {

struct SyncProgress {
  bool syncing = false;
  int64_t stage1_last_block = 0;
  int64_t stage2_last_block = 0;
  int64_t behind_chunks = 0;
  double last_build_ms = 0;
  int64_t last_build_events = 0;
  int64_t last_build_users = 0;
};

class EventSync {
public:
  static constexpr int64_t CHUNK_THRESHOLD = 1000;

  EventSync(Database &stage1_db, Database &stage2_db, int chunk_size, int base_interval = 2)
      : stage1_db_(stage1_db), stage2_db_(stage2_db), builder_(stage1_db, stage2_db),
        chunk_size_(chunk_size), base_interval_(base_interval) {}

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    schedule_sync(1);
  }

  const SyncProgress &progress() const { return progress_; }
  const BuildProgress &build_progress() const { return builder_.progress(); }

  EventBuilder &builder() { return builder_; }

private:
  void schedule_sync(int delay_seconds) {
    auto timer = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
    timer->async_wait([this, timer](const boost::system::error_code &ec) {
      if (ec)
        return;
      do_sync();
    });
  }

  void do_sync() {
    int64_t stage1_last = get_stage1_last_block();
    int64_t stage2_last = progress_.stage2_last_block;
    int64_t behind_blocks = stage1_last - stage2_last;
    int64_t behind_chunks = (behind_blocks + chunk_size_ - 1) / chunk_size_;

    progress_.stage1_last_block = stage1_last;
    progress_.behind_chunks = behind_chunks;

    if (behind_chunks < CHUNK_THRESHOLD) {
      int sleep_sec = std::max(1, static_cast<int>(behind_chunks * base_interval_));
      std::cout << "[Stage2Sync] 落后 " << behind_chunks << " chunks, 睡眠 " << sleep_sec << "s" << std::endl;
      schedule_sync(sleep_sec);
      return;
    }

    progress_.syncing = true;
    std::cout << "[Stage2Sync] 落后 " << behind_chunks << " chunks (>= " << CHUNK_THRESHOLD
              << "), 开始构建..." << std::endl;

    auto t0 = std::chrono::steady_clock::now();
    builder_.build_all();
    auto t1 = std::chrono::steady_clock::now();

    progress_.last_build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    progress_.stage2_last_block = stage1_last;
    progress_.last_build_events = builder_.progress().transfer.events;
    progress_.last_build_users = builder_.progress().total_users;
    progress_.behind_chunks = 0;
    progress_.syncing = false;

    std::cout << "[Stage2Sync] 构建完成: " << progress_.last_build_events << " events, "
              << progress_.last_build_users << " users (" << (progress_.last_build_ms / 1000.0) << "s)" << std::endl;

    schedule_sync(1);
  }

  int64_t get_stage1_last_block() {
    return stage1_db_.get_last_block();
  }

  Database &stage1_db_;
  Database &stage2_db_;
  EventBuilder builder_;
  asio::io_context *ioc_ = nullptr;
  int chunk_size_;
  int base_interval_;
  SyncProgress progress_;
};

} // namespace stage2
