#pragma once

#include <string>
#include <vector>
#include <cassert>
#include <iostream>
#include <duckdb.hpp>
#include <nlohmann/json.hpp>
#include "entities.hpp"

using json = nlohmann::json;

class Database {
public:
    explicit Database(const std::string& path) {
        db_ = std::make_unique<duckdb::DuckDB>(path);
        conn_ = std::make_unique<duckdb::Connection>(*db_);        // 写连接
        read_conn_ = std::make_unique<duckdb::Connection>(*db_);   // 读连接
    }
    
    // ========================================================================
    // 表初始化
    // ========================================================================
    
    void init_sync_state() {
        execute(entities::SYNC_STATE_DDL);
    }
    
    void init_entity(const entities::EntityDef* entity) {
        execute(entity->ddl);
    }
    
    // ========================================================================
    // 事务控制
    // ========================================================================
    
    void begin_transaction() {
        auto result = conn_->Query("BEGIN TRANSACTION");
        assert(!result->HasError() && "BEGIN TRANSACTION failed");
    }
    
    void commit() {
        auto result = conn_->Query("COMMIT");
        assert(!result->HasError() && "COMMIT failed");
    }
    
    // ========================================================================
    // 游标管理
    // ========================================================================
    
    std::string get_cursor(const std::string& source, const std::string& entity) {
        std::string sql = "SELECT last_id FROM sync_state WHERE source = '" + 
                          entities::escape_sql_raw(source) + "' AND entity = '" + 
                          entities::escape_sql_raw(entity) + "'";
        auto result = conn_->Query(sql);
        
        if (result->RowCount() == 0) return "";
        auto value = result->GetValue(0, 0);
        return value.IsNull() ? "" : value.ToString();
    }
    
    void save_cursor(const std::string& source, const std::string& entity, 
                     const std::string& last_id, int64_t batch_count) {
        int64_t current = get_total_synced(source, entity);
        
        std::string sql = 
            "INSERT OR REPLACE INTO sync_state (source, entity, last_id, last_sync_at, total_synced) "
            "VALUES (" + entities::escape_sql(source) + ", " + 
            entities::escape_sql(entity) + ", " + 
            entities::escape_sql(last_id) + ", CURRENT_TIMESTAMP, " + 
            std::to_string(current + batch_count) + ")";
        
        auto result = conn_->Query(sql);
        assert(!result->HasError() && "save_cursor failed");
    }
    
    int64_t get_total_synced(const std::string& source, const std::string& entity) {
        std::string sql = "SELECT total_synced FROM sync_state WHERE source = '" + 
                          entities::escape_sql_raw(source) + "' AND entity = '" + 
                          entities::escape_sql_raw(entity) + "'";
        auto result = conn_->Query(sql);
        
        if (result->RowCount() == 0) return 0;
        auto value = result->GetValue(0, 0);
        return value.IsNull() ? 0 : value.GetValue<int64_t>();
    }
    
    // ========================================================================
    // 批量操作
    // ========================================================================
    
    void batch_insert(const std::string& table, const std::string& columns,
                      const std::vector<std::string>& values_list) {
        if (values_list.empty()) return;
        
        std::string sql = "INSERT OR REPLACE INTO " + table + " (" + columns + ") VALUES ";
        for (size_t i = 0; i < values_list.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += "(" + values_list[i] + ")";
        }
        
        auto result = conn_->Query(sql);
        if (result->HasError()) {
            std::cerr << "[DB] batch_insert 失败: " << result->GetError() << std::endl;
            std::cerr << "[DB] SQL: " << sql.substr(0, 500) << "..." << std::endl;
            assert(false);
        }
    }
    
    // 原子操作：插入 + 保存游标
    void atomic_insert_and_save_cursor(
        const std::string& table,
        const std::string& columns,
        const std::vector<std::string>& values_list,
        const std::string& source,
        const std::string& entity,
        const std::string& cursor
    ) {
        begin_transaction();
        if (!values_list.empty()) {
            batch_insert(table, columns, values_list);
        }
        save_cursor(source, entity, cursor, values_list.size());
        commit();
    }
    
    // ========================================================================
    // 通用执行
    // ========================================================================
    
    void execute(const std::string& sql) {
        auto result = conn_->Query(sql);
        if (result->HasError()) {
            std::cerr << "[DB] SQL 执行失败: " << result->GetError() << std::endl;
        }
    }
    
    // ========================================================================
    // 只读查询(用于 HTTP API)
    // ========================================================================
    
    json query_json(const std::string& sql) {
        auto result = read_conn_->Query(sql);
        if (result->HasError()) {
            throw std::runtime_error(result->GetError());
        }
        
        json rows = json::array();
        auto& types = result->types;
        auto names = result->names;
        
        for (size_t row = 0; row < result->RowCount(); ++row) {
            json obj = json::object();
            for (size_t col = 0; col < result->ColumnCount(); ++col) {
                auto value = result->GetValue(col, row);
                if (value.IsNull()) {
                    obj[names[col]] = nullptr;
                } else {
                    // 根据类型转换
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
    std::unique_ptr<duckdb::Connection> conn_;       // 写连接
    std::unique_ptr<duckdb::Connection> read_conn_;  // 读连接
};
