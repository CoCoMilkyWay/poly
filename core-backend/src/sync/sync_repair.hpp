#pragma once

// ============================================================================
// 大sync - 修复数据完整性
// Phase 1: 删除 null timestamp conditions, 全量重拉
// Phase 2: 合并 pnl_condition → condition.positionIds
// Phase 3: 填充剩余 null positionIds
// ============================================================================

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "../core/entity_definition.hpp"
#include "../infra/https_pool.hpp"
#include "sync_incremental_executor.hpp"

using json = nlohmann::json;

class SyncRepair {
public:
  struct Progress {
    bool running = false;
    std::string phase;
    int64_t total = 0;
    int64_t processed = 0;
    std::string error;
  };

  SyncRepair(Database &db, HttpsPool &pool, const Config &config)
      : db_(db), pool_(pool) {
    // 找 Polymarket source (拉 Condition)
    for (const auto &src : config.sources) {
      for (const auto &ent : src.entities) {
        auto it = src.entity_table_map.find(ent);
        if (it != src.entity_table_map.end() && it->second == "condition") {
          polymarket_subgraph_id_ = src.subgraph_id;
        }
        if (it != src.entity_table_map.end() && it->second == "pnl_condition") {
          pnl_subgraph_id_ = src.subgraph_id;
        }
      }
    }
    assert(!polymarket_subgraph_id_.empty() && "Polymarket source not found");
    assert(!pnl_subgraph_id_.empty() && "PnL source not found");
  }

  bool start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
      return false;

    progress_mutex_.lock();
    progress_ = {true, "starting", 0, 0, ""};
    progress_mutex_.unlock();

    std::thread([this]() {
      run();
    }).detach();
    return true;
  }

  bool is_running() const { return running_; }

  Progress get_progress() const {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    return progress_;
  }

private:
  void set_progress(const std::string &phase, int64_t total = 0, int64_t processed = 0) {
    std::lock_guard<std::mutex> lock(progress_mutex_);
    progress_.phase = phase;
    progress_.total = total;
    progress_.processed = processed;
  }

  void run() {
    std::cout << "[SyncRepair] 开始" << std::endl;

    // Phase 1: 重拉 conditions
    set_progress("phase1_delete_null");
    std::cout << "[SyncRepair] Phase 1: 删除 null timestamp conditions" << std::endl;
    db_.delete_null_conditions();

    set_progress("phase1_repull");
    std::cout << "[SyncRepair] Phase 1: 全量重拉 conditions" << std::endl;
    phase1_repull_conditions();

    // Phase 2: 合并 pnl_condition
    set_progress("phase2_merge_pnl");
    std::cout << "[SyncRepair] Phase 2: 合并 pnl_condition → condition" << std::endl;
    if (db_.table_exists("pnl_condition")) {
      db_.merge_pnl_into_condition();
      db_.drop_pnl_condition();
      std::cout << "[SyncRepair] Phase 2: pnl_condition 已合并并删除" << std::endl;
    } else {
      std::cout << "[SyncRepair] Phase 2: pnl_condition 不存在, 跳过" << std::endl;
    }

    // Phase 3: 填充 missing positionIds
    set_progress("phase3_fill_positionids");
    std::cout << "[SyncRepair] Phase 3: 填充 missing positionIds" << std::endl;
    phase3_fill_position_ids();

    set_progress("done");
    std::cout << "[SyncRepair] 完成" << std::endl;
    running_ = false;
  }

  // Phase 1: 用 id_gt 全量拉 Condition (同步阻塞)
  void phase1_repull_conditions() {
    std::string target = "/api/subgraphs/id/" + polymarket_subgraph_id_;
    std::string cursor;
    int64_t total_pulled = 0;

    while (true) {
      std::string query;
      if (cursor.empty()) {
        query = R"({"query":"{conditions(first:1000,orderBy:id,orderDirection:asc){)" +
                std::string(entities::Condition.fields) + R"(}}"})";
      } else {
        query = R"({"query":"{conditions(first:1000,orderBy:id,orderDirection:asc,where:{id_gt:\")" +
                graphql::escape_json(cursor) + R"(\"}){)" +
                std::string(entities::Condition.fields) + R"(}}"})";
      }

      // 同步等待异步请求
      std::string response = sync_request(target, query);
      assert(!response.empty() && "SyncRepair: network failure");

      json j = json::parse(response);
      assert(!j.contains("errors") && "SyncRepair: GraphQL error");
      assert(j.contains("data") && j["data"].contains("conditions"));

      auto &items = j["data"]["conditions"];
      if (items.empty())
        break;

      // 写入 DB
      std::vector<std::string> values_list;
      values_list.reserve(items.size());
      for (const auto &item : items) {
        values_list.push_back(entities::condition_to_values(item));
      }

      cursor = items.back()["id"].get<std::string>();

      db_.upsert_batch("condition", entities::Condition.columns, values_list);

      total_pulled += items.size();
      set_progress("phase1_repull", 0, total_pulled);

      if (items.size() < 1000)
        break;
    }

    std::cout << "[SyncRepair] Phase 1: 拉取 " << total_pulled << " 条 conditions" << std::endl;
  }

  // Phase 3: 批量查 PnL subgraph 填 missing positionIds
  void phase3_fill_position_ids() {
    std::string target = "/api/subgraphs/id/" + pnl_subgraph_id_;
    int64_t total_filled = 0;

    while (true) {
      auto ids = db_.get_null_positionid_condition_ids(100);
      if (ids.empty())
        break;

      set_progress("phase3_fill_positionids", 0, total_filled);

      // 构建 id_in 查询
      std::string id_list;
      for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0)
          id_list += ",";
        id_list += "\\\"" + graphql::escape_json(ids[i]) + "\\\"";
      }

      std::string query = R"({"query":"{conditions(first:100,where:{id_in:[)" +
                          id_list + R"(]}){id positionIds}}"})";

      std::string response = sync_request(target, query);
      if (response.empty()) {
        std::cerr << "[SyncRepair] Phase 3: network failure, retrying..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      json j;
      try {
        j = json::parse(response);
      } catch (...) {
        std::cerr << "[SyncRepair] Phase 3: JSON parse failure" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      if (j.contains("errors") || !j.contains("data") || !j["data"].contains("conditions")) {
        std::cerr << "[SyncRepair] Phase 3: GraphQL error" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      auto &items = j["data"]["conditions"];
      for (const auto &item : items) {
        std::string id = item["id"].get<std::string>();
        std::string position_ids = item.contains("positionIds") && !item["positionIds"].is_null()
                                       ? item["positionIds"].dump()
                                       : "";
        if (!position_ids.empty()) {
          db_.update_condition_position_ids(id, position_ids);
          ++total_filled;
        }
      }

      // 如果 PnL 没有返回某些 id, 说明 PnL subgraph 也没有这些数据
      // 标记为空数组避免无限循环
      std::set<std::string> found_ids;
      for (const auto &item : items) {
        found_ids.insert(item["id"].get<std::string>());
      }
      for (const auto &id : ids) {
        if (found_ids.find(id) == found_ids.end()) {
          db_.update_condition_position_ids(id, "[]");
        }
      }
    }

    std::cout << "[SyncRepair] Phase 3: 填充 " << total_filled << " 条 positionIds" << std::endl;
  }

  // 同步等待异步请求 (在后台线程中使用)
  std::string sync_request(const std::string &target, const std::string &query) {
    std::mutex mtx;
    std::condition_variable cv;
    std::string result;
    bool done = false;

    pool_.async_post(target, query, [&](std::string body) {
      std::lock_guard<std::mutex> lock(mtx);
      result = std::move(body);
      done = true;
      cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&] { return done; });
    return result;
  }

  Database &db_;
  HttpsPool &pool_;
  std::string polymarket_subgraph_id_;
  std::string pnl_subgraph_id_;

  std::atomic<bool> running_{false};
  mutable std::mutex progress_mutex_;
  Progress progress_;
};
