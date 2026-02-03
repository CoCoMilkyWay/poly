#pragma once

#include <string>
#include <vector>
#include <cstdint>

// 所有 Entity 的 C++ 结构定义

namespace schema {

// Activity subgraph
struct Split {
    std::string id;
    int64_t timestamp;
    std::string stakeholder;
    std::string condition;
    std::string amount;  // BigInt 用字符串存储
};

struct Merge {
    std::string id;
    int64_t timestamp;
    std::string stakeholder;
    std::string condition;
    std::string amount;
};

struct Redemption {
    std::string id;
    int64_t timestamp;
    std::string redeemer;
    std::string condition;
    std::vector<std::string> indexSets;
    std::string payout;
};

struct NegRiskConversion {
    std::string id;
    int64_t timestamp;
    std::string stakeholder;
    std::string negRiskMarketId;
    std::string amount;
    std::string indexSet;
    int questionCount;
};

// PnL subgraph
struct UserPosition {
    std::string id;
    std::string user;
    std::string tokenId;
    std::string amount;
    std::string avgPrice;
    std::string realizedPnl;
    std::string totalBought;
};

struct NegRiskEvent {
    std::string id;
    int questionCount;
};

struct Condition {
    std::string id;
};

struct ConditionPnl {
    std::string id;
    std::vector<std::string> positionIds;
    std::vector<std::string> payoutNumerators;
    std::string payoutDenominator;
};

// Orderbook subgraph
struct OrderFilledEvent {
    std::string id;
    std::string transactionHash;
    int64_t timestamp;
    std::string orderHash;
    std::string maker;
    std::string taker;
    std::string makerAssetId;
    std::string takerAssetId;
    std::string makerAmountFilled;
    std::string takerAmountFilled;
    std::string fee;
};

struct Orderbook {
    std::string id;
    std::string tradesQuantity;
    std::string buysQuantity;
    std::string sellsQuantity;
    std::string collateralVolume;
    std::string scaledCollateralVolume;
    std::string collateralBuyVolume;
    std::string scaledCollateralBuyVolume;
    std::string collateralSellVolume;
    std::string scaledCollateralSellVolume;
};

struct MarketData {
    std::string id;
    std::string condition;
    std::string outcomeIndex;
};

// OI subgraph
struct MarketOpenInterest {
    std::string id;
    std::string amount;
};

struct GlobalOpenInterest {
    std::string id;
    std::string amount;
};

// FPMM subgraph
struct FixedProductMarketMaker {
    std::string id;
    std::string creator;
    int64_t creationTimestamp;
    std::string fee;
    std::string tradesQuantity;
    std::string buysQuantity;
    std::string sellsQuantity;
    std::string collateralVolume;
    std::string scaledCollateralVolume;
    std::string liquidityParameter;
};

struct FpmmTransaction {
    std::string id;
    std::string type;  // Buy/Sell
    int64_t timestamp;
    std::string user;
    std::string tradeAmount;
    std::string feeAmount;
    std::string outcomeIndex;
    std::string outcomeTokensAmount;
};

// DuckDB 建表 SQL
namespace ddl {

inline const char* SYNC_STATE = R"(
CREATE TABLE IF NOT EXISTS sync_state (
    subgraph_id VARCHAR,
    entity_name VARCHAR,
    last_id VARCHAR,
    last_sync_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (subgraph_id, entity_name)
);
)";

inline const char* SPLIT = R"(
CREATE TABLE IF NOT EXISTS split (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT,
    stakeholder VARCHAR,
    condition VARCHAR,
    amount VARCHAR
);
)";

inline const char* MERGE = R"(
CREATE TABLE IF NOT EXISTS merge (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT,
    stakeholder VARCHAR,
    condition VARCHAR,
    amount VARCHAR
);
)";

inline const char* REDEMPTION = R"(
CREATE TABLE IF NOT EXISTS redemption (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT,
    redeemer VARCHAR,
    condition VARCHAR,
    index_sets VARCHAR,
    payout VARCHAR
);
)";

inline const char* NEG_RISK_CONVERSION = R"(
CREATE TABLE IF NOT EXISTS neg_risk_conversion (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT,
    stakeholder VARCHAR,
    neg_risk_market_id VARCHAR,
    amount VARCHAR,
    index_set VARCHAR,
    question_count INTEGER
);
)";

inline const char* USER_POSITION = R"(
CREATE TABLE IF NOT EXISTS user_position (
    id VARCHAR PRIMARY KEY,
    user_addr VARCHAR,
    token_id VARCHAR,
    amount VARCHAR,
    avg_price VARCHAR,
    realized_pnl VARCHAR,
    total_bought VARCHAR
);
)";

inline const char* NEG_RISK_EVENT = R"(
CREATE TABLE IF NOT EXISTS neg_risk_event (
    id VARCHAR PRIMARY KEY,
    question_count INTEGER
);
)";

inline const char* CONDITION = R"(
CREATE TABLE IF NOT EXISTS condition (
    id VARCHAR PRIMARY KEY
);
)";

inline const char* ORDER_FILLED_EVENT = R"(
CREATE TABLE IF NOT EXISTS order_filled_event (
    id VARCHAR PRIMARY KEY,
    transaction_hash VARCHAR,
    timestamp BIGINT,
    order_hash VARCHAR,
    maker VARCHAR,
    taker VARCHAR,
    maker_asset_id VARCHAR,
    taker_asset_id VARCHAR,
    maker_amount_filled VARCHAR,
    taker_amount_filled VARCHAR,
    fee VARCHAR
);
)";

inline const char* ORDERBOOK = R"(
CREATE TABLE IF NOT EXISTS orderbook (
    id VARCHAR PRIMARY KEY,
    trades_quantity VARCHAR,
    buys_quantity VARCHAR,
    sells_quantity VARCHAR,
    collateral_volume VARCHAR,
    scaled_collateral_volume VARCHAR,
    collateral_buy_volume VARCHAR,
    scaled_collateral_buy_volume VARCHAR,
    collateral_sell_volume VARCHAR,
    scaled_collateral_sell_volume VARCHAR
);
)";

inline const char* MARKET_DATA = R"(
CREATE TABLE IF NOT EXISTS market_data (
    id VARCHAR PRIMARY KEY,
    condition VARCHAR,
    outcome_index VARCHAR
);
)";

inline const char* MARKET_OPEN_INTEREST = R"(
CREATE TABLE IF NOT EXISTS market_open_interest (
    id VARCHAR PRIMARY KEY,
    amount VARCHAR
);
)";

inline const char* GLOBAL_OPEN_INTEREST = R"(
CREATE TABLE IF NOT EXISTS global_open_interest (
    id VARCHAR PRIMARY KEY,
    amount VARCHAR
);
)";

inline const char* FIXED_PRODUCT_MARKET_MAKER = R"(
CREATE TABLE IF NOT EXISTS fixed_product_market_maker (
    id VARCHAR PRIMARY KEY,
    creator VARCHAR,
    creation_timestamp BIGINT,
    fee VARCHAR,
    trades_quantity VARCHAR,
    buys_quantity VARCHAR,
    sells_quantity VARCHAR,
    collateral_volume VARCHAR,
    scaled_collateral_volume VARCHAR,
    liquidity_parameter VARCHAR
);
)";

inline const char* FPMM_TRANSACTION = R"(
CREATE TABLE IF NOT EXISTS fpmm_transaction (
    id VARCHAR PRIMARY KEY,
    type VARCHAR,
    timestamp BIGINT,
    user_addr VARCHAR,
    trade_amount VARCHAR,
    fee_amount VARCHAR,
    outcome_index VARCHAR,
    outcome_tokens_amount VARCHAR
);
)";

} // namespace ddl

} // namespace schema
