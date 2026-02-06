#pragma once

// ============================================================================
// 小sync - Source调度器（中间层，依赖 Executor）
// ============================================================================

#include <cassert>
#include <functional>
#include <string>
#include <vector>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "../core/entity_definition.hpp"
#include "../infra/https_pool.hpp"
#include "../stats/stats_manager.hpp"
#include "sync_incremental_executor.hpp"

#define PARALLEL_PER_SOURCE 9999

// ============================================================================
// SyncIncrementalScheduler - 单个 source 的调度器
// ============================================================================
class SyncIncrementalScheduler {
public:
  using DoneCallback = std::function<void()>;
  using SlotAcquireFunc = std::function<bool()>;
  using SlotReleaseFunc = std::function<void()>;

  SyncIncrementalScheduler(const SourceConfig &config, Database &db, HttpsPool &pool,
                           SlotAcquireFunc try_acquire, SlotReleaseFunc release, DoneCallback on_done)
      : source_name_(config.name), db_(db), pool_(pool),
        try_acquire_slot_(std::move(try_acquire)),
        release_slot_(std::move(release)),
        on_done_(std::move(on_done)) {

    for (const auto &entity_name : config.entities) {
      auto it = config.entity_table_map.find(entity_name);
      assert(it != config.entity_table_map.end());
      auto *e = entities::find_entity_by_table(it->second.c_str());
      assert(e && "Unknown entity table");

      if (db_.is_entity_disabled(source_name_, e->name)) {
        std::cout << "[Scheduler] " << source_name_ << "/" << e->name << " disabled, skip" << std::endl;
        continue;
      }

      db_.init_entity(e);

      int64_t count = db_.get_table_count(e->table);
      int64_t row_size_bytes = entities::estimate_row_size_bytes(e);
      StatsManager::instance().init(source_name_, e->name, count, row_size_bytes);

      executors_.emplace_back(config.subgraph_id, source_name_, e, db_, pool_,
                              [this]() { on_executor_done(); });
    }
  }

  void start() {
    std::cout << "[Scheduler] " << source_name_ << " start, " << executors_.size() << " entities" << std::endl;
    if (executors_.empty()) {
      on_done_();
      return;
    }
    start_next();
  }

  const std::string &name() const { return source_name_; }
  bool all_done() const { return done_count_ == static_cast<int>(executors_.size()); }
  int active_count() const { return active_count_; }

private:
  void on_executor_done() {
    ++done_count_;
    --active_count_;
    release_slot_();

    if (all_done()) {
      on_done_();
      return;
    }

    start_next();
  }

  void start_next() {
    while (next_idx_ < executors_.size() &&
           active_count_ < PARALLEL_PER_SOURCE &&
           try_acquire_slot_()) {
      executors_[next_idx_].start();
      ++active_count_;
      ++next_idx_;
    }
  }

  std::string source_name_;
  Database &db_;
  HttpsPool &pool_;
  SlotAcquireFunc try_acquire_slot_;
  SlotReleaseFunc release_slot_;
  DoneCallback on_done_;

  std::vector<SyncIncrementalExecutor> executors_;
  size_t next_idx_ = 0;
  int active_count_ = 0;
  int done_count_ = 0;
};
