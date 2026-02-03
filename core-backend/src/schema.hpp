#pragma once

#include <string>
#include <vector>
#include <cstdint>

// 所有 Entity 的 C++ 结构定义和 DuckDB DDL
// 数据类型规范：
// - BigInt/BigDecimal: DECIMAL(38,0) 支持 uint256
// - 地址: VARCHAR(66) 支持 bytes32
// - 时间戳: BIGINT (Unix timestamp)
// - 布尔: BOOLEAN
// - 数组: VARCHAR (JSON 序列化)

namespace schema {

// ============================================================================
// Activity subgraph structs
// ============================================================================
struct Split {
    std::string id;
    int64_t timestamp;
    std::string stakeholder;
    std::string condition;
    std::string amount;
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

struct Position {
    std::string id;
    std::string condition;
    std::string outcomeIndex;
};

// ============================================================================
// PnL subgraph structs
// ============================================================================
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
    std::string feeBps;  // OI subgraph only
};

struct Condition {
    std::string id;
    std::vector<std::string> positionIds;
    std::vector<std::string> payoutNumerators;
    std::string payoutDenominator;
    std::vector<std::string> payouts;  // Positions subgraph
};

struct FPMM {
    std::string id;
    std::string conditionId;
};

// ============================================================================
// Orderbook subgraph structs
// ============================================================================
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

struct OrdersMatchedEvent {
    std::string id;
    int64_t timestamp;
    std::string makerAssetID;
    std::string takerAssetID;
    std::string makerAmountFilled;
    std::string takerAmountFilled;
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

struct OrdersMatchedGlobal {
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

// ============================================================================
// OI subgraph structs
// ============================================================================
struct MarketOpenInterest {
    std::string id;
    std::string amount;
};

struct GlobalOpenInterest {
    std::string id;
    std::string amount;
};

// ============================================================================
// FPMM subgraph structs
// ============================================================================
struct Collateral {
    std::string id;
    std::string name;
    std::string symbol;
    int decimals;
};

struct FixedProductMarketMaker {
    std::string id;
    std::string creator;
    int64_t creationTimestamp;
    std::string creationTransactionHash;
    std::string collateralTokenId;
    std::string conditionalTokenAddress;
    std::vector<std::string> conditions;
    std::string fee;
    std::string tradesQuantity;
    std::string buysQuantity;
    std::string sellsQuantity;
    std::string liquidityAddQuantity;
    std::string liquidityRemoveQuantity;
    std::string collateralVolume;
    std::string scaledCollateralVolume;
    std::string collateralBuyVolume;
    std::string scaledCollateralBuyVolume;
    std::string collateralSellVolume;
    std::string scaledCollateralSellVolume;
    std::string feeVolume;
    std::string scaledFeeVolume;
    std::string liquidityParameter;
    std::string scaledLiquidityParameter;
    std::vector<std::string> outcomeTokenAmounts;
    std::vector<std::string> outcomeTokenPrices;
    int outcomeSlotCount;
    int64_t lastActiveDay;
    std::string totalSupply;
};

struct FpmmFundingAddition {
    std::string id;
    int64_t timestamp;
    std::string fpmmId;
    std::string funder;
    std::vector<std::string> amountsAdded;
    std::vector<std::string> amountsRefunded;
    std::string sharesMinted;
};

struct FpmmFundingRemoval {
    std::string id;
    int64_t timestamp;
    std::string fpmmId;
    std::string funder;
    std::vector<std::string> amountsRemoved;
    std::string collateralRemoved;
    std::string sharesBurnt;
};

struct FpmmTransaction {
    std::string id;
    std::string type;
    int64_t timestamp;
    std::string marketId;
    std::string user;
    std::string tradeAmount;
    std::string feeAmount;
    std::string outcomeIndex;
    std::string outcomeTokensAmount;
};

struct FpmmPoolMembership {
    std::string id;
    std::string poolId;
    std::string funder;
    std::string amount;
};

// ============================================================================
// Resolution subgraph structs
// ============================================================================
struct MarketResolution {
    std::string id;
    bool newVersionQ;
    std::string author;
    std::string ancillaryData;
    int64_t lastUpdateTimestamp;
    std::string status;
    bool wasDisputed;
    std::string proposedPrice;
    std::string reproposedPrice;
    std::string price;
    std::string updates;
    std::string transactionHash;
    std::string logIndex;
    bool approved;
};

struct AncillaryDataHashToQuestionId {
    std::string id;
    std::string questionId;
};

struct Moderator {
    std::string id;
    bool canMod;
};

struct Revision {
    std::string id;
    std::string moderator;
    std::string questionId;
    int64_t timestamp;
    std::string update;
    std::string transactionHash;
};

// ============================================================================
// Positions subgraph structs
// ============================================================================
struct UserBalance {
    std::string id;
    std::string user;
    std::string assetId;
    std::string balance;
};

struct NetUserBalance {
    std::string id;
    std::string user;
    std::string assetId;
    std::string balance;
};

struct TokenIdCondition {
    std::string id;
    std::string conditionId;
    std::string complement;
    std::string outcomeIndex;
};

// ============================================================================
// Fee-module subgraph structs
// ============================================================================
struct FeeRefundedEntity {
    std::string id;
    std::string orderHash;
    std::string tokenId;
    int64_t timestamp;
    std::string refundee;
    std::string feeRefunded;
    std::string feeCharged;
    bool negRisk;
};

// ============================================================================
// Wallet subgraph structs
// ============================================================================
struct Wallet {
    std::string id;
    std::string signer;
    std::string type;
    std::string balance;
    int64_t lastTransfer;
    int64_t createdAt;
};

struct GlobalUSDCBalance {
    std::string id;
    std::string balance;
};

// ============================================================================
// Sports-oracle subgraph structs
// ============================================================================
struct Game {
    std::string id;
    std::string ancillaryData;
    std::string ordering;
    std::string state;
    std::string homeScore;
    std::string awayScore;
};

struct Market {
    std::string id;
    std::string gameId;
    std::string state;
    std::string marketType;
    std::string underdog;
    std::string line;
    std::vector<std::string> payouts;
};

// ============================================================================
// DuckDB DDL - 优化数据类型和索引
// ============================================================================
namespace ddl {

inline const char* SYNC_STATE = R"(
CREATE TABLE IF NOT EXISTS sync_state (
    subgraph_id VARCHAR NOT NULL,
    entity_name VARCHAR NOT NULL,
    last_id VARCHAR,
    last_sync_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    total_synced BIGINT DEFAULT 0,
    PRIMARY KEY (subgraph_id, entity_name)
);
)";

// ============================================================================
// Activity subgraph tables
// ============================================================================
inline const char* SPLIT = R"(
CREATE TABLE IF NOT EXISTS split (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT NOT NULL,
    stakeholder VARCHAR(42) NOT NULL,
    condition VARCHAR(66) NOT NULL,
    amount DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_split_timestamp ON split(timestamp);
CREATE INDEX IF NOT EXISTS idx_split_stakeholder ON split(stakeholder);
CREATE INDEX IF NOT EXISTS idx_split_condition ON split(condition);
)";

inline const char* MERGE = R"(
CREATE TABLE IF NOT EXISTS merge (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT NOT NULL,
    stakeholder VARCHAR(42) NOT NULL,
    condition VARCHAR(66) NOT NULL,
    amount DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_merge_timestamp ON merge(timestamp);
CREATE INDEX IF NOT EXISTS idx_merge_stakeholder ON merge(stakeholder);
CREATE INDEX IF NOT EXISTS idx_merge_condition ON merge(condition);
)";

inline const char* REDEMPTION = R"(
CREATE TABLE IF NOT EXISTS redemption (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT NOT NULL,
    redeemer VARCHAR(42) NOT NULL,
    condition VARCHAR(66) NOT NULL,
    index_sets VARCHAR NOT NULL,
    payout DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_redemption_timestamp ON redemption(timestamp);
CREATE INDEX IF NOT EXISTS idx_redemption_redeemer ON redemption(redeemer);
CREATE INDEX IF NOT EXISTS idx_redemption_condition ON redemption(condition);
)";

inline const char* NEG_RISK_CONVERSION = R"(
CREATE TABLE IF NOT EXISTS neg_risk_conversion (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT NOT NULL,
    stakeholder VARCHAR(42) NOT NULL,
    neg_risk_market_id VARCHAR(66) NOT NULL,
    amount DECIMAL(38,0) NOT NULL,
    index_set DECIMAL(38,0) NOT NULL,
    question_count INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_neg_risk_conversion_timestamp ON neg_risk_conversion(timestamp);
CREATE INDEX IF NOT EXISTS idx_neg_risk_conversion_stakeholder ON neg_risk_conversion(stakeholder);
CREATE INDEX IF NOT EXISTS idx_neg_risk_conversion_market ON neg_risk_conversion(neg_risk_market_id);
)";

inline const char* POSITION = R"(
CREATE TABLE IF NOT EXISTS position (
    id VARCHAR PRIMARY KEY,
    condition VARCHAR(66) NOT NULL,
    outcome_index DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_position_condition ON position(condition);
)";

inline const char* NEG_RISK_EVENT = R"(
CREATE TABLE IF NOT EXISTS neg_risk_event (
    id VARCHAR PRIMARY KEY,
    question_count INTEGER NOT NULL,
    fee_bps DECIMAL(38,0)
);
)";

inline const char* CONDITION_ACTIVITY = R"(
CREATE TABLE IF NOT EXISTS condition_activity (
    id VARCHAR PRIMARY KEY
);
)";

// ============================================================================
// PnL subgraph tables
// ============================================================================
inline const char* USER_POSITION = R"(
CREATE TABLE IF NOT EXISTS user_position (
    id VARCHAR PRIMARY KEY,
    user_addr VARCHAR(42) NOT NULL,
    token_id DECIMAL(38,0) NOT NULL,
    amount DECIMAL(38,0) NOT NULL,
    avg_price DECIMAL(38,0) NOT NULL,
    realized_pnl DECIMAL(38,0) NOT NULL,
    total_bought DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_user_position_user ON user_position(user_addr);
CREATE INDEX IF NOT EXISTS idx_user_position_token ON user_position(token_id);
)";

inline const char* CONDITION_PNL = R"(
CREATE TABLE IF NOT EXISTS condition_pnl (
    id VARCHAR PRIMARY KEY,
    position_ids VARCHAR NOT NULL,
    payout_numerators VARCHAR NOT NULL,
    payout_denominator DECIMAL(38,0) NOT NULL
);
)";

inline const char* FPMM_PNL = R"(
CREATE TABLE IF NOT EXISTS fpmm_pnl (
    id VARCHAR PRIMARY KEY,
    condition_id VARCHAR(66) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fpmm_pnl_condition ON fpmm_pnl(condition_id);
)";

// ============================================================================
// Orderbook subgraph tables
// ============================================================================
inline const char* ORDER_FILLED_EVENT = R"(
CREATE TABLE IF NOT EXISTS order_filled_event (
    id VARCHAR PRIMARY KEY,
    transaction_hash VARCHAR(66) NOT NULL,
    timestamp BIGINT NOT NULL,
    order_hash VARCHAR(66) NOT NULL,
    maker VARCHAR(42) NOT NULL,
    taker VARCHAR(42) NOT NULL,
    maker_asset_id VARCHAR NOT NULL,
    taker_asset_id VARCHAR NOT NULL,
    maker_amount_filled DECIMAL(38,0) NOT NULL,
    taker_amount_filled DECIMAL(38,0) NOT NULL,
    fee DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_order_filled_timestamp ON order_filled_event(timestamp);
CREATE INDEX IF NOT EXISTS idx_order_filled_maker ON order_filled_event(maker);
CREATE INDEX IF NOT EXISTS idx_order_filled_taker ON order_filled_event(taker);
CREATE INDEX IF NOT EXISTS idx_order_filled_order_hash ON order_filled_event(order_hash);
)";

inline const char* ORDERS_MATCHED_EVENT = R"(
CREATE TABLE IF NOT EXISTS orders_matched_event (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT NOT NULL,
    maker_asset_id DECIMAL(38,0) NOT NULL,
    taker_asset_id DECIMAL(38,0) NOT NULL,
    maker_amount_filled DECIMAL(38,0) NOT NULL,
    taker_amount_filled DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_orders_matched_timestamp ON orders_matched_event(timestamp);
)";

inline const char* ORDERBOOK = R"(
CREATE TABLE IF NOT EXISTS orderbook (
    id VARCHAR PRIMARY KEY,
    trades_quantity DECIMAL(38,0) NOT NULL,
    buys_quantity DECIMAL(38,0) NOT NULL,
    sells_quantity DECIMAL(38,0) NOT NULL,
    collateral_volume DECIMAL(38,0) NOT NULL,
    scaled_collateral_volume DECIMAL(38,18) NOT NULL,
    collateral_buy_volume DECIMAL(38,0) NOT NULL,
    scaled_collateral_buy_volume DECIMAL(38,18) NOT NULL,
    collateral_sell_volume DECIMAL(38,0) NOT NULL,
    scaled_collateral_sell_volume DECIMAL(38,18) NOT NULL
);
)";

inline const char* MARKET_DATA = R"(
CREATE TABLE IF NOT EXISTS market_data (
    id VARCHAR PRIMARY KEY,
    condition VARCHAR(66) NOT NULL,
    outcome_index DECIMAL(38,0)
);
CREATE INDEX IF NOT EXISTS idx_market_data_condition ON market_data(condition);
)";

inline const char* ORDERS_MATCHED_GLOBAL = R"(
CREATE TABLE IF NOT EXISTS orders_matched_global (
    id VARCHAR PRIMARY KEY,
    trades_quantity DECIMAL(38,0) NOT NULL,
    buys_quantity DECIMAL(38,0) NOT NULL,
    sells_quantity DECIMAL(38,0) NOT NULL,
    collateral_volume DECIMAL(38,18) NOT NULL,
    scaled_collateral_volume DECIMAL(38,18) NOT NULL,
    collateral_buy_volume DECIMAL(38,18) NOT NULL,
    scaled_collateral_buy_volume DECIMAL(38,18) NOT NULL,
    collateral_sell_volume DECIMAL(38,18) NOT NULL,
    scaled_collateral_sell_volume DECIMAL(38,18) NOT NULL
);
)";

// ============================================================================
// OI subgraph tables
// ============================================================================
inline const char* MARKET_OPEN_INTEREST = R"(
CREATE TABLE IF NOT EXISTS market_open_interest (
    id VARCHAR PRIMARY KEY,
    amount DECIMAL(38,0) NOT NULL
);
)";

inline const char* GLOBAL_OPEN_INTEREST = R"(
CREATE TABLE IF NOT EXISTS global_open_interest (
    id VARCHAR PRIMARY KEY,
    amount DECIMAL(38,0) NOT NULL
);
)";

inline const char* CONDITION_OI = R"(
CREATE TABLE IF NOT EXISTS condition_oi (
    id VARCHAR PRIMARY KEY
);
)";

inline const char* NEG_RISK_EVENT_OI = R"(
CREATE TABLE IF NOT EXISTS neg_risk_event_oi (
    id VARCHAR PRIMARY KEY,
    fee_bps DECIMAL(38,0) NOT NULL,
    question_count INTEGER NOT NULL
);
)";

// ============================================================================
// FPMM subgraph tables
// ============================================================================
inline const char* COLLATERAL = R"(
CREATE TABLE IF NOT EXISTS collateral (
    id VARCHAR PRIMARY KEY,
    name VARCHAR NOT NULL,
    symbol VARCHAR NOT NULL,
    decimals INTEGER NOT NULL
);
)";

inline const char* FIXED_PRODUCT_MARKET_MAKER = R"(
CREATE TABLE IF NOT EXISTS fixed_product_market_maker (
    id VARCHAR PRIMARY KEY,
    creator VARCHAR(42) NOT NULL,
    creation_timestamp BIGINT NOT NULL,
    creation_transaction_hash VARCHAR(66) NOT NULL,
    collateral_token_id VARCHAR NOT NULL,
    collateral_token_name VARCHAR,
    collateral_token_symbol VARCHAR,
    collateral_token_decimals INTEGER,
    conditional_token_address VARCHAR(42) NOT NULL,
    conditions VARCHAR NOT NULL,
    fee DECIMAL(38,0) NOT NULL,
    trades_quantity DECIMAL(38,0) NOT NULL,
    buys_quantity DECIMAL(38,0) NOT NULL,
    sells_quantity DECIMAL(38,0) NOT NULL,
    liquidity_add_quantity DECIMAL(38,0) NOT NULL,
    liquidity_remove_quantity DECIMAL(38,0) NOT NULL,
    collateral_volume DECIMAL(38,0) NOT NULL,
    scaled_collateral_volume DECIMAL(38,18) NOT NULL,
    collateral_buy_volume DECIMAL(38,0) NOT NULL,
    scaled_collateral_buy_volume DECIMAL(38,18) NOT NULL,
    collateral_sell_volume DECIMAL(38,0) NOT NULL,
    scaled_collateral_sell_volume DECIMAL(38,18) NOT NULL,
    fee_volume DECIMAL(38,0) NOT NULL,
    scaled_fee_volume DECIMAL(38,18) NOT NULL,
    liquidity_parameter DECIMAL(38,0) NOT NULL,
    scaled_liquidity_parameter DECIMAL(38,18) NOT NULL,
    outcome_token_amounts VARCHAR NOT NULL,
    outcome_token_prices VARCHAR NOT NULL,
    outcome_slot_count INTEGER,
    last_active_day BIGINT NOT NULL,
    total_supply DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fpmm_creator ON fixed_product_market_maker(creator);
CREATE INDEX IF NOT EXISTS idx_fpmm_creation_timestamp ON fixed_product_market_maker(creation_timestamp);
)";

inline const char* FPMM_FUNDING_ADDITION = R"(
CREATE TABLE IF NOT EXISTS fpmm_funding_addition (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT NOT NULL,
    fpmm_id VARCHAR NOT NULL,
    funder VARCHAR(42) NOT NULL,
    amounts_added VARCHAR NOT NULL,
    amounts_refunded VARCHAR NOT NULL,
    shares_minted DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fpmm_funding_add_timestamp ON fpmm_funding_addition(timestamp);
CREATE INDEX IF NOT EXISTS idx_fpmm_funding_add_funder ON fpmm_funding_addition(funder);
CREATE INDEX IF NOT EXISTS idx_fpmm_funding_add_fpmm ON fpmm_funding_addition(fpmm_id);
)";

inline const char* FPMM_FUNDING_REMOVAL = R"(
CREATE TABLE IF NOT EXISTS fpmm_funding_removal (
    id VARCHAR PRIMARY KEY,
    timestamp BIGINT NOT NULL,
    fpmm_id VARCHAR NOT NULL,
    funder VARCHAR(42) NOT NULL,
    amounts_removed VARCHAR NOT NULL,
    collateral_removed DECIMAL(38,0) NOT NULL,
    shares_burnt DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fpmm_funding_rm_timestamp ON fpmm_funding_removal(timestamp);
CREATE INDEX IF NOT EXISTS idx_fpmm_funding_rm_funder ON fpmm_funding_removal(funder);
CREATE INDEX IF NOT EXISTS idx_fpmm_funding_rm_fpmm ON fpmm_funding_removal(fpmm_id);
)";

inline const char* FPMM_TRANSACTION = R"(
CREATE TABLE IF NOT EXISTS fpmm_transaction (
    id VARCHAR PRIMARY KEY,
    type VARCHAR NOT NULL,
    timestamp BIGINT NOT NULL,
    market_id VARCHAR NOT NULL,
    user_addr VARCHAR(42) NOT NULL,
    trade_amount DECIMAL(38,0) NOT NULL,
    fee_amount DECIMAL(38,0) NOT NULL,
    outcome_index DECIMAL(38,0) NOT NULL,
    outcome_tokens_amount DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fpmm_tx_timestamp ON fpmm_transaction(timestamp);
CREATE INDEX IF NOT EXISTS idx_fpmm_tx_user ON fpmm_transaction(user_addr);
CREATE INDEX IF NOT EXISTS idx_fpmm_tx_market ON fpmm_transaction(market_id);
)";

inline const char* FPMM_POOL_MEMBERSHIP = R"(
CREATE TABLE IF NOT EXISTS fpmm_pool_membership (
    id VARCHAR PRIMARY KEY,
    pool_id VARCHAR NOT NULL,
    funder VARCHAR(42) NOT NULL,
    amount DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fpmm_pool_funder ON fpmm_pool_membership(funder);
CREATE INDEX IF NOT EXISTS idx_fpmm_pool_pool ON fpmm_pool_membership(pool_id);
)";

inline const char* CONDITION_FPMM = R"(
CREATE TABLE IF NOT EXISTS condition_fpmm (
    id VARCHAR PRIMARY KEY
);
)";

// ============================================================================
// Resolution subgraph tables
// ============================================================================
inline const char* MARKET_RESOLUTION = R"(
CREATE TABLE IF NOT EXISTS market_resolution (
    id VARCHAR PRIMARY KEY,
    new_version_q BOOLEAN NOT NULL,
    author VARCHAR(42) NOT NULL,
    ancillary_data VARCHAR NOT NULL,
    last_update_timestamp BIGINT NOT NULL,
    status VARCHAR NOT NULL,
    was_disputed BOOLEAN NOT NULL,
    proposed_price DECIMAL(38,0) NOT NULL,
    reproposed_price DECIMAL(38,0) NOT NULL,
    price DECIMAL(38,0) NOT NULL,
    updates VARCHAR NOT NULL,
    transaction_hash VARCHAR(66),
    log_index DECIMAL(38,0),
    approved BOOLEAN
);
CREATE INDEX IF NOT EXISTS idx_market_resolution_status ON market_resolution(status);
CREATE INDEX IF NOT EXISTS idx_market_resolution_timestamp ON market_resolution(last_update_timestamp);
)";

inline const char* ANCILLARY_DATA_HASH_TO_QUESTION_ID = R"(
CREATE TABLE IF NOT EXISTS ancillary_data_hash_to_question_id (
    id VARCHAR PRIMARY KEY,
    question_id VARCHAR NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_ancillary_question ON ancillary_data_hash_to_question_id(question_id);
)";

inline const char* MODERATOR = R"(
CREATE TABLE IF NOT EXISTS moderator (
    id VARCHAR PRIMARY KEY,
    can_mod BOOLEAN NOT NULL
);
)";

inline const char* REVISION = R"(
CREATE TABLE IF NOT EXISTS revision (
    id VARCHAR PRIMARY KEY,
    moderator VARCHAR(42) NOT NULL,
    question_id VARCHAR NOT NULL,
    timestamp BIGINT NOT NULL,
    update_text VARCHAR NOT NULL,
    transaction_hash VARCHAR(66) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_revision_question ON revision(question_id);
CREATE INDEX IF NOT EXISTS idx_revision_timestamp ON revision(timestamp);
)";

// ============================================================================
// Positions subgraph tables
// ============================================================================
inline const char* USER_BALANCE = R"(
CREATE TABLE IF NOT EXISTS user_balance (
    id VARCHAR PRIMARY KEY,
    user_addr VARCHAR(42) NOT NULL,
    asset_id VARCHAR NOT NULL,
    balance DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_user_balance_user ON user_balance(user_addr);
CREATE INDEX IF NOT EXISTS idx_user_balance_asset ON user_balance(asset_id);
)";

inline const char* NET_USER_BALANCE = R"(
CREATE TABLE IF NOT EXISTS net_user_balance (
    id VARCHAR PRIMARY KEY,
    user_addr VARCHAR(42) NOT NULL,
    asset_id VARCHAR NOT NULL,
    balance DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_net_user_balance_user ON net_user_balance(user_addr);
CREATE INDEX IF NOT EXISTS idx_net_user_balance_asset ON net_user_balance(asset_id);
)";

inline const char* TOKEN_ID_CONDITION = R"(
CREATE TABLE IF NOT EXISTS token_id_condition (
    id VARCHAR PRIMARY KEY,
    condition_id VARCHAR(66) NOT NULL,
    complement VARCHAR NOT NULL,
    outcome_index DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_token_condition ON token_id_condition(condition_id);
)";

inline const char* CONDITION_POSITIONS = R"(
CREATE TABLE IF NOT EXISTS condition_positions (
    id VARCHAR PRIMARY KEY,
    payouts VARCHAR
);
)";

// ============================================================================
// Fee-module subgraph tables
// ============================================================================
inline const char* FEE_REFUNDED_ENTITY = R"(
CREATE TABLE IF NOT EXISTS fee_refunded_entity (
    id VARCHAR PRIMARY KEY,
    order_hash VARCHAR(66) NOT NULL,
    token_id VARCHAR NOT NULL,
    timestamp BIGINT NOT NULL,
    refundee VARCHAR(42) NOT NULL,
    fee_refunded DECIMAL(38,0) NOT NULL,
    fee_charged DECIMAL(38,0) NOT NULL,
    neg_risk BOOLEAN NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_fee_refunded_timestamp ON fee_refunded_entity(timestamp);
CREATE INDEX IF NOT EXISTS idx_fee_refunded_refundee ON fee_refunded_entity(refundee);
)";

// ============================================================================
// Wallet subgraph tables
// ============================================================================
inline const char* WALLET = R"(
CREATE TABLE IF NOT EXISTS wallet (
    id VARCHAR PRIMARY KEY,
    signer VARCHAR(42) NOT NULL,
    type VARCHAR NOT NULL,
    balance DECIMAL(38,0) NOT NULL,
    last_transfer BIGINT NOT NULL,
    created_at BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_wallet_signer ON wallet(signer);
CREATE INDEX IF NOT EXISTS idx_wallet_type ON wallet(type);
)";

inline const char* GLOBAL_USDC_BALANCE = R"(
CREATE TABLE IF NOT EXISTS global_usdc_balance (
    id VARCHAR PRIMARY KEY,
    balance DECIMAL(38,0) NOT NULL
);
)";

// ============================================================================
// Sports-oracle subgraph tables
// ============================================================================
inline const char* GAME = R"(
CREATE TABLE IF NOT EXISTS game (
    id VARCHAR PRIMARY KEY,
    ancillary_data VARCHAR NOT NULL,
    ordering VARCHAR NOT NULL,
    state VARCHAR NOT NULL,
    home_score DECIMAL(38,0) NOT NULL,
    away_score DECIMAL(38,0) NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_game_state ON game(state);
)";

inline const char* MARKET = R"(
CREATE TABLE IF NOT EXISTS market (
    id VARCHAR PRIMARY KEY,
    game_id VARCHAR NOT NULL,
    state VARCHAR NOT NULL,
    market_type VARCHAR NOT NULL,
    underdog VARCHAR NOT NULL,
    line DECIMAL(38,0) NOT NULL,
    payouts VARCHAR NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_market_game ON market(game_id);
CREATE INDEX IF NOT EXISTS idx_market_state ON market(state);
)";

} // namespace ddl

} // namespace schema
