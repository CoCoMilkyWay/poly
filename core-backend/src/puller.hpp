#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define PARALLEL_PER_SOURCE 9999              // 每个 source 内最多并行 entity 数
#define PARALLEL_TOTAL 9999                   // 全局最大并发请求数
#define GRAPHQL_BATCH_SIZE 1000               // 每次请求的 limit
#define DB_FLUSH_THRESHOLD GRAPHQL_BATCH_SIZE // 累积多少条刷入 DB
#define PULL_RETRY_DELAY_MS 50                // 初始重试延迟(ms)
#define PULL_RETRY_MAX_DELAY_MS 50            // 最大重试延迟(ms)
#define BUFFER_HARD_LIMIT 10000               // buffer 硬上限

#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "config.hpp"
#include "db.hpp"
#include "entities.hpp"
#include "entity_stats.hpp"
#include "https_pool.hpp"

using json = nlohmann::json;

class Puller;
class SourceScheduler;

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
// EntityPuller - 单个 entity 的拉取器
// ============================================================================
class EntityPuller {
public:
  EntityPuller(const std::string &subgraph_id, const std::string &source_name,
               const entities::EntityDef *entity, Database &db, HttpsPool &pool,
               SourceScheduler *scheduler)
      : source_name_(source_name), entity_(entity), db_(db), pool_(pool),
        scheduler_(scheduler), target_(graphql::build_target(subgraph_id)) {
    buffer_.reserve(DB_FLUSH_THRESHOLD);
  }

  void start();
  void on_response(const std::string &body);

  bool is_done() const { return done_; }
  const char *name() const { return entity_->name; }

private:
  void send_request();
  void flush_buffer();
  bool should_flush() const;
  void parse_indexer_errors(const json &errors, EntityStatsManager &stats);
  void finish_sync();

  std::string build_query();
  void update_cursor(const json &items);

  static std::string extract_order_value(const json &item, const char *field) {
    auto &val = item[field];
    if (val.is_null())
      return "";
    if (val.is_string())
      return val.get<std::string>();
    if (val.is_number())
      return std::to_string(val.get<int64_t>());
    return val.dump();
  }

  std::string source_name_;
  const entities::EntityDef *entity_;
  Database &db_;
  HttpsPool &pool_;
  SourceScheduler *scheduler_;
  std::string target_;

  // 游标状态
  std::string cursor_value_;
  int cursor_skip_ = 0;

  std::vector<std::string> buffer_;
  bool done_ = false;

  std::chrono::steady_clock::time_point request_start_;
  int retry_count_ = 0;
};

// ============================================================================
// SourceScheduler - 单个 source 的调度器
// ============================================================================
class SourceScheduler {
public:
  SourceScheduler(const SourceConfig &config, Database &db, HttpsPool &pool,
                  Puller *puller)
      : source_name_(config.name), db_(db), pool_(pool), puller_(puller) {
    for (const auto &entity_name : config.entities) {
      auto it = config.entity_table_map.find(entity_name);
      assert(it != config.entity_table_map.end());
      auto *e = entities::find_entity_by_table(it->second.c_str());
      assert(e && "Unknown entity table");
      pullers_.emplace_back(config.subgraph_id, source_name_, e, db_, pool_, this);
      db_.init_entity(e);

      int64_t count = db_.get_table_count(e->table);
      int64_t row_size_bytes = entities::estimate_row_size_bytes(e);
      EntityStatsManager::instance().init(source_name_, e->name, count, row_size_bytes);
    }
  }

  void start();
  void on_entity_done(EntityPuller *puller);

  bool all_done() const { return done_count_ == static_cast<int>(pullers_.size()); }
  const std::string &name() const { return source_name_; }
  int active_count() const { return active_count_; }

private:
  void start_next();

  std::string source_name_;
  Database &db_;
  HttpsPool &pool_;
  Puller *puller_;

  std::vector<EntityPuller> pullers_;
  size_t next_idx_ = 0;
  int active_count_ = 0;
  int done_count_ = 0;
};

// ============================================================================
// Puller - 全局协调器 (周期性小 sync)
// ============================================================================
class Puller {
public:
  Puller(const Config &config, Database &db, HttpsPool &pool)
      : config_(config), db_(db), pool_(pool), sync_interval_(config.sync_interval_seconds) {
    db_.init_sync_state();
    EntityStatsManager::instance().set_database(&db_);
  }

  // 启动周期 sync (不返回, 持续运行)
  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    start_sync_round();
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
        s.on_entity_done(nullptr);
        break;
      }
    }
  }

  void on_source_done() {
    ++done_source_count_;
    if (done_source_count_ < static_cast<int>(schedulers_.size()))
      return;

    std::cout << "[Puller] 本轮 sync 完成, " << sync_interval_ << "s 后开始下一轮" << std::endl;

    // 用 post 延迟, 避免在 EntityPuller 回调栈内 clear schedulers
    asio::post(*ioc_, [this]() {
      auto timer = std::make_shared<asio::steady_timer>(*ioc_);
      timer->expires_after(std::chrono::seconds(sync_interval_));
      timer->async_wait([this, timer](boost::system::error_code) {
        start_sync_round();
      });
    });
  }

private:
  void start_sync_round() {
    schedulers_.clear();
    total_active_ = 0;
    done_source_count_ = 0;

    schedulers_.reserve(config_.sources.size());
    for (const auto &src : config_.sources) {
      schedulers_.emplace_back(src, db_, pool_, this);
    }

    std::cout << "[Puller] 开始 sync, 共 " << schedulers_.size() << " 个 source" << std::endl;
    for (auto &s : schedulers_) {
      s.start();
    }
  }

  const Config &config_;
  Database &db_;
  HttpsPool &pool_;
  asio::io_context *ioc_ = nullptr;

  std::vector<SourceScheduler> schedulers_;
  int total_active_ = 0;
  int done_source_count_ = 0;
  int sync_interval_;
};

// ============================================================================
// EntityPuller 实现
// ============================================================================

inline std::string EntityPuller::build_query() {
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

  // TIMESTAMP 或 RESOLUTION_TS: 始终有 where 子句 (排除 null)
  std::string cv = cursor_value_.empty() ? "0" : cursor_value_;
  return R"({"query":"{)" + plural +
         "(first:" + limit + ",orderBy:" + entity_->order_field +
         ",orderDirection:asc,where:{" + entity_->where_field + ":" + cv +
         "},skip:" + std::to_string(cursor_skip_) + "){" +
         fields + R"(}}"})";
}

inline void EntityPuller::update_cursor(const json &items) {
  assert(!items.empty());

  if (entity_->sync_mode == entities::SyncMode::ID) {
    cursor_value_ = items.back()["id"].get<std::string>();
    cursor_skip_ = 0;
    return;
  }

  // TIMESTAMP 或 RESOLUTION_TS
  const char *order_field = entity_->order_field;
  std::string last_val = extract_order_value(items.back(), order_field);

  if (static_cast<int>(items.size()) < GRAPHQL_BATCH_SIZE) {
    // 最后一批, 直接更新
    cursor_value_ = last_val;
    cursor_skip_ = 0;
  } else if (last_val == cursor_value_) {
    // 同一 timestamp 还没翻完
    cursor_skip_ += GRAPHQL_BATCH_SIZE;
  } else {
    // 推进到新 timestamp, count trailing
    cursor_value_ = last_val;
    cursor_skip_ = 0;
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      if (extract_order_value(*it, order_field) == last_val)
        ++cursor_skip_;
      else
        break;
    }
  }
}

inline void EntityPuller::start() {
  auto cursor = db_.get_cursor(source_name_, entity_->name);
  cursor_value_ = cursor.value;
  cursor_skip_ = cursor.skip;
  EntityStatsManager::instance().start_sync(source_name_, entity_->name);

  std::cout << "[Pull] " << source_name_ << "/" << entity_->name
            << " start; cursor=" << (cursor_value_.empty() ? "(empty)" : cursor_value_.substr(0, 20) + "...")
            << " skip=" << cursor_skip_ << std::endl;
  send_request();
}

inline void EntityPuller::send_request() {
  std::string query = build_query();

  request_start_ = std::chrono::steady_clock::now();
  EntityStatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::CALLING);

  pool_.async_post(target_, query, [this](std::string body) {
    EntityStatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::PROCESSING);
    on_response(body);
  });
}

inline void EntityPuller::flush_buffer() {
  assert(!buffer_.empty());

  db_.atomic_insert_with_cursor(entity_->table, entity_->columns, buffer_,
                                source_name_, entity_->name,
                                cursor_value_, cursor_skip_);

  buffer_.clear();
}

inline bool EntityPuller::should_flush() const {
  return !buffer_.empty();
}

inline void EntityPuller::on_response(const std::string &body) {
  auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - request_start_)
                        .count();

  auto &stats = EntityStatsManager::instance();

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

  // 没有更多数据
  if (items.empty()) {
    if (should_flush())
      flush_buffer();
    finish_sync();
    return;
  }

  // 更新游标
  update_cursor(items);

  // 处理数据
  for (const auto &item : items) {
    std::string values = entity_->to_values(item);
    assert(!values.empty());
    buffer_.push_back(std::move(values));
  }

  assert(buffer_.size() <= BUFFER_HARD_LIMIT && "buffer overflow");

  if (buffer_.size() >= DB_FLUSH_THRESHOLD) {
    flush_buffer();
  }

  // 最后一批
  if (items.size() < GRAPHQL_BATCH_SIZE) {
    if (should_flush())
      flush_buffer();
    finish_sync();
    return;
  }

  send_request();
}

inline void EntityPuller::parse_indexer_errors(const json &errors, EntityStatsManager &stats) {
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

inline void EntityPuller::finish_sync() {
  EntityStatsManager::instance().end_sync(source_name_, entity_->name);
  std::cout << "[Pull] " << source_name_ << "/" << entity_->name << " done" << std::endl;
  done_ = true;
  scheduler_->on_entity_done(this);
}

// ============================================================================
// SourceScheduler 实现
// ============================================================================

inline void SourceScheduler::start() {
  std::cout << "[Scheduler] " << source_name_ << " start, " << pullers_.size() << " entities" << std::endl;
  start_next();
}

inline void SourceScheduler::on_entity_done(EntityPuller *puller) {
  if (puller) {
    assert(puller->is_done());
    ++done_count_;
    --active_count_;
    puller_->release_slot();
  }

  if (all_done()) {
    puller_->on_source_done();
    return;
  }

  start_next();
}

inline void SourceScheduler::start_next() {
  while (next_idx_ < pullers_.size() &&
         active_count_ < PARALLEL_PER_SOURCE &&
         puller_->try_acquire_slot()) {
    pullers_[next_idx_].start();
    ++active_count_;
    ++next_idx_;
  }
}
