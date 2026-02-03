#pragma once

#include <string>
#include <sstream>

// ============================================================================
// GraphQL 查询构建器
// ============================================================================
class GraphQL {
public:
    // 构建请求 target path（不含 host，用于 boost::beast）
    static std::string build_target(const std::string& subgraph_id) {
        return "/api/subgraphs/id/" + subgraph_id;
    }
    
    // JSON 字符串转义
    static std::string escape_json(const std::string& s) {
        std::string result;
        result.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '"':  result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default:   result += c; break;
            }
        }
        return result;
    }
    
    // 构建 entity 查询 JSON body（使用 GraphQL Variables）
    static std::string build_query(const std::string& entity_plural, 
                                   const char* fields,
                                   const std::string& cursor = "",
                                   int limit = 1000) {
        std::ostringstream oss;
        
        // query 部分使用变量占位
        oss << R"({"query":"query Q($limit:Int!,$cursor:ID){)"
            << entity_plural 
            << R"((first:$limit,orderBy:id,orderDirection:asc,where:{id_gt:$cursor}){)"
            << fields
            << R"(}}","variables":{"limit":)" << limit << ",\"cursor\":";
        
        // cursor 为空传 null，否则传字符串
        if (cursor.empty()) {
            oss << "null";
        } else {
            oss << "\"" << escape_json(cursor) << "\"";
        }
        oss << "}}";
        
        return oss.str();
    }
    
    // 各 Entity 的查询字段定义（完整版）
    struct EntityFields {
        // ====================================================================
        // Activity subgraph
        // ====================================================================
        static constexpr const char* Split = 
            "id timestamp stakeholder condition amount";
        static constexpr const char* Merge = 
            "id timestamp stakeholder condition amount";
        static constexpr const char* Redemption = 
            "id timestamp redeemer condition indexSets payout";
        static constexpr const char* NegRiskConversion = 
            "id timestamp stakeholder negRiskMarketId amount indexSet questionCount";
        // Activity subgraph 的 NegRiskEvent
        static constexpr const char* NegRiskEvent_Activity = 
            "id questionCount";
        // Activity subgraph 的 FixedProductMarketMaker (只有 id)
        static constexpr const char* FixedProductMarketMaker_Activity = 
            "id";
        // Activity subgraph 的 Position
        static constexpr const char* Position = 
            "id condition outcomeIndex";
        // Activity subgraph 的 Condition (只有 id)
        static constexpr const char* Condition_Activity = 
            "id";
        
        // ====================================================================
        // PnL subgraph
        // ====================================================================
        static constexpr const char* UserPosition = 
            "id user tokenId amount avgPrice realizedPnl totalBought";
        // PnL subgraph 的 NegRiskEvent
        static constexpr const char* NegRiskEvent_Pnl = 
            "id questionCount";
        // PnL subgraph 的 Condition (含 payouts)
        static constexpr const char* Condition_Pnl = 
            "id positionIds payoutNumerators payoutDenominator";
        // PnL subgraph 的 FPMM
        static constexpr const char* FPMM = 
            "id conditionId";
        
        // ====================================================================
        // Orderbook subgraph
        // ====================================================================
        static constexpr const char* OrderFilledEvent = 
            "id transactionHash timestamp orderHash maker taker "
            "makerAssetId takerAssetId makerAmountFilled takerAmountFilled fee";
        static constexpr const char* OrdersMatchedEvent = 
            "id timestamp makerAssetID takerAssetID makerAmountFilled takerAmountFilled";
        static constexpr const char* Orderbook = 
            "id tradesQuantity buysQuantity sellsQuantity "
            "collateralVolume scaledCollateralVolume "
            "collateralBuyVolume scaledCollateralBuyVolume "
            "collateralSellVolume scaledCollateralSellVolume";
        static constexpr const char* MarketData = 
            "id condition outcomeIndex";
        static constexpr const char* OrdersMatchedGlobal = 
            "id tradesQuantity buysQuantity sellsQuantity "
            "collateralVolume scaledCollateralVolume "
            "collateralBuyVolume scaledCollateralBuyVolume "
            "collateralSellVolume scaledCollateralSellVolume";
        
        // ====================================================================
        // OI subgraph
        // ====================================================================
        static constexpr const char* MarketOpenInterest = 
            "id amount";
        static constexpr const char* GlobalOpenInterest = 
            "id amount";
        // OI subgraph 的 Condition (只有 id)
        static constexpr const char* Condition_OI = 
            "id";
        // OI subgraph 的 NegRiskEvent (含 feeBps)
        static constexpr const char* NegRiskEvent_OI = 
            "id feeBps questionCount";
        
        // ====================================================================
        // FPMM subgraph (完整字段)
        // ====================================================================
        // FPMM subgraph 的 Condition (只有 id)
        static constexpr const char* Condition_FPMM = 
            "id";
        static constexpr const char* Collateral = 
            "id name symbol decimals";
        // FixedProductMarketMaker 完整版 - 注意 collateralToken 需要展开
        static constexpr const char* FixedProductMarketMaker = 
            "id creator creationTimestamp creationTransactionHash "
            "collateralToken{id name symbol decimals} conditionalTokenAddress conditions fee "
            "tradesQuantity buysQuantity sellsQuantity liquidityAddQuantity liquidityRemoveQuantity "
            "collateralVolume scaledCollateralVolume "
            "collateralBuyVolume scaledCollateralBuyVolume "
            "collateralSellVolume scaledCollateralSellVolume "
            "feeVolume scaledFeeVolume "
            "liquidityParameter scaledLiquidityParameter "
            "outcomeTokenAmounts outcomeTokenPrices outcomeSlotCount "
            "lastActiveDay totalSupply";
        // FpmmFundingAddition - fpmm 需要展开
        static constexpr const char* FpmmFundingAddition = 
            "id timestamp fpmm{id} funder amountsAdded amountsRefunded sharesMinted";
        // FpmmFundingRemoval - fpmm 需要展开
        static constexpr const char* FpmmFundingRemoval = 
            "id timestamp fpmm{id} funder amountsRemoved collateralRemoved sharesBurnt";
        // FpmmTransaction - market 需要展开
        static constexpr const char* FpmmTransaction = 
            "id type timestamp market{id} user tradeAmount feeAmount outcomeIndex outcomeTokensAmount";
        // FpmmPoolMembership - pool 需要展开
        static constexpr const char* FpmmPoolMembership = 
            "id pool{id} funder amount";
        
        // ====================================================================
        // Resolution subgraph
        // ====================================================================
        static constexpr const char* MarketResolution = 
            "id newVersionQ author ancillaryData lastUpdateTimestamp status wasDisputed "
            "proposedPrice reproposedPrice price updates transactionHash logIndex approved";
        static constexpr const char* AncillaryDataHashToQuestionId = 
            "id questionId";
        static constexpr const char* Moderator = 
            "id canMod";
        static constexpr const char* Revision = 
            "id moderator questionId timestamp update transactionHash";
        
        // ====================================================================
        // Positions subgraph
        // ====================================================================
        // UserBalance - asset 需要展开
        static constexpr const char* UserBalance = 
            "id user asset{id} balance";
        // NetUserBalance - asset 需要展开
        static constexpr const char* NetUserBalance = 
            "id user asset{id} balance";
        // TokenIdCondition - condition 需要展开
        static constexpr const char* TokenIdCondition = 
            "id condition{id} complement outcomeIndex";
        // Positions subgraph 的 Condition
        static constexpr const char* Condition_Positions = 
            "id payouts";
        
        // ====================================================================
        // Fee-module subgraph
        // ====================================================================
        static constexpr const char* FeeRefundedEntity = 
            "id orderHash tokenId timestamp refundee feeRefunded feeCharged negRisk";
        
        // ====================================================================
        // Wallet subgraph
        // ====================================================================
        static constexpr const char* Wallet = 
            "id signer type balance lastTransfer createdAt";
        static constexpr const char* GlobalUSDCBalance = 
            "id balance";
        
        // ====================================================================
        // Sports-oracle subgraph
        // ====================================================================
        static constexpr const char* Game = 
            "id ancillaryData ordering state homeScore awayScore";
        static constexpr const char* Market = 
            "id gameId state marketType underdog line payouts";
    };
    
    // 获取 entity 的字段定义（根据 subgraph 上下文）
    static const char* get_fields(const std::string& entity, const std::string& subgraph = "") {
        // Activity subgraph
        if (entity == "Split") return EntityFields::Split;
        if (entity == "Merge") return EntityFields::Merge;
        if (entity == "Redemption") return EntityFields::Redemption;
        if (entity == "NegRiskConversion") return EntityFields::NegRiskConversion;
        if (entity == "Position") return EntityFields::Position;
        
        // NegRiskEvent - 根据 subgraph 返回不同字段
        if (entity == "NegRiskEvent") {
            if (subgraph == "oi") return EntityFields::NegRiskEvent_OI;
            return EntityFields::NegRiskEvent_Activity;  // activity/pnl 相同
        }
        
        // Condition - 根据 subgraph 返回不同字段
        if (entity == "Condition") {
            if (subgraph == "pnl") return EntityFields::Condition_Pnl;
            if (subgraph == "positions") return EntityFields::Condition_Positions;
            return EntityFields::Condition_Activity;  // activity/oi/fpmm 只有 id
        }
        
        // FixedProductMarketMaker - 根据 subgraph 返回不同字段
        if (entity == "FixedProductMarketMaker") {
            if (subgraph == "activity") return EntityFields::FixedProductMarketMaker_Activity;
            return EntityFields::FixedProductMarketMaker;  // fpmm subgraph 完整版
        }
        
        // PnL subgraph
        if (entity == "UserPosition") return EntityFields::UserPosition;
        if (entity == "FPMM") return EntityFields::FPMM;
        
        // Orderbook subgraph
        if (entity == "OrderFilledEvent") return EntityFields::OrderFilledEvent;
        if (entity == "OrdersMatchedEvent") return EntityFields::OrdersMatchedEvent;
        if (entity == "Orderbook") return EntityFields::Orderbook;
        if (entity == "MarketData") return EntityFields::MarketData;
        if (entity == "OrdersMatchedGlobal") return EntityFields::OrdersMatchedGlobal;
        
        // OI subgraph
        if (entity == "MarketOpenInterest") return EntityFields::MarketOpenInterest;
        if (entity == "GlobalOpenInterest") return EntityFields::GlobalOpenInterest;
        
        // FPMM subgraph
        if (entity == "Collateral") return EntityFields::Collateral;
        if (entity == "FpmmFundingAddition") return EntityFields::FpmmFundingAddition;
        if (entity == "FpmmFundingRemoval") return EntityFields::FpmmFundingRemoval;
        if (entity == "FpmmTransaction") return EntityFields::FpmmTransaction;
        if (entity == "FpmmPoolMembership") return EntityFields::FpmmPoolMembership;
        
        // Resolution subgraph
        if (entity == "MarketResolution") return EntityFields::MarketResolution;
        if (entity == "AncillaryDataHashToQuestionId") return EntityFields::AncillaryDataHashToQuestionId;
        if (entity == "Moderator") return EntityFields::Moderator;
        if (entity == "Revision") return EntityFields::Revision;
        
        // Positions subgraph
        if (entity == "UserBalance") return EntityFields::UserBalance;
        if (entity == "NetUserBalance") return EntityFields::NetUserBalance;
        if (entity == "TokenIdCondition") return EntityFields::TokenIdCondition;
        
        // Fee-module subgraph
        if (entity == "FeeRefundedEntity") return EntityFields::FeeRefundedEntity;
        
        // Wallet subgraph
        if (entity == "Wallet") return EntityFields::Wallet;
        if (entity == "GlobalUSDCBalance") return EntityFields::GlobalUSDCBalance;
        
        // Sports-oracle subgraph
        if (entity == "Game") return EntityFields::Game;
        if (entity == "Market") return EntityFields::Market;
        
        return "id";
    }
    
    // 将 Entity 名称转为复数形式（GraphQL 查询用）
    static std::string to_plural(const std::string& entity) {
        // 首字母小写
        std::string result = entity;
        if (!result.empty()) {
            result[0] = std::tolower(result[0]);
        }
        
        // 特殊复数形式
        if (entity == "MarketData") return "marketDatas";
        if (entity == "Orderbook") return "orderbooks";
        if (entity == "FPMM") return "fpmms";
        if (entity == "UserBalance") return "userBalances";
        if (entity == "NetUserBalance") return "netUserBalances";
        if (entity == "TokenIdCondition") return "tokenIdConditions";
        if (entity == "AncillaryDataHashToQuestionId") return "ancillaryDataHashToQuestionIds";
        if (entity == "FeeRefundedEntity") return "feeRefundedEntities";
        if (entity == "GlobalUSDCBalance") return "globalUSDCBalances";
        
        // 一般规则：加 s
        return result + "s";
    }
};
