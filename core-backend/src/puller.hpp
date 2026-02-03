#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define PARALLEL_PER_SUBGRAPH 4     // 每个 subgraph 内最多并行 entity 数
#define PARALLEL_TOTAL 16           // 全局最大并发请求数
#define GRAPHQL_BATCH_SIZE 1000     // 每次请求的 limit
#define DB_FLUSH_THRESHOLD 5000     // 累积多少条刷入 DB
#define PULL_INTERVAL_SEC 60        // 拉取间隔

#include <string>
#include <vector>
#include <functional>
#include <iostream>
#include <cassert>
#include <nlohmann/json.hpp>

#include "config.hpp"
#include "db.hpp"
#include "graphql.hpp"
#include "https_pool.hpp"

using json = nlohmann::json;

class Puller;
class SubgraphScheduler;

// ============================================================================
// EntityPuller - 单个 entity 的拉取流
// ============================================================================
class EntityPuller {
public:
    EntityPuller(const std::string& subgraph_id, const std::string& subgraph_name,
                 const std::string& entity, Database& db, HttpsPool& pool,
                 SubgraphScheduler* scheduler)
        : subgraph_id_(subgraph_id)
        , subgraph_name_(subgraph_name)
        , entity_(entity)
        , db_(db)
        , pool_(pool)
        , scheduler_(scheduler)
        , entity_plural_(GraphQL::to_plural(entity))
        , fields_(GraphQL::get_fields(entity, subgraph_name))
        , target_(GraphQL::build_target(subgraph_id)) {}
    
    void start(const std::string& api_key);
    void on_response(const std::string& body);
    
    bool is_done() const { return done_; }
    const std::string& entity() const { return entity_; }
    int total_synced() const { return total_synced_; }

private:
    void send_request();
    void insert_items(const json& items);
    std::string build_values(const json& item);
    std::string entity_to_table();
    std::string get_columns();
    
    std::string subgraph_id_;
    std::string subgraph_name_;
    std::string entity_;
    Database& db_;
    HttpsPool& pool_;
    SubgraphScheduler* scheduler_;
    
    std::string api_key_;
    std::string cursor_;
    std::string entity_plural_;
    const char* fields_;
    std::string target_;
    
    int total_synced_ = 0;
    bool done_ = false;
};

// ============================================================================
// SubgraphScheduler - 单个 subgraph 的调度器
// ============================================================================
class SubgraphScheduler {
public:
    SubgraphScheduler(const SubgraphConfig& config, Database& db, HttpsPool& pool, Puller* puller)
        : subgraph_id_(config.id)
        , subgraph_name_(config.name)
        , db_(db)
        , pool_(pool)
        , puller_(puller) {
        for (const auto& entity : config.entities) {
            pullers_.emplace_back(subgraph_id_, subgraph_name_, entity, db_, pool_, this);
        }
    }
    
    void start(const std::string& api_key);
    void on_entity_done(EntityPuller* puller);
    
    bool all_done() const {
        for (const auto& p : pullers_) {
            if (!p.is_done()) return false;
        }
        return true;
    }
    
    const std::string& name() const { return subgraph_name_; }
    int active_count() const { return active_count_; }

private:
    void start_next();
    
    std::string subgraph_id_;
    std::string subgraph_name_;
    Database& db_;
    HttpsPool& pool_;
    Puller* puller_;
    
    std::string api_key_;
    std::vector<EntityPuller> pullers_;
    size_t next_idx_ = 0;
    int active_count_ = 0;
};

// ============================================================================
// Puller - 全局协调器
// ============================================================================
class Puller {
public:
    Puller(const Config& config, Database& db, HttpsPool& pool)
        : config_(config), db_(db), pool_(pool) {
        // 只加载有效的 subgraph（过滤掉占位符 ID）
        auto active_subgraphs = config.get_active_subgraphs();
        for (const auto& sg : active_subgraphs) {
            schedulers_.emplace_back(sg, db_, pool_, this);
        }
    }
    
    void run(asio::io_context& ioc) {
        std::cout << "[Puller] 启动，共 " << schedulers_.size() << " 个 subgraph" << std::endl;
        std::cout << "[Puller] PARALLEL_PER_SUBGRAPH=" << PARALLEL_PER_SUBGRAPH 
                  << ", PARALLEL_TOTAL=" << PARALLEL_TOTAL << std::endl;
        
        // 启动所有 scheduler
        for (auto& s : schedulers_) {
            s.start(config_.api_key);
        }
        
        // 运行事件循环
        ioc.run();
        
        std::cout << "[Puller] 所有 subgraph 同步完成" << std::endl;
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
        // 通知其他 scheduler 可能有空闲槽
        for (auto& s : schedulers_) {
            if (!s.all_done() && s.active_count() < PARALLEL_PER_SUBGRAPH) {
                s.on_entity_done(nullptr);  // 触发尝试启动
                break;
            }
        }
    }

private:
    const Config& config_;
    Database& db_;
    HttpsPool& pool_;
    std::vector<SubgraphScheduler> schedulers_;
    int total_active_ = 0;
};

// ============================================================================
// EntityPuller 实现
// ============================================================================
inline void EntityPuller::start(const std::string& api_key) {
    api_key_ = api_key;
    cursor_ = db_.get_cursor(subgraph_id_, entity_);
    std::cout << "[Pull] " << subgraph_name_ << "/" << entity_ 
              << " 开始，cursor=" << (cursor_.empty() ? "(empty)" : cursor_.substr(0, 20) + "...") 
              << std::endl;
    send_request();
}

inline void EntityPuller::send_request() {
    std::string query = GraphQL::build_query(entity_plural_, fields_, cursor_, GRAPHQL_BATCH_SIZE);
    
    pool_.async_post(target_, query, [this](std::string body) {
        on_response(body);
    });
}

inline void EntityPuller::on_response(const std::string& body) {
    json j;
    try {
        j = json::parse(body);
    } catch (...) {
        std::cerr << "[Pull] " << entity_ << " JSON 解析失败" << std::endl;
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    if (j.contains("errors")) {
        std::cerr << "[Pull] " << entity_ << " GraphQL 错误: " << j["errors"].dump() << std::endl;
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    if (!j.contains("data") || !j["data"].contains(entity_plural_)) {
        std::cerr << "[Pull] " << entity_ << " 响应格式错误" << std::endl;
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    auto& items = j["data"][entity_plural_];
    if (items.empty()) {
        std::cout << "[Pull] " << subgraph_name_ << "/" << entity_ 
                  << " 完成，共 " << total_synced_ << " 条" << std::endl;
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    // 构建 values 列表
    std::vector<std::string> values_list;
    values_list.reserve(items.size());
    for (const auto& item : items) {
        std::string values = build_values(item);
        if (!values.empty()) {
            values_list.push_back(values);
        }
    }
    
    // 获取最后一条的 id 作为新 cursor
    std::string new_cursor = items.back()["id"].get<std::string>();
    
    // 原子操作：插入数据并保存游标
    db_.atomic_insert_and_save_cursor(
        entity_to_table(),
        get_columns(),
        values_list,
        subgraph_id_,
        entity_,
        new_cursor
    );
    
    cursor_ = new_cursor;
    total_synced_ += items.size();
    
    std::cout << "[Pull] " << subgraph_name_ << "/" << entity_ 
              << " +" << items.size() << " (total=" << total_synced_ << ")" << std::endl;
    
    if (items.size() < GRAPHQL_BATCH_SIZE) {
        std::cout << "[Pull] " << subgraph_name_ << "/" << entity_ 
                  << " 完成，共 " << total_synced_ << " 条" << std::endl;
        done_ = true;
        scheduler_->on_entity_done(this);
        return;
    }
    
    // 继续拉取下一批
    send_request();
}

inline std::string EntityPuller::build_values(const json& item) {
    // SQL 字符串转义
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
    
    // 获取字符串字段
    auto get_str = [&](const std::string& key) -> std::string {
        if (!item.contains(key) || item[key].is_null()) return "NULL";
        if (item[key].is_string()) return escape(item[key].get<std::string>());
        return escape(item[key].dump());
    };
    
    // 获取整数字段
    auto get_int = [&](const std::string& key) -> std::string {
        if (!item.contains(key) || item[key].is_null()) return "NULL";
        if (item[key].is_number()) return std::to_string(item[key].get<int64_t>());
        if (item[key].is_string()) return item[key].get<std::string>();
        return "NULL";
    };
    
    // 获取布尔字段
    auto get_bool = [&](const std::string& key) -> std::string {
        if (!item.contains(key) || item[key].is_null()) return "NULL";
        if (item[key].is_boolean()) return item[key].get<bool>() ? "TRUE" : "FALSE";
        if (item[key].is_string()) {
            std::string s = item[key].get<std::string>();
            return (s == "true" || s == "TRUE") ? "TRUE" : "FALSE";
        }
        return "NULL";
    };
    
    // 获取数组字段（序列化为 JSON 字符串）
    auto get_array_str = [&](const std::string& key) -> std::string {
        if (!item.contains(key) || item[key].is_null()) return "NULL";
        return escape(item[key].dump());
    };
    
    // 获取嵌套对象的 id 字段
    auto get_nested_id = [&](const std::string& key) -> std::string {
        if (!item.contains(key) || item[key].is_null()) return "NULL";
        if (item[key].is_object() && item[key].contains("id")) {
            return escape(item[key]["id"].get<std::string>());
        }
        return "NULL";
    };
    
    // 获取嵌套对象的指定字段
    auto get_nested = [&](const std::string& obj_key, const std::string& field_key) -> std::string {
        if (!item.contains(obj_key) || item[obj_key].is_null()) return "NULL";
        if (item[obj_key].is_object() && item[obj_key].contains(field_key)) {
            auto& v = item[obj_key][field_key];
            if (v.is_null()) return "NULL";
            if (v.is_string()) return escape(v.get<std::string>());
            if (v.is_number()) return std::to_string(v.get<int64_t>());
            return escape(v.dump());
        }
        return "NULL";
    };
    
    // ========================================================================
    // Activity subgraph
    // ========================================================================
    if (entity_ == "Split") {
        return get_str("id") + "," + get_int("timestamp") + "," + 
               get_str("stakeholder") + "," + get_str("condition") + "," + get_str("amount");
    }
    if (entity_ == "Merge") {
        return get_str("id") + "," + get_int("timestamp") + "," + 
               get_str("stakeholder") + "," + get_str("condition") + "," + get_str("amount");
    }
    if (entity_ == "Redemption") {
        return get_str("id") + "," + get_int("timestamp") + "," + 
               get_str("redeemer") + "," + get_str("condition") + "," + 
               get_array_str("indexSets") + "," + get_str("payout");
    }
    if (entity_ == "NegRiskConversion") {
        return get_str("id") + "," + get_int("timestamp") + "," + 
               get_str("stakeholder") + "," + get_str("negRiskMarketId") + "," + 
               get_str("amount") + "," + get_str("indexSet") + "," + get_int("questionCount");
    }
    if (entity_ == "Position") {
        return get_str("id") + "," + get_str("condition") + "," + get_str("outcomeIndex");
    }
    
    // NegRiskEvent - 根据 subgraph 有不同字段
    if (entity_ == "NegRiskEvent") {
        if (subgraph_name_ == "oi") {
            // OI subgraph 有 feeBps
            return get_str("id") + "," + get_str("feeBps") + "," + get_int("questionCount");
        }
        // activity/pnl subgraph
        return get_str("id") + "," + get_int("questionCount") + ",NULL";
    }
    
    // Condition - 根据 subgraph 有不同字段和表
    if (entity_ == "Condition") {
        if (subgraph_name_ == "pnl") {
            return get_str("id") + "," + get_array_str("positionIds") + "," + 
                   get_array_str("payoutNumerators") + "," + get_str("payoutDenominator");
        }
        if (subgraph_name_ == "positions") {
            return get_str("id") + "," + get_array_str("payouts");
        }
        // activity/oi/fpmm subgraph - 只有 id
        return get_str("id");
    }
    
    // FixedProductMarketMaker - 根据 subgraph 有不同字段
    if (entity_ == "FixedProductMarketMaker") {
        if (subgraph_name_ == "activity") {
            return get_str("id");  // activity subgraph 只有 id
        }
        // fpmm subgraph - 完整字段
        return get_str("id") + "," + get_str("creator") + "," + get_int("creationTimestamp") + "," +
               get_str("creationTransactionHash") + "," + 
               get_nested_id("collateralToken") + "," +
               get_nested("collateralToken", "name") + "," +
               get_nested("collateralToken", "symbol") + "," +
               get_nested("collateralToken", "decimals") + "," +
               get_str("conditionalTokenAddress") + "," + get_array_str("conditions") + "," +
               get_str("fee") + "," + get_str("tradesQuantity") + "," +
               get_str("buysQuantity") + "," + get_str("sellsQuantity") + "," +
               get_str("liquidityAddQuantity") + "," + get_str("liquidityRemoveQuantity") + "," +
               get_str("collateralVolume") + "," + get_str("scaledCollateralVolume") + "," +
               get_str("collateralBuyVolume") + "," + get_str("scaledCollateralBuyVolume") + "," +
               get_str("collateralSellVolume") + "," + get_str("scaledCollateralSellVolume") + "," +
               get_str("feeVolume") + "," + get_str("scaledFeeVolume") + "," +
               get_str("liquidityParameter") + "," + get_str("scaledLiquidityParameter") + "," +
               get_array_str("outcomeTokenAmounts") + "," + get_array_str("outcomeTokenPrices") + "," +
               get_int("outcomeSlotCount") + "," + get_int("lastActiveDay") + "," + get_str("totalSupply");
    }
    
    // ========================================================================
    // PnL subgraph
    // ========================================================================
    if (entity_ == "UserPosition") {
        return get_str("id") + "," + get_str("user") + "," + get_str("tokenId") + "," + 
               get_str("amount") + "," + get_str("avgPrice") + "," + 
               get_str("realizedPnl") + "," + get_str("totalBought");
    }
    if (entity_ == "FPMM") {
        return get_str("id") + "," + get_str("conditionId");
    }
    
    // ========================================================================
    // Orderbook subgraph
    // ========================================================================
    if (entity_ == "OrderFilledEvent") {
        return get_str("id") + "," + get_str("transactionHash") + "," + get_int("timestamp") + "," + 
               get_str("orderHash") + "," + get_str("maker") + "," + get_str("taker") + "," + 
               get_str("makerAssetId") + "," + get_str("takerAssetId") + "," + 
               get_str("makerAmountFilled") + "," + get_str("takerAmountFilled") + "," + get_str("fee");
    }
    if (entity_ == "OrdersMatchedEvent") {
        return get_str("id") + "," + get_int("timestamp") + "," + 
               get_str("makerAssetID") + "," + get_str("takerAssetID") + "," +
               get_str("makerAmountFilled") + "," + get_str("takerAmountFilled");
    }
    if (entity_ == "Orderbook") {
        return get_str("id") + "," + get_str("tradesQuantity") + "," + 
               get_str("buysQuantity") + "," + get_str("sellsQuantity") + "," + 
               get_str("collateralVolume") + "," + get_str("scaledCollateralVolume") + "," + 
               get_str("collateralBuyVolume") + "," + get_str("scaledCollateralBuyVolume") + "," + 
               get_str("collateralSellVolume") + "," + get_str("scaledCollateralSellVolume");
    }
    if (entity_ == "MarketData") {
        return get_str("id") + "," + get_str("condition") + "," + get_str("outcomeIndex");
    }
    if (entity_ == "OrdersMatchedGlobal") {
        return get_str("id") + "," + get_str("tradesQuantity") + "," + 
               get_str("buysQuantity") + "," + get_str("sellsQuantity") + "," + 
               get_str("collateralVolume") + "," + get_str("scaledCollateralVolume") + "," + 
               get_str("collateralBuyVolume") + "," + get_str("scaledCollateralBuyVolume") + "," + 
               get_str("collateralSellVolume") + "," + get_str("scaledCollateralSellVolume");
    }
    
    // ========================================================================
    // OI subgraph
    // ========================================================================
    if (entity_ == "MarketOpenInterest") {
        return get_str("id") + "," + get_str("amount");
    }
    if (entity_ == "GlobalOpenInterest") {
        return get_str("id") + "," + get_str("amount");
    }
    
    // ========================================================================
    // FPMM subgraph
    // ========================================================================
    if (entity_ == "Collateral") {
        return get_str("id") + "," + get_str("name") + "," + get_str("symbol") + "," + get_int("decimals");
    }
    if (entity_ == "FpmmFundingAddition") {
        return get_str("id") + "," + get_int("timestamp") + "," + get_nested_id("fpmm") + "," +
               get_str("funder") + "," + get_array_str("amountsAdded") + "," +
               get_array_str("amountsRefunded") + "," + get_str("sharesMinted");
    }
    if (entity_ == "FpmmFundingRemoval") {
        return get_str("id") + "," + get_int("timestamp") + "," + get_nested_id("fpmm") + "," +
               get_str("funder") + "," + get_array_str("amountsRemoved") + "," +
               get_str("collateralRemoved") + "," + get_str("sharesBurnt");
    }
    if (entity_ == "FpmmTransaction") {
        return get_str("id") + "," + get_str("type") + "," + get_int("timestamp") + "," + 
               get_nested_id("market") + "," + get_str("user") + "," + 
               get_str("tradeAmount") + "," + get_str("feeAmount") + "," + 
               get_str("outcomeIndex") + "," + get_str("outcomeTokensAmount");
    }
    if (entity_ == "FpmmPoolMembership") {
        return get_str("id") + "," + get_nested_id("pool") + "," + get_str("funder") + "," + get_str("amount");
    }
    
    // ========================================================================
    // Resolution subgraph
    // ========================================================================
    if (entity_ == "MarketResolution") {
        return get_str("id") + "," + get_bool("newVersionQ") + "," + get_str("author") + "," +
               get_str("ancillaryData") + "," + get_int("lastUpdateTimestamp") + "," +
               get_str("status") + "," + get_bool("wasDisputed") + "," +
               get_str("proposedPrice") + "," + get_str("reproposedPrice") + "," +
               get_str("price") + "," + get_str("updates") + "," +
               get_str("transactionHash") + "," + get_str("logIndex") + "," + get_bool("approved");
    }
    if (entity_ == "AncillaryDataHashToQuestionId") {
        return get_str("id") + "," + get_str("questionId");
    }
    if (entity_ == "Moderator") {
        return get_str("id") + "," + get_bool("canMod");
    }
    if (entity_ == "Revision") {
        return get_str("id") + "," + get_str("moderator") + "," + get_str("questionId") + "," +
               get_int("timestamp") + "," + get_str("update") + "," + get_str("transactionHash");
    }
    
    // ========================================================================
    // Positions subgraph
    // ========================================================================
    if (entity_ == "UserBalance") {
        return get_str("id") + "," + get_str("user") + "," + get_nested_id("asset") + "," + get_str("balance");
    }
    if (entity_ == "NetUserBalance") {
        return get_str("id") + "," + get_str("user") + "," + get_nested_id("asset") + "," + get_str("balance");
    }
    if (entity_ == "TokenIdCondition") {
        return get_str("id") + "," + get_nested_id("condition") + "," + 
               get_str("complement") + "," + get_str("outcomeIndex");
    }
    
    // ========================================================================
    // Fee-module subgraph
    // ========================================================================
    if (entity_ == "FeeRefundedEntity") {
        return get_str("id") + "," + get_str("orderHash") + "," + get_str("tokenId") + "," +
               get_int("timestamp") + "," + get_str("refundee") + "," +
               get_str("feeRefunded") + "," + get_str("feeCharged") + "," + get_bool("negRisk");
    }
    
    // ========================================================================
    // Wallet subgraph
    // ========================================================================
    if (entity_ == "Wallet") {
        return get_str("id") + "," + get_str("signer") + "," + get_str("type") + "," +
               get_str("balance") + "," + get_int("lastTransfer") + "," + get_int("createdAt");
    }
    if (entity_ == "GlobalUSDCBalance") {
        return get_str("id") + "," + get_str("balance");
    }
    
    // ========================================================================
    // Sports-oracle subgraph
    // ========================================================================
    if (entity_ == "Game") {
        return get_str("id") + "," + get_str("ancillaryData") + "," + get_str("ordering") + "," +
               get_str("state") + "," + get_str("homeScore") + "," + get_str("awayScore");
    }
    if (entity_ == "Market") {
        return get_str("id") + "," + get_str("gameId") + "," + get_str("state") + "," +
               get_str("marketType") + "," + get_str("underdog") + "," +
               get_str("line") + "," + get_array_str("payouts");
    }
    
    return "";
}

inline std::string EntityPuller::entity_to_table() {
    // Activity subgraph
    if (entity_ == "Split") return "split";
    if (entity_ == "Merge") return "merge";
    if (entity_ == "Redemption") return "redemption";
    if (entity_ == "NegRiskConversion") return "neg_risk_conversion";
    if (entity_ == "Position") return "position";
    
    // NegRiskEvent - 根据 subgraph 存不同表
    if (entity_ == "NegRiskEvent") {
        if (subgraph_name_ == "oi") return "neg_risk_event_oi";
        return "neg_risk_event";
    }
    
    // Condition - 根据 subgraph 存不同表
    if (entity_ == "Condition") {
        if (subgraph_name_ == "pnl") return "condition_pnl";
        if (subgraph_name_ == "positions") return "condition_positions";
        if (subgraph_name_ == "oi") return "condition_oi";
        if (subgraph_name_ == "fpmm") return "condition_fpmm";
        return "condition_activity";
    }
    
    // FixedProductMarketMaker - 根据 subgraph 存不同表（activity 只有 id）
    if (entity_ == "FixedProductMarketMaker") {
        if (subgraph_name_ == "activity") return "condition_activity";  // 复用 condition_activity 表
        return "fixed_product_market_maker";
    }
    
    // PnL subgraph
    if (entity_ == "UserPosition") return "user_position";
    if (entity_ == "FPMM") return "fpmm_pnl";
    
    // Orderbook subgraph
    if (entity_ == "OrderFilledEvent") return "order_filled_event";
    if (entity_ == "OrdersMatchedEvent") return "orders_matched_event";
    if (entity_ == "Orderbook") return "orderbook";
    if (entity_ == "MarketData") return "market_data";
    if (entity_ == "OrdersMatchedGlobal") return "orders_matched_global";
    
    // OI subgraph
    if (entity_ == "MarketOpenInterest") return "market_open_interest";
    if (entity_ == "GlobalOpenInterest") return "global_open_interest";
    
    // FPMM subgraph
    if (entity_ == "Collateral") return "collateral";
    if (entity_ == "FpmmFundingAddition") return "fpmm_funding_addition";
    if (entity_ == "FpmmFundingRemoval") return "fpmm_funding_removal";
    if (entity_ == "FpmmTransaction") return "fpmm_transaction";
    if (entity_ == "FpmmPoolMembership") return "fpmm_pool_membership";
    
    // Resolution subgraph
    if (entity_ == "MarketResolution") return "market_resolution";
    if (entity_ == "AncillaryDataHashToQuestionId") return "ancillary_data_hash_to_question_id";
    if (entity_ == "Moderator") return "moderator";
    if (entity_ == "Revision") return "revision";
    
    // Positions subgraph
    if (entity_ == "UserBalance") return "user_balance";
    if (entity_ == "NetUserBalance") return "net_user_balance";
    if (entity_ == "TokenIdCondition") return "token_id_condition";
    
    // Fee-module subgraph
    if (entity_ == "FeeRefundedEntity") return "fee_refunded_entity";
    
    // Wallet subgraph
    if (entity_ == "Wallet") return "wallet";
    if (entity_ == "GlobalUSDCBalance") return "global_usdc_balance";
    
    // Sports-oracle subgraph
    if (entity_ == "Game") return "game";
    if (entity_ == "Market") return "market";
    
    return entity_;
}

inline std::string EntityPuller::get_columns() {
    // Activity subgraph
    if (entity_ == "Split") return "id, timestamp, stakeholder, condition, amount";
    if (entity_ == "Merge") return "id, timestamp, stakeholder, condition, amount";
    if (entity_ == "Redemption") return "id, timestamp, redeemer, condition, index_sets, payout";
    if (entity_ == "NegRiskConversion") return "id, timestamp, stakeholder, neg_risk_market_id, amount, index_set, question_count";
    if (entity_ == "Position") return "id, condition, outcome_index";
    
    // NegRiskEvent
    if (entity_ == "NegRiskEvent") {
        if (subgraph_name_ == "oi") return "id, fee_bps, question_count";
        return "id, question_count, fee_bps";
    }
    
    // Condition - 根据 subgraph
    if (entity_ == "Condition") {
        if (subgraph_name_ == "pnl") return "id, position_ids, payout_numerators, payout_denominator";
        if (subgraph_name_ == "positions") return "id, payouts";
        return "id";  // activity/oi/fpmm
    }
    
    // FixedProductMarketMaker
    if (entity_ == "FixedProductMarketMaker") {
        if (subgraph_name_ == "activity") return "id";
        return "id, creator, creation_timestamp, creation_transaction_hash, "
               "collateral_token_id, collateral_token_name, collateral_token_symbol, collateral_token_decimals, "
               "conditional_token_address, conditions, fee, trades_quantity, "
               "buys_quantity, sells_quantity, liquidity_add_quantity, liquidity_remove_quantity, "
               "collateral_volume, scaled_collateral_volume, "
               "collateral_buy_volume, scaled_collateral_buy_volume, "
               "collateral_sell_volume, scaled_collateral_sell_volume, "
               "fee_volume, scaled_fee_volume, "
               "liquidity_parameter, scaled_liquidity_parameter, "
               "outcome_token_amounts, outcome_token_prices, outcome_slot_count, "
               "last_active_day, total_supply";
    }
    
    // PnL subgraph
    if (entity_ == "UserPosition") return "id, user_addr, token_id, amount, avg_price, realized_pnl, total_bought";
    if (entity_ == "FPMM") return "id, condition_id";
    
    // Orderbook subgraph
    if (entity_ == "OrderFilledEvent") return "id, transaction_hash, timestamp, order_hash, maker, taker, maker_asset_id, taker_asset_id, maker_amount_filled, taker_amount_filled, fee";
    if (entity_ == "OrdersMatchedEvent") return "id, timestamp, maker_asset_id, taker_asset_id, maker_amount_filled, taker_amount_filled";
    if (entity_ == "Orderbook") return "id, trades_quantity, buys_quantity, sells_quantity, collateral_volume, scaled_collateral_volume, collateral_buy_volume, scaled_collateral_buy_volume, collateral_sell_volume, scaled_collateral_sell_volume";
    if (entity_ == "MarketData") return "id, condition, outcome_index";
    if (entity_ == "OrdersMatchedGlobal") return "id, trades_quantity, buys_quantity, sells_quantity, collateral_volume, scaled_collateral_volume, collateral_buy_volume, scaled_collateral_buy_volume, collateral_sell_volume, scaled_collateral_sell_volume";
    
    // OI subgraph
    if (entity_ == "MarketOpenInterest") return "id, amount";
    if (entity_ == "GlobalOpenInterest") return "id, amount";
    
    // FPMM subgraph
    if (entity_ == "Collateral") return "id, name, symbol, decimals";
    if (entity_ == "FpmmFundingAddition") return "id, timestamp, fpmm_id, funder, amounts_added, amounts_refunded, shares_minted";
    if (entity_ == "FpmmFundingRemoval") return "id, timestamp, fpmm_id, funder, amounts_removed, collateral_removed, shares_burnt";
    if (entity_ == "FpmmTransaction") return "id, type, timestamp, market_id, user_addr, trade_amount, fee_amount, outcome_index, outcome_tokens_amount";
    if (entity_ == "FpmmPoolMembership") return "id, pool_id, funder, amount";
    
    // Resolution subgraph
    if (entity_ == "MarketResolution") return "id, new_version_q, author, ancillary_data, last_update_timestamp, status, was_disputed, proposed_price, reproposed_price, price, updates, transaction_hash, log_index, approved";
    if (entity_ == "AncillaryDataHashToQuestionId") return "id, question_id";
    if (entity_ == "Moderator") return "id, can_mod";
    if (entity_ == "Revision") return "id, moderator, question_id, timestamp, update_text, transaction_hash";
    
    // Positions subgraph
    if (entity_ == "UserBalance") return "id, user_addr, asset_id, balance";
    if (entity_ == "NetUserBalance") return "id, user_addr, asset_id, balance";
    if (entity_ == "TokenIdCondition") return "id, condition_id, complement, outcome_index";
    
    // Fee-module subgraph
    if (entity_ == "FeeRefundedEntity") return "id, order_hash, token_id, timestamp, refundee, fee_refunded, fee_charged, neg_risk";
    
    // Wallet subgraph
    if (entity_ == "Wallet") return "id, signer, type, balance, last_transfer, created_at";
    if (entity_ == "GlobalUSDCBalance") return "id, balance";
    
    // Sports-oracle subgraph
    if (entity_ == "Game") return "id, ancillary_data, ordering, state, home_score, away_score";
    if (entity_ == "Market") return "id, game_id, state, market_type, underdog, line, payouts";
    
    return "id";
}

// ============================================================================
// SubgraphScheduler 实现
// ============================================================================
inline void SubgraphScheduler::start(const std::string& api_key) {
    api_key_ = api_key;
    std::cout << "[Scheduler] " << subgraph_name_ << " 启动，共 " 
              << pullers_.size() << " 个 entity" << std::endl;
    
    // 启动尽可能多的 puller
    while (next_idx_ < pullers_.size() && 
           active_count_ < PARALLEL_PER_SUBGRAPH && 
           puller_->try_acquire_slot()) {
        pullers_[next_idx_].start(api_key_);
        ++active_count_;
        ++next_idx_;
    }
}

inline void SubgraphScheduler::on_entity_done(EntityPuller* puller) {
    if (puller != nullptr) {
        --active_count_;
        puller_->release_slot();
    }
    
    // 尝试启动下一个
    start_next();
}

inline void SubgraphScheduler::start_next() {
    while (next_idx_ < pullers_.size() && 
           active_count_ < PARALLEL_PER_SUBGRAPH && 
           puller_->try_acquire_slot()) {
        pullers_[next_idx_].start(api_key_);
        ++active_count_;
        ++next_idx_;
    }
}
