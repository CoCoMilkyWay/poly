#pragma once

#include <string>
#include <vector>
#include <iostream>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>
#include "config.hpp"
#include "http_client.hpp"
#include "graphql.hpp"
#include "db.hpp"

using json = nlohmann::json;

class Syncer {
public:
    Syncer(const Config& config, Database& db) 
        : config_(config), db_(db) {}
    
    // 同步所有 subgraph
    void sync_all() {
        for (const auto& sg : config_.subgraphs) {
            for (const auto& entity : sg.entities) {
                sync_entity(sg.id, sg.name, entity);
            }
        }
    }
    
    // 运行同步循环
    void run() {
        std::cout << "[Syncer] 开始同步循环，间隔: " 
                  << config_.sync_interval_seconds << " 秒" << std::endl;
        
        while (true) {
            auto start = std::chrono::steady_clock::now();
            
            sync_all();
            
            auto elapsed = std::chrono::steady_clock::now() - start;
            auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
            
            std::cout << "[Syncer] 同步完成，耗时: " << elapsed_sec << " 秒" << std::endl;
            
            // 等待下一次同步
            int sleep_sec = config_.sync_interval_seconds - static_cast<int>(elapsed_sec);
            if (sleep_sec > 0) {
                std::this_thread::sleep_for(std::chrono::seconds(sleep_sec));
            }
        }
    }

private:
    void sync_entity(const std::string& subgraph_id, const std::string& subgraph_name,
                     const std::string& entity) {
        std::cout << "[Syncer] 同步 " << subgraph_name << "/" << entity << std::endl;
        
        std::string cursor = db_.get_cursor(subgraph_id, entity);
        std::string url = GraphQLBuilder::build_url(config_.api_key, subgraph_id);
        std::string entity_plural = GraphQLBuilder::to_plural(entity);
        const char* fields = GraphQLBuilder::get_fields(entity);
        
        int total_synced = 0;
        
        while (true) {
            // 构建查询
            std::string query = GraphQLBuilder::build_query(entity_plural, fields, cursor, 1000);
            
            // 发送请求
            std::string response;
            try {
                response = http_.post(url, query);
            } catch (...) {
                std::cerr << "[Syncer] HTTP 请求失败" << std::endl;
                break;
            }
            
            // 解析响应
            json j;
            try {
                j = json::parse(response);
            } catch (...) {
                std::cerr << "[Syncer] JSON 解析失败: " << response.substr(0, 200) << std::endl;
                break;
            }
            
            // 检查错误
            if (j.contains("errors")) {
                std::cerr << "[Syncer] GraphQL 错误: " << j["errors"].dump() << std::endl;
                break;
            }
            
            // 获取数据
            if (!j.contains("data") || !j["data"].contains(entity_plural)) {
                std::cerr << "[Syncer] 响应格式错误" << std::endl;
                break;
            }
            
            auto& items = j["data"][entity_plural];
            if (items.empty()) {
                std::cout << "[Syncer] " << entity << " 同步完成，共 " << total_synced << " 条" << std::endl;
                break;
            }
            
            // 插入数据
            insert_items(entity, items);
            
            total_synced += items.size();
            
            // 更新游标
            cursor = items.back()["id"].get<std::string>();
            db_.save_cursor(subgraph_id, entity, cursor);
            
            std::cout << "[Syncer] " << entity << " 已同步 " << total_synced 
                      << " 条，游标: " << cursor.substr(0, 20) << "..." << std::endl;
            
            // 如果返回数量小于 limit，说明已经同步完成
            if (items.size() < 1000) {
                std::cout << "[Syncer] " << entity << " 同步完成，共 " << total_synced << " 条" << std::endl;
                break;
            }
        }
    }
    
    void insert_items(const std::string& entity, const json& items) {
        if (items.empty()) return;
        
        std::vector<std::string> values_list;
        values_list.reserve(items.size());
        
        for (const auto& item : items) {
            std::string values = build_values(entity, item);
            if (!values.empty()) {
                values_list.push_back(values);
            }
        }
        
        if (values_list.empty()) return;
        
        std::string table = entity_to_table(entity);
        std::string columns = get_columns(entity);
        
        db_.batch_insert(table, columns, values_list);
    }
    
    std::string build_values(const std::string& entity, const json& item) {
        // 根据 entity 类型构建 VALUES 字符串
        auto escape = [](const std::string& s) -> std::string {
            std::string result;
            result.reserve(s.size() + 2);
            result += "'";
            for (char c : s) {
                if (c == '\'') result += "''";
                else result += c;
            }
            result += "'";
            return result;
        };
        
        auto get_str = [&](const std::string& key) -> std::string {
            if (!item.contains(key) || item[key].is_null()) return "NULL";
            if (item[key].is_string()) return escape(item[key].get<std::string>());
            return escape(item[key].dump());
        };
        
        auto get_int = [&](const std::string& key) -> std::string {
            if (!item.contains(key) || item[key].is_null()) return "NULL";
            if (item[key].is_number()) return std::to_string(item[key].get<int64_t>());
            if (item[key].is_string()) return item[key].get<std::string>();
            return "NULL";
        };
        
        auto get_array_str = [&](const std::string& key) -> std::string {
            if (!item.contains(key) || item[key].is_null()) return "NULL";
            return escape(item[key].dump());
        };
        
        if (entity == "Split") {
            return get_str("id") + "," + get_int("timestamp") + "," + 
                   get_str("stakeholder") + "," + get_str("condition") + "," + get_str("amount");
        }
        if (entity == "Merge") {
            return get_str("id") + "," + get_int("timestamp") + "," + 
                   get_str("stakeholder") + "," + get_str("condition") + "," + get_str("amount");
        }
        if (entity == "Redemption") {
            return get_str("id") + "," + get_int("timestamp") + "," + 
                   get_str("redeemer") + "," + get_str("condition") + "," + 
                   get_array_str("indexSets") + "," + get_str("payout");
        }
        if (entity == "NegRiskConversion") {
            return get_str("id") + "," + get_int("timestamp") + "," + 
                   get_str("stakeholder") + "," + get_str("negRiskMarketId") + "," + 
                   get_str("amount") + "," + get_str("indexSet") + "," + get_int("questionCount");
        }
        if (entity == "UserPosition") {
            return get_str("id") + "," + get_str("user") + "," + get_str("tokenId") + "," + 
                   get_str("amount") + "," + get_str("avgPrice") + "," + 
                   get_str("realizedPnl") + "," + get_str("totalBought");
        }
        if (entity == "NegRiskEvent") {
            return get_str("id") + "," + get_int("questionCount");
        }
        if (entity == "Condition") {
            return get_str("id");
        }
        if (entity == "OrderFilledEvent") {
            return get_str("id") + "," + get_str("transactionHash") + "," + get_int("timestamp") + "," + 
                   get_str("orderHash") + "," + get_str("maker") + "," + get_str("taker") + "," + 
                   get_str("makerAssetId") + "," + get_str("takerAssetId") + "," + 
                   get_str("makerAmountFilled") + "," + get_str("takerAmountFilled") + "," + get_str("fee");
        }
        if (entity == "Orderbook") {
            return get_str("id") + "," + get_str("tradesQuantity") + "," + 
                   get_str("buysQuantity") + "," + get_str("sellsQuantity") + "," + 
                   get_str("collateralVolume") + "," + get_str("scaledCollateralVolume") + "," + 
                   get_str("collateralBuyVolume") + "," + get_str("scaledCollateralBuyVolume") + "," + 
                   get_str("collateralSellVolume") + "," + get_str("scaledCollateralSellVolume");
        }
        if (entity == "MarketData") {
            return get_str("id") + "," + get_str("condition") + "," + get_str("outcomeIndex");
        }
        if (entity == "MarketOpenInterest") {
            return get_str("id") + "," + get_str("amount");
        }
        if (entity == "GlobalOpenInterest") {
            return get_str("id") + "," + get_str("amount");
        }
        if (entity == "FixedProductMarketMaker") {
            return get_str("id") + "," + get_str("creator") + "," + get_int("creationTimestamp") + "," + 
                   get_str("fee") + "," + get_str("tradesQuantity") + "," + 
                   get_str("buysQuantity") + "," + get_str("sellsQuantity") + "," + 
                   get_str("collateralVolume") + "," + get_str("scaledCollateralVolume") + "," + 
                   get_str("liquidityParameter");
        }
        if (entity == "FpmmTransaction") {
            return get_str("id") + "," + get_str("type") + "," + get_int("timestamp") + "," + 
                   get_str("user") + "," + get_str("tradeAmount") + "," + get_str("feeAmount") + "," + 
                   get_str("outcomeIndex") + "," + get_str("outcomeTokensAmount");
        }
        
        return "";
    }
    
    std::string entity_to_table(const std::string& entity) {
        if (entity == "Split") return "split";
        if (entity == "Merge") return "merge";
        if (entity == "Redemption") return "redemption";
        if (entity == "NegRiskConversion") return "neg_risk_conversion";
        if (entity == "UserPosition") return "user_position";
        if (entity == "NegRiskEvent") return "neg_risk_event";
        if (entity == "Condition") return "condition";
        if (entity == "OrderFilledEvent") return "order_filled_event";
        if (entity == "Orderbook") return "orderbook";
        if (entity == "MarketData") return "market_data";
        if (entity == "MarketOpenInterest") return "market_open_interest";
        if (entity == "GlobalOpenInterest") return "global_open_interest";
        if (entity == "FixedProductMarketMaker") return "fixed_product_market_maker";
        if (entity == "FpmmTransaction") return "fpmm_transaction";
        return entity;
    }
    
    std::string get_columns(const std::string& entity) {
        if (entity == "Split") return "id, timestamp, stakeholder, condition, amount";
        if (entity == "Merge") return "id, timestamp, stakeholder, condition, amount";
        if (entity == "Redemption") return "id, timestamp, redeemer, condition, index_sets, payout";
        if (entity == "NegRiskConversion") return "id, timestamp, stakeholder, neg_risk_market_id, amount, index_set, question_count";
        if (entity == "UserPosition") return "id, user_addr, token_id, amount, avg_price, realized_pnl, total_bought";
        if (entity == "NegRiskEvent") return "id, question_count";
        if (entity == "Condition") return "id";
        if (entity == "OrderFilledEvent") return "id, transaction_hash, timestamp, order_hash, maker, taker, maker_asset_id, taker_asset_id, maker_amount_filled, taker_amount_filled, fee";
        if (entity == "Orderbook") return "id, trades_quantity, buys_quantity, sells_quantity, collateral_volume, scaled_collateral_volume, collateral_buy_volume, scaled_collateral_buy_volume, collateral_sell_volume, scaled_collateral_sell_volume";
        if (entity == "MarketData") return "id, condition, outcome_index";
        if (entity == "MarketOpenInterest") return "id, amount";
        if (entity == "GlobalOpenInterest") return "id, amount";
        if (entity == "FixedProductMarketMaker") return "id, creator, creation_timestamp, fee, trades_quantity, buys_quantity, sells_quantity, collateral_volume, scaled_collateral_volume, liquidity_parameter";
        if (entity == "FpmmTransaction") return "id, type, timestamp, user_addr, trade_amount, fee_amount, outcome_index, outcome_tokens_amount";
        return "id";
    }
    
    const Config& config_;
    Database& db_;
    HttpClient http_;
};
