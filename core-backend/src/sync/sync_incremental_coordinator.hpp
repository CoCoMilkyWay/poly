#pragma once

// ============================================================================
// 小sync - 全局协调器（最外层，依赖 Scheduler）
// ============================================================================

#include <iostream>
#include <vector>

#include <boost/asio.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "../infra/https_pool.hpp"
#include "../stats/stats_manager.hpp"
#include "sync_incremental_scheduler.hpp"

namespace asio = boost::asio;

#define PARALLEL_TOTAL 9999

// ============================================================================
// SyncIncrementalCoordinator - 全局协调器（周期性小sync）
// ============================================================================
class SyncIncrementalCoordinator {
public:
  SyncIncrementalCoordinator(const Config &config, Database &db, HttpsPool &pool)
      : config_(config), db_(db), pool_(pool), sync_interval_(config.sync_interval_seconds) {
    db_.init_sync_state();
    StatsManager::instance().set_database(&db_);
  }

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    start_sync_round();
  }

private:
  void start_sync_round() {
    schedulers_.clear();
    total_active_ = 0;
    done_source_count_ = 0;

    schedulers_.reserve(config_.sources.size());
    for (const auto &src : config_.sources) {
      schedulers_.emplace_back(
          src, db_, pool_,
          [this]() -> bool { return try_acquire_slot(); },
          [this]() { release_slot(); },
          [this]() { on_source_done(); });
    }

    std::cout << "[Puller] 开始 sync, 共 " << schedulers_.size() << " 个 source" << std::endl;
    for (auto &s : schedulers_) {
      s.start();
    }
  }

  bool try_acquire_slot() {
    if (total_active_ < PARALLEL_TOTAL) {
      ++total_active_;
      return true;
    }
    return false;
  }

  void release_slot() {
    --total_active_;
    for (auto &s : schedulers_) {
      if (!s.all_done() && s.active_count() < PARALLEL_PER_SOURCE) {
        // 尝试启动更多任务
        break;
      }
    }
  }

  void on_source_done() {
    ++done_source_count_;
    if (done_source_count_ < static_cast<int>(schedulers_.size()))
      return;

    std::cout << "[Puller] 本轮 sync 完成, " << sync_interval_ << "s 后开始下一轮" << std::endl;

    asio::post(*ioc_, [this]() {
      auto timer = std::make_shared<asio::steady_timer>(*ioc_);
      timer->expires_after(std::chrono::seconds(sync_interval_));
      timer->async_wait([this, timer](boost::system::error_code) {
        start_sync_round();
      });
    });
  }

  const Config &config_;
  Database &db_;
  HttpsPool &pool_;
  asio::io_context *ioc_ = nullptr;

  std::vector<SyncIncrementalScheduler> schedulers_;
  int total_active_ = 0;
  int done_source_count_ = 0;
  int sync_interval_;
};
