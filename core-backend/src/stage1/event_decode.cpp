#include "event_decode.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>

namespace stage1 {

thread_local std::string EventDecoder::current_log_json_;

const bool EventDecoder::crash_handler_installed_ = []() {
  std::set_terminate([]() {
    std::cerr << "\n[CRASH] terminate called\n";
    const auto &log = EventDecoder::current_log_json_;
    if (!log.empty()) {
      std::cerr << "[CRASH] stage1 current log: " << log << "\n";
    }
    std::abort();
  });
  return true;
}();

DecodedEvents EventDecoder::decode_logs(const std::vector<json> &results) {
  [[maybe_unused]] bool installed = crash_handler_installed_;
  DecodedEvents events;
  std::set<std::string> fpmm_addrs;
  // 第一趟: FPMM创建（扫描所有Factory，不只是Polymarket的）
  for (const auto &result : results) {
    for (const auto &log : result) {
      const auto &topics_arr = log["topics"];
      if (topics_arr.empty()) {
        continue;
      }
      std::string topic0 = to_lower(topics_arr[0].get<std::string>());
      if (topic0 == topics::FPMM_CREATE) {
        std::string factory_addr = to_lower(log["address"].get<std::string>());
        auto new_addr = parse_fpmm_create(log, factory_addr, events);
        if (new_addr) {
          fpmm_addrs.insert(*new_addr);
        }
      }
    }
  }

  // 第二趟: 所有事件
  for (const auto &result : results) {
    for (const auto &log : result) {
      parse_log(log, fpmm_addrs, events);
    }
  }
  return events;
}

const std::string &EventDecoder::current_log_json() {
  return current_log_json_;
}

std::string EventDecoder::to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

int64_t EventDecoder::hex_to_int64(const std::string &hex) {
  std::string s = hex;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s = s.substr(2);
  }
  if (s.empty()) {
    return 0;
  }
  assert(std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }));
  size_t first_non_zero = s.find_first_not_of('0');
  if (first_non_zero == std::string::npos) {
    return 0;
  }
  s = s.substr(first_non_zero);
  assert(s.size() <= 16);
  return static_cast<int64_t>(std::stoull(s, nullptr, 16));
}

std::string EventDecoder::normalize_uint256_hex(const std::string &hex) {
  std::string s = hex;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s = s.substr(2);
  }
  if (s.empty()) {
    s = "0";
  }
  assert(std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }));
  size_t first_non_zero = s.find_first_not_of('0');
  if (first_non_zero == std::string::npos) {
    s = "0";
  } else {
    s = s.substr(first_non_zero);
  }
  assert(s.size() <= 64);
  if (s.size() < 64) {
    s = std::string(64 - s.size(), '0') + s;
  }
  return "0x" + s;
}

std::string EventDecoder::extract_address_from_topic(const std::string &topic) {
  return "0x" + topic.substr(26);
}

std::string EventDecoder::extract_bytes32_from_data(const std::string &data, size_t index) {
  size_t start = 2 + index * 64;
  return "0x" + data.substr(start, 64);
}

std::string EventDecoder::extract_uint256_hex_from_data(const std::string &data, size_t index) {
  std::string hex = extract_bytes32_from_data(data, index);
  return normalize_uint256_hex(hex);
}

int64_t EventDecoder::extract_uint256_i64_from_data(const std::string &data, size_t index) {
  return hex_to_int64(extract_uint256_hex_from_data(data, index));
}

bool EventDecoder::is_fpmm_topic(const std::string &topic0) {
  return topic0 == topics::FPMM_BUY ||
         topic0 == topics::FPMM_SELL ||
         topic0 == topics::FPMM_FUNDING_ADD ||
         topic0 == topics::FPMM_FUNDING_REMOVE;
}

void EventDecoder::parse_log(const json &log, const std::set<std::string> &fpmm_addrs, DecodedEvents &events) {
  (void)fpmm_addrs;
  current_log_json_ = log.dump();
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
  } else if (is_fpmm_topic(topic0)) {
    // stage1 只按 topic 记录 FPMM 事件，不依赖当前批次是否出现 FPMM_CREATE。
    parse_fpmm_event(topic0, address, topics_arr, data, tx_hash, block_number, log_index, events);
  }
}

void EventDecoder::parse_conditional_tokens_event(const std::string &topic0, const json &topics,
                                                  const std::string &data, const std::string &tx_hash,
                                                  int64_t block_number, int64_t log_index,
                                                  DecodedEvents &events) {
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

void EventDecoder::parse_exchange_event(const std::string &topic0, const json &topics,
                                        const std::string &data, const std::string &tx_hash,
                                        int64_t block_number, int64_t log_index,
                                        const std::string &exchange, DecodedEvents &events) {
  if (topic0 == topics::ORDER_FILL) {
    parse_order_filled(topics, data, tx_hash, block_number, log_index, exchange, events);
  } else if (topic0 == topics::TOKEN_REGISTER) {
    parse_token_map(topics, tx_hash, block_number, log_index, exchange, events);
  }
}

void EventDecoder::parse_neg_risk_adapter_event(const std::string &topic0, const json &topics,
                                                const std::string &data, const std::string &tx_hash,
                                                int64_t block_number, int64_t log_index,
                                                DecodedEvents &events) {
  if (topic0 == topics::MARKET_PREPARE) {
    parse_neg_risk_market(topics, data, tx_hash, block_number, log_index, events);
  } else if (topic0 == topics::QUESTION_PREPARE) {
    parse_neg_risk_question(topics, data, tx_hash, block_number, log_index, events);
  } else if (topic0 == topics::POSITION_CONVERT) {
    parse_convert(topics, data, tx_hash, block_number, log_index, events);
  }
}

void EventDecoder::parse_transfer_single(const json &topics, const std::string &data,
                                         const std::string &tx_hash, int64_t block_number,
                                         int64_t log_index, DecodedEvents &events) {
  events.transfer.push_back({block_number, tx_hash, log_index * 1000,
                             extract_address_from_topic(topics[1].get<std::string>()),
                             extract_address_from_topic(topics[2].get<std::string>()),
                             extract_address_from_topic(topics[3].get<std::string>()),
                             extract_bytes32_from_data(data, 0),
                             extract_uint256_hex_from_data(data, 1)});
}

void EventDecoder::parse_transfer_batch(const json &topics, const std::string &data,
                                        const std::string &tx_hash, int64_t block_number,
                                        int64_t log_index, DecodedEvents &events) {
  std::string op = extract_address_from_topic(topics[1].get<std::string>());
  std::string from = extract_address_from_topic(topics[2].get<std::string>());
  std::string to = extract_address_from_topic(topics[3].get<std::string>());

  int64_t ids_offset = extract_uint256_i64_from_data(data, 0);
  int64_t values_offset = extract_uint256_i64_from_data(data, 1);

  int64_t ids_len = extract_uint256_i64_from_data(data, ids_offset / 32);
  int64_t values_len = extract_uint256_i64_from_data(data, values_offset / 32);
  assert(ids_len == values_len);

  for (int64_t i = 0; i < ids_len; ++i) {
    events.transfer.push_back({block_number, tx_hash, log_index * 1000 + i,
                               op, from, to,
                               extract_bytes32_from_data(data, ids_offset / 32 + 1 + i),
                               extract_uint256_hex_from_data(data, values_offset / 32 + 1 + i)});
  }
}

void EventDecoder::parse_split_or_merge(const json &topics, const std::string &data,
                                        const std::string &tx_hash, int64_t block_number,
                                        int64_t log_index, std::vector<SplitMergeEvent> &out) {
  int64_t partition_offset = extract_uint256_i64_from_data(data, 1);
  int64_t partition_len = extract_uint256_i64_from_data(data, partition_offset / 32);
  std::ostringstream partition_ss;
  partition_ss << "[";
  for (int64_t i = 0; i < partition_len; ++i) {
    if (i > 0) {
      partition_ss << ",";
    }
    partition_ss << "\"" << extract_uint256_hex_from_data(data, partition_offset / 32 + 1 + i) << "\"";
  }
  partition_ss << "]";

  out.push_back({block_number, tx_hash, log_index,
                 extract_address_from_topic(topics[1].get<std::string>()),
                 extract_address_from_topic("0x" + data.substr(2, 64)),
                 topics[2].get<std::string>(),
                 topics[3].get<std::string>(),
                 partition_ss.str(),
                 extract_uint256_hex_from_data(data, 2)});
}

void EventDecoder::parse_redemption(const json &topics, const std::string &data,
                                    const std::string &tx_hash, int64_t block_number,
                                    int64_t log_index, DecodedEvents &events) {
  int64_t index_sets_offset = extract_uint256_i64_from_data(data, 1);
  int64_t index_sets_len = extract_uint256_i64_from_data(data, index_sets_offset / 32);
  std::ostringstream index_sets_ss;
  index_sets_ss << "[";
  for (int64_t i = 0; i < index_sets_len; ++i) {
    if (i > 0) {
      index_sets_ss << ",";
    }
    index_sets_ss << "\"" << extract_uint256_hex_from_data(data, index_sets_offset / 32 + 1 + i) << "\"";
  }
  index_sets_ss << "]";

  events.redemption.push_back({block_number, tx_hash, log_index,
                               extract_address_from_topic(topics[1].get<std::string>()),
                               extract_address_from_topic(topics[2].get<std::string>()),
                               topics[3].get<std::string>(),
                               extract_bytes32_from_data(data, 0),
                               index_sets_ss.str(),
                               extract_uint256_hex_from_data(data, 2)});
}

void EventDecoder::parse_condition_preparation(const json &topics, const std::string &data,
                                               const std::string &tx_hash, int64_t block_number,
                                               int64_t log_index, DecodedEvents &events) {
  events.condition_preparation.push_back({block_number, tx_hash, log_index,
                                          topics[1].get<std::string>(),
                                          extract_address_from_topic(topics[2].get<std::string>()),
                                          topics[3].get<std::string>(),
                                          extract_uint256_hex_from_data(data, 0)});
}

void EventDecoder::parse_condition_resolution(const json &topics, const std::string &data,
                                              const std::string &tx_hash, int64_t block_number,
                                              int64_t log_index, DecodedEvents &events) {
  int64_t payout_offset = extract_uint256_i64_from_data(data, 1);
  int64_t payout_len = extract_uint256_i64_from_data(data, payout_offset / 32);
  std::ostringstream payout_ss;
  payout_ss << "[";
  for (int64_t i = 0; i < payout_len; ++i) {
    if (i > 0) {
      payout_ss << ",";
    }
    payout_ss << "\"" << extract_uint256_hex_from_data(data, payout_offset / 32 + 1 + i) << "\"";
  }
  payout_ss << "]";

  events.condition_resolution.push_back({block_number, tx_hash, log_index,
                                         topics[1].get<std::string>(),
                                         extract_address_from_topic(topics[2].get<std::string>()),
                                         topics[3].get<std::string>(),
                                         extract_uint256_hex_from_data(data, 0),
                                         payout_ss.str()});
}

void EventDecoder::parse_order_filled(const json &topics, const std::string &data,
                                      const std::string &tx_hash, int64_t block_number,
                                      int64_t log_index, const std::string &exchange,
                                      DecodedEvents &events) {
  events.order_filled.push_back({block_number, tx_hash, log_index,
                                 exchange,
                                 topics[1].get<std::string>(),
                                 extract_address_from_topic(topics[2].get<std::string>()),
                                 extract_address_from_topic(topics[3].get<std::string>()),
                                 extract_bytes32_from_data(data, 0),
                                 extract_bytes32_from_data(data, 1),
                                 extract_uint256_hex_from_data(data, 2),
                                 extract_uint256_hex_from_data(data, 3),
                                 extract_uint256_hex_from_data(data, 4)});
}

void EventDecoder::parse_token_map(const json &topics, const std::string &tx_hash,
                                   int64_t block_number, int64_t log_index,
                                   const std::string &exchange, DecodedEvents &events) {
  events.token_map.push_back({block_number, tx_hash, log_index,
                              exchange,
                              topics[1].get<std::string>(),
                              topics[2].get<std::string>(),
                              topics[3].get<std::string>()});
}

void EventDecoder::parse_convert(const json &topics, const std::string &data,
                                 const std::string &tx_hash, int64_t block_number,
                                 int64_t log_index, DecodedEvents &events) {
  events.convert.push_back({block_number, tx_hash, log_index,
                            extract_address_from_topic(topics[1].get<std::string>()),
                            topics[2].get<std::string>(),
                            normalize_uint256_hex(topics[3].get<std::string>()),
                            extract_uint256_hex_from_data(data, 0)});
}

void EventDecoder::parse_neg_risk_market(const json &topics, const std::string &data,
                                         const std::string &tx_hash, int64_t block_number,
                                         int64_t log_index, DecodedEvents &events) {
  int64_t data_offset = extract_uint256_i64_from_data(data, 1);
  int64_t data_len = extract_uint256_i64_from_data(data, data_offset / 32);
  std::optional<std::string> market_data;
  if (data_len > 0) {
    size_t start = 2 + (data_offset / 32 + 1) * 64;
    size_t hex_len = static_cast<size_t>(data_len) * 2;
    if (start + hex_len <= data.size()) {
      market_data = "0x" + data.substr(start, hex_len);
    }
  }

  events.neg_risk_market.push_back({block_number, tx_hash, log_index,
                                    topics[1].get<std::string>(),
                                    extract_address_from_topic(topics[2].get<std::string>()),
                                    extract_uint256_hex_from_data(data, 0),
                                    market_data});
}

void EventDecoder::parse_neg_risk_question(const json &topics, const std::string &data,
                                           const std::string &tx_hash, int64_t block_number,
                                           int64_t log_index, DecodedEvents &events) {
  int64_t data_offset = extract_uint256_i64_from_data(data, 1);
  int64_t data_len = extract_uint256_i64_from_data(data, data_offset / 32);
  std::optional<std::string> question_data;
  if (data_len > 0) {
    size_t start = 2 + (data_offset / 32 + 1) * 64;
    size_t hex_len = static_cast<size_t>(data_len) * 2;
    if (start + hex_len <= data.size()) {
      question_data = "0x" + data.substr(start, hex_len);
    }
  }

  events.neg_risk_question.push_back({block_number, tx_hash, log_index,
                                      topics[1].get<std::string>(),
                                      topics[2].get<std::string>(),
                                      extract_uint256_hex_from_data(data, 0),
                                      question_data});
}

std::optional<std::string> EventDecoder::parse_fpmm_create(const json &log, const std::string &factory_addr, DecodedEvents &events) {
  const auto &topics_arr = log["topics"];
  std::string topic0 = to_lower(topics_arr[0].get<std::string>());
  if (topic0 != topics::FPMM_CREATE) {
    return std::nullopt;
  }

  std::string data = log["data"].get<std::string>();
  std::string tx_hash = log["transactionHash"].get<std::string>();
  int64_t block_number = hex_to_int64(log["blockNumber"].get<std::string>());
  int64_t log_index = hex_to_int64(log["logIndex"].get<std::string>());

  // 两种工厂都发同名事件（topic0 相同），但 indexed 布局不同：
  // 1) FixedProductMarketMakerFactory:
  //    topics: [sig, creator, conditionalTokens, collateralToken]
  //    data  : [fixedProductMarketMaker, conditionIds(offset), fee]
  // 2) FPMMDeterministicFactory:
  //    topics: [sig, creator]
  //    data  : [fixedProductMarketMaker, conditionalTokens, collateralToken, conditionIds(offset), fee]
  std::string fpmm_addr;
  std::string conditional_tokens;
  std::string collateral_token;
  int64_t cond_ids_offset = 0;
  std::string fee;

  assert(topics_arr.size() == 2 || topics_arr.size() == 4);
  if (topics_arr.size() == 4) {
    fpmm_addr = extract_address_from_topic("0x" + data.substr(2, 64));
    conditional_tokens = extract_address_from_topic(topics_arr[2].get<std::string>());
    collateral_token = extract_address_from_topic(topics_arr[3].get<std::string>());
    cond_ids_offset = extract_uint256_i64_from_data(data, 1);
    fee = extract_uint256_hex_from_data(data, 2);
  } else {
    fpmm_addr = extract_address_from_topic("0x" + data.substr(2, 64));
    conditional_tokens = extract_address_from_topic("0x" + data.substr(2 + 64, 64));
    collateral_token = extract_address_from_topic("0x" + data.substr(2 + 128, 64));
    cond_ids_offset = extract_uint256_i64_from_data(data, 3);
    fee = extract_uint256_hex_from_data(data, 4);
  }

  int64_t cond_ids_len = extract_uint256_i64_from_data(data, cond_ids_offset / 32);

  std::ostringstream cond_ids_ss;
  cond_ids_ss << "[";
  for (int64_t i = 0; i < cond_ids_len; ++i) {
    if (i > 0) {
      cond_ids_ss << ",";
    }
    cond_ids_ss << "\"" << extract_bytes32_from_data(data, cond_ids_offset / 32 + 1 + i) << "\"";
  }
  cond_ids_ss << "]";

  events.fpmm.push_back({block_number, tx_hash, log_index,
                         factory_addr, // 记录factory地址
                         extract_address_from_topic(topics_arr[1].get<std::string>()),
                         fpmm_addr,
                         conditional_tokens,
                         collateral_token,
                         cond_ids_ss.str(),
                         fee});

  return to_lower(fpmm_addr);
}

void EventDecoder::parse_fpmm_event(const std::string &topic0, const std::string &fpmm_addr,
                                    const json &topics, const std::string &data,
                                    const std::string &tx_hash, int64_t block_number,
                                    int64_t log_index, DecodedEvents &events) {
  if (topic0 == topics::FPMM_BUY) {
    events.fpmm_trade.push_back({block_number, tx_hash, log_index,
                                 fpmm_addr,
                                 extract_address_from_topic(topics[1].get<std::string>()),
                                 1,
                                 normalize_uint256_hex(topics[2].get<std::string>()),
                                 extract_uint256_hex_from_data(data, 0),
                                 extract_uint256_hex_from_data(data, 2),
                                 extract_uint256_hex_from_data(data, 1)});
  } else if (topic0 == topics::FPMM_SELL) {
    events.fpmm_trade.push_back({block_number, tx_hash, log_index,
                                 fpmm_addr,
                                 extract_address_from_topic(topics[1].get<std::string>()),
                                 2,
                                 normalize_uint256_hex(topics[2].get<std::string>()),
                                 extract_uint256_hex_from_data(data, 0),
                                 extract_uint256_hex_from_data(data, 2),
                                 extract_uint256_hex_from_data(data, 1)});
  } else if (topic0 == topics::FPMM_FUNDING_ADD) {
    int64_t amounts_offset = extract_uint256_i64_from_data(data, 0);
    int64_t amounts_len = extract_uint256_i64_from_data(data, amounts_offset / 32);
    std::ostringstream amounts_ss;
    amounts_ss << "[";
    for (int64_t i = 0; i < amounts_len; ++i) {
      if (i > 0) {
        amounts_ss << ",";
      }
      amounts_ss << "\"" << extract_uint256_hex_from_data(data, amounts_offset / 32 + 1 + i) << "\"";
    }
    amounts_ss << "]";

    events.fpmm_funding.push_back({block_number, tx_hash, log_index,
                                   fpmm_addr,
                                   extract_address_from_topic(topics[1].get<std::string>()),
                                   1,
                                   amounts_ss.str(),
                                   normalize_uint256_hex("0x0"),
                                   extract_uint256_hex_from_data(data, 1)});
  } else if (topic0 == topics::FPMM_FUNDING_REMOVE) {
    int64_t amounts_offset = extract_uint256_i64_from_data(data, 0);
    int64_t amounts_len = extract_uint256_i64_from_data(data, amounts_offset / 32);
    std::ostringstream amounts_ss;
    amounts_ss << "[";
    for (int64_t i = 0; i < amounts_len; ++i) {
      if (i > 0) {
        amounts_ss << ",";
      }
      amounts_ss << "\"" << extract_uint256_hex_from_data(data, amounts_offset / 32 + 1 + i) << "\"";
    }
    amounts_ss << "]";

    events.fpmm_funding.push_back({block_number, tx_hash, log_index,
                                   fpmm_addr,
                                   extract_address_from_topic(topics[1].get<std::string>()),
                                   2,
                                   amounts_ss.str(),
                                   extract_uint256_hex_from_data(data, 1),
                                   extract_uint256_hex_from_data(data, 2)});
  }
}

} // namespace stage1
