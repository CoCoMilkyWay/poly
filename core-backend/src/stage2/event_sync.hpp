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
  int64_t stage1_transfer_count = 0;
  int64_t last_build_transfer_count = 0;
  int64_t pending_transfers = 0;
  double last_build_ms = 0;
  int64_t last_build_events = 0;
  int64_t last_build_users = 0;
};

class EventSync {
public:
  EventSync(Database &stage1_db, Database &stage2_db, int interval_seconds = 30)
      : stage1_db_(stage1_db), stage2_db_(stage2_db), builder_(stage1_db, stage2_db),
        interval_seconds_(interval_seconds) {}

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
      schedule_sync(interval_seconds_);
    });
  }

  void do_sync() {
    int64_t stage1_count = get_stage1_transfer_count();
    progress_.stage1_transfer_count = stage1_count;
    progress_.pending_transfers = stage1_count - progress_.last_build_transfer_count;

    if (progress_.pending_transfers > 0) {
      progress_.syncing = true;
      std::cout << "[Stage2Sync] 检测到 " << progress_.pending_transfers << " 条新 transfer，开始构建..." << std::endl;

      auto t0 = std::chrono::steady_clock::now();
      builder_.build_all();
      auto t1 = std::chrono::steady_clock::now();

      progress_.last_build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
      progress_.last_build_transfer_count = stage1_count;
      progress_.last_build_events = builder_.progress().transfer.events;
      progress_.last_build_users = builder_.progress().total_users;
      progress_.pending_transfers = 0;
      progress_.syncing = false;

      std::cout << "[Stage2Sync] 构建完成: " << progress_.last_build_events << " events, "
                << progress_.last_build_users << " users (" << (progress_.last_build_ms / 1000.0) << "s)" << std::endl;
    }
  }

  int64_t get_stage1_transfer_count() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query("SELECT COUNT(*) FROM transfer");
    assert(!result->HasError());
    return result->GetValue(0, 0).GetValue<int64_t>();
  }

  Database &stage1_db_;
  Database &stage2_db_;
  EventBuilder builder_;
  asio::io_context *ioc_ = nullptr;
  int interval_seconds_;
  SyncProgress progress_;
};

} // namespace stage2
