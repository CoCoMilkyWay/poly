#pragma once

#include <string>
#include <vector>
#include <cassert>
#include <iostream>
#include <duckdb.hpp>
#include "schema.hpp"

class Database {
public:
    explicit Database(const std::string& path) {
        db_ = std::make_unique<duckdb::DuckDB>(path);
        conn_ = std::make_unique<duckdb::Connection>(*db_);
        
        init_tables();
    }
    
    // 获取同步游标
    std::string get_cursor(const std::string& subgraph_id, const std::string& entity) {
        auto result = conn_->Query(
            "SELECT last_id FROM sync_state WHERE subgraph_id = '" + subgraph_id + 
            "' AND entity_name = '" + entity + "'"
        );
        
        if (result->RowCount() == 0) {
            return "";
        }
        
        auto value = result->GetValue(0, 0);
        if (value.IsNull()) {
            return "";
        }
        return value.ToString();
    }
    
    // 保存同步游标
    void save_cursor(const std::string& subgraph_id, const std::string& entity, 
                     const std::string& last_id) {
        conn_->Query(
            "INSERT OR REPLACE INTO sync_state (subgraph_id, entity_name, last_id, last_sync_at) "
            "VALUES ('" + subgraph_id + "', '" + entity + "', '" + last_id + "', CURRENT_TIMESTAMP)"
        );
    }
    
    // 批量插入 - 通用方法
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
            std::cerr << "批量插入失败: " << result->GetError() << std::endl;
        }
    }
    
    // 执行原始 SQL
    void execute(const std::string& sql) {
        auto result = conn_->Query(sql);
        if (result->HasError()) {
            std::cerr << "SQL 执行失败: " << result->GetError() << std::endl;
        }
    }
    
    // 查询
    std::unique_ptr<duckdb::MaterializedQueryResult> query(const std::string& sql) {
        return conn_->Query(sql);
    }

private:
    void init_tables() {
        // 创建所有表
        execute(schema::ddl::SYNC_STATE);
        execute(schema::ddl::SPLIT);
        execute(schema::ddl::MERGE);
        execute(schema::ddl::REDEMPTION);
        execute(schema::ddl::NEG_RISK_CONVERSION);
        execute(schema::ddl::USER_POSITION);
        execute(schema::ddl::NEG_RISK_EVENT);
        execute(schema::ddl::CONDITION);
        execute(schema::ddl::ORDER_FILLED_EVENT);
        execute(schema::ddl::ORDERBOOK);
        execute(schema::ddl::MARKET_DATA);
        execute(schema::ddl::MARKET_OPEN_INTEREST);
        execute(schema::ddl::GLOBAL_OPEN_INTEREST);
        execute(schema::ddl::FIXED_PRODUCT_MARKET_MAKER);
        execute(schema::ddl::FPMM_TRANSACTION);
        
        std::cout << "[DB] 数据库初始化完成" << std::endl;
    }
    
    std::unique_ptr<duckdb::DuckDB> db_;
    std::unique_ptr<duckdb::Connection> conn_;
};
