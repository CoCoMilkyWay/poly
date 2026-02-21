#pragma once

#include <algorithm>
#include <cassert>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
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

struct ParsedEvents {
  // ConditionalTokens 转账
  std::vector<std::string> transfer;
  // ConditionalTokens 条件
  std::vector<std::string> condition_preparation;
  std::vector<std::string> condition_resolution;
  // ConditionalTokens 持仓操作
  std::vector<std::string> split;
  std::vector<std::string> merge;
  std::vector<std::string> redemption;
  // FPMM
  std::vector<std::string> fpmm;
  std::vector<std::string> fpmm_trade;
  std::vector<std::string> fpmm_funding;
  // CTFExchange 订单
  std::vector<std::string> order_filled;
  std::vector<std::string> token_map;
  // NegRiskAdapter 市场
  std::vector<std::string> neg_risk_market;
  std::vector<std::string> neg_risk_question;
  std::vector<std::string> convert;
};

class EventParser {
public:
  static ParsedEvents parse_logs(const json &logs) {
    ParsedEvents events;
    std::set<std::string> fpmm_addrs;
    // 第一趟: FPMM创建
    for (const auto &log : logs) {
      std::string addr = to_lower(log["address"].get<std::string>());
      if (addr == contracts::FPMM_FACTORY) {
        auto new_addr = parse_fpmm_create(log, events);
        if (new_addr)
          fpmm_addrs.insert(*new_addr);
      }
    }
    // 第二趟: 所有事件
    for (const auto &log : logs) {
      parse_log(log, fpmm_addrs, events);
    }
    return events;
  }

private:
  static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  }

  static int64_t hex_to_int64(const std::string &hex) {
    return static_cast<int64_t>(std::stoull(hex, nullptr, 16));
  }

  static std::string sql_blob(const std::string &hex) {
    std::string h = hex;
    if (h.starts_with("0x"))
      h = h.substr(2);
    return "x'" + h + "'";
  }

  static std::string sql_str(const std::string &s) {
    return "'" + s + "'";
  }

  static std::string extract_address_from_topic(const std::string &topic) {
    return "0x" + topic.substr(26);
  }

  static std::string extract_bytes32_from_data(const std::string &data, size_t index) {
    size_t start = 2 + index * 64;
    return "0x" + data.substr(start, 64);
  }

  static int64_t extract_uint256_from_data(const std::string &data, size_t index) {
    std::string hex = extract_bytes32_from_data(data, index);
    return hex_to_int64(hex);
  }

  static void parse_log(const json &log, const std::set<std::string> &fpmm_addrs, ParsedEvents &events) {
    std::string address = to_lower(log["address"].get<std::string>());
    const auto &topics_arr = log["topics"];
    assert(!topics_arr.empty());

    std::string topic0 = to_lower(topics_arr[0].get<std::string>());
    std::string data = log["data"].get<std::string>();
    std::string tx_hash = log["transactionHash"].get<std::string>();
    int64_t block_number = hex_to_int64(log["blockNumber"].get<std::string>());
    int64_t log_index = hex_to_int64(log["logIndex"].get<std::string>());

    if (address == contracts::CONDITIONAL_TOKENS) {
      parse_conditional_tokens_event(topic0, topics_arr, data, tx_hash, block_number, log_index, events);
    } else if (address == contracts::CTF_EXCHANGE) {
      parse_exchange_event(topic0, topics_arr, data, tx_hash, block_number, log_index, "CTF", events);
    } else if (address == contracts::NEG_RISK_CTF_EXCHANGE) {
      parse_exchange_event(topic0, topics_arr, data, tx_hash, block_number, log_index, "NegRisk", events);
    } else if (address == contracts::NEG_RISK_ADAPTER) {
      parse_neg_risk_adapter_event(topic0, topics_arr, data, tx_hash, block_number, log_index, events);
    } else if (address == contracts::FPMM_FACTORY) {
      // 第一趟已处理，跳过
    } else if (fpmm_addrs.contains(address)) {
      parse_fpmm_event(topic0, address, topics_arr, data, tx_hash, block_number, log_index, events);
    }
  }

  static void parse_conditional_tokens_event(const std::string &topic0, const json &topics,
                                             const std::string &data, const std::string &tx_hash,
                                             int64_t block_number, int64_t log_index,
                                             ParsedEvents &events) {
    if (topic0 == topics::TRANSFER_SINGLE) {
      parse_transfer_single(topics, data, tx_hash, block_number, log_index, events);
    } else if (topic0 == topics::TRANSFER_BATCH) {
      parse_transfer_batch(topics, data, tx_hash, block_number, log_index, events);
    } else if (topic0 == topics::CONDITION_PREPARE) {
      parse_condition_preparation(topics, data, tx_hash, block_number, log_index, events);
    } else if (topic0 == topics::CONDITION_RESOLVE) {
      parse_condition_resolution(topics, data, tx_hash, block_number, log_index, events);
    } else if (topic0 == topics::POSITION_SPLIT) {
      parse_split_or_merge(topics, data, tx_hash, block_number, log_index, events.split);
    } else if (topic0 == topics::POSITION_MERGE) {
      parse_split_or_merge(topics, data, tx_hash, block_number, log_index, events.merge);
    } else if (topic0 == topics::POSITION_REDEEM) {
      parse_redemption(topics, data, tx_hash, block_number, log_index, events);
    }
  }

  static void parse_exchange_event(const std::string &topic0, const json &topics,
                                   const std::string &data, const std::string &tx_hash,
                                   int64_t block_number, int64_t log_index,
                                   const std::string &exchange, ParsedEvents &events) {
    if (topic0 == topics::ORDER_FILL) {
      parse_order_filled(topics, data, tx_hash, block_number, log_index, exchange, events);
    } else if (topic0 == topics::TOKEN_REGISTER) {
      parse_token_map(topics, tx_hash, block_number, log_index, exchange, events);
    }
  }

  static void parse_neg_risk_adapter_event(const std::string &topic0, const json &topics,
                                           const std::string &data, const std::string &tx_hash,
                                           int64_t block_number, int64_t log_index,
                                           ParsedEvents &events) {
    if (topic0 == topics::MARKET_PREPARE) {
      parse_neg_risk_market(topics, data, tx_hash, block_number, log_index, events);
    } else if (topic0 == topics::QUESTION_PREPARE) {
      parse_neg_risk_question(topics, data, tx_hash, block_number, log_index, events);
    } else if (topic0 == topics::POSITION_CONVERT) {
      parse_convert(topics, data, tx_hash, block_number, log_index, events);
    }
  }

  static void parse_transfer_single(const json &topics, const std::string &data,
                                    const std::string &tx_hash, int64_t block_number,
                                    int64_t log_index, ParsedEvents &events) {
    std::string op = extract_address_from_topic(topics[1].get<std::string>());
    std::string from = extract_address_from_topic(topics[2].get<std::string>());
    std::string to = extract_address_from_topic(topics[3].get<std::string>());

    std::string token_id = extract_bytes32_from_data(data, 0);
    int64_t amount = extract_uint256_from_data(data, 1);

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << (log_index * 1000) << ", "
       << sql_blob(op) << ", " << sql_blob(from) << ", " << sql_blob(to) << ", "
       << sql_blob(token_id) << ", " << amount;
    events.transfer.push_back(ss.str());
  }

  static void parse_transfer_batch(const json &topics, const std::string &data,
                                   const std::string &tx_hash, int64_t block_number,
                                   int64_t log_index, ParsedEvents &events) {
    std::string op = extract_address_from_topic(topics[1].get<std::string>());
    std::string from = extract_address_from_topic(topics[2].get<std::string>());
    std::string to = extract_address_from_topic(topics[3].get<std::string>());

    int64_t ids_offset = extract_uint256_from_data(data, 0);
    int64_t values_offset = extract_uint256_from_data(data, 1);

    int64_t ids_len = extract_uint256_from_data(data, ids_offset / 32);
    int64_t values_len = extract_uint256_from_data(data, values_offset / 32);
    assert(ids_len == values_len);

    for (int64_t i = 0; i < ids_len; ++i) {
      std::string token_id = extract_bytes32_from_data(data, ids_offset / 32 + 1 + i);
      int64_t amount = extract_uint256_from_data(data, values_offset / 32 + 1 + i);

      std::ostringstream ss;
      ss << block_number << ", " << sql_blob(tx_hash) << ", " << (log_index * 1000 + i) << ", "
         << sql_blob(op) << ", " << sql_blob(from) << ", " << sql_blob(to) << ", "
         << sql_blob(token_id) << ", " << amount;
      events.transfer.push_back(ss.str());
    }
  }

  static void parse_split_or_merge(const json &topics, const std::string &data,
                                   const std::string &tx_hash, int64_t block_number,
                                   int64_t log_index, std::vector<std::string> &out) {
    std::string stakeholder = extract_address_from_topic(topics[1].get<std::string>());
    std::string parent_collection_id = topics[2].get<std::string>();
    std::string condition_id = topics[3].get<std::string>();

    std::string collateral_token = extract_address_from_topic("0x" + data.substr(2, 64));
    int64_t partition_offset = extract_uint256_from_data(data, 1);
    int64_t amount = extract_uint256_from_data(data, 2);

    int64_t partition_len = extract_uint256_from_data(data, partition_offset / 32);
    std::ostringstream partition_ss;
    partition_ss << "[";
    for (int64_t i = 0; i < partition_len; ++i) {
      if (i > 0)
        partition_ss << ",";
      partition_ss << extract_uint256_from_data(data, partition_offset / 32 + 1 + i);
    }
    partition_ss << "]";

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(stakeholder) << ", " << sql_blob(collateral_token) << ", "
       << sql_blob(parent_collection_id) << ", " << sql_blob(condition_id) << ", "
       << sql_str(partition_ss.str()) << ", " << amount;
    out.push_back(ss.str());
  }

  static void parse_redemption(const json &topics, const std::string &data,
                               const std::string &tx_hash, int64_t block_number,
                               int64_t log_index, ParsedEvents &events) {
    std::string redeemer = extract_address_from_topic(topics[1].get<std::string>());
    std::string collateral_token = extract_address_from_topic(topics[2].get<std::string>());
    std::string parent_collection_id = topics[3].get<std::string>();

    std::string condition_id = extract_bytes32_from_data(data, 0);
    int64_t index_sets_offset = extract_uint256_from_data(data, 1);
    int64_t payout = extract_uint256_from_data(data, 2);

    int64_t index_sets_len = extract_uint256_from_data(data, index_sets_offset / 32);
    std::ostringstream index_sets_ss;
    index_sets_ss << "[";
    for (int64_t i = 0; i < index_sets_len; ++i) {
      if (i > 0)
        index_sets_ss << ",";
      index_sets_ss << extract_uint256_from_data(data, index_sets_offset / 32 + 1 + i);
    }
    index_sets_ss << "]";

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(redeemer) << ", " << sql_blob(collateral_token) << ", "
       << sql_blob(parent_collection_id) << ", " << sql_blob(condition_id) << ", "
       << sql_str(index_sets_ss.str()) << ", " << payout;
    events.redemption.push_back(ss.str());
  }

  static void parse_condition_preparation(const json &topics, const std::string &data,
                                          const std::string &tx_hash, int64_t block_number,
                                          int64_t log_index, ParsedEvents &events) {
    std::string condition_id = topics[1].get<std::string>();
    std::string oracle = extract_address_from_topic(topics[2].get<std::string>());
    std::string question_id = topics[3].get<std::string>();
    int64_t outcome_slot_count = extract_uint256_from_data(data, 0);

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(condition_id) << ", " << sql_blob(oracle) << ", "
       << sql_blob(question_id) << ", " << outcome_slot_count;
    events.condition_preparation.push_back(ss.str());
  }

  static void parse_condition_resolution(const json &topics, const std::string &data,
                                         const std::string &tx_hash, int64_t block_number,
                                         int64_t log_index, ParsedEvents &events) {
    std::string condition_id = topics[1].get<std::string>();
    std::string oracle = extract_address_from_topic(topics[2].get<std::string>());
    std::string question_id = topics[3].get<std::string>();

    int64_t outcome_slot_count = extract_uint256_from_data(data, 0);
    int64_t payout_offset = extract_uint256_from_data(data, 1);
    int64_t payout_len = extract_uint256_from_data(data, payout_offset / 32);

    std::ostringstream payout_ss;
    payout_ss << "[";
    for (int64_t i = 0; i < payout_len; ++i) {
      if (i > 0)
        payout_ss << ",";
      payout_ss << extract_uint256_from_data(data, payout_offset / 32 + 1 + i);
    }
    payout_ss << "]";

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(condition_id) << ", " << sql_blob(oracle) << ", "
       << sql_blob(question_id) << ", " << outcome_slot_count << ", "
       << sql_str(payout_ss.str());
    events.condition_resolution.push_back(ss.str());
  }

  static void parse_order_filled(const json &topics, const std::string &data,
                                 const std::string &tx_hash, int64_t block_number,
                                 int64_t log_index, const std::string &exchange,
                                 ParsedEvents &events) {
    std::string order_hash = topics[1].get<std::string>();
    std::string maker = extract_address_from_topic(topics[2].get<std::string>());
    std::string taker = extract_address_from_topic(topics[3].get<std::string>());

    std::string maker_asset_id = extract_bytes32_from_data(data, 0);
    std::string taker_asset_id = extract_bytes32_from_data(data, 1);
    int64_t maker_amount = extract_uint256_from_data(data, 2);
    int64_t taker_amount = extract_uint256_from_data(data, 3);
    int64_t fee = extract_uint256_from_data(data, 4);

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_str(exchange) << ", " << sql_blob(order_hash) << ", "
       << sql_blob(maker) << ", " << sql_blob(taker) << ", "
       << sql_blob(maker_asset_id) << ", " << sql_blob(taker_asset_id) << ", "
       << maker_amount << ", " << taker_amount << ", " << fee;
    events.order_filled.push_back(ss.str());
  }

  static void parse_token_map(const json &topics, const std::string &tx_hash,
                              int64_t block_number, int64_t log_index,
                              const std::string &exchange, ParsedEvents &events) {
    std::string token0 = topics[1].get<std::string>();
    std::string token1 = topics[2].get<std::string>();
    std::string condition_id = topics[3].get<std::string>();

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_str(exchange) << ", " << sql_blob(token0) << ", " << sql_blob(token1) << ", "
       << sql_blob(condition_id);
    events.token_map.push_back(ss.str());
  }

  static void parse_convert(const json &topics, const std::string &data,
                            const std::string &tx_hash, int64_t block_number,
                            int64_t log_index, ParsedEvents &events) {
    std::string stakeholder = extract_address_from_topic(topics[1].get<std::string>());
    std::string market_id = topics[2].get<std::string>();
    int64_t index_set = hex_to_int64(topics[3].get<std::string>());

    int64_t amount = extract_uint256_from_data(data, 0);

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(stakeholder) << ", " << sql_blob(market_id) << ", "
       << index_set << ", " << amount;
    events.convert.push_back(ss.str());
  }

  static void parse_neg_risk_market(const json &topics, const std::string &data,
                                    const std::string &tx_hash, int64_t block_number,
                                    int64_t log_index, ParsedEvents &events) {
    std::string market_id = topics[1].get<std::string>();
    std::string oracle = extract_address_from_topic(topics[2].get<std::string>());

    int64_t fee_bips = extract_uint256_from_data(data, 0);
    int64_t data_offset = extract_uint256_from_data(data, 1);
    int64_t data_len = extract_uint256_from_data(data, data_offset / 32);

    std::string market_data;
    if (data_len > 0) {
      size_t start = 2 + (data_offset / 32 + 1) * 64;
      size_t hex_len = static_cast<size_t>(data_len) * 2;
      if (start + hex_len <= data.size()) {
        market_data = "0x" + data.substr(start, hex_len);
      }
    }

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(market_id) << ", " << sql_blob(oracle) << ", " << fee_bips;
    if (market_data.empty()) {
      ss << ", NULL";
    } else {
      ss << ", " << sql_blob(market_data);
    }
    events.neg_risk_market.push_back(ss.str());
  }

  static void parse_neg_risk_question(const json &topics, const std::string &data,
                                      const std::string &tx_hash, int64_t block_number,
                                      int64_t log_index, ParsedEvents &events) {
    std::string market_id = topics[1].get<std::string>();
    std::string question_id = topics[2].get<std::string>();

    int64_t question_index = extract_uint256_from_data(data, 0);
    int64_t data_offset = extract_uint256_from_data(data, 1);
    int64_t data_len = extract_uint256_from_data(data, data_offset / 32);

    std::string question_data;
    if (data_len > 0) {
      size_t start = 2 + (data_offset / 32 + 1) * 64;
      size_t hex_len = static_cast<size_t>(data_len) * 2;
      if (start + hex_len <= data.size()) {
        question_data = "0x" + data.substr(start, hex_len);
      }
    }

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(market_id) << ", " << sql_blob(question_id) << ", " << question_index;
    if (question_data.empty()) {
      ss << ", NULL";
    } else {
      ss << ", " << sql_blob(question_data);
    }
    events.neg_risk_question.push_back(ss.str());
  }

  static std::optional<std::string> parse_fpmm_create(const json &log, ParsedEvents &events) {
    const auto &topics_arr = log["topics"];
    std::string topic0 = to_lower(topics_arr[0].get<std::string>());
    if (topic0 != topics::FPMM_CREATE)
      return std::nullopt;

    std::string data = log["data"].get<std::string>();
    std::string tx_hash = log["transactionHash"].get<std::string>();
    int64_t block_number = hex_to_int64(log["blockNumber"].get<std::string>());
    int64_t log_index = hex_to_int64(log["logIndex"].get<std::string>());

    std::string creator = extract_address_from_topic(topics_arr[1].get<std::string>());
    std::string conditional_tokens = extract_address_from_topic(topics_arr[2].get<std::string>());
    std::string collateral_token = extract_address_from_topic(topics_arr[3].get<std::string>());

    std::string fpmm_addr = extract_address_from_topic("0x" + data.substr(2, 64));
    int64_t cond_ids_offset = extract_uint256_from_data(data, 1);
    int64_t cond_ids_len = extract_uint256_from_data(data, cond_ids_offset / 32);
    int64_t fee = extract_uint256_from_data(data, 2);

    std::ostringstream cond_ids_ss;
    cond_ids_ss << "[";
    for (int64_t i = 0; i < cond_ids_len; ++i) {
      if (i > 0)
        cond_ids_ss << ",";
      cond_ids_ss << "\"" << extract_bytes32_from_data(data, cond_ids_offset / 32 + 1 + i) << "\"";
    }
    cond_ids_ss << "]";

    std::ostringstream ss;
    ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
       << sql_blob(creator) << ", " << sql_blob(fpmm_addr) << ", "
       << sql_blob(conditional_tokens) << ", " << sql_blob(collateral_token) << ", "
       << sql_str(cond_ids_ss.str()) << ", " << fee;
    events.fpmm.push_back(ss.str());

    return to_lower(fpmm_addr);
  }

  static void parse_fpmm_event(const std::string &topic0, const std::string &fpmm_addr,
                               const json &topics, const std::string &data,
                               const std::string &tx_hash, int64_t block_number,
                               int64_t log_index, ParsedEvents &events) {
    if (topic0 == topics::FPMM_BUY) {
      std::string buyer = extract_address_from_topic(topics[1].get<std::string>());
      int64_t outcome_index = hex_to_int64(topics[2].get<std::string>());
      int64_t investment = extract_uint256_from_data(data, 0);
      int64_t fee = extract_uint256_from_data(data, 1);
      int64_t tokens_bought = extract_uint256_from_data(data, 2);

      std::ostringstream ss;
      ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
         << sql_blob(fpmm_addr) << ", " << sql_blob(buyer) << ", 1, " << outcome_index << ", "
         << investment << ", " << tokens_bought << ", " << fee;
      events.fpmm_trade.push_back(ss.str());
    } else if (topic0 == topics::FPMM_SELL) {
      std::string seller = extract_address_from_topic(topics[1].get<std::string>());
      int64_t outcome_index = hex_to_int64(topics[2].get<std::string>());
      int64_t return_amount = extract_uint256_from_data(data, 0);
      int64_t fee = extract_uint256_from_data(data, 1);
      int64_t tokens_sold = extract_uint256_from_data(data, 2);

      std::ostringstream ss;
      ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
         << sql_blob(fpmm_addr) << ", " << sql_blob(seller) << ", 2, " << outcome_index << ", "
         << return_amount << ", " << tokens_sold << ", " << fee;
      events.fpmm_trade.push_back(ss.str());
    } else if (topic0 == topics::FPMM_FUNDING_ADD) {
      std::string funder = extract_address_from_topic(topics[1].get<std::string>());
      int64_t amounts_offset = extract_uint256_from_data(data, 0);
      int64_t shares_minted = extract_uint256_from_data(data, 1);
      int64_t amounts_len = extract_uint256_from_data(data, amounts_offset / 32);

      std::ostringstream amounts_ss;
      amounts_ss << "[";
      for (int64_t i = 0; i < amounts_len; ++i) {
        if (i > 0)
          amounts_ss << ",";
        amounts_ss << extract_uint256_from_data(data, amounts_offset / 32 + 1 + i);
      }
      amounts_ss << "]";

      std::ostringstream ss;
      ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
         << sql_blob(fpmm_addr) << ", " << sql_blob(funder) << ", 1, "
         << sql_str(amounts_ss.str()) << ", 0, " << shares_minted;
      events.fpmm_funding.push_back(ss.str());
    } else if (topic0 == topics::FPMM_FUNDING_REMOVE) {
      std::string funder = extract_address_from_topic(topics[1].get<std::string>());
      int64_t amounts_offset = extract_uint256_from_data(data, 0);
      int64_t collateral_from_fee_pool = extract_uint256_from_data(data, 1);
      int64_t shares_burnt = extract_uint256_from_data(data, 2);
      int64_t amounts_len = extract_uint256_from_data(data, amounts_offset / 32);

      std::ostringstream amounts_ss;
      amounts_ss << "[";
      for (int64_t i = 0; i < amounts_len; ++i) {
        if (i > 0)
          amounts_ss << ",";
        amounts_ss << extract_uint256_from_data(data, amounts_offset / 32 + 1 + i);
      }
      amounts_ss << "]";

      std::ostringstream ss;
      ss << block_number << ", " << sql_blob(tx_hash) << ", " << log_index << ", "
         << sql_blob(fpmm_addr) << ", " << sql_blob(funder) << ", 2, "
         << sql_str(amounts_ss.str()) << ", " << collateral_from_fee_pool << ", " << shares_burnt;
      events.fpmm_funding.push_back(ss.str());
    }
  }
};
