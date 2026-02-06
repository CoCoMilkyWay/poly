#pragma once

// ============================================================================
// 小sync - Entity执行器（最内层，无外部依赖）
// ============================================================================

#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "../core/database.hpp"
#include "../core/entity_definition.hpp"
#include "../infra/https_pool.hpp"
#include "../stats/stats_manager.hpp"

using json = nlohmann::json;

// ============================================================================
// GraphQL 工具
// ============================================================================
namespace graphql {

inline std::string escape_json(const std::string &s) {
  std::string result;
  result.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"':
      result += "\\\"";
      break;
    case '\\':
      result += "\\\\";
      break;
    case '\n':
      result += "\\n";
      break;
    default:
      result += c;
      break;
    }
  }
  return result;
}

inline std::string build_target(const std::string &subgraph_id) {
  return "/api/subgraphs/id/" + subgraph_id;
}

} // namespace graphql

// ============================================================================
// 宏配置
// ============================================================================
#define GRAPHQL_BATCH_SIZE 1000
#define DB_FLUSH_THRESHOLD GRAPHQL_BATCH_SIZE
#define PULL_RETRY_DELAY_MS 50
#define PULL_RETRY_MAX_DELAY_MS 50
#define BUFFER_HARD_LIMIT 10000

// ============================================================================
// SyncIncrementalExecutor - 单个 entity 的拉取执行器
// ============================================================================
class SyncIncrementalExecutor {
public:
  using DoneCallback = std::function<void()>;

  SyncIncrementalExecutor(const std::string &subgraph_id, const std::string &source_name,
                          const entities::EntityDef *entity, Database &db, HttpsPool &pool,
                          DoneCallback on_done)
      : source_name_(source_name), entity_(entity), db_(db), pool_(pool),
        on_done_(std::move(on_done)), target_(graphql::build_target(subgraph_id)) {
    buffer_.reserve(DB_FLUSH_THRESHOLD);
  }

  void start() {
    auto cursor = db_.get_cursor(source_name_, entity_->name);
    cursor_value_ = cursor.value;
    cursor_skip_ = cursor.skip;
    StatsManager::instance().start_sync(source_name_, entity_->name);

    std::cout << "[Pull] " << source_name_ << "/" << entity_->name
              << " start; cursor=" << (cursor_value_.empty() ? "(empty)" : cursor_value_.substr(0, 20) + "...")
              << " skip=" << cursor_skip_ << std::endl;
    send_request();
  }

  bool is_done() const { return done_; }
  const char *name() const { return entity_->name; }

private:
  void send_request() {
    std::string query = build_query();
    request_start_ = std::chrono::steady_clock::now();
    StatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::CALLING);

    pool_.async_post(target_, query, [this](std::string body) {
      StatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::PROCESSING);
      on_response(body);
    });
  }

  void on_response(const std::string &body) {
    auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - request_start_)
                          .count();

    auto &stats = StatsManager::instance();

    auto calc_retry_delay = [this]() {
      int delay = PULL_RETRY_DELAY_MS * (1 << std::min(retry_count_, 10));
      return std::min(delay, PULL_RETRY_MAX_DELAY_MS);
    };

    if (body.empty()) {
      stats.record_failure(source_name_, entity_->name, FailureKind::NETWORK, latency_ms);
      int delay = calc_retry_delay();
      std::cerr << "[Pull] " << entity_->name << " network fail, retry in " << delay << "ms" << std::endl;
      ++retry_count_;
      pool_.schedule_retry([this]() { send_request(); }, delay);
      return;
    }

    json j;
    try {
      j = json::parse(body);
    } catch (...) {
      stats.record_failure(source_name_, entity_->name, FailureKind::JSON, latency_ms);
      int delay = calc_retry_delay();
      std::cerr << "[Pull] " << entity_->name << " JSON parse fail, retry in " << delay << "ms" << std::endl;
      ++retry_count_;
      pool_.schedule_retry([this]() { send_request(); }, delay);
      return;
    }

    if (j.contains("errors")) {
      stats.record_failure(source_name_, entity_->name, FailureKind::GRAPHQL, latency_ms);
      parse_indexer_errors(j["errors"], stats);
      int delay = calc_retry_delay();
      std::cerr << "[Pull] " << entity_->name << " GraphQL error, retry in " << delay << "ms" << std::endl;
      ++retry_count_;
      pool_.schedule_retry([this]() { send_request(); }, delay);
      return;
    }

    if (!j.contains("data") || !j["data"].contains(entity_->plural)) {
      stats.record_failure(source_name_, entity_->name, FailureKind::FORMAT, latency_ms);
      int delay = calc_retry_delay();
      std::cerr << "[Pull] " << entity_->name << " format error, retry in " << delay << "ms" << std::endl;
      ++retry_count_;
      pool_.schedule_retry([this]() { send_request(); }, delay);
      return;
    }

    auto &items = j["data"][entity_->plural];
    stats.record_success(source_name_, entity_->name, items.size(), latency_ms);
    retry_count_ = 0;

    if (items.empty()) {
      if (!buffer_.empty())
        flush_buffer();
      finish_sync();
      return;
    }

    update_cursor(items);

    for (const auto &item : items) {
      std::string values = entity_->to_values(item);
      assert(!values.empty());
      buffer_.push_back(std::move(values));
    }

    assert(buffer_.size() <= BUFFER_HARD_LIMIT && "buffer overflow");

    if (buffer_.size() >= DB_FLUSH_THRESHOLD) {
      flush_buffer();
    }

    if (items.size() < GRAPHQL_BATCH_SIZE) {
      if (!buffer_.empty())
        flush_buffer();
      finish_sync();
      return;
    }

    send_request();
  }

  std::string build_query() {
    std::string limit = std::to_string(GRAPHQL_BATCH_SIZE);
    std::string plural = entity_->plural;
    std::string fields = entity_->fields;

    if (entity_->sync_mode == entities::SyncMode::ID) {
      if (cursor_value_.empty()) {
        return R"({"query":"{)" + plural +
               "(first:" + limit + ",orderBy:id,orderDirection:asc){" +
               fields + R"(}}"})";
      }
      return R"({"query":"{)" + plural +
             R"((first:)" + limit + R"(,orderBy:id,orderDirection:asc,where:{id_gt:\")" +
             graphql::escape_json(cursor_value_) + R"(\"}){)" +
             fields + R"(}}"})";
    }

    std::string cv = cursor_value_.empty() ? "0" : cursor_value_;
    return R"({"query":"{)" + plural +
           "(first:" + limit + ",orderBy:" + entity_->order_field +
           ",orderDirection:asc,where:{" + entity_->where_field + ":" + cv +
           "},skip:" + std::to_string(cursor_skip_) + "){" +
           fields + R"(}}"})";
  }

  void update_cursor(const json &items) {
    assert(!items.empty());

    if (entity_->sync_mode == entities::SyncMode::ID) {
      cursor_value_ = items.back()["id"].get<std::string>();
      cursor_skip_ = 0;
      return;
    }

    const char *order_field = entity_->order_field;
    auto extract_value = [&](const json &item) -> std::string {
      auto &val = item[order_field];
      if (val.is_null())
        return "";
      if (val.is_string())
        return val.get<std::string>();
      if (val.is_number())
        return std::to_string(val.get<int64_t>());
      return val.dump();
    };

    std::string last_val = extract_value(items.back());

    if (static_cast<int>(items.size()) < GRAPHQL_BATCH_SIZE) {
      cursor_value_ = last_val;
      cursor_skip_ = 0;
    } else if (last_val == cursor_value_) {
      cursor_skip_ += GRAPHQL_BATCH_SIZE;
    } else {
      cursor_value_ = last_val;
      cursor_skip_ = 0;
      for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (extract_value(*it) == last_val)
          ++cursor_skip_;
        else
          break;
      }
    }
  }

  void flush_buffer() {
    assert(!buffer_.empty());
    db_.atomic_insert_with_cursor(entity_->table, entity_->columns, buffer_,
                                  source_name_, entity_->name,
                                  cursor_value_, cursor_skip_);
    buffer_.clear();
  }

  void parse_indexer_errors(const json &errors, StatsManager &stats) {
    if (!errors.is_array())
      return;
    for (auto &err : errors) {
      if (!err.contains("message") || !err["message"].is_string())
        continue;
      const std::string msg = err["message"].get<std::string>();
      auto p = msg.find("bad indexers:");
      if (p == std::string::npos)
        continue;
      auto lb = msg.find('{', p);
      auto rb = msg.find('}', lb);
      if (lb == std::string::npos || rb == std::string::npos || rb <= lb)
        continue;
      std::string inside = msg.substr(lb + 1, rb - lb - 1);
      size_t pos = 0;
      while (pos < inside.size()) {
        auto comma = inside.find(',', pos);
        std::string part = (comma == std::string::npos) ? inside.substr(pos) : inside.substr(pos, comma - pos);
        pos = (comma == std::string::npos) ? inside.size() : comma + 1;
        auto colon = part.find(':');
        if (colon == std::string::npos)
          continue;
        std::string indexer = part.substr(0, colon);
        std::string reason = part.substr(colon + 1);
        while (!indexer.empty() && indexer.front() == ' ')
          indexer.erase(0, 1);
        while (!indexer.empty() && indexer.back() == ' ')
          indexer.pop_back();
        if (!indexer.empty() && reason.find("BadResponse") != std::string::npos) {
          stats.record_indexer_fail(source_name_, entity_->name, indexer);
        }
      }
    }
  }

  void finish_sync() {
    StatsManager::instance().end_sync(source_name_, entity_->name);
    std::cout << "[Pull] " << source_name_ << "/" << entity_->name << " done" << std::endl;
    done_ = true;
    on_done_();
  }

  std::string source_name_;
  const entities::EntityDef *entity_;
  Database &db_;
  HttpsPool &pool_;
  DoneCallback on_done_;
  std::string target_;

  std::string cursor_value_;
  int cursor_skip_ = 0;
  std::vector<std::string> buffer_;
  bool done_ = false;
  std::chrono::steady_clock::time_point request_start_;
  int retry_count_ = 0;
};
