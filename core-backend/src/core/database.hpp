#pragma once

#include "entity_definition.hpp"
#include <cassert>
#include <duckdb.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

struct SyncCursor {
  std::string value;
  int skip = 0;
};

class Database {
public:
  explicit Database(const std::string &path) {
    db_ = std::make_unique<duckdb::DuckDB>(path);
    conn_ = std::make_unique<duckdb::Connection>(*db_);
    read_conn_ = std::make_unique<duckdb::Connection>(*db_);
  }

  // 表初始化
  void init_sync_state() {
    execute(entities::SYNC_STATE_DDL);
    execute(entities::ENTITY_STATS_META_DDL);
    execute(entities::INDEXER_FAIL_META_DDL);
  }

  void init_entity(const entities::EntityDef *entity) { execute(entity->ddl); }

  // 游标管理
  SyncCursor get_cursor(const std::string &source, const std::string &entity) {
    std::string sql = "SELECT cursor_value, cursor_skip FROM sync_state WHERE source = '" +
                      entities::escape_sql_raw(source) + "' AND entity = '" +
                      entities::escape_sql_raw(entity) + "'";
    std::lock_guard<std::mutex> rlock(read_mutex_);
    auto result = read_conn_->Query(sql);
    if (result->RowCount() == 0)
      return {"", 0};
    auto val = result->GetValue(0, 0);
    auto skip = result->GetValue(1, 0);
    return {
        val.IsNull() ? "" : val.ToString(),
        skip.IsNull() ? 0 : skip.GetValue<int32_t>()};
  }

  // 原子写入：数据 + cursor 在同一事务
  void atomic_insert_with_cursor(
      const std::string &table, const std::string &columns,
      const std::vector<std::string> &values_list,
      const std::string &source, const std::string &entity,
      const std::string &cursor_value, int cursor_skip) {
    assert(!values_list.empty());

    std::string insert_sql = "INSERT INTO " + table + " (" + columns + ") VALUES ";
    for (size_t i = 0; i < values_list.size(); ++i) {
      if (i > 0)
        insert_sql += ", ";
      insert_sql += "(" + values_list[i] + ")";
    }
    insert_sql += build_on_conflict_clause(columns);

    std::string cursor_sql =
        "INSERT OR REPLACE INTO sync_state (source, entity, cursor_value, cursor_skip, last_sync_at) VALUES (" +
        entities::escape_sql(source) + ", " +
        entities::escape_sql(entity) + ", " +
        entities::escape_sql(cursor_value) + ", " +
        std::to_string(cursor_skip) + ", CURRENT_TIMESTAMP)";

    std::lock_guard<std::mutex> lock(write_mutex_);

    auto r1 = conn_->Query("BEGIN TRANSACTION");
    assert(!r1->HasError());
    auto r2 = conn_->Query(insert_sql);
    assert(!r2->HasError());
    auto r3 = conn_->Query(cursor_sql);
    assert(!r3->HasError());
    auto r4 = conn_->Query("COMMIT");
    assert(!r4->HasError());
  }

  void execute(const std::string &sql) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto result = conn_->Query(sql);
    assert(!result->HasError() && "execute failed");
  }

  // 只读查询
  int64_t get_table_count(const std::string &table) {
    std::lock_guard<std::mutex> rlock(read_mutex_);
    auto result = read_conn_->Query("SELECT COUNT(*) FROM " + table);
    if (result->HasError() || result->RowCount() == 0)
      return 0;
    return result->GetValue(0, 0).GetValue<int64_t>();
  }

  json query_json(const std::string &sql) {
    std::lock_guard<std::mutex> rlock(read_mutex_);
    auto result = read_conn_->Query(sql);
    if (result->HasError()) {
      throw std::runtime_error(result->GetError());
    }

    json rows = json::array();
    auto &types = result->types;
    auto names = result->names;

    for (size_t row = 0; row < result->RowCount(); ++row) {
      json obj = json::object();
      for (size_t col = 0; col < result->ColumnCount(); ++col) {
        auto value = result->GetValue(col, row);
        if (value.IsNull()) {
          obj[names[col]] = nullptr;
        } else {
          switch (types[col].id()) {
          case duckdb::LogicalTypeId::BOOLEAN:
            obj[names[col]] = value.GetValue<bool>();
            break;
          case duckdb::LogicalTypeId::TINYINT:
          case duckdb::LogicalTypeId::SMALLINT:
          case duckdb::LogicalTypeId::INTEGER:
            obj[names[col]] = value.GetValue<int32_t>();
            break;
          case duckdb::LogicalTypeId::BIGINT:
            obj[names[col]] = value.GetValue<int64_t>();
            break;
          case duckdb::LogicalTypeId::FLOAT:
          case duckdb::LogicalTypeId::DOUBLE:
            obj[names[col]] = value.GetValue<double>();
            break;
          default:
            obj[names[col]] = value.ToString();
            break;
          }
        }
      }
      rows.push_back(std::move(obj));
    }
    return rows;
  }

  // ============================================================================
  // Token ID 填充
  // ============================================================================

  void merge_pnl_into_condition() {
    execute(
        "UPDATE condition SET positionIds = pnl.positionIds "
        "FROM pnl_condition pnl WHERE condition.id = pnl.id "
        "AND condition.positionIds IS NULL");
  }

  std::vector<std::string> get_null_positionid_conditions(int limit = 100) {
    std::lock_guard<std::mutex> rlock(read_mutex_);
    auto result = read_conn_->Query(
        "SELECT id FROM condition WHERE positionIds IS NULL "
        "ORDER BY resolutionTimestamp LIMIT " +
        std::to_string(limit));
    std::vector<std::string> ids;
    assert(!result->HasError());
    for (size_t i = 0; i < result->RowCount(); ++i) {
      ids.push_back(result->GetValue(0, i).ToString());
    }
    return ids;
  }

  void update_condition_position_ids(const std::string &id, const std::string &position_ids) {
    execute("UPDATE condition SET positionIds = " +
            entities::escape_sql(position_ids) +
            " WHERE id = " + entities::escape_sql(id));
  }

  // ============================================================================
  // Sync 进度查询
  // ============================================================================

  int64_t query_single_int(const std::string &sql) {
    std::lock_guard<std::mutex> rlock(read_mutex_);
    auto result = read_conn_->Query(sql);
    if (result->HasError() || result->RowCount() == 0)
      return 0;
    auto val = result->GetValue(0, 0);
    return val.IsNull() ? 0 : val.GetValue<int64_t>();
  }

  // 获取底层 DuckDB 引用
  duckdb::DuckDB &get_duckdb() { return *db_; }

private:
  static std::string build_on_conflict_clause(const std::string &columns) {
    std::string clause = " ON CONFLICT(id) DO UPDATE SET ";
    bool first = true;
    size_t pos = 0;
    while (pos < columns.size()) {
      size_t comma = columns.find(',', pos);
      std::string col_raw = (comma == std::string::npos)
          ? columns.substr(pos) : columns.substr(pos, comma - pos);
      size_t b = col_raw.find_first_not_of(" ");
      size_t e = col_raw.find_last_not_of(" ");
      std::string col = (b != std::string::npos) ? col_raw.substr(b, e - b + 1) : "";
      pos = (comma == std::string::npos) ? columns.size() : comma + 1;
      if (col.empty() || col == "id") continue;
      if (!first) clause += ", ";
      clause += col + "=excluded." + col;
      first = false;
    }
    return clause;
  }

  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;
  std::unique_ptr<duckdb::Connection> read_conn_;
  std::mutex write_mutex_;
  std::mutex read_mutex_;
};
