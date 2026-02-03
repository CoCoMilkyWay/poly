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
    
    void rollback() {
        auto result = conn_->Query("ROLLBACK");
        // rollback 不应该失败，但如果失败了也不 assert，因为可能是事务已经结束
    }
    
    // ========================================================================
    // 游标管理
    // ========================================================================
    
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
    
    // 保存同步游标（需要在事务中调用）
    void save_cursor(const std::string& subgraph_id, const std::string& entity, 
                     const std::string& last_id, int64_t batch_count = 0) {
        std::string sql = 
            "INSERT OR REPLACE INTO sync_state (subgraph_id, entity_name, last_id, last_sync_at, total_synced) "
            "VALUES ('" + subgraph_id + "', '" + entity + "', '" + last_id + "', CURRENT_TIMESTAMP, "
            "COALESCE((SELECT total_synced FROM sync_state WHERE subgraph_id = '" + subgraph_id + 
            "' AND entity_name = '" + entity + "'), 0) + " + std::to_string(batch_count) + ")";
        
        auto result = conn_->Query(sql);
        assert(!result->HasError() && "save_cursor failed");
    }
    
    // 获取已同步总数
    int64_t get_total_synced(const std::string& subgraph_id, const std::string& entity) {
        auto result = conn_->Query(
            "SELECT total_synced FROM sync_state WHERE subgraph_id = '" + subgraph_id + 
            "' AND entity_name = '" + entity + "'"
        );
        
        if (result->RowCount() == 0) {
            return 0;
        }
        
        auto value = result->GetValue(0, 0);
        if (value.IsNull()) {
            return 0;
        }
        return value.GetValue<int64_t>();
    }
    
    // ========================================================================
    // 批量插入
    // ========================================================================
    
    // 批量插入（需要在事务中调用）
    void batch_insert(const std::string& table, const std::string& columns,
                      const std::vector<std::string>& values_list) {
        if (values_list.empty()) return;
        
        std::string sql = "INSERT OR REPLACE INTO " + table + " (" + columns + ") VALUES ";
        
        for (size_t i = 0; i < values_list.size(); ++i) {
            if (i > 0) sql += ", ";
            sql += "(" + values_list[i] + ")";
        }
        
        auto result = conn_->Query(sql);
        assert(!result->HasError() && ("batch_insert failed: " + result->GetError()).c_str());
    }
    
    // ========================================================================
    // 原子操作：插入数据并保存游标（事务保护）
    // ========================================================================
    void atomic_insert_and_save_cursor(
        const std::string& table,
        const std::string& columns,
        const std::vector<std::string>& values_list,
        const std::string& subgraph_id,
        const std::string& entity,
        const std::string& cursor
    ) {
        begin_transaction();
        
        // 插入数据
        if (!values_list.empty()) {
            batch_insert(table, columns, values_list);
        }
        
        // 保存游标和计数
        save_cursor(subgraph_id, entity, cursor, values_list.size());
        
        commit();
    }
    
    // ========================================================================
    // 数据校验
    // ========================================================================
    
    // 获取表的记录数
    int64_t get_table_count(const std::string& table) {
        auto result = conn_->Query("SELECT COUNT(*) FROM " + table);
        
        if (result->HasError() || result->RowCount() == 0) {
            return -1;
        }
        
        return result->GetValue(0, 0).GetValue<int64_t>();
    }
    
    // 验证同步完整性：对比 sync_state 中的 total_synced 和实际表记录数
    bool verify_sync_integrity(const std::string& subgraph_id, const std::string& entity, 
                                const std::string& table) {
        int64_t total_synced = get_total_synced(subgraph_id, entity);
        int64_t actual_count = get_table_count(table);
        
        if (actual_count < 0) {
            std::cerr << "[DB] 无法获取表 " << table << " 的记录数" << std::endl;
            return false;
        }
        
        // 由于 INSERT OR REPLACE，实际记录数可能小于等于 total_synced
        // 但不应该大于 total_synced
        if (actual_count > total_synced) {
            std::cerr << "[DB] 数据完整性警告: " << table 
                      << " 实际记录 " << actual_count 
                      << " > 同步记录 " << total_synced << std::endl;
            return false;
        }
        
        return true;
    }
    
    // ========================================================================
    // 原始 SQL 执行
    // ========================================================================
    
    void execute(const std::string& sql) {
        auto result = conn_->Query(sql);
        if (result->HasError()) {
            std::cerr << "SQL 执行失败: " << result->GetError() << std::endl;
            std::cerr << "SQL: " << sql.substr(0, 200) << "..." << std::endl;
        }
    }
    
    std::unique_ptr<duckdb::MaterializedQueryResult> query(const std::string& sql) {
        return conn_->Query(sql);
    }

private:
    void init_tables() {
        // Activity subgraph tables
        execute(schema::ddl::SYNC_STATE);
        execute(schema::ddl::SPLIT);
        execute(schema::ddl::MERGE);
        execute(schema::ddl::REDEMPTION);
        execute(schema::ddl::NEG_RISK_CONVERSION);
        execute(schema::ddl::POSITION);
        execute(schema::ddl::NEG_RISK_EVENT);
        execute(schema::ddl::CONDITION_ACTIVITY);
        
        // PnL subgraph tables
        execute(schema::ddl::USER_POSITION);
        execute(schema::ddl::CONDITION_PNL);
        execute(schema::ddl::FPMM_PNL);
        
        // Orderbook subgraph tables
        execute(schema::ddl::ORDER_FILLED_EVENT);
        execute(schema::ddl::ORDERS_MATCHED_EVENT);
        execute(schema::ddl::ORDERBOOK);
        execute(schema::ddl::MARKET_DATA);
        execute(schema::ddl::ORDERS_MATCHED_GLOBAL);
        
        // OI subgraph tables
        execute(schema::ddl::MARKET_OPEN_INTEREST);
        execute(schema::ddl::GLOBAL_OPEN_INTEREST);
        execute(schema::ddl::CONDITION_OI);
        execute(schema::ddl::NEG_RISK_EVENT_OI);
        
        // FPMM subgraph tables
        execute(schema::ddl::COLLATERAL);
        execute(schema::ddl::FIXED_PRODUCT_MARKET_MAKER);
        execute(schema::ddl::FPMM_FUNDING_ADDITION);
        execute(schema::ddl::FPMM_FUNDING_REMOVAL);
        execute(schema::ddl::FPMM_TRANSACTION);
        execute(schema::ddl::FPMM_POOL_MEMBERSHIP);
        execute(schema::ddl::CONDITION_FPMM);
        
        // Resolution subgraph tables
        execute(schema::ddl::MARKET_RESOLUTION);
        execute(schema::ddl::ANCILLARY_DATA_HASH_TO_QUESTION_ID);
        execute(schema::ddl::MODERATOR);
        execute(schema::ddl::REVISION);
        
        // Positions subgraph tables
        execute(schema::ddl::USER_BALANCE);
        execute(schema::ddl::NET_USER_BALANCE);
        execute(schema::ddl::TOKEN_ID_CONDITION);
        execute(schema::ddl::CONDITION_POSITIONS);
        
        // Fee-module subgraph tables
        execute(schema::ddl::FEE_REFUNDED_ENTITY);
        
        // Wallet subgraph tables
        execute(schema::ddl::WALLET);
        execute(schema::ddl::GLOBAL_USDC_BALANCE);
        
        // Sports-oracle subgraph tables
        execute(schema::ddl::GAME);
        execute(schema::ddl::MARKET);
        
        std::cout << "[DB] 数据库初始化完成" << std::endl;
    }
    
    std::unique_ptr<duckdb::DuckDB> db_;
    std::unique_ptr<duckdb::Connection> conn_;
};
