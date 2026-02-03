#pragma once

#include <string>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class GraphQLBuilder {
public:
    // 构建查询 URL
    static std::string build_url(const std::string& api_key, const std::string& subgraph_id) {
        return "https://gateway.thegraph.com/api/" + api_key + "/subgraphs/id/" + subgraph_id;
    }
    
    // 构建 entity 查询
    // entity 名称首字母小写，复数形式（如 splits, merges）
    static std::string build_query(const std::string& entity_plural, 
                                   const std::string& fields,
                                   const std::string& cursor = "",
                                   int limit = 1000) {
        std::ostringstream oss;
        oss << "{ \"query\": \"{ " << entity_plural << "(";
        oss << "first: " << limit << ", ";
        oss << "orderBy: id, ";
        oss << "orderDirection: asc";
        
        if (!cursor.empty()) {
            oss << ", where: { id_gt: \\\"" << cursor << "\\\" }";
        }
        
        oss << ") { " << fields << " } }\" }";
        
        return oss.str();
    }
    
    // 各 Entity 的查询字段定义
    struct EntityFields {
        // Activity subgraph
        static constexpr const char* Split = 
            "id timestamp stakeholder condition amount";
        static constexpr const char* Merge = 
            "id timestamp stakeholder condition amount";
        static constexpr const char* Redemption = 
            "id timestamp redeemer condition indexSets payout";
        static constexpr const char* NegRiskConversion = 
            "id timestamp stakeholder negRiskMarketId amount indexSet questionCount";
        
        // PnL subgraph
        static constexpr const char* UserPosition = 
            "id user tokenId amount avgPrice realizedPnl totalBought";
        static constexpr const char* NegRiskEvent = 
            "id questionCount";
        static constexpr const char* Condition = 
            "id";
        static constexpr const char* ConditionPnl = 
            "id positionIds payoutNumerators payoutDenominator";
        
        // Orderbook subgraph
        static constexpr const char* OrderFilledEvent = 
            "id transactionHash timestamp orderHash maker taker "
            "makerAssetId takerAssetId makerAmountFilled takerAmountFilled fee";
        static constexpr const char* Orderbook = 
            "id tradesQuantity buysQuantity sellsQuantity "
            "collateralVolume scaledCollateralVolume "
            "collateralBuyVolume scaledCollateralBuyVolume "
            "collateralSellVolume scaledCollateralSellVolume";
        static constexpr const char* MarketData = 
            "id condition outcomeIndex";
        
        // OI subgraph
        static constexpr const char* MarketOpenInterest = 
            "id amount";
        static constexpr const char* GlobalOpenInterest = 
            "id amount";
        
        // FPMM subgraph
        static constexpr const char* FixedProductMarketMaker = 
            "id creator creationTimestamp fee tradesQuantity buysQuantity sellsQuantity "
            "collateralVolume scaledCollateralVolume liquidityParameter";
        static constexpr const char* FpmmTransaction = 
            "id type timestamp user tradeAmount feeAmount outcomeIndex outcomeTokensAmount";
    };
    
    // 获取 entity 的字段定义
    static const char* get_fields(const std::string& entity) {
        if (entity == "Split") return EntityFields::Split;
        if (entity == "Merge") return EntityFields::Merge;
        if (entity == "Redemption") return EntityFields::Redemption;
        if (entity == "NegRiskConversion") return EntityFields::NegRiskConversion;
        if (entity == "UserPosition") return EntityFields::UserPosition;
        if (entity == "NegRiskEvent") return EntityFields::NegRiskEvent;
        if (entity == "Condition") return EntityFields::Condition;
        if (entity == "OrderFilledEvent") return EntityFields::OrderFilledEvent;
        if (entity == "Orderbook") return EntityFields::Orderbook;
        if (entity == "MarketData") return EntityFields::MarketData;
        if (entity == "MarketOpenInterest") return EntityFields::MarketOpenInterest;
        if (entity == "GlobalOpenInterest") return EntityFields::GlobalOpenInterest;
        if (entity == "FixedProductMarketMaker") return EntityFields::FixedProductMarketMaker;
        if (entity == "FpmmTransaction") return EntityFields::FpmmTransaction;
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
        
        // 一般规则：加 s
        return result + "s";
    }
};
