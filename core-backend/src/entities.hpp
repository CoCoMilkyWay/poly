#pragma once

// ============================================================================
// Entity 定义
// 每个 entity 包含：GraphQL 字段、DDL、转换函数
// ============================================================================

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace entities {

// ============================================================================
// 工具函数
// ============================================================================

// SQL 字符串转义(不带引号)
inline std::string escape_sql_raw(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\'') r += "''";
        else r += c;
    }
    return r;
}

// SQL 字符串转义(带引号)
inline std::string escape_sql(const std::string& s) {
    return "'" + escape_sql_raw(s) + "'";
}

// JSON 提取：字符串
inline std::string json_str(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return "NULL";
    if (j[key].is_string()) return escape_sql(j[key].get<std::string>());
    return escape_sql(j[key].dump());
}

// JSON 提取：整数
inline std::string json_int(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return "NULL";
    if (j[key].is_number()) return std::to_string(j[key].get<int64_t>());
    if (j[key].is_string()) return j[key].get<std::string>();
    return "NULL";
}

// JSON 提取：布尔
inline std::string json_bool(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return "NULL";
    if (j[key].is_boolean()) return j[key].get<bool>() ? "TRUE" : "FALSE";
    return "NULL";
}

// JSON 提取：浮点
inline std::string json_decimal(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return "NULL";
    if (j[key].is_number()) return std::to_string(j[key].get<double>());
    if (j[key].is_string()) return j[key].get<std::string>();
    return "NULL";
}

// JSON 提取：嵌套对象的 id
inline std::string json_ref(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return "NULL";
    if (j[key].is_object() && j[key].contains("id")) {
        return escape_sql(j[key]["id"].get<std::string>());
    }
    return "NULL";
}

// JSON 提取：数组(序列化为 JSON 字符串)
inline std::string json_array(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return "NULL";
    return escape_sql(j[key].dump());
}

// ============================================================================
// 基础设施：同步状态表
// ============================================================================

inline const char* SYNC_STATE_DDL = R"(
CREATE TABLE IF NOT EXISTS sync_state (
    source VARCHAR NOT NULL,
    entity VARCHAR NOT NULL,
    last_id VARCHAR,
    last_sync_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    total_synced BIGINT DEFAULT 0,
    PRIMARY KEY (source, entity)
))";

// ============================================================================
// Entity 定义结构
// ============================================================================

struct EntityDef {
    const char* name;           // Entity 名称(GraphQL 单数)
    const char* plural;         // GraphQL 复数形式
    const char* table;          // 数据库表名
    const char* fields;         // GraphQL 查询字段
    const char* ddl;            // CREATE TABLE 语句
    const char* columns;        // INSERT 列名
    std::string (*to_values)(const json&);  // JSON 转 SQL values
};

// ============================================================================
// Main Subgraph Entities
// ============================================================================

// ----------------------------------------------------------------------------
// Condition - 条件
// ----------------------------------------------------------------------------
inline std::string condition_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_str(j, "oracle") + "," +
           json_str(j, "questionId") + "," +
           json_int(j, "outcomeSlotCount") + "," +
           json_int(j, "resolutionTimestamp") + "," +
           json_array(j, "payoutNumerators") + "," +
           json_int(j, "payoutDenominator") + "," +
           json_str(j, "resolutionHash");
}

inline const EntityDef Condition = {
    .name    = "Condition",
    .plural  = "conditions",
    .table   = "condition",
    .fields  = "id oracle questionId outcomeSlotCount resolutionTimestamp payoutNumerators payoutDenominator resolutionHash",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS condition (
        id VARCHAR PRIMARY KEY,
        oracle VARCHAR,
        question_id VARCHAR,
        outcome_slot_count INT,
        resolution_timestamp BIGINT,
        payout_numerators VARCHAR,
        payout_denominator VARCHAR,
        resolution_hash VARCHAR
    ))",
    .columns = "id, oracle, question_id, outcome_slot_count, resolution_timestamp, payout_numerators, payout_denominator, resolution_hash",
    .to_values = condition_to_values
};

// ----------------------------------------------------------------------------
// Split - 拆分
// ----------------------------------------------------------------------------
inline std::string split_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_int(j, "timestamp") + "," +
           json_ref(j, "stakeholder") + "," +
           json_ref(j, "collateralToken") + "," +
           json_str(j, "parentCollectionId") + "," +
           json_ref(j, "condition") + "," +
           json_array(j, "partition") + "," +
           json_int(j, "amount");
}

inline const EntityDef Split = {
    .name    = "Split",
    .plural  = "splits",
    .table   = "split",
    .fields  = "id timestamp stakeholder{id} collateralToken{id} parentCollectionId condition{id} partition amount",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS split (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT,
        stakeholder VARCHAR,
        collateral_token VARCHAR,
        parent_collection_id VARCHAR,
        condition_id VARCHAR,
        partition VARCHAR,
        amount VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_split_ts ON split(timestamp);
    CREATE INDEX IF NOT EXISTS idx_split_user ON split(stakeholder))",
    .columns = "id, timestamp, stakeholder, collateral_token, parent_collection_id, condition_id, partition, amount",
    .to_values = split_to_values
};

// ----------------------------------------------------------------------------
// Merge - 合并
// ----------------------------------------------------------------------------
inline std::string merge_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_int(j, "timestamp") + "," +
           json_ref(j, "stakeholder") + "," +
           json_ref(j, "collateralToken") + "," +
           json_str(j, "parentCollectionId") + "," +
           json_ref(j, "condition") + "," +
           json_array(j, "partition") + "," +
           json_int(j, "amount");
}

inline const EntityDef Merge = {
    .name    = "Merge",
    .plural  = "merges",
    .table   = "merge",
    .fields  = "id timestamp stakeholder{id} collateralToken{id} parentCollectionId condition{id} partition amount",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS merge (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT,
        stakeholder VARCHAR,
        collateral_token VARCHAR,
        parent_collection_id VARCHAR,
        condition_id VARCHAR,
        partition VARCHAR,
        amount VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_merge_ts ON merge(timestamp);
    CREATE INDEX IF NOT EXISTS idx_merge_user ON merge(stakeholder))",
    .columns = "id, timestamp, stakeholder, collateral_token, parent_collection_id, condition_id, partition, amount",
    .to_values = merge_to_values
};

// ----------------------------------------------------------------------------
// Redemption - 赎回
// ----------------------------------------------------------------------------
inline std::string redemption_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_int(j, "timestamp") + "," +
           json_ref(j, "redeemer") + "," +
           json_ref(j, "collateralToken") + "," +
           json_str(j, "parentCollectionId") + "," +
           json_ref(j, "condition") + "," +
           json_array(j, "indexSets") + "," +
           json_int(j, "payout");
}

inline const EntityDef Redemption = {
    .name    = "Redemption",
    .plural  = "redemptions",
    .table   = "redemption",
    .fields  = "id timestamp redeemer{id} collateralToken{id} parentCollectionId condition{id} indexSets payout",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS redemption (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT,
        redeemer VARCHAR,
        collateral_token VARCHAR,
        parent_collection_id VARCHAR,
        condition_id VARCHAR,
        index_sets VARCHAR,
        payout VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_redemption_ts ON redemption(timestamp);
    CREATE INDEX IF NOT EXISTS idx_redemption_user ON redemption(redeemer))",
    .columns = "id, timestamp, redeemer, collateral_token, parent_collection_id, condition_id, index_sets, payout",
    .to_values = redemption_to_values
};

// ----------------------------------------------------------------------------
// Account - 账户
// ----------------------------------------------------------------------------
inline std::string account_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_int(j, "creationTimestamp") + "," +
           json_int(j, "lastSeenTimestamp") + "," +
           json_int(j, "collateralVolume") + "," +
           json_int(j, "numTrades") + "," +
           json_decimal(j, "scaledCollateralVolume") + "," +
           json_int(j, "lastTradedTimestamp") + "," +
           json_int(j, "profit") + "," +
           json_decimal(j, "scaledProfit");
}

inline const EntityDef Account = {
    .name    = "Account",
    .plural  = "accounts",
    .table   = "account",
    .fields  = "id creationTimestamp lastSeenTimestamp collateralVolume numTrades scaledCollateralVolume lastTradedTimestamp profit scaledProfit",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS account (
        id VARCHAR PRIMARY KEY,
        creation_timestamp BIGINT,
        last_seen_timestamp BIGINT,
        collateral_volume VARCHAR,
        num_trades VARCHAR,
        scaled_collateral_volume DOUBLE,
        last_traded_timestamp BIGINT,
        profit VARCHAR,
        scaled_profit DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_account_profit ON account(profit))",
    .columns = "id, creation_timestamp, last_seen_timestamp, collateral_volume, num_trades, scaled_collateral_volume, last_traded_timestamp, profit, scaled_profit",
    .to_values = account_to_values
};

// ----------------------------------------------------------------------------
// EnrichedOrderFilled - 订单成交
// ----------------------------------------------------------------------------
inline std::string enriched_order_filled_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_str(j, "transactionHash") + "," +
           json_int(j, "timestamp") + "," +
           json_ref(j, "maker") + "," +
           json_ref(j, "taker") + "," +
           json_str(j, "orderHash") + "," +
           json_ref(j, "market") + "," +
           json_str(j, "side") + "," +
           json_int(j, "size") + "," +
           json_decimal(j, "price");
}

inline const EntityDef EnrichedOrderFilled = {
    .name    = "EnrichedOrderFilled",
    .plural  = "enrichedOrderFilleds",
    .table   = "enriched_order_filled",
    .fields  = "id transactionHash timestamp maker{id} taker{id} orderHash market{id} side size price",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS enriched_order_filled (
        id VARCHAR PRIMARY KEY,
        transaction_hash VARCHAR,
        timestamp BIGINT,
        maker VARCHAR,
        taker VARCHAR,
        order_hash VARCHAR,
        market_id VARCHAR,
        side VARCHAR,
        size VARCHAR,
        price DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_eof_ts ON enriched_order_filled(timestamp);
    CREATE INDEX IF NOT EXISTS idx_eof_maker ON enriched_order_filled(maker);
    CREATE INDEX IF NOT EXISTS idx_eof_market ON enriched_order_filled(market_id))",
    .columns = "id, transaction_hash, timestamp, maker, taker, order_hash, market_id, side, size, price",
    .to_values = enriched_order_filled_to_values
};

// ----------------------------------------------------------------------------
// Orderbook - 订单簿
// ----------------------------------------------------------------------------
inline std::string orderbook_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_int(j, "tradesQuantity") + "," +
           json_int(j, "buysQuantity") + "," +
           json_int(j, "sellsQuantity") + "," +
           json_int(j, "collateralVolume") + "," +
           json_decimal(j, "scaledCollateralVolume") + "," +
           json_int(j, "collateralBuyVolume") + "," +
           json_decimal(j, "scaledCollateralBuyVolume") + "," +
           json_int(j, "collateralSellVolume") + "," +
           json_decimal(j, "scaledCollateralSellVolume") + "," +
           json_int(j, "lastActiveDay");
}

inline const EntityDef Orderbook = {
    .name    = "Orderbook",
    .plural  = "orderbooks",
    .table   = "orderbook",
    .fields  = "id tradesQuantity buysQuantity sellsQuantity collateralVolume scaledCollateralVolume collateralBuyVolume scaledCollateralBuyVolume collateralSellVolume scaledCollateralSellVolume lastActiveDay",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS orderbook (
        id VARCHAR PRIMARY KEY,
        trades_quantity VARCHAR,
        buys_quantity VARCHAR,
        sells_quantity VARCHAR,
        collateral_volume VARCHAR,
        scaled_collateral_volume DOUBLE,
        collateral_buy_volume VARCHAR,
        scaled_collateral_buy_volume DOUBLE,
        collateral_sell_volume VARCHAR,
        scaled_collateral_sell_volume DOUBLE,
        last_active_day BIGINT
    ))",
    .columns = "id, trades_quantity, buys_quantity, sells_quantity, collateral_volume, scaled_collateral_volume, collateral_buy_volume, scaled_collateral_buy_volume, collateral_sell_volume, scaled_collateral_sell_volume, last_active_day",
    .to_values = orderbook_to_values
};

// ----------------------------------------------------------------------------
// MarketData - 市场数据
// ----------------------------------------------------------------------------
inline std::string market_data_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_ref(j, "condition") + "," +
           json_int(j, "outcomeIndex") + "," +
           json_ref(j, "fpmm") + "," +
           json_decimal(j, "priceOrderbook");
}

inline const EntityDef MarketData = {
    .name    = "MarketData",
    .plural  = "marketDatas",
    .table   = "market_data",
    .fields  = "id condition{id} outcomeIndex fpmm{id} priceOrderbook",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS market_data (
        id VARCHAR PRIMARY KEY,
        condition_id VARCHAR,
        outcome_index INT,
        fpmm_id VARCHAR,
        price_orderbook DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_md_cond ON market_data(condition_id))",
    .columns = "id, condition_id, outcome_index, fpmm_id, price_orderbook",
    .to_values = market_data_to_values
};

// ----------------------------------------------------------------------------
// MarketPosition - 市场持仓
// ----------------------------------------------------------------------------
inline std::string market_position_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_ref(j, "market") + "," +
           json_ref(j, "user") + "," +
           json_int(j, "quantityBought") + "," +
           json_int(j, "quantitySold") + "," +
           json_int(j, "netQuantity") + "," +
           json_int(j, "valueBought") + "," +
           json_int(j, "valueSold") + "," +
           json_int(j, "netValue") + "," +
           json_int(j, "feesPaid");
}

inline const EntityDef MarketPosition = {
    .name    = "MarketPosition",
    .plural  = "marketPositions",
    .table   = "market_position",
    .fields  = "id market{id} user{id} quantityBought quantitySold netQuantity valueBought valueSold netValue feesPaid",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS market_position (
        id VARCHAR PRIMARY KEY,
        market_id VARCHAR,
        user_id VARCHAR,
        quantity_bought VARCHAR,
        quantity_sold VARCHAR,
        net_quantity VARCHAR,
        value_bought VARCHAR,
        value_sold VARCHAR,
        net_value VARCHAR,
        fees_paid VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_mp_user ON market_position(user_id);
    CREATE INDEX IF NOT EXISTS idx_mp_market ON market_position(market_id))",
    .columns = "id, market_id, user_id, quantity_bought, quantity_sold, net_quantity, value_bought, value_sold, net_value, fees_paid",
    .to_values = market_position_to_values
};

// ----------------------------------------------------------------------------
// MarketProfit - 市场盈亏
// ----------------------------------------------------------------------------
inline std::string market_profit_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_ref(j, "user") + "," +
           json_ref(j, "condition") + "," +
           json_int(j, "profit") + "," +
           json_decimal(j, "scaledProfit");
}

inline const EntityDef MarketProfit = {
    .name    = "MarketProfit",
    .plural  = "marketProfits",
    .table   = "market_profit",
    .fields  = "id user{id} condition{id} profit scaledProfit",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS market_profit (
        id VARCHAR PRIMARY KEY,
        user_id VARCHAR,
        condition_id VARCHAR,
        profit VARCHAR,
        scaled_profit DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_mprofit_user ON market_profit(user_id))",
    .columns = "id, user_id, condition_id, profit, scaled_profit",
    .to_values = market_profit_to_values
};

// ----------------------------------------------------------------------------
// FixedProductMarketMaker - FPMM 做市商
// ----------------------------------------------------------------------------
inline std::string fpmm_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_str(j, "creator") + "," +
           json_int(j, "creationTimestamp") + "," +
           json_str(j, "creationTransactionHash") + "," +
           json_ref(j, "collateralToken") + "," +
           json_str(j, "conditionalTokenAddress") + "," +
           json_array(j, "conditions") + "," +
           json_int(j, "fee") + "," +
           json_int(j, "tradesQuantity") + "," +
           json_int(j, "buysQuantity") + "," +
           json_int(j, "sellsQuantity") + "," +
           json_int(j, "liquidityAddQuantity") + "," +
           json_int(j, "liquidityRemoveQuantity") + "," +
           json_int(j, "collateralVolume") + "," +
           json_decimal(j, "scaledCollateralVolume") + "," +
           json_int(j, "collateralBuyVolume") + "," +
           json_decimal(j, "scaledCollateralBuyVolume") + "," +
           json_int(j, "collateralSellVolume") + "," +
           json_decimal(j, "scaledCollateralSellVolume") + "," +
           json_int(j, "feeVolume") + "," +
           json_decimal(j, "scaledFeeVolume") + "," +
           json_int(j, "liquidityParameter") + "," +
           json_decimal(j, "scaledLiquidityParameter") + "," +
           json_array(j, "outcomeTokenAmounts") + "," +
           json_array(j, "outcomeTokenPrices") + "," +
           json_int(j, "outcomeSlotCount") + "," +
           json_int(j, "lastActiveDay") + "," +
           json_int(j, "totalSupply");
}

inline const EntityDef FixedProductMarketMaker = {
    .name    = "FixedProductMarketMaker",
    .plural  = "fixedProductMarketMakers",
    .table   = "fpmm",
    .fields  = "id creator creationTimestamp creationTransactionHash collateralToken{id} conditionalTokenAddress conditions fee tradesQuantity buysQuantity sellsQuantity liquidityAddQuantity liquidityRemoveQuantity collateralVolume scaledCollateralVolume collateralBuyVolume scaledCollateralBuyVolume collateralSellVolume scaledCollateralSellVolume feeVolume scaledFeeVolume liquidityParameter scaledLiquidityParameter outcomeTokenAmounts outcomeTokenPrices outcomeSlotCount lastActiveDay totalSupply",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS fpmm (
        id VARCHAR PRIMARY KEY,
        creator VARCHAR,
        creation_timestamp BIGINT,
        creation_transaction_hash VARCHAR,
        collateral_token VARCHAR,
        conditional_token_address VARCHAR,
        conditions VARCHAR,
        fee VARCHAR,
        trades_quantity VARCHAR,
        buys_quantity VARCHAR,
        sells_quantity VARCHAR,
        liquidity_add_quantity VARCHAR,
        liquidity_remove_quantity VARCHAR,
        collateral_volume VARCHAR,
        scaled_collateral_volume DOUBLE,
        collateral_buy_volume VARCHAR,
        scaled_collateral_buy_volume DOUBLE,
        collateral_sell_volume VARCHAR,
        scaled_collateral_sell_volume DOUBLE,
        fee_volume VARCHAR,
        scaled_fee_volume DOUBLE,
        liquidity_parameter VARCHAR,
        scaled_liquidity_parameter DOUBLE,
        outcome_token_amounts VARCHAR,
        outcome_token_prices VARCHAR,
        outcome_slot_count INT,
        last_active_day BIGINT,
        total_supply VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_fpmm_creator ON fpmm(creator))",
    .columns = "id, creator, creation_timestamp, creation_transaction_hash, collateral_token, conditional_token_address, conditions, fee, trades_quantity, buys_quantity, sells_quantity, liquidity_add_quantity, liquidity_remove_quantity, collateral_volume, scaled_collateral_volume, collateral_buy_volume, scaled_collateral_buy_volume, collateral_sell_volume, scaled_collateral_sell_volume, fee_volume, scaled_fee_volume, liquidity_parameter, scaled_liquidity_parameter, outcome_token_amounts, outcome_token_prices, outcome_slot_count, last_active_day, total_supply",
    .to_values = fpmm_to_values
};

// ----------------------------------------------------------------------------
// Transaction - FPMM 交易
// ----------------------------------------------------------------------------
inline std::string transaction_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_str(j, "type") + "," +
           json_int(j, "timestamp") + "," +
           json_ref(j, "market") + "," +
           json_ref(j, "user") + "," +
           json_int(j, "tradeAmount") + "," +
           json_int(j, "feeAmount") + "," +
           json_int(j, "outcomeIndex") + "," +
           json_int(j, "outcomeTokensAmount");
}

inline const EntityDef Transaction = {
    .name    = "Transaction",
    .plural  = "transactions",
    .table   = "fpmm_transaction",
    .fields  = "id type timestamp market{id} user{id} tradeAmount feeAmount outcomeIndex outcomeTokensAmount",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS fpmm_transaction (
        id VARCHAR PRIMARY KEY,
        type VARCHAR,
        timestamp BIGINT,
        market_id VARCHAR,
        user_id VARCHAR,
        trade_amount VARCHAR,
        fee_amount VARCHAR,
        outcome_index INT,
        outcome_tokens_amount VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_ftx_ts ON fpmm_transaction(timestamp);
    CREATE INDEX IF NOT EXISTS idx_ftx_user ON fpmm_transaction(user_id);
    CREATE INDEX IF NOT EXISTS idx_ftx_market ON fpmm_transaction(market_id))",
    .columns = "id, type, timestamp, market_id, user_id, trade_amount, fee_amount, outcome_index, outcome_tokens_amount",
    .to_values = transaction_to_values
};

// ----------------------------------------------------------------------------
// Global - 全局统计
// ----------------------------------------------------------------------------
inline std::string global_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_int(j, "numConditions") + "," +
           json_int(j, "numOpenConditions") + "," +
           json_int(j, "numClosedConditions") + "," +
           json_int(j, "numTraders") + "," +
           json_int(j, "tradesQuantity") + "," +
           json_int(j, "buysQuantity") + "," +
           json_int(j, "sellsQuantity") + "," +
           json_int(j, "collateralVolume") + "," +
           json_decimal(j, "scaledCollateralVolume") + "," +
           json_int(j, "collateralFees") + "," +
           json_decimal(j, "scaledCollateralFees") + "," +
           json_int(j, "collateralBuyVolume") + "," +
           json_decimal(j, "scaledCollateralBuyVolume") + "," +
           json_int(j, "collateralSellVolume") + "," +
           json_decimal(j, "scaledCollateralSellVolume");
}

inline const EntityDef Global = {
    .name    = "Global",
    .plural  = "globals",
    .table   = "global",
    .fields  = "id numConditions numOpenConditions numClosedConditions numTraders tradesQuantity buysQuantity sellsQuantity collateralVolume scaledCollateralVolume collateralFees scaledCollateralFees collateralBuyVolume scaledCollateralBuyVolume collateralSellVolume scaledCollateralSellVolume",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS global (
        id VARCHAR PRIMARY KEY,
        num_conditions INT,
        num_open_conditions INT,
        num_closed_conditions INT,
        num_traders VARCHAR,
        trades_quantity VARCHAR,
        buys_quantity VARCHAR,
        sells_quantity VARCHAR,
        collateral_volume VARCHAR,
        scaled_collateral_volume DOUBLE,
        collateral_fees VARCHAR,
        scaled_collateral_fees DOUBLE,
        collateral_buy_volume VARCHAR,
        scaled_collateral_buy_volume DOUBLE,
        collateral_sell_volume VARCHAR,
        scaled_collateral_sell_volume DOUBLE
    ))",
    .columns = "id, num_conditions, num_open_conditions, num_closed_conditions, num_traders, trades_quantity, buys_quantity, sells_quantity, collateral_volume, scaled_collateral_volume, collateral_fees, scaled_collateral_fees, collateral_buy_volume, scaled_collateral_buy_volume, collateral_sell_volume, scaled_collateral_sell_volume",
    .to_values = global_to_values
};

// ============================================================================
// PnL Subgraph Entities
// ============================================================================

// ----------------------------------------------------------------------------
// UserPosition - 用户持仓
// ----------------------------------------------------------------------------
inline std::string user_position_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_str(j, "user") + "," +
           json_int(j, "tokenId") + "," +
           json_int(j, "amount") + "," +
           json_int(j, "avgPrice") + "," +
           json_int(j, "realizedPnl") + "," +
           json_int(j, "totalBought");
}

inline const EntityDef UserPosition = {
    .name    = "UserPosition",
    .plural  = "userPositions",
    .table   = "user_position",
    .fields  = "id user tokenId amount avgPrice realizedPnl totalBought",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS user_position (
        id VARCHAR PRIMARY KEY,
        user_addr VARCHAR,
        token_id VARCHAR,
        amount VARCHAR,
        avg_price VARCHAR,
        realized_pnl VARCHAR,
        total_bought VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_up_user ON user_position(user_addr);
    CREATE INDEX IF NOT EXISTS idx_up_token ON user_position(token_id))",
    .columns = "id, user_addr, token_id, amount, avg_price, realized_pnl, total_bought",
    .to_values = user_position_to_values
};

// ----------------------------------------------------------------------------
// PnlCondition - PnL 条件(简化版)
// ----------------------------------------------------------------------------
inline std::string pnl_condition_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_array(j, "positionIds") + "," +
           json_array(j, "payoutNumerators") + "," +
           json_int(j, "payoutDenominator");
}

inline const EntityDef PnlCondition = {
    .name    = "Condition",
    .plural  = "conditions",
    .table   = "pnl_condition",
    .fields  = "id positionIds payoutNumerators payoutDenominator",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS pnl_condition (
        id VARCHAR PRIMARY KEY,
        position_ids VARCHAR,
        payout_numerators VARCHAR,
        payout_denominator VARCHAR
    ))",
    .columns = "id, position_ids, payout_numerators, payout_denominator",
    .to_values = pnl_condition_to_values
};

// ----------------------------------------------------------------------------
// NegRiskEvent - 负风险事件
// ----------------------------------------------------------------------------
inline std::string neg_risk_event_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_int(j, "questionCount");
}

inline const EntityDef NegRiskEvent = {
    .name    = "NegRiskEvent",
    .plural  = "negRiskEvents",
    .table   = "neg_risk_event",
    .fields  = "id questionCount",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS neg_risk_event (
        id VARCHAR PRIMARY KEY,
        question_count INT
    ))",
    .columns = "id, question_count",
    .to_values = neg_risk_event_to_values
};

// ----------------------------------------------------------------------------
// PnlFPMM - PnL FPMM(简化版)
// ----------------------------------------------------------------------------
inline std::string pnl_fpmm_to_values(const json& j) {
    return json_str(j, "id") + "," +
           json_str(j, "conditionId");
}

inline const EntityDef PnlFPMM = {
    .name    = "FPMM",
    .plural  = "fpmms",
    .table   = "pnl_fpmm",
    .fields  = "id conditionId",
    .ddl     = R"(CREATE TABLE IF NOT EXISTS pnl_fpmm (
        id VARCHAR PRIMARY KEY,
        condition_id VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_pnl_fpmm_cond ON pnl_fpmm(condition_id))",
    .columns = "id, condition_id",
    .to_values = pnl_fpmm_to_values
};

// ============================================================================
// Entity 注册表
// ============================================================================

inline const EntityDef* MAIN_ENTITIES[] = {
    &Condition, &Split, &Merge, &Redemption,
    &Account, &EnrichedOrderFilled, &Orderbook,
    &MarketData, &MarketPosition, &MarketProfit,
    &FixedProductMarketMaker, &Transaction, &Global
};
inline constexpr size_t MAIN_ENTITY_COUNT = sizeof(MAIN_ENTITIES) / sizeof(MAIN_ENTITIES[0]);

inline const EntityDef* PNL_ENTITIES[] = {
    &UserPosition, &PnlCondition, &NegRiskEvent, &PnlFPMM
};
inline constexpr size_t PNL_ENTITY_COUNT = sizeof(PNL_ENTITIES) / sizeof(PNL_ENTITIES[0]);

// 查找 entity
inline const EntityDef* find_entity(const char* name, const EntityDef* const* list, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (std::string(list[i]->name) == name) {
            return list[i];
        }
    }
    return nullptr;
}

} // namespace entities
