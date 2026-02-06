#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

// 前向声明
class Database;

// ============================================================================
// API 状态枚举
// ============================================================================
enum class ApiState {
  IDLE,      // 空闲
  CALLING,   // API 调用中
  PROCESSING // 本地解析/backoff
};

// ============================================================================
// 失败分类(用于 hover 展示)
// ============================================================================
enum class FailureKind : uint8_t {
  NETWORK = 0, // body empty / 网络错误
  JSON = 1,    // JSON parse 失败
  GRAPHQL = 2, // 响应包含 errors
  FORMAT = 3,  // 响应结构不符合预期
};

// ============================================================================
// 单个 Entity 的实时统计
// ============================================================================
struct EntityStat {
  std::string source;
  std::string entity;

  // 记录数(从 DB 初始化，之后累加)
  int64_t count = 0;

  // 估算：单条记录的"结构体大小"(字节)，用于展示 DB 规模
  int64_t row_size_bytes = 0;

  // 速度统计(历史累积，持久化到DB)
  int64_t total_rows_synced = 0; // 历史总同步行数
  int64_t total_api_time_ms = 0; // 历史总API调用时间(不含本地处理)

  // QoS 统计
  std::deque<int64_t> recent_latencies; // 最近20个请求的延时(不持久化)
  double success_rate = 100.0;          // 持久化到DB
  int64_t total_requests = 0;           // 历史总请求数(持久化)
  int64_t success_requests = 0;         // 历史成功请求数(持久化)
  int64_t fail_network = 0;             // 失败分类计数(持久化)
  int64_t fail_json = 0;
  int64_t fail_graphql = 0;
  int64_t fail_format = 0;

  // 状态
  bool is_syncing = false;
  bool sync_done = false;
  ApiState api_state = ApiState::IDLE;
  std::chrono::steady_clock::time_point last_update;

  // 节流持久化：允许丢最近几秒的 meta，但长期准确
  std::chrono::steady_clock::time_point last_persist;
};

// ============================================================================
// 全局 Stats 管理器
// ============================================================================
class StatsManager {
public:
  static StatsManager &instance() {
    static StatsManager inst;
    return inst;
  }

  // 设置数据库连接(在启动时调用一次)
  void set_database(Database *db) {
    db_ = db;
  }

  // 获取指定 entity(跨 source 汇总)的 count
  // found=true 表示至少存在一个 source/entity 的统计项(即已经 init 过)
  int64_t get_total_count_for_entity(const std::string &entity, bool *found = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t sum = 0;
    bool any = false;
    for (auto &[key, stat] : stats_) {
      (void)key;
      if (stat.entity == entity) {
        sum += stat.count;
        any = true;
      }
    }
    if (found)
      *found = any;
    return sum;
  }

  // 初始化 entity(设置初始 count，并从DB加载历史统计)
  void init(const std::string &source, const std::string &entity, int64_t count, int64_t row_size_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];
    stat.source = source;
    stat.entity = entity;
    stat.count = count;
    stat.row_size_bytes = row_size_bytes;
    stat.last_update = std::chrono::steady_clock::now();
    stat.last_persist = stat.last_update;

    // 从数据库加载历史统计
    load_from_db(stat);
  }

  // 开始同步
  void start_sync(const std::string &source, const std::string &entity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];
    stat.is_syncing = true;
    stat.sync_done = false;
    stat.api_state = ApiState::IDLE;
  }

  // 完成同步
  void end_sync(const std::string &source, const std::string &entity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    if (stats_.count(key)) {
      auto &stat = stats_[key];
      stat.is_syncing = false;
      stat.sync_done = true;
      stat.api_state = ApiState::IDLE;
      save_to_db(stat); // 结束时强制落盘
      stat.last_persist = std::chrono::steady_clock::now();
    }
  }

  // 设置API状态
  void set_api_state(const std::string &source, const std::string &entity, ApiState state) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    if (stats_.count(key)) {
      stats_[key].api_state = state;
    }
  }

  // 记录成功的请求(latency_ms是纯API调用时间，不含本地处理)
  void record_success(const std::string &source, const std::string &entity,
                      int64_t records, int64_t latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];

    stat.count += records;
    stat.total_requests++;
    stat.success_requests++;
    stat.total_rows_synced += records;
    stat.total_api_time_ms += latency_ms; // 累加API调用时间
    stat.last_update = std::chrono::steady_clock::now();

    // 记录最近20个延时(不持久化)
    stat.recent_latencies.push_back(latency_ms);
    if (stat.recent_latencies.size() > 20) {
      stat.recent_latencies.pop_front();
    }

    // 更新成功率
    stat.success_rate = static_cast<double>(stat.success_requests) / stat.total_requests * 100.0;

    // 节流：每隔一小段时间落盘一次，允许丢最近几秒
    auto now = std::chrono::steady_clock::now();
    if ((now - stat.last_persist) >= kPersistInterval) {
      save_to_db(stat);
      stat.last_persist = now;
    }
  }

  // 记录失败的请求(latency_ms是纯API调用时间，不含本地处理)
  void record_failure(const std::string &source, const std::string &entity, FailureKind kind, int64_t latency_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto key = make_key(source, entity);
    auto &stat = stats_[key];

    stat.total_requests++;
    stat.total_api_time_ms += latency_ms; // 累加API调用时间
    stat.last_update = std::chrono::steady_clock::now();

    if (kind == FailureKind::NETWORK)
      ++stat.fail_network;
    else if (kind == FailureKind::JSON)
      ++stat.fail_json;
    else if (kind == FailureKind::GRAPHQL)
      ++stat.fail_graphql;
    else if (kind == FailureKind::FORMAT)
      ++stat.fail_format;
    else
      assert(false && "Unknown FailureKind");

    // 记录最近20个延时(不持久化)
    stat.recent_latencies.push_back(latency_ms);
    if (stat.recent_latencies.size() > 20) {
      stat.recent_latencies.pop_front();
    }

    // 更新成功率
    stat.success_rate = static_cast<double>(stat.success_requests) / stat.total_requests * 100.0;

    // 节流：每隔一小段时间落盘一次，允许丢最近几秒
    auto now = std::chrono::steady_clock::now();
    if ((now - stat.last_persist) >= kPersistInterval) {
      save_to_db(stat);
      stat.last_persist = now;
    }
  }

  // indexer 维度失败计数(只有失败能归因)
  void record_indexer_fail(const std::string &source, const std::string &entity, const std::string &indexer) {
    std::lock_guard<std::mutex> lock(mutex_);
    assert(!indexer.empty());
    auto key = make_indexer_key(source, entity, indexer);
    auto &st = indexer_fail_[key];
    st.source = source;
    st.entity = entity;
    st.indexer = indexer;
    if (!st.loaded && db_) {
      load_indexer_fail_from_db_unsafe(st);
      st.loaded = true;
    }
    ++st.fail_requests;

    auto now = std::chrono::steady_clock::now();
    if (st.last_persist.time_since_epoch().count() == 0)
      st.last_persist = now;
    if ((now - st.last_persist) >= kPersistInterval) {
      save_indexer_fail_to_db_unsafe(st);
      st.last_persist = now;
    }
  }

  // 获取所有统计(JSON dump 字符串；用于 HTTP 直接返回，避免重复序列化)
  const std::string &get_all_dump() {
    std::lock_guard<std::mutex> lock(mutex_);
    rebuild_cache_if_needed_unsafe();
    return cached_dump_;
  }

private:
  StatsManager() = default;

  std::string make_key(const std::string &source, const std::string &entity) {
    return source + "/" + entity;
  }

  struct IndexerFailStat {
    std::string source;
    std::string entity;
    std::string indexer;
    int64_t fail_requests = 0;
    bool loaded = false;
    std::chrono::steady_clock::time_point last_persist{};
  };

  std::string make_indexer_key(const std::string &source, const std::string &entity, const std::string &indexer) {
    return source + "/" + entity + "/" + indexer;
  }

  void load_indexer_fail_from_db_unsafe(IndexerFailStat &st);
  void save_indexer_fail_to_db_unsafe(const IndexerFailStat &st);

  void rebuild_cache_if_needed_unsafe() {
    auto now = std::chrono::steady_clock::now();
    if (!cached_dump_.empty() && (now - cached_at_) < kCacheTtl)
      return;

    json result = json::object();

    for (auto &[key, stat] : stats_) {
      // 计算速度：基于历史累积的准确数据
      // speed = 总同步行数 / 总API调用时间(秒)
      double speed = 0.0;
      if (stat.total_rows_synced > 0 && stat.total_api_time_ms > 0) {
        speed = static_cast<double>(stat.total_rows_synced) / (static_cast<double>(stat.total_api_time_ms) / 1000.0);
      }

      // 计算平均延时：最近20个请求，done后显示0ms
      double avg_latency = 0.0;
      if (!stat.sync_done && !stat.recent_latencies.empty()) {
        int64_t sum = 0;
        for (auto latency : stat.recent_latencies) {
          sum += latency;
        }
        avg_latency = static_cast<double>(sum) / stat.recent_latencies.size();
      }

      // API状态字符串
      std::string api_state_str = "idle";
      if (stat.api_state == ApiState::CALLING) {
        api_state_str = "calling";
      } else if (stat.api_state == ApiState::PROCESSING) {
        api_state_str = "processing";
      }

      result[key] = {
          {"source", stat.source},
          {"entity", stat.entity},
          {"count", stat.count},
          {"row_size_bytes", stat.row_size_bytes},
          {"db_size_mb", (stat.row_size_bytes > 0) ? (static_cast<double>(stat.row_size_bytes) * static_cast<double>(stat.count) / (1024.0 * 1024.0)) : 0.0},
          {"speed", std::round(speed * 10) / 10},
          {"avg_latency_ms", std::round(avg_latency)},
          {"success_rate", std::round(stat.success_rate * 10) / 10},
          {"success_requests", stat.success_requests},
          {"total_requests", stat.total_requests},
          {"fail_network", stat.fail_network},
          {"fail_json", stat.fail_json},
          {"fail_graphql", stat.fail_graphql},
          {"fail_format", stat.fail_format},
          {"is_syncing", stat.is_syncing},
          {"sync_done", stat.sync_done},
          {"total_rows_synced", stat.total_rows_synced},
          {"api_state", api_state_str},
      };
    }

    cached_dump_ = result.dump();
    cached_at_ = now;
  }

  // 从数据库加载历史统计
  void load_from_db(EntityStat &stat);

  // 保存统计到数据库
  void save_to_db(const EntityStat &stat);

  std::mutex mutex_;
  std::unordered_map<std::string, EntityStat> stats_;
  std::unordered_map<std::string, IndexerFailStat> indexer_fail_;
  Database *db_ = nullptr;

  // 缓存(避免频繁构建/序列化)
  static constexpr auto kCacheTtl = std::chrono::milliseconds(200);
  std::chrono::steady_clock::time_point cached_at_{};
  std::string cached_dump_;

  // meta 落盘节流
  static constexpr auto kPersistInterval = std::chrono::seconds(5);
};

// ============================================================================
// StatsManager 数据库操作实现
// ============================================================================

#include "../core/database.hpp"
#include "../core/entity_definition.hpp"

inline void StatsManager::load_from_db(EntityStat &stat) {
  if (!db_)
    return;

  std::string sql =
      "SELECT total_requests, success_requests, fail_network, fail_json, fail_graphql, fail_format, total_rows_synced, total_api_time_ms, success_rate "
      "FROM entity_stats_meta "
      "WHERE source = " +
      entities::escape_sql(stat.source) +
      " AND entity = " + entities::escape_sql(stat.entity);
  auto result = db_->query_json(sql);

  if (!result.empty()) {
    auto &row = result[0];
    stat.total_requests = row["total_requests"].get<int64_t>();
    stat.success_requests = row["success_requests"].get<int64_t>();
    stat.fail_network = row["fail_network"].get<int64_t>();
    stat.fail_json = row["fail_json"].get<int64_t>();
    stat.fail_graphql = row["fail_graphql"].get<int64_t>();
    stat.fail_format = row["fail_format"].get<int64_t>();
    stat.total_rows_synced = row["total_rows_synced"].get<int64_t>();
    stat.total_api_time_ms = row["total_api_time_ms"].get<int64_t>();
    stat.success_rate = row["success_rate"].get<double>();
  }
}

inline void StatsManager::save_to_db(const EntityStat &stat) {
  if (!db_)
    return;

  std::string sql =
      "INSERT OR REPLACE INTO entity_stats_meta "
      "(source, entity, total_requests, success_requests, fail_network, fail_json, fail_graphql, fail_format, total_rows_synced, total_api_time_ms, success_rate, updated_at) "
      "VALUES (" +
      entities::escape_sql(stat.source) + ", " +
      entities::escape_sql(stat.entity) + ", " +
      std::to_string(stat.total_requests) + ", " +
      std::to_string(stat.success_requests) + ", " +
      std::to_string(stat.fail_network) + ", " +
      std::to_string(stat.fail_json) + ", " +
      std::to_string(stat.fail_graphql) + ", " +
      std::to_string(stat.fail_format) + ", " +
      std::to_string(stat.total_rows_synced) + ", " +
      std::to_string(stat.total_api_time_ms) + ", " +
      std::to_string(stat.success_rate) + ", CURRENT_TIMESTAMP)";
  db_->execute(sql);
}

inline void StatsManager::load_indexer_fail_from_db_unsafe(StatsManager::IndexerFailStat &st) {
  assert(db_);
  std::string sql =
      "SELECT fail_requests FROM indexer_fail_meta WHERE source = " +
      entities::escape_sql(st.source) +
      " AND entity = " + entities::escape_sql(st.entity) +
      " AND indexer = " + entities::escape_sql(st.indexer);
  auto result = db_->query_json(sql);
  if (!result.empty() && result[0].contains("fail_requests")) {
    st.fail_requests = result[0]["fail_requests"].get<int64_t>();
  }
}

inline void StatsManager::save_indexer_fail_to_db_unsafe(const StatsManager::IndexerFailStat &st) {
  assert(db_);
  std::string sql =
      "INSERT OR REPLACE INTO indexer_fail_meta "
      "(source, entity, indexer, fail_requests, updated_at) "
      "VALUES (" +
      entities::escape_sql(st.source) + ", " +
      entities::escape_sql(st.entity) + ", " +
      entities::escape_sql(st.indexer) + ", " +
      std::to_string(st.fail_requests) + ", CURRENT_TIMESTAMP)";
  db_->execute(sql);
}
