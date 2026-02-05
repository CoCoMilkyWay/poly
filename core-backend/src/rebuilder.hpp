#pragma once

// ============================================================================
// PnL Rebuilder 核心
// ============================================================================

#include "rebuilder_loader.hpp"
#include "rebuilder_types.hpp"
#include "rebuilder_worker.hpp"

#include <atomic>
#include <duckdb.hpp>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace rebuilder {

class Rebuilder {
public:
  explicit Rebuilder(duckdb::DuckDB &db) : db_(db) {}

  // 重建单个用户
  UserResult rebuild_user(const std::string &user) {
    auto conn = std::make_unique<duckdb::Connection>(db_);
    Loader loader(*conn);

    auto configs = loader.load_conditions();
    auto events = loader.load_user_events(user);

    return Worker::process_user(user, events, configs);
  }

  // 全量重建（并行）
  std::vector<UserResult> rebuild_all() {
    progress_.running = true;
    progress_.error.clear();

    auto conn = std::make_unique<duckdb::Connection>(db_);
    Loader loader(*conn);

    // 加载配置（共享）
    std::cout << "[Rebuilder] Loading conditions..." << std::endl;
    auto configs = loader.load_conditions();
    std::cout << "[Rebuilder] Loaded " << configs.size() << " conditions" << std::endl;

    // 获取所有用户
    std::cout << "[Rebuilder] Loading users..." << std::endl;
    auto users = loader.get_all_users();
    progress_.total_users = users.size();
    std::cout << "[Rebuilder] Found " << users.size() << " users" << std::endl;

    // 统计总事件数
    progress_.total_events = loader.count_total_events();
    std::cout << "[Rebuilder] Total events: " << progress_.total_events << std::endl;

    // 并行处理
    int num_workers = std::max(1u, std::thread::hardware_concurrency());
    std::cout << "[Rebuilder] Using " << num_workers << " workers" << std::endl;

    std::vector<std::future<std::vector<UserResult>>> futures;
    size_t chunk_size = (users.size() + num_workers - 1) / num_workers;

    for (int i = 0; i < num_workers; ++i) {
      size_t start = i * chunk_size;
      size_t end = std::min(start + chunk_size, users.size());
      if (start >= users.size())
        break;

      std::vector<std::string> user_slice(users.begin() + start, users.begin() + end);

      futures.push_back(std::async(std::launch::async, [this, user_slice, &configs]() {
        return process_user_batch(user_slice, configs);
      }));
    }

    // 收集结果
    std::vector<UserResult> all_results;
    for (auto &f : futures) {
      auto batch = f.get();
      all_results.insert(all_results.end(),
                         std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
    }

    progress_.running = false;
    std::cout << "[Rebuilder] Completed " << all_results.size() << " users" << std::endl;

    return all_results;
  }

  // 获取进度
  RebuildProgress get_progress() const {
    return progress_;
  }

  // UserResult 转 JSON
  static json result_to_json(const UserResult &r, bool include_snapshots = true) {
    json positions = json::object();
    for (const auto &[token_id, pos] : r.positions) {
      positions[token_id] = {
          {"amount", pos.amount},
          {"avg_price", pos.avg_price},
          {"realized_pnl", pos.realized_pnl},
          {"total_bought", pos.total_bought}};
    }

    json neg_amounts = json::array();
    for (const auto &[tid, ts, amt] : r.meta.negative_amounts) {
      neg_amounts.push_back({{"token_id", tid}, {"timestamp", ts}, {"amount", amt}});
    }

    json result = {
        {"user", r.user},
        {"meta",
         {{"first_ts", r.meta.first_ts},
          {"last_ts", r.meta.last_ts},
          {"order_count", r.meta.order_count},
          {"merge_count", r.meta.merge_count},
          {"redemption_count", r.meta.redemption_count},
          {"skipped_count", r.meta.skipped_count},
          {"missing_conditions", r.meta.missing_conditions},
          {"unresolved_redemptions", r.meta.unresolved_redemptions},
          {"negative_amounts", neg_amounts}}},
        {"positions", positions},
        {"snapshot_count", r.snapshots.size()}};

    // 包含 snapshots 详情（用于前端时间线）
    if (include_snapshots && !r.snapshots.empty()) {
      json snapshots = json::array();
      for (const auto &snap : r.snapshots) {
        snapshots.push_back({
            {"timestamp", snap.timestamp},
            {"event_type", snap.event_type},
            {"event_id", snap.event_id},
            {"token_id", snap.token_id},
            {"side", snap.side},
            {"size", snap.size},
            {"price", snap.price},
            {"total_pnl", snap.total_pnl}});
      }
      result["snapshots"] = std::move(snapshots);
    }

    return result;
  }

private:
  // 处理一批用户
  std::vector<UserResult> process_user_batch(
      const std::vector<std::string> &users,
      const std::unordered_map<std::string, ConditionConfig> &configs) {
    // 每个 worker 创建自己的连接
    auto conn = std::make_unique<duckdb::Connection>(db_);
    Loader loader(*conn);

    std::vector<UserResult> results;
    results.reserve(users.size());

    for (const auto &user : users) {
      auto events = loader.load_user_events(user);
      auto result = Worker::process_user(user, events, configs);
      results.push_back(std::move(result));

      // 更新进度
      progress_.processed_users.fetch_add(1);
      progress_.processed_events.fetch_add(events.size());
    }

    return results;
  }

  duckdb::DuckDB &db_;

  // 进度跟踪（原子操作）
  struct AtomicProgress {
    std::atomic<int64_t> total_users{0};
    std::atomic<int64_t> processed_users{0};
    std::atomic<int64_t> total_events{0};
    std::atomic<int64_t> processed_events{0};
    std::atomic<bool> running{false};
    std::string error;

    operator RebuildProgress() const {
      return {
          total_users.load(),
          processed_users.load(),
          total_events.load(),
          processed_events.load(),
          running.load(),
          error};
    }
  } progress_;
};

} // namespace rebuilder
