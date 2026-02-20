#pragma once

#include <algorithm>
#include <cassert>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace contracts {
constexpr const char *CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";
constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";
} // namespace contracts

namespace topics {
constexpr const char *CONDITION_PREPARATION = "0xab3760c3bd2bb38b5bcf54dc79802ed67338b4cf29f3054ded67ed24661e4177";
constexpr const char *POSITION_SPLIT = "0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298";
constexpr const char *POSITIONS_MERGE = "0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca";
constexpr const char *TRANSFER_SINGLE = "0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62";
constexpr const char *TRANSFER_BATCH = "0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb";
constexpr const char *CONDITION_RESOLUTION = "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894";
constexpr const char *PAYOUT_REDEMPTION = "0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d";
constexpr const char *TOKEN_REGISTERED = "0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d";
constexpr const char *ORDER_FILLED = "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6";
constexpr const char *ORDERS_MATCHED = "0x63bf4d16b7fa898ef4c4b2b6d90fd201e9c56313b65638af6088d149d2ce956c";
constexpr const char *MARKET_PREPARED = "0xf059ab16d1ca60e123eab60e3c02b68faf060347c701a5d14885a8e1def7b3a8";
constexpr const char *QUESTION_PREPARED = "0xaac410f87d423a922a7b226ac68f0c2eaf5bf6d15e644ac0758c7f96e2c253f7";
constexpr const char *OUTCOME_REPORTED = "0x9e9fa7fd355160bd4cd3f22d4333519354beff1f5689bde87f2c5e63d8d484b2";
constexpr const char *POSITIONS_CONVERTED = "0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e";
} // namespace topics

struct ParsedEvents {
  std::vector<std::string> order_filled;
  std::vector<std::string> split;
  std::vector<std::string> merge;
  std::vector<std::string> redemption;
  std::vector<std::string> convert;
  std::vector<std::string> transfer;
  std::vector<std::string> token_map;
  std::vector<std::string> condition;
  std::vector<std::string> condition_resolution;
  std::vector<std::string> neg_risk_market;
  std::vector<std::string> neg_risk_question;
};

class EventParser {
public:
  static ParsedEvents parse_logs(const json &logs) {
    ParsedEvents events;
    for (const auto &log : logs) {
      parse_log(log, events);
    }
    return events;
  }

private:
  static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  }

  static int64_t hex_to_int64(const std::string &hex) {
    return std::stoll(hex, nullptr, 16);
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

  static void parse_log(const json &log, ParsedEvents &events) {
    std::string address = to_lower(log["address"].get<std::string>());
    const auto &topics_arr = log["topics"];
    assert(!topics_arr.empty());

    std::string topic0 = to_lower(topics_arr[0].get<std::string>());
    std::string data = log["data"].get<std::string>();
    int64_t block_number = hex_to_int64(log["blockNumber"].get<std::string>());
    int64_t log_index = hex_to_int64(log["logIndex"].get<std::string>());

    if (address == contracts::CONDITIONAL_TOKENS) {
      parse_conditional_tokens_event(topic0, topics_arr, data, block_number, log_index, events);
    } else if (address == contracts::CTF_EXCHANGE) {
      parse_exchange_event(topic0, topics_arr, data, block_number, log_index, "CTF", events);
    } else if (address == contracts::NEG_RISK_CTF_EXCHANGE) {
      parse_exchange_event(topic0, topics_arr, data, block_number, log_index, "NegRisk", events);
    } else if (address == contracts::NEG_RISK_ADAPTER) {
      parse_neg_risk_adapter_event(topic0, topics_arr, data, block_number, log_index, events);
    }
  }

  static void parse_conditional_tokens_event(const std::string &topic0, const json &topics,
                                             const std::string &data, int64_t block_number,
                                             int64_t log_index, ParsedEvents &events) {
    if (topic0 == topics::TRANSFER_SINGLE) {
      parse_transfer_single(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::TRANSFER_BATCH) {
      parse_transfer_batch(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::POSITION_SPLIT) {
      parse_position_split(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::POSITIONS_MERGE) {
      parse_positions_merge(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::PAYOUT_REDEMPTION) {
      parse_payout_redemption(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::CONDITION_PREPARATION) {
      parse_condition_preparation(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::CONDITION_RESOLUTION) {
      parse_condition_resolution(topics, data, block_number, log_index, events);
    }
  }

  static void parse_exchange_event(const std::string &topic0, const json &topics,
                                   const std::string &data, int64_t block_number,
                                   int64_t log_index, const std::string &exchange,
                                   ParsedEvents &events) {
    if (topic0 == topics::ORDER_FILLED) {
      parse_order_filled(topics, data, block_number, log_index, exchange, events);
    } else if (topic0 == topics::TOKEN_REGISTERED) {
      parse_token_registered(topics, data, block_number, log_index, exchange, events);
    }
  }

  static void parse_neg_risk_adapter_event(const std::string &topic0, const json &topics,
                                           const std::string &data, int64_t block_number,
                                           int64_t log_index, ParsedEvents &events) {
    if (topic0 == topics::POSITIONS_CONVERTED) {
      parse_positions_converted(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::MARKET_PREPARED) {
      parse_market_prepared(topics, data, block_number, log_index, events);
    } else if (topic0 == topics::QUESTION_PREPARED) {
      parse_question_prepared(topics, data, block_number, log_index, events);
    }
  }

  static void parse_transfer_single(const json &topics, const std::string &data,
                                    int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string op = extract_address_from_topic(topics[1].get<std::string>());
    std::string from = extract_address_from_topic(topics[2].get<std::string>());
    std::string to = extract_address_from_topic(topics[3].get<std::string>());

    std::string token_id = extract_bytes32_from_data(data, 0);
    int64_t amount = extract_uint256_from_data(data, 1);

    if (from == "0x0000000000000000000000000000000000000000" ||
        to == "0x0000000000000000000000000000000000000000") {
      return;
    }

    std::string op_lower = to_lower(op);
    if (op_lower == contracts::CTF_EXCHANGE ||
        op_lower == contracts::NEG_RISK_CTF_EXCHANGE ||
        op_lower == contracts::NEG_RISK_ADAPTER) {
      return;
    }

    std::ostringstream ss;
    ss << block_number << ", " << log_index << ", "
       << sql_blob(from) << ", " << sql_blob(to) << ", "
       << sql_blob(token_id) << ", " << amount;
    events.transfer.push_back(ss.str());
  }

  static void parse_transfer_batch(const json &topics, const std::string &data,
                                   int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string op = extract_address_from_topic(topics[1].get<std::string>());
    std::string from = extract_address_from_topic(topics[2].get<std::string>());
    std::string to = extract_address_from_topic(topics[3].get<std::string>());

    if (from == "0x0000000000000000000000000000000000000000" ||
        to == "0x0000000000000000000000000000000000000000") {
      return;
    }

    std::string op_lower = to_lower(op);
    if (op_lower == contracts::CTF_EXCHANGE ||
        op_lower == contracts::NEG_RISK_CTF_EXCHANGE ||
        op_lower == contracts::NEG_RISK_ADAPTER) {
      return;
    }

    int64_t ids_offset = extract_uint256_from_data(data, 0);
    int64_t values_offset = extract_uint256_from_data(data, 1);

    int64_t ids_len = extract_uint256_from_data(data, ids_offset / 32);
    int64_t values_len = extract_uint256_from_data(data, values_offset / 32);
    assert(ids_len == values_len);

    for (int64_t i = 0; i < ids_len; ++i) {
      std::string token_id = extract_bytes32_from_data(data, ids_offset / 32 + 1 + i);
      int64_t amount = extract_uint256_from_data(data, values_offset / 32 + 1 + i);

      std::ostringstream ss;
      ss << block_number << ", " << (log_index * 1000 + i) << ", "
         << sql_blob(from) << ", " << sql_blob(to) << ", "
         << sql_blob(token_id) << ", " << amount;
      events.transfer.push_back(ss.str());
    }
  }

  static void parse_position_split(const json &topics, const std::string &data,
                                   int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string stakeholder = extract_address_from_topic(topics[1].get<std::string>());
    std::string condition_id = topics[3].get<std::string>();

    int64_t amount = extract_uint256_from_data(data, 2);

    std::ostringstream ss;
    ss << block_number << ", " << log_index << ", "
       << sql_blob(stakeholder) << ", " << sql_blob(condition_id) << ", " << amount;
    events.split.push_back(ss.str());
  }

  static void parse_positions_merge(const json &topics, const std::string &data,
                                    int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string stakeholder = extract_address_from_topic(topics[1].get<std::string>());
    std::string condition_id = topics[3].get<std::string>();

    int64_t amount = extract_uint256_from_data(data, 2);

    std::ostringstream ss;
    ss << block_number << ", " << log_index << ", "
       << sql_blob(stakeholder) << ", " << sql_blob(condition_id) << ", " << amount;
    events.merge.push_back(ss.str());
  }

  static void parse_payout_redemption(const json &topics, const std::string &data,
                                      int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string redeemer = extract_address_from_topic(topics[1].get<std::string>());

    std::string condition_id = extract_bytes32_from_data(data, 0);
    int64_t index_sets_offset = extract_uint256_from_data(data, 1);
    int64_t payout = extract_uint256_from_data(data, 2);

    int64_t index_sets_len = extract_uint256_from_data(data, index_sets_offset / 32);
    int index_sets = 0;
    for (int64_t i = 0; i < index_sets_len; ++i) {
      int64_t val = extract_uint256_from_data(data, index_sets_offset / 32 + 1 + i);
      index_sets |= static_cast<int>(val);
    }

    std::ostringstream ss;
    ss << block_number << ", " << log_index << ", "
       << sql_blob(redeemer) << ", " << sql_blob(condition_id) << ", "
       << index_sets << ", " << payout;
    events.redemption.push_back(ss.str());
  }

  static void parse_condition_preparation(const json &topics, const std::string &data,
                                          int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string condition_id = topics[1].get<std::string>();
    std::string oracle = extract_address_from_topic(topics[2].get<std::string>());
    std::string question_id = topics[3].get<std::string>();

    std::ostringstream ss;
    ss << sql_blob(condition_id) << ", " << sql_blob(oracle) << ", "
       << sql_blob(question_id) << ", NULL, NULL";
    events.condition.push_back(ss.str());
  }

  static void parse_condition_resolution(const json &topics, const std::string &data,
                                         int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string condition_id = topics[1].get<std::string>();

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
    ss << sql_blob(condition_id) << ", " << sql_str(payout_ss.str()) << ", " << block_number;
    events.condition_resolution.push_back(ss.str());
  }

  static void parse_order_filled(const json &topics, const std::string &data,
                                 int64_t block_number, int64_t log_index,
                                 const std::string &exchange, ParsedEvents &events) {
    std::string maker = extract_address_from_topic(topics[2].get<std::string>());
    std::string taker = extract_address_from_topic(topics[3].get<std::string>());

    int64_t maker_asset_id = extract_uint256_from_data(data, 0);
    int64_t taker_asset_id = extract_uint256_from_data(data, 1);
    int64_t maker_amount = extract_uint256_from_data(data, 2);
    int64_t taker_amount = extract_uint256_from_data(data, 3);
    int64_t fee = extract_uint256_from_data(data, 4);

    std::string token_id;
    int side;
    int64_t usdc_amount, token_amount;

    if (maker_asset_id == 0) {
      token_id = extract_bytes32_from_data(data, 1);
      side = 1;
      usdc_amount = maker_amount;
      token_amount = taker_amount;
    } else {
      token_id = extract_bytes32_from_data(data, 0);
      side = 2;
      usdc_amount = taker_amount;
      token_amount = maker_amount;
    }

    std::ostringstream ss;
    ss << block_number << ", " << log_index << ", " << sql_str(exchange) << ", "
       << sql_blob(maker) << ", " << sql_blob(taker) << ", "
       << sql_blob(token_id) << ", " << side << ", "
       << usdc_amount << ", " << token_amount << ", " << fee;
    events.order_filled.push_back(ss.str());
  }

  static void parse_token_registered(const json &topics, const std::string &data,
                                     int64_t block_number, int64_t log_index,
                                     const std::string &exchange, ParsedEvents &events) {
    std::string token0 = topics[1].get<std::string>();
    std::string token1 = topics[2].get<std::string>();
    std::string condition_id = topics[3].get<std::string>();

    if (token0 > token1) {
      std::swap(token0, token1);
    }

    std::ostringstream ss0;
    ss0 << sql_blob(token0) << ", " << sql_blob(condition_id) << ", "
        << sql_str(exchange) << ", 1";
    events.token_map.push_back(ss0.str());

    std::ostringstream ss1;
    ss1 << sql_blob(token1) << ", " << sql_blob(condition_id) << ", "
        << sql_str(exchange) << ", 0";
    events.token_map.push_back(ss1.str());
  }

  static void parse_positions_converted(const json &topics, const std::string &data,
                                        int64_t block_number, int64_t log_index, ParsedEvents &events) {
    std::string stakeholder = extract_address_from_topic(topics[1].get<std::string>());
    std::string market_id = topics[2].get<std::string>();
    int64_t index_set = hex_to_int64(topics[3].get<std::string>());

    int64_t amount = extract_uint256_from_data(data, 0);

    std::ostringstream ss;
    ss << block_number << ", " << log_index << ", "
       << sql_blob(stakeholder) << ", " << sql_blob(market_id) << ", "
       << index_set << ", " << amount;
    events.convert.push_back(ss.str());
  }

  static void parse_market_prepared(const json &topics, const std::string &data,
                                    int64_t block_number, int64_t log_index, ParsedEvents &events) {
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
    ss << sql_blob(market_id) << ", " << sql_blob(oracle) << ", " << fee_bips;
    if (market_data.empty()) {
      ss << ", NULL";
    } else {
      ss << ", " << sql_blob(market_data);
    }
    events.neg_risk_market.push_back(ss.str());
  }

  static void parse_question_prepared(const json &topics, const std::string &data,
                                      int64_t block_number, int64_t log_index, ParsedEvents &events) {
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
    ss << sql_blob(question_id) << ", " << sql_blob(market_id) << ", " << question_index;
    if (question_data.empty()) {
      ss << ", NULL";
    } else {
      ss << ", " << sql_blob(question_data);
    }
    events.neg_risk_question.push_back(ss.str());
  }
};
