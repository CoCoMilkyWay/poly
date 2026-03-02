#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace contracts {
constexpr const char *CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";
constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";
constexpr const char *FPMM_FACTORY = "0x8b9805a2f595b6705e74f7310829f2d299d21522";
} // namespace contracts

namespace topics {
// ConditionalTokens: 转账
constexpr const char *TRANSFER_SINGLE = "0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62";
constexpr const char *TRANSFER_BATCH = "0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb";
// ConditionalTokens: 条件
constexpr const char *CONDITION_PREPARE = "0xab3760c3bd2bb38b5bcf54dc79802ed67338b4cf29f3054ded67ed24661e4177";
constexpr const char *CONDITION_RESOLVE = "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894";
// ConditionalTokens: 持仓操作
constexpr const char *POSITION_SPLIT = "0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298";
constexpr const char *POSITION_MERGE = "0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca";
constexpr const char *POSITION_REDEEM = "0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d";
// FPMM: AMM池
constexpr const char *FPMM_CREATE = "0x92e0912d3d7f3192cad5c7ae3b47fb97f9c465c1dd12a5c24fd901ddb3905f43";
constexpr const char *FPMM_BUY = "0x4f62630f51608fc8a7603a9391a5101e58bd7c276139366fc107dc3b67c3dcf8";
constexpr const char *FPMM_SELL = "0xadcf2a240ed9300d681d9a3f5382b6c1beed1b7e46643e0c7b42cbe6e2d766b4";
constexpr const char *FPMM_FUNDING_ADD = "0xec2dc3e5a3bb9aa0a1deb905d2bd23640d07f107e6ceb484024501aad964a951";
constexpr const char *FPMM_FUNDING_REMOVE = "0x8b4b2c8ebd04c47fc8bce136a85df9b93fcb1f47c8aa296457d4391519d190e7";
// CTFExchange: 订单
constexpr const char *ORDER_FILL = "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6";
constexpr const char *TOKEN_REGISTER = "0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d";
// NegRiskAdapter: 市场
constexpr const char *MARKET_PREPARE = "0xf059ab16d1ca60e123eab60e3c02b68faf060347c701a5d14885a8e1def7b3a8";
constexpr const char *QUESTION_PREPARE = "0xaac410f87d423a922a7b226ac68f0c2eaf5bf6d15e644ac0758c7f96e2c253f7";
constexpr const char *POSITION_CONVERT = "0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e";
} // namespace topics

namespace stage1 {

static constexpr int64_t TRANSFER_FLAT_LOG_SCALE = 10000;

struct TransferEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string op, from, to, token_id;
  std::string amount;
};

struct ConditionPrepEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string condition_id, oracle, question_id;
  std::string outcome_slot_count;
};

struct ConditionResolveEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string condition_id, oracle, question_id;
  std::string outcome_slot_count;
  std::vector<std::string> payout_numerators;
};

struct SplitMergeEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string stakeholder, collateral_token, parent_collection_id, condition_id;
  std::vector<std::string> partition;
  std::string amount;
};

struct RedemptionEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string redeemer, collateral_token, parent_collection_id, condition_id;
  std::vector<std::string> index_sets;
  std::string payout;
};

struct FpmmEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string factory; // FPMM Factory地址
  int64_t creation_topics_count; // FixedProductMarketMakerCreation topics长度（2/4）
  std::string creation_layout;   // 按 topics 布局标记: fixed_factory_v1 / deterministic_factory_v1
  std::string creator, fpmm_addr, conditional_tokens, collateral_token;
  std::vector<std::string> condition_ids;
  std::string fee;
};

struct FpmmTradeEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string fpmm_addr, trader;
  int64_t side;
  std::string outcome_index, collateral_amount, token_amount, fee;
};

struct FpmmFundingEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string fpmm_addr, funder;
  int64_t side;
  std::vector<std::string> amounts;
  std::string collateral_from_fee_pool, shares;
};

struct OrderFilledEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string exchange, order_hash, maker, taker, maker_asset_id, taker_asset_id;
  std::string maker_amount, taker_amount, fee;
};

struct TokenMapEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string exchange, token0, token1, condition_id;
};

struct NegRiskMarketEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string market_id, oracle;
  std::string fee_bips;
  std::optional<std::string> data;
};

struct NegRiskQuestionEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string market_id, question_id;
  std::string question_index;
  std::optional<std::string> data;
};

struct ConvertEvent {
  int64_t block_number;
  std::string tx_hash;
  int64_t log_index;
  std::string stakeholder, market_id;
  std::string index_set, amount;
};

struct DecodedEvents {
  std::vector<TransferEvent> transfer;
  std::vector<ConditionPrepEvent> condition_preparation;
  std::vector<ConditionResolveEvent> condition_resolution;
  std::vector<SplitMergeEvent> split;
  std::vector<SplitMergeEvent> merge;
  std::vector<RedemptionEvent> redemption;
  std::vector<FpmmEvent> fpmm;
  std::vector<FpmmTradeEvent> fpmm_trade;
  std::vector<FpmmFundingEvent> fpmm_funding;
  std::vector<OrderFilledEvent> order_filled;
  std::vector<TokenMapEvent> token_map;
  std::vector<NegRiskMarketEvent> neg_risk_market;
  std::vector<NegRiskQuestionEvent> neg_risk_question;
  std::vector<ConvertEvent> convert;
};

class EventDecoder {
public:
  static DecodedEvents decode_logs(std::vector<json> &&results);
  static const std::string &current_log_json();

private:
  static thread_local std::string current_log_json_;
  static const bool crash_handler_installed_;

  static std::string to_lower(std::string s);
  static int64_t hex_to_int64(const std::string &hex);
  static std::string normalize_uint256_hex(const std::string &hex);
  static std::string extract_address_from_topic(const std::string &topic);
  static std::string extract_address_from_data_word(const std::string &data, size_t word_index);
  static std::string extract_bytes32_from_data(const std::string &data, size_t index);
  static std::string extract_uint256_hex_from_data(const std::string &data, size_t index);
  static int64_t extract_uint256_i64_from_data(const std::string &data, size_t index);
  static bool is_fpmm_topic(const std::string &topic0);

  static void parse_log(const json &log, DecodedEvents &events);
  static void parse_conditional_tokens_event(const std::string &topic0, const json &topics,
                                             const std::string &data, const std::string &tx_hash,
                                             int64_t block_number, int64_t log_index,
                                             DecodedEvents &events);
  static void parse_exchange_event(const std::string &topic0, const json &topics,
                                   const std::string &data, const std::string &tx_hash,
                                   int64_t block_number, int64_t log_index,
                                   const std::string &exchange, DecodedEvents &events);
  static void parse_neg_risk_adapter_event(const std::string &topic0, const json &topics,
                                           const std::string &data, const std::string &tx_hash,
                                           int64_t block_number, int64_t log_index,
                                           DecodedEvents &events);

  static void parse_transfer_single(const json &topics, const std::string &data,
                                    const std::string &tx_hash, int64_t block_number,
                                    int64_t log_index, DecodedEvents &events);
  static void parse_transfer_batch(const json &topics, const std::string &data,
                                   const std::string &tx_hash, int64_t block_number,
                                   int64_t log_index, DecodedEvents &events);
  static void parse_split_or_merge(const json &topics, const std::string &data,
                                   const std::string &tx_hash, int64_t block_number,
                                   int64_t log_index, std::vector<SplitMergeEvent> &out);
  static void parse_redemption(const json &topics, const std::string &data,
                               const std::string &tx_hash, int64_t block_number,
                               int64_t log_index, DecodedEvents &events);
  static void parse_condition_preparation(const json &topics, const std::string &data,
                                          const std::string &tx_hash, int64_t block_number,
                                          int64_t log_index, DecodedEvents &events);
  static void parse_condition_resolution(const json &topics, const std::string &data,
                                         const std::string &tx_hash, int64_t block_number,
                                         int64_t log_index, DecodedEvents &events);
  static void parse_order_filled(const json &topics, const std::string &data,
                                 const std::string &tx_hash, int64_t block_number,
                                 int64_t log_index, const std::string &exchange,
                                 DecodedEvents &events);
  static void parse_token_map(const json &topics, const std::string &tx_hash,
                              int64_t block_number, int64_t log_index,
                              const std::string &exchange, DecodedEvents &events);
  static void parse_convert(const json &topics, const std::string &data,
                            const std::string &tx_hash, int64_t block_number,
                            int64_t log_index, DecodedEvents &events);
  static void parse_neg_risk_market(const json &topics, const std::string &data,
                                    const std::string &tx_hash, int64_t block_number,
                                    int64_t log_index, DecodedEvents &events);
  static void parse_neg_risk_question(const json &topics, const std::string &data,
                                      const std::string &tx_hash, int64_t block_number,
                                      int64_t log_index, DecodedEvents &events);
  static std::optional<std::string> parse_fpmm_create(const json &log, const std::string &factory_addr,
                                                      DecodedEvents &events);
  static void parse_fpmm_event(const std::string &topic0, const std::string &fpmm_addr,
                               const json &topics, const std::string &data,
                               const std::string &tx_hash, int64_t block_number,
                               int64_t log_index, DecodedEvents &events);
};

} // namespace stage1
