#include "event_decode.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <iostream>

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

DecodedEvents EventDecoder::decode_logs(std::vector<json> &&results) {
  [[maybe_unused]] bool installed = crash_handler_installed_;
  DecodedEvents events;
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
        parse_fpmm_create(log, factory_addr, events);
      }
    }
  }

  // 第二趟: 所有事件
  for (auto &result : results) {
    for (const auto &log : result) {
      parse_log(log, events);
    }
    // 逐批释放原始RPC日志，避免把整个basic的json长期留在内存里。
    json().swap(result);
  }
  std::vector<json>().swap(results);
  return events;
}

const std::string &EventDecoder::current_log_json() {
  return current_log_json_;
}

std::string EventDecoder::to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

uint8_t EventDecoder::hex_nibble(unsigned char ch) {
  if (ch >= '0' && ch <= '9') {
    return static_cast<uint8_t>(ch - '0');
  }
  if (ch >= 'a' && ch <= 'f') {
    return static_cast<uint8_t>(ch - 'a' + 10);
  }
  if (ch >= 'A' && ch <= 'F') {
    return static_cast<uint8_t>(ch - 'A' + 10);
  }
  assert(false && "invalid hex char");
  return 0;
}

std::string EventDecoder::hex_to_bytes(const std::string &hex) {
  std::string s = hex;
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s = s.substr(2);
  }
  assert((s.size() % 2) == 0);
  std::string out;
  out.resize(s.size() / 2);
  for (size_t i = 0; i < out.size(); ++i) {
    uint8_t hi = hex_nibble(static_cast<unsigned char>(s[2 * i]));
    uint8_t lo = hex_nibble(static_cast<unsigned char>(s[2 * i + 1]));
    out[i] = static_cast<char>((hi << 4) | lo);
  }
  return out;
}

Bytes20 EventDecoder::address_hex_to_bytes20(const std::string &hex) {
  std::string bytes = hex_to_bytes(hex);
  assert(bytes.size() == 20);
  Bytes20 out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

Bytes32 EventDecoder::hex_to_bytes32(const std::string &hex) {
  std::string bytes = hex_to_bytes(hex);
  assert(bytes.size() == 32);
  Bytes32 out{};
  std::copy(bytes.begin(), bytes.end(), out.begin());
  return out;
}

Bytes32 EventDecoder::uint256_hex_to_bytes32(const std::string &hex) {
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
  return hex_to_bytes32(s);
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

Bytes20 EventDecoder::extract_address_from_topic(const std::string &topic) {
  assert(topic.size() == 66);
  return address_hex_to_bytes20(topic.substr(26));
}

Bytes20 EventDecoder::extract_address_from_data_word(const std::string &data, size_t word_index) {
  size_t start = 2 + word_index * 64 + 24;
  assert(start + 40 <= data.size());
  return address_hex_to_bytes20(data.substr(start, 40));
}

Bytes32 EventDecoder::extract_bytes32_from_data(const std::string &data, size_t index) {
  size_t start = 2 + index * 64;
  assert(start + 64 <= data.size());
  return hex_to_bytes32(data.substr(start, 64));
}

Bytes32 EventDecoder::extract_uint256_from_data(const std::string &data, size_t index) {
  size_t start = 2 + index * 64;
  assert(start + 64 <= data.size());
  return uint256_hex_to_bytes32(data.substr(start, 64));
}

int64_t EventDecoder::extract_uint256_i64_from_data(const std::string &data, size_t index) {
  Bytes32 value = extract_uint256_from_data(data, index);
  size_t first_non_zero = 0;
  while (first_non_zero < value.size() && value[first_non_zero] == 0) {
    ++first_non_zero;
  }
  if (first_non_zero == value.size()) {
    return 0;
  }
  assert(value.size() - first_non_zero <= 8);
  uint64_t out = 0;
  for (size_t i = first_non_zero; i < value.size(); ++i) {
    out = (out << 8) | static_cast<uint64_t>(value[i]);
  }
  return static_cast<int64_t>(out);
}

bool EventDecoder::is_fpmm_topic(const std::string &topic0) {
  return topic0 == topics::FPMM_BUY ||
         topic0 == topics::FPMM_SELL ||
         topic0 == topics::FPMM_FUNDING_ADD ||
         topic0 == topics::FPMM_FUNDING_REMOVE;
}

void EventDecoder::parse_log(const json &log, DecodedEvents &events) {
  current_log_json_ = log.dump();
  std::string address = to_lower(log["address"].get<std::string>());
  const auto &topics_arr = log["topics"];
  assert(!topics_arr.empty());

  std::string topic0 = to_lower(topics_arr[0].get<std::string>());
  std::string data = log["data"].get<std::string>();
  Bytes32 tx_hash = hex_to_bytes32(log["transactionHash"].get<std::string>());
  int64_t block_number = hex_to_int64(log["blockNumber"].get<std::string>());
  int64_t log_index = hex_to_int64(log["logIndex"].get<std::string>());

  if (address == contracts::CONDITIONAL_TOKENS) {
    parse_conditional_tokens_event(topic0, topics_arr, data, tx_hash, block_number, log_index, events);
  } else if (address == contracts::CTF_EXCHANGE) {
    parse_exchange_event(topic0, topics_arr, data, tx_hash, block_number, log_index, "ctf", events);
  } else if (address == contracts::NEG_RISK_CTF_EXCHANGE) {
    parse_exchange_event(topic0, topics_arr, data, tx_hash, block_number, log_index, "nrx", events);
  } else if (address == contracts::NEG_RISK_ADAPTER) {
    parse_neg_risk_adapter_event(topic0, topics_arr, data, tx_hash, block_number, log_index, events);
  } else if (address == contracts::FPMM_FACTORY) {
    // 第一趟已处理，跳过
  } else if (is_fpmm_topic(topic0)) {
    // stage1 只按 topic 记录 FPMM 事件，不依赖当前批次是否出现 FPMM_CREATE。
    parse_fpmm_event(topic0, address_hex_to_bytes20(address), topics_arr, data, tx_hash, block_number, log_index, events);
  }
}

void EventDecoder::parse_conditional_tokens_event(const std::string &topic0, const json &topics,
                                                  const std::string &data, const Bytes32 &tx_hash,
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
                                        const std::string &data, const Bytes32 &tx_hash,
                                        int64_t block_number, int64_t log_index,
                                        const std::string &exchange, DecodedEvents &events) {
  if (topic0 == topics::ORDER_FILL) {
    parse_order_filled(topics, data, tx_hash, block_number, log_index, exchange, events);
  } else if (topic0 == topics::TOKEN_REGISTER) {
    parse_token_map(topics, tx_hash, block_number, log_index, exchange, events);
  }
}

void EventDecoder::parse_neg_risk_adapter_event(const std::string &topic0, const json &topics,
                                                const std::string &data, const Bytes32 &tx_hash,
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
                                         const Bytes32 &tx_hash, int64_t block_number,
                                         int64_t log_index, DecodedEvents &events) {
  events.transfer.push_back({block_number, tx_hash, log_index * TRANSFER_FLAT_LOG_SCALE,
                             extract_address_from_topic(topics[1].get<std::string>()),
                             extract_address_from_topic(topics[2].get<std::string>()),
                             extract_address_from_topic(topics[3].get<std::string>()),
                             extract_bytes32_from_data(data, 0),
                             extract_uint256_from_data(data, 1)});
}

void EventDecoder::parse_transfer_batch(const json &topics, const std::string &data,
                                        const Bytes32 &tx_hash, int64_t block_number,
                                        int64_t log_index, DecodedEvents &events) {
  Bytes20 op = extract_address_from_topic(topics[1].get<std::string>());
  Bytes20 from = extract_address_from_topic(topics[2].get<std::string>());
  Bytes20 to = extract_address_from_topic(topics[3].get<std::string>());

  int64_t ids_offset = extract_uint256_i64_from_data(data, 0);
  int64_t values_offset = extract_uint256_i64_from_data(data, 1);

  int64_t ids_len = extract_uint256_i64_from_data(data, ids_offset / 32);
  int64_t values_len = extract_uint256_i64_from_data(data, values_offset / 32);
  assert(ids_len == values_len);
  assert(ids_len < TRANSFER_FLAT_LOG_SCALE);

  for (int64_t i = 0; i < ids_len; ++i) {
    events.transfer.push_back({block_number, tx_hash, log_index * TRANSFER_FLAT_LOG_SCALE + i,
                               op, from, to,
                               extract_bytes32_from_data(data, ids_offset / 32 + 1 + i),
                               extract_uint256_from_data(data, values_offset / 32 + 1 + i)});
  }
}

void EventDecoder::parse_split_or_merge(const json &topics, const std::string &data,
                                        const Bytes32 &tx_hash, int64_t block_number,
                                        int64_t log_index, std::vector<SplitMergeEvent> &out) {
  int64_t partition_offset = extract_uint256_i64_from_data(data, 1);
  int64_t partition_len = extract_uint256_i64_from_data(data, partition_offset / 32);
  std::vector<Bytes32> partition;
  partition.reserve(static_cast<size_t>(partition_len));
  for (int64_t i = 0; i < partition_len; ++i) {
    partition.push_back(extract_uint256_from_data(data, partition_offset / 32 + 1 + i));
  }

  out.push_back({block_number, tx_hash, log_index,
                 extract_address_from_topic(topics[1].get<std::string>()),
                 extract_address_from_data_word(data, 0),
                 hex_to_bytes32(topics[2].get<std::string>()),
                 hex_to_bytes32(topics[3].get<std::string>()),
                 std::move(partition),
                 extract_uint256_from_data(data, 2)});
}

void EventDecoder::parse_redemption(const json &topics, const std::string &data,
                                    const Bytes32 &tx_hash, int64_t block_number,
                                    int64_t log_index, DecodedEvents &events) {
  int64_t index_sets_offset = extract_uint256_i64_from_data(data, 1);
  int64_t index_sets_len = extract_uint256_i64_from_data(data, index_sets_offset / 32);
  std::vector<Bytes32> index_sets;
  index_sets.reserve(static_cast<size_t>(index_sets_len));
  for (int64_t i = 0; i < index_sets_len; ++i) {
    index_sets.push_back(extract_uint256_from_data(data, index_sets_offset / 32 + 1 + i));
  }

  events.redemption.push_back({block_number, tx_hash, log_index,
                               extract_address_from_topic(topics[1].get<std::string>()),
                               extract_address_from_topic(topics[2].get<std::string>()),
                               hex_to_bytes32(topics[3].get<std::string>()),
                               extract_bytes32_from_data(data, 0),
                               std::move(index_sets),
                               extract_uint256_from_data(data, 2)});
}

void EventDecoder::parse_condition_preparation(const json &topics, const std::string &data,
                                               const Bytes32 &tx_hash, int64_t block_number,
                                               int64_t log_index, DecodedEvents &events) {
  events.condition_preparation.push_back({block_number, tx_hash, log_index,
                                          hex_to_bytes32(topics[1].get<std::string>()),
                                          hex_to_bytes32(topics[3].get<std::string>()),
                                          extract_address_from_topic(topics[2].get<std::string>()),
                                          extract_uint256_from_data(data, 0)});
}

void EventDecoder::parse_condition_resolution(const json &topics, const std::string &data,
                                              const Bytes32 &tx_hash, int64_t block_number,
                                              int64_t log_index, DecodedEvents &events) {
  int64_t payout_offset = extract_uint256_i64_from_data(data, 1);
  int64_t payout_len = extract_uint256_i64_from_data(data, payout_offset / 32);
  std::vector<Bytes32> payouts;
  payouts.reserve(static_cast<size_t>(payout_len));
  for (int64_t i = 0; i < payout_len; ++i) {
    payouts.push_back(extract_uint256_from_data(data, payout_offset / 32 + 1 + i));
  }

  events.condition_resolution.push_back({block_number, tx_hash, log_index,
                                         hex_to_bytes32(topics[1].get<std::string>()),
                                         hex_to_bytes32(topics[3].get<std::string>()),
                                         extract_address_from_topic(topics[2].get<std::string>()),
                                         extract_uint256_from_data(data, 0),
                                         std::move(payouts)});
}

void EventDecoder::parse_order_filled(const json &topics, const std::string &data,
                                      const Bytes32 &tx_hash, int64_t block_number,
                                      int64_t log_index, const std::string &exchange,
                                      DecodedEvents &events) {
  events.order_filled.push_back({block_number, tx_hash, log_index,
                                 exchange,
                                 hex_to_bytes32(topics[1].get<std::string>()),
                                 extract_bytes32_from_data(data, 0),
                                 extract_bytes32_from_data(data, 1),
                                 extract_address_from_topic(topics[2].get<std::string>()),
                                 extract_address_from_topic(topics[3].get<std::string>()),
                                 extract_uint256_from_data(data, 2),
                                 extract_uint256_from_data(data, 3),
                                 extract_uint256_from_data(data, 4)});
}

void EventDecoder::parse_token_map(const json &topics, const Bytes32 &tx_hash,
                                   int64_t block_number, int64_t log_index,
                                   const std::string &exchange, DecodedEvents &events) {
  events.token_map.push_back({block_number, tx_hash, log_index,
                              exchange,
                              extract_address_from_topic(topics[1].get<std::string>()),
                              extract_address_from_topic(topics[2].get<std::string>()),
                              hex_to_bytes32(topics[3].get<std::string>())});
}

void EventDecoder::parse_convert(const json &topics, const std::string &data,
                                 const Bytes32 &tx_hash, int64_t block_number,
                                 int64_t log_index, DecodedEvents &events) {
  events.convert.push_back({block_number, tx_hash, log_index,
                            extract_address_from_topic(topics[1].get<std::string>()),
                            hex_to_bytes32(topics[2].get<std::string>()),
                            uint256_hex_to_bytes32(topics[3].get<std::string>()),
                            extract_uint256_from_data(data, 0)});
}

void EventDecoder::parse_neg_risk_market(const json &topics, const std::string &data,
                                         const Bytes32 &tx_hash, int64_t block_number,
                                         int64_t log_index, DecodedEvents &events) {
  int64_t data_offset = extract_uint256_i64_from_data(data, 1);
  int64_t data_len = extract_uint256_i64_from_data(data, data_offset / 32);
  std::optional<std::string> market_data;
  if (data_len > 0) {
    size_t start = 2 + (data_offset / 32 + 1) * 64;
    size_t hex_len = static_cast<size_t>(data_len) * 2;
    if (start + hex_len <= data.size()) {
      market_data = hex_to_bytes(data.substr(start, hex_len));
    }
  }

  events.neg_risk_market.push_back({block_number, tx_hash, log_index,
                                    hex_to_bytes32(topics[1].get<std::string>()),
                                    extract_address_from_topic(topics[2].get<std::string>()),
                                    extract_uint256_from_data(data, 0),
                                    market_data});
}

void EventDecoder::parse_neg_risk_question(const json &topics, const std::string &data,
                                           const Bytes32 &tx_hash, int64_t block_number,
                                           int64_t log_index, DecodedEvents &events) {
  int64_t data_offset = extract_uint256_i64_from_data(data, 1);
  int64_t data_len = extract_uint256_i64_from_data(data, data_offset / 32);
  std::optional<std::string> question_data;
  if (data_len > 0) {
    size_t start = 2 + (data_offset / 32 + 1) * 64;
    size_t hex_len = static_cast<size_t>(data_len) * 2;
    if (start + hex_len <= data.size()) {
      question_data = hex_to_bytes(data.substr(start, hex_len));
    }
  }

  events.neg_risk_question.push_back({block_number, tx_hash, log_index,
                                      hex_to_bytes32(topics[1].get<std::string>()),
                                      hex_to_bytes32(topics[2].get<std::string>()),
                                      extract_uint256_from_data(data, 0),
                                      question_data});
}

std::optional<std::string> EventDecoder::parse_fpmm_create(const json &log, const std::string &factory_addr, DecodedEvents &events) {
  const auto &topics_arr = log["topics"];
  std::string topic0 = to_lower(topics_arr[0].get<std::string>());
  if (topic0 != topics::FPMM_CREATE) {
    return std::nullopt;
  }

  std::string data = log["data"].get<std::string>();
  Bytes32 tx_hash = hex_to_bytes32(log["transactionHash"].get<std::string>());
  int64_t block_number = hex_to_int64(log["blockNumber"].get<std::string>());
  int64_t log_index = hex_to_int64(log["logIndex"].get<std::string>());

  // 两种工厂都发同名事件（topic0 相同），但 indexed 布局不同：
  // 1) FixedProductMarketMakerFactory:
  //    topics: [sig, creator, conditionalTokens, collateralToken]
  //    data  : [fixedProductMarketMaker, conditionIds(offset), fee]
  // 2) FPMMDeterministicFactory:
  //    topics: [sig, creator]
  //    data  : [fixedProductMarketMaker, conditionalTokens, collateralToken, conditionIds(offset), fee]
  Bytes20 fpmm_addr{};
  Bytes20 conditional_tokens{};
  Bytes20 collateral_token{};
  int64_t cond_ids_offset = 0;
  Bytes32 fee{};

  int64_t creation_topics_count = static_cast<int64_t>(topics_arr.size());
  assert(creation_topics_count == 2 || creation_topics_count == 4);
  std::string creation_layout;
  if (creation_topics_count == 4) {
    creation_layout = "fix_v1";
    fpmm_addr = extract_address_from_data_word(data, 0);
    conditional_tokens = extract_address_from_topic(topics_arr[2].get<std::string>());
    collateral_token = extract_address_from_topic(topics_arr[3].get<std::string>());
    cond_ids_offset = extract_uint256_i64_from_data(data, 1);
    fee = extract_uint256_from_data(data, 2);
  } else {
    creation_layout = "det_v1";
    fpmm_addr = extract_address_from_data_word(data, 0);
    conditional_tokens = extract_address_from_data_word(data, 1);
    collateral_token = extract_address_from_data_word(data, 2);
    cond_ids_offset = extract_uint256_i64_from_data(data, 3);
    fee = extract_uint256_from_data(data, 4);
  }

  int64_t cond_ids_len = extract_uint256_i64_from_data(data, cond_ids_offset / 32);

  std::vector<Bytes32> condition_ids;
  condition_ids.reserve(static_cast<size_t>(cond_ids_len));
  for (int64_t i = 0; i < cond_ids_len; ++i) {
    condition_ids.push_back(extract_bytes32_from_data(data, cond_ids_offset / 32 + 1 + i));
  }

  events.fpmm.push_back({block_number, tx_hash, log_index,
                         address_hex_to_bytes20(factory_addr),
                         creation_topics_count,
                         creation_layout,
                         extract_address_from_topic(topics_arr[1].get<std::string>()),
                         fpmm_addr,
                         conditional_tokens,
                         collateral_token,
                         std::move(condition_ids),
                         fee});

  return std::nullopt;
}

void EventDecoder::parse_fpmm_event(const std::string &topic0, const Bytes20 &fpmm_addr,
                                    const json &topics, const std::string &data,
                                    const Bytes32 &tx_hash, int64_t block_number,
                                    int64_t log_index, DecodedEvents &events) {
  if (topic0 == topics::FPMM_BUY) {
    events.fpmm_trade.push_back({block_number, tx_hash, log_index,
                                 fpmm_addr,
                                 extract_address_from_topic(topics[1].get<std::string>()),
                                 1,
                                 uint256_hex_to_bytes32(topics[2].get<std::string>()),
                                 extract_uint256_from_data(data, 0),
                                 extract_uint256_from_data(data, 2),
                                 extract_uint256_from_data(data, 1)});
  } else if (topic0 == topics::FPMM_SELL) {
    events.fpmm_trade.push_back({block_number, tx_hash, log_index,
                                 fpmm_addr,
                                 extract_address_from_topic(topics[1].get<std::string>()),
                                 2,
                                 uint256_hex_to_bytes32(topics[2].get<std::string>()),
                                 extract_uint256_from_data(data, 0),
                                 extract_uint256_from_data(data, 2),
                                 extract_uint256_from_data(data, 1)});
  } else if (topic0 == topics::FPMM_FUNDING_ADD) {
    int64_t amounts_offset = extract_uint256_i64_from_data(data, 0);
    int64_t amounts_len = extract_uint256_i64_from_data(data, amounts_offset / 32);
    std::vector<Bytes32> amounts;
    amounts.reserve(static_cast<size_t>(amounts_len));
    for (int64_t i = 0; i < amounts_len; ++i) {
      amounts.push_back(extract_uint256_from_data(data, amounts_offset / 32 + 1 + i));
    }

    events.fpmm_funding.push_back({block_number, tx_hash, log_index,
                                   fpmm_addr,
                                   extract_address_from_topic(topics[1].get<std::string>()),
                                   1,
                                   std::move(amounts),
                                   uint256_hex_to_bytes32("0x0"),
                                   extract_uint256_from_data(data, 1)});
  } else if (topic0 == topics::FPMM_FUNDING_REMOVE) {
    int64_t amounts_offset = extract_uint256_i64_from_data(data, 0);
    int64_t amounts_len = extract_uint256_i64_from_data(data, amounts_offset / 32);
    std::vector<Bytes32> amounts;
    amounts.reserve(static_cast<size_t>(amounts_len));
    for (int64_t i = 0; i < amounts_len; ++i) {
      amounts.push_back(extract_uint256_from_data(data, amounts_offset / 32 + 1 + i));
    }

    events.fpmm_funding.push_back({block_number, tx_hash, log_index,
                                   fpmm_addr,
                                   extract_address_from_topic(topics[1].get<std::string>()),
                                   2,
                                   std::move(amounts),
                                   extract_uint256_from_data(data, 1),
                                   extract_uint256_from_data(data, 2)});
  }
}

} // namespace stage1
