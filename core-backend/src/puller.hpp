#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define PARALLEL_PER_SOURCE 9999      // 每个 source 内最多并行 entity 数
#define PARALLEL_TOTAL 9999           // 全局最大并发请求数
#define GRAPHQL_BATCH_SIZE 1000       // 每次请求的 limit
#define DB_FLUSH_THRESHOLD 1000       // 累积多少条刷入 DB
#define PULL_RETRY_DELAY_MS 100       // 初始重试延迟(ms)
#define PULL_RETRY_MAX_DELAY_MS 30000 // 最大重试延迟(ms)
#define BUFFER_HARD_LIMIT 10000       // buffer 硬上限，超过则 assert

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

// 前向声明
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
    build_query_cache_();
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

  std::string source_name_;
  const entities::EntityDef *entity_;
  Database &db_;
  HttpsPool &pool_;
  SourceScheduler *scheduler_;
  std::string target_;

  // 状态
  std::string cursor_;
  std::vector<std::string> buffer_;
  bool done_ = false;

  // 请求计时
  std::chrono::steady_clock::time_point request_start_;

  // 重试指数退避
  int retry_count_ = 0;

  // GraphQL query 缓存
  std::string query_initial_;
  std::string query_cursor_prefix_;
  std::string query_cursor_suffix_;

  void build_query_cache_() {
    const std::string limit_str = std::to_string(GRAPHQL_BATCH_SIZE);

    // cursor 为空时的完整 query
    query_initial_ = R"({"query":"query Q($limit:Int!){)" +
                     std::string(entity_->plural) +
                     R"((first:$limit,orderBy:id,orderDirection:asc){)" +
                     entity_->fields +
                     R"(}}","variables":{"limit":)" + limit_str + "}}";

    // cursor 非空时：prefix + escaped(cursor) + suffix
    query_cursor_prefix_ = R"({"query":"query Q($limit:Int!){)" +
                           std::string(entity_->plural) +
                           R"((first:$limit,orderBy:id,orderDirection:asc,where:{id_gt:\")";

    query_cursor_suffix_ = R"(\"}){)" + std::string(entity_->fields) +
                           R"(}}","variables":{"limit":)" + limit_str + "}}";
  }
};

// ============================================================================
// SourceScheduler - 单个 source 的调度器
// ============================================================================
class SourceScheduler {
public:
  SourceScheduler(const SourceConfig &config, Database &db, HttpsPool &pool,
                  Puller *puller, bool is_pnl)
      : source_name_(config.name), db_(db), pool_(pool), puller_(puller) {
    auto entity_list = is_pnl ? entities::PNL_ENTITIES : entities::MAIN_ENTITIES;
    auto entity_count = is_pnl ? entities::PNL_ENTITY_COUNT : entities::MAIN_ENTITY_COUNT;

    for (const auto &name : config.entities) {
      auto *e = entities::find_entity(name.c_str(), entity_list, entity_count);
      assert(e && "Unknown entity");
      pullers_.emplace_back(config.subgraph_id, source_name_, e, db_, pool_, this);
      db_.init_entity(e);

      // 初始化 entity stats
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
// Puller - 全局协调器
// ============================================================================
class Puller {
public:
  Puller(const Config &config, Database &db, HttpsPool &pool)
      : db_(db), pool_(pool) {
    db_.init_sync_state();
    EntityStatsManager::instance().set_database(&db_);

    // 预分配空间，避免向量重新分配导致 scheduler_ 指针失效
    schedulers_.reserve(config.sources.size());
    for (const auto &src : config.sources) {
      bool is_pnl = std::any_of(src.entities.begin(), src.entities.end(),
                                [](const std::string &e) { return e == "UserPosition"; });
      schedulers_.emplace_back(src, db_, pool_, this, is_pnl);
    }
  }

  void run(asio::io_context &ioc) {
    std::cout << "[Puller] 启动，共 " << schedulers_.size() << " 个 source" << std::endl;
    for (auto &s : schedulers_) {
      s.start();
    }
    ioc.run();
    std::cout << "[Puller] 所有 source 同步完成" << std::endl;
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

private:
  Database &db_;
  HttpsPool &pool_;
  std::vector<SourceScheduler> schedulers_;
  int total_active_ = 0;
};

// ============================================================================
// EntityPuller 实现
// ============================================================================

inline void EntityPuller::start() {
  cursor_ = db_.get_cursor(source_name_, entity_->name);
  EntityStatsManager::instance().start_sync(source_name_, entity_->name);

  std::cout << "[Pull] " << source_name_ << "/" << entity_->name
            << " cursor=" << (cursor_.empty() ? "(empty)" : cursor_.substr(0, 20) + "...")
            << std::endl;
  send_request();
}

inline void EntityPuller::send_request() {
  std::string query = cursor_.empty()
                          ? query_initial_
                          : query_cursor_prefix_ + graphql::escape_json(cursor_) + query_cursor_suffix_;

  request_start_ = std::chrono::steady_clock::now();
  EntityStatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::CALLING);

  pool_.async_post(target_, query, [this](std::string body) {
    EntityStatsManager::instance().set_api_state(source_name_, entity_->name, ApiState::PROCESSING);
    on_response(body);
  });
}

inline void EntityPuller::flush_buffer() {
  assert(!buffer_.empty());
  assert(!cursor_.empty()); // cursor 必须在数据处理时被设置

  db_.atomic_insert_with_cursor(entity_->table, entity_->columns, buffer_,
                                source_name_, entity_->name, cursor_);

  std::cout << "[Pull] " << source_name_ << "/" << entity_->name
            << " flush " << buffer_.size() << " rows" << std::endl;

  buffer_.clear();
}

inline bool EntityPuller::should_flush() const {
  return !buffer_.empty() && !cursor_.empty();
}

inline void EntityPuller::on_response(const std::string &body) {
  auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - request_start_)
                        .count();

  auto &stats = EntityStatsManager::instance();

  // 计算重试延迟（指数退避）
  auto calc_retry_delay = [this]() {
    int delay = PULL_RETRY_DELAY_MS * (1 << std::min(retry_count_, 10)); // 防止溢出
    return std::min(delay, PULL_RETRY_MAX_DELAY_MS);
  };

  // 请求失败
  if (body.empty()) {
    stats.record_failure(source_name_, entity_->name, FailureKind::NETWORK, latency_ms);
    int delay = calc_retry_delay();
    std::cerr << "[Pull] " << entity_->name << " network fail, retry in " << delay << "ms" << std::endl;
    ++retry_count_;
    pool_.schedule_retry([this]() { send_request(); }, delay);
    return;
  }

  // JSON 解析
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

  // GraphQL 错误
  if (j.contains("errors")) {
    stats.record_failure(source_name_, entity_->name, FailureKind::GRAPHQL, latency_ms);
    parse_indexer_errors(j["errors"], stats);
    int delay = calc_retry_delay();
    std::cerr << "[Pull] " << entity_->name << " GraphQL error, retry in " << delay << "ms" << std::endl;
    ++retry_count_;
    pool_.schedule_retry([this]() { send_request(); }, delay);
    return;
  }

  // 响应格式错误
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

  // 成功，重置重试计数
  retry_count_ = 0;

  // 没有更多数据 - 完成
  if (items.empty()) {
    if (should_flush())
      flush_buffer();
    finish_sync();
    return;
  }

  // 处理数据：先更新 cursor，再添加到 buffer
  cursor_ = items.back()["id"].get<std::string>();
  for (const auto &item : items) {
    std::string values = entity_->to_values(item);
    assert(!values.empty());
    buffer_.push_back(std::move(values));
  }

  // 内存安全检查：buffer 不能无限增长
  assert(buffer_.size() <= BUFFER_HARD_LIMIT && "buffer overflow, check flush logic");

  // 达到阈值则 flush
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
      // trim
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
