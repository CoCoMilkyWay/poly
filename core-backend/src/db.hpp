#pragma once

#include "entities.hpp"
#include <cassert>
#include <duckdb.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

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
  std::string get_cursor(const std::string &source, const std::string &entity) {
    std::string sql = "SELECT last_id FROM sync_state WHERE source = '" +
                      entities::escape_sql_raw(source) + "' AND entity = '" +
                      entities::escape_sql_raw(entity) + "'";
    auto result = conn_->Query(sql);
    if (result->RowCount() == 0)
      return "";
    auto value = result->GetValue(0, 0);
    return value.IsNull() ? "" : value.ToString();
  }

  // 原子写入：数据 + cursor 在同一事务
  void atomic_insert_with_cursor(
      const std::string &table, const std::string &columns,
      const std::vector<std::string> &values_list,
      const std::string &source, const std::string &entity,
      const std::string &cursor) {
    assert(!values_list.empty());
    assert(!cursor.empty());

    // 构建 batch insert SQL
    std::string insert_sql = "INSERT OR REPLACE INTO " + table + " (" + columns + ") VALUES ";
    for (size_t i = 0; i < values_list.size(); ++i) {
      if (i > 0)
        insert_sql += ", ";
      insert_sql += "(" + values_list[i] + ")";
    }

    // 构建 cursor update SQL
    std::string cursor_sql =
        "INSERT OR REPLACE INTO sync_state (source, entity, last_id, last_sync_at) VALUES (" +
        entities::escape_sql(source) + ", " +
        entities::escape_sql(entity) + ", " +
        entities::escape_sql(cursor) + ", CURRENT_TIMESTAMP)";

    // 加锁保护写操作
    std::lock_guard<std::mutex> lock(write_mutex_);

    // 单事务执行
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
    auto result = read_conn_->Query("SELECT COUNT(*) FROM " + table);
    if (result->HasError() || result->RowCount() == 0)
      return 0;
    return result->GetValue(0, 0).GetValue<int64_t>();
  }

  json query_json(const std::string &sql) {
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

private:
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> conn_;
  std::unique_ptr<duckdb::Connection> read_conn_;
  std::mutex write_mutex_; // 保护 conn_ 写操作
};
