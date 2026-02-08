#pragma once

// ============================================================================
// Token ID Filler - 填充 condition.positionIds
// 1. bulk merge pnl_condition → condition (幂等)
// 2. 按 resolutionTimestamp 顺序逐批查 PnL subgraph 填充剩余 NULL
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
#include "../infra/https_pool.hpp"
#include "sync_incremental_executor.hpp" // for graphql::escape_json, graphql::build_target

using json = nlohmann::json;

class SyncTokenFiller {
public:
  SyncTokenFiller(Database &db, HttpsPool &pool, const Config &config)
      : db_(db), pool_(pool) {
    for (const auto &src : config.sources) {
      for (const auto &ent : src.entities) {
        auto it = src.entity_table_map.find(ent);
        if (it != src.entity_table_map.end() && it->second == "pnl_condition") {
          pnl_target_ = graphql::build_target(src.subgraph_id);
        }
      }
    }
    assert(!pnl_target_.empty() && "PnL source not found in config");
  }

  std::string start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
      return "already_running";
    processed_ = 0;
    merged_ = 0;
    not_found_ = 0;
    errors_ = 0;
    total_null_ = 0;
    phase_ = 0;
    start_ts_ = 0;
    std::thread([this]() { run(); }).detach();
    return "started";
  }

  bool is_running() const { return running_; }
  int64_t processed() const { return processed_; }
  int phase() const { return phase_; }
  int64_t total_null() const { return total_null_; }
  int64_t merged() const { return merged_; }
  int64_t not_found() const { return not_found_; }
  int64_t errors() const { return errors_; }
  int64_t start_ts() const { return start_ts_; }

private:
  void run() {
    start_ts_ = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
    total_null_ = db_.query_single_int(
        "SELECT COUNT(*) FROM condition WHERE positionIds IS NULL");
    std::cout << "[TokenFiller] 开始, " << total_null_ << " NULL rows" << std::endl;

    // Phase 1: bulk merge
    phase_ = 1;
    std::cout << "[TokenFiller] Phase 1: bulk merge pnl_condition → condition" << std::endl;
    db_.merge_pnl_into_condition();
    int64_t after_merge = db_.query_single_int(
        "SELECT COUNT(*) FROM condition WHERE positionIds IS NULL");
    merged_ = total_null_ - after_merge;
    std::cout << "[TokenFiller] Phase 1 done: merged " << merged_
              << ", remaining " << after_merge << std::endl;

    // Phase 2: 逐批填充剩余 NULL
    phase_ = 2;
    std::cout << "[TokenFiller] Phase 2: 填充剩余 " << after_merge << " NULL positionIds" << std::endl;
    while (true) {
      auto ids = db_.get_null_positionid_conditions(100);
      if (ids.empty())
        break;

      // 构建 id_in 查询
      std::string id_list;
      for (size_t i = 0; i < ids.size(); ++i) {
        if (i > 0)
          id_list += ",";
        id_list += "\\\"" + graphql::escape_json(ids[i]) + "\\\"";
      }

      std::string query = R"({"query":"{conditions(first:100,where:{id_in:[)" +
                          id_list + R"(]}){id positionIds}}"})";

      std::string response = sync_request(query);
      if (response.empty()) {
        ++errors_;
        std::cerr << "[TokenFiller] network failure, retrying..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      json j;
      try {
        j = json::parse(response);
      } catch (...) {
        ++errors_;
        std::cerr << "[TokenFiller] JSON parse failure, retrying..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      if (j.contains("errors") || !j.contains("data") || !j["data"].contains("conditions")) {
        ++errors_;
        std::cerr << "[TokenFiller] GraphQL error, retrying..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }

      auto &items = j["data"]["conditions"];

      // 更新找到的
      std::set<std::string> found_ids;
      for (const auto &item : items) {
        std::string id = item["id"].get<std::string>();
        found_ids.insert(id);
        if (item.contains("positionIds") && !item["positionIds"].is_null()) {
          db_.update_condition_position_ids(id, item["positionIds"].dump());
          ++processed_;
        }
      }

      // PnL subgraph 也没有的 → 标记空数组，防死循环
      for (const auto &id : ids) {
        if (found_ids.find(id) == found_ids.end()) {
          db_.update_condition_position_ids(id, "[]");
          ++not_found_;
        }
      }
    }

    std::cout << "[TokenFiller] 完成, 填充 " << processed_
              << ", 合并 " << merged_
              << ", 未找到 " << not_found_
              << ", 错误 " << errors_ << std::endl;
    phase_ = 0;
    running_ = false;
  }

  // 同步等待异步请求
  std::string sync_request(const std::string &query) {
    std::mutex mtx;
    std::condition_variable cv;
    std::string result;
    bool done = false;

    pool_.async_post(pnl_target_, query, [&](std::string body) {
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
  std::string pnl_target_;

  std::atomic<bool> running_{false};
  std::atomic<int64_t> processed_{0};
  std::atomic<int> phase_{0};        // 0=idle, 1=merge, 2=fill
  std::atomic<int64_t> total_null_{0};
  std::atomic<int64_t> merged_{0};
  std::atomic<int64_t> not_found_{0};
  std::atomic<int64_t> errors_{0};
  int64_t start_ts_{0};
};
