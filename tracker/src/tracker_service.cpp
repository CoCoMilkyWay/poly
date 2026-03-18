#include "tracker/tracker_service.hpp"
#include "tracker/http_client.hpp"
#include "tracker/json_store.hpp"
#include "tracker/logger.hpp"
#include "tracker/progress_board.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/ssl.h>

#include <cctype>
#include <chrono>
#include <optional>
#include <thread>

namespace tracker {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

inline constexpr const char *kGammaApiBase = "https://gamma-api.polymarket.com";
inline constexpr const char *kPolymarketSubgraphId = "81Dm16JjuFSrqz813HysXoUPvzTwE7fsfPk2RTf66nyC";
inline constexpr const char *kPnlSubgraphId = "6c58N5U4MtQE2Y8njfVrrAfRykzfqajMGeTMEvMmskVz";

inline constexpr const char *kMetaQuery = R"(
query Meta {
  _meta {
    block {
      number
    }
  }
}
)";

inline constexpr int64_t kSnapshotBlockLag = 64;

inline constexpr const char *kUserPositionsQuery = R"(
query UserPositions($users: [String!]!, $after: String!, $block: Int!, $first: Int!) {
  userPositions(
    first: $first
    orderBy: id
    orderDirection: asc
    block: { number: $block }
    where: {user_in: $users, amount_gt: "0", id_gt: $after}
  ) {
    id
    user
    tokenId
    amount
  }
}
)";

inline constexpr const char *kMarketDataLatestQuery = R"(
query MarketDatas($ids: [ID!]!) {
  marketDatas(
    where: {id_in: $ids}
    orderBy: id
    orderDirection: asc
  ) {
    id
    outcomeIndex
    priceOrderbook
    condition {
      id
      questionId
      outcomeSlotCount
      resolutionTimestamp
      payoutNumerators
      payoutDenominator
    }
  }
}
)";

inline constexpr const char *kConditionsLatestQuery = R"(
query Conditions($ids: [ID!]!) {
  conditions(
    where: {id_in: $ids}
    orderBy: id
    orderDirection: asc
  ) {
    id
    positionIds
    payoutNumerators
    payoutDenominator
  }
}
)";

class RpcWebSocketStream {
public:
  explicit RpcWebSocketStream(const std::string &url)
      : parts_(parse_url(url)),
        ssl_ctx_(ssl::context::tlsv12_client),
        resolver_(ioc_) {
    assert(parts_.websocket());
    if (parts_.secure()) {
      ssl_ctx_.set_default_verify_paths();
      ssl_ctx_.set_verify_mode(ssl::verify_peer);
    }
  }

  void start() {
    const tcp::resolver::results_type endpoints = resolver_.resolve(parts_.host, parts_.port);
    const std::string host = parts_.host + ":" + parts_.port;
    if (parts_.secure()) {
      ssl_ws_.emplace(ioc_, ssl_ctx_);
      ssl_ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
      [[maybe_unused]] const int sni = SSL_set_tlsext_host_name(
          ssl_ws_->next_layer().native_handle(), parts_.host.c_str());
      assert(sni == 1);
      beast::get_lowest_layer(*ssl_ws_).connect(endpoints);
      ssl_ws_->next_layer().handshake(ssl::stream_base::client);
      ssl_ws_->handshake(host, parts_.target);
    } else {
      plain_ws_.emplace(ioc_);
      plain_ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
      beast::get_lowest_layer(*plain_ws_).connect(endpoints);
      plain_ws_->handshake(host, parts_.target);
    }
  }

  std::string subscribe_new_heads() {
    return subscribe(json::array({"newHeads"}), "newHeads");
  }

  std::string subscribe_logs(const json &filter) {
    return subscribe(json::array({"logs", filter}), "logs");
  }

  std::optional<json> try_read_message() {
    if (!queued_messages_.empty()) {
      json payload = std::move(queued_messages_.front());
      queued_messages_.pop_front();
      return payload;
    }
    if (!has_pending_frame()) {
      return std::nullopt;
    }
    return read_json();
  }

private:
  std::string subscribe(const json &params, const std::string &label) {
    const int request_id = next_request_id_++;
    write_json({
        {"jsonrpc", "2.0"},
        {"id", request_id},
        {"method", "eth_subscribe"},
        {"params", params},
    });
    while (true) {
      const json payload = read_json();
      if (payload.contains("id")) {
        assert(payload.at("id").get<int>() == request_id);
        assert(payload.contains("result"));
        const std::string subscription_id = normalize_hex(payload.at("result").get<std::string>());
        sync_logger().info("rpc ws subscribed " + label + " " + subscription_id);
        return subscription_id;
      }
      queued_messages_.push_back(payload);
    }
  }

  void write_json(const json &payload) {
    const std::string body = payload.dump();
    if (parts_.secure()) {
      ssl_ws_->write(asio::buffer(body));
      return;
    }
    plain_ws_->write(asio::buffer(body));
  }

  json read_json() {
    buffer_.consume(buffer_.size());
    if (parts_.secure()) {
      ssl_ws_->read(buffer_);
      assert(ssl_ws_->got_text());
    } else {
      plain_ws_->read(buffer_);
      assert(plain_ws_->got_text());
    }
    const std::string body = beast::buffers_to_string(buffer_.cdata());
    buffer_.consume(buffer_.size());
    return safe_json_parse(body);
  }

  bool has_pending_frame() {
    boost::system::error_code ec;
    const size_t available = parts_.secure()
                                 ? beast::get_lowest_layer(*ssl_ws_).socket().available(ec)
                                 : beast::get_lowest_layer(*plain_ws_).socket().available(ec);
    assert(!ec);
    return available > 0;
  }

  UrlParts parts_;
  asio::io_context ioc_;
  ssl::context ssl_ctx_;
  tcp::resolver resolver_;
  std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ssl_ws_;
  std::optional<websocket::stream<beast::tcp_stream>> plain_ws_;
  beast::flat_buffer buffer_;
  std::deque<json> queued_messages_;
  int next_request_id_ = 1;
};

struct MarketDataRow {
  std::string token_id;
  std::string condition_id;
  std::string question_id;
  int outcome_slot_count = -1;
  int64_t resolution_timestamp = -1;
  int outcome_index = -1;
  std::optional<long double> price_orderbook;
  std::vector<BigInt> payout_numerators;
  BigInt payout_denominator = 0;
  bool has_payout_denominator = false;
};

struct ConditionDataRow {
  std::string condition_id;
  std::vector<std::string> position_ids;
  std::vector<BigInt> payout_numerators;
  BigInt payout_denominator = 0;
  bool has_payout_denominator = false;
};

struct PricePoint {
  long double value = -1.0L;
  std::string source;
};

struct TransferLeg {
  uint64_t block_number = 0;
  uint64_t transaction_index = 0;
  std::string tx_hash;
  int64_t log_index = 0;
  std::string operator_addr;
  std::string from_addr;
  std::string to_addr;
  std::string token_id;
  BigInt amount_raw = 0;
};

struct OrderFillRow {
  uint64_t block_number = 0;
  uint64_t transaction_index = 0;
  std::string tx_hash;
  int64_t log_index = 0;
  std::string exchange;
  std::string maker;
  std::string taker;
  std::string buyer;
  std::string seller;
  std::string token_id;
  BigInt token_amount_raw = 0;
  BigInt collateral_amount_raw = 0;
  BigInt fee_raw = 0;

  [[nodiscard]] long double price() const {
    const long double token = token_amount_raw.convert_to<long double>();
    assert(token > 0.0L);
    return collateral_amount_raw.convert_to<long double>() / token;
  }
};

struct TxContext {
  uint64_t block_number = 0;
  uint64_t transaction_index = 0;
  std::string tx_hash;
  std::vector<TransferLeg> transfers;
  std::set<std::string> split_users;
  std::set<std::string> merge_users;
  std::set<std::string> redemption_users;
  std::set<std::string> convert_users;
  std::vector<OrderFillRow> order_fills;
};

std::string json_string_or_empty(const json &row, const char *key) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return "";
  }
  if (row.at(key).is_string()) {
    return row.at(key).get<std::string>();
  }
  return row.at(key).dump();
}

int json_int_or(const json &row, const char *key, int fallback = -1) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return fallback;
  }
  if (row.at(key).is_number_integer()) {
    return row.at(key).get<int>();
  }
  if (row.at(key).is_string()) {
    return std::stoi(row.at(key).get<std::string>());
  }
  assert(false);
  return fallback;
}

int64_t json_i64_or(const json &row, const char *key, int64_t fallback = -1) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return fallback;
  }
  if (row.at(key).is_number_integer()) {
    return row.at(key).get<int64_t>();
  }
  if (row.at(key).is_string()) {
    return std::stoll(row.at(key).get<std::string>());
  }
  assert(false);
  return fallback;
}

BigInt json_bigint_or(const json &row, const char *key) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return 0;
  }
  if (row.at(key).is_string()) {
    return big_int_from_dec(row.at(key).get<std::string>());
  }
  if (row.at(key).is_number_integer()) {
    return big_int_from_dec(std::to_string(row.at(key).get<int64_t>()));
  }
  assert(false);
  return 0;
}

std::vector<BigInt> json_bigint_array_or(const json &row, const char *key) {
  std::vector<BigInt> result;
  if (!row.contains(key) || row.at(key).is_null()) {
    return result;
  }
  assert(row.at(key).is_array());
  for (const json &item : row.at(key)) {
    if (item.is_string()) {
      result.push_back(big_int_from_dec(item.get<std::string>()));
    } else if (item.is_number_integer()) {
      result.push_back(big_int_from_dec(std::to_string(item.get<int64_t>())));
    } else {
      assert(false);
    }
  }
  return result;
}

std::vector<std::string> json_string_array_or(const json &row, const char *key) {
  std::vector<std::string> result;
  if (!row.contains(key) || row.at(key).is_null()) {
    return result;
  }
  assert(row.at(key).is_array());
  for (const json &item : row.at(key)) {
    if (item.is_string()) {
      result.push_back(item.get<std::string>());
      continue;
    }
    if (item.is_number_integer()) {
      result.push_back(std::to_string(item.get<int64_t>()));
      continue;
    }
    if (item.is_number_unsigned()) {
      result.push_back(std::to_string(item.get<uint64_t>()));
      continue;
    }
    assert(false);
  }
  return result;
}

std::string json_string_or_integer_string(const json &value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<int64_t>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<uint64_t>());
  }
  assert(false);
  return "";
}

void merge_condition_meta(ConditionMeta &target, const ConditionMeta &source) {
  if (target.condition_id.empty()) {
    target.condition_id = source.condition_id;
  }
  if (target.question_id.empty()) {
    target.question_id = source.question_id;
  }
  if (target.outcome_slot_count < 0) {
    target.outcome_slot_count = source.outcome_slot_count;
  }
  if (target.resolution_timestamp < 0) {
    target.resolution_timestamp = source.resolution_timestamp;
  }
  if (target.token_ids.empty()) {
    target.token_ids = source.token_ids;
  }
  if (target.payout_numerators.empty()) {
    target.payout_numerators = source.payout_numerators;
  }
  if (!target.has_payout_denominator && source.has_payout_denominator) {
    target.payout_denominator = source.payout_denominator;
    target.has_payout_denominator = true;
  }
  if (target.market_question.empty()) {
    target.market_question = source.market_question;
  }
  if (target.market_description.empty()) {
    target.market_description = source.market_description;
  }
  if (target.market_event_title.empty()) {
    target.market_event_title = source.market_event_title;
  }
  if (target.market_slug.empty()) {
    target.market_slug = source.market_slug;
  }
  if (target.market_url.empty()) {
    target.market_url = source.market_url;
  }
  if (target.market_outcomes.empty()) {
    target.market_outcomes = source.market_outcomes;
  }
}

void merge_token_meta(TokenMeta &target, const TokenMeta &source) {
  if (target.token_id.empty()) {
    target.token_id = source.token_id;
  }
  if (target.condition_id.empty()) {
    target.condition_id = source.condition_id;
  }
  if (target.question_id.empty()) {
    target.question_id = source.question_id;
  }
  if (target.outcome_index < 0) {
    target.outcome_index = source.outcome_index;
  }
  if (target.outcome_slot_count < 0) {
    target.outcome_slot_count = source.outcome_slot_count;
  }
  if (target.resolution_timestamp < 0) {
    target.resolution_timestamp = source.resolution_timestamp;
  }
  if (target.payout_numerators.empty()) {
    target.payout_numerators = source.payout_numerators;
  }
  if (!target.has_payout_denominator && source.has_payout_denominator) {
    target.payout_denominator = source.payout_denominator;
    target.has_payout_denominator = true;
  }
  if (source.price >= 0.0L) {
    target.price = source.price;
    target.price_source = source.price_source;
  }
}

std::optional<long double> resolved_price_from_payouts(const std::vector<BigInt> &payout_numerators,
                                                       const BigInt &payout_denominator,
                                                       bool has_payout_denominator,
                                                       int outcome_index) {
  if (!has_payout_denominator) {
    return std::nullopt;
  }
  if (outcome_index < 0 || static_cast<size_t>(outcome_index) >= payout_numerators.size()) {
    return std::nullopt;
  }
  const long double denominator = payout_denominator.convert_to<long double>();
  if (denominator <= 0.0L) {
    return std::nullopt;
  }
  return payout_numerators[static_cast<size_t>(outcome_index)].convert_to<long double>() / denominator;
}

std::string log_unique_key(const json &log) {
  return log.at("blockNumber").get<std::string>() + "|" +
         normalize_hex(log.at("transactionHash").get<std::string>()) + "|" +
         log.at("logIndex").get<std::string>() + "|" +
         normalize_hex(log.at("address").get<std::string>());
}

std::tuple<uint64_t, uint64_t, uint64_t, std::string> log_sort_key(const json &log) {
  return {
      hex_to_u64(log.at("blockNumber").get<std::string>()),
      hex_to_u64(log.at("transactionIndex").get<std::string>()),
      hex_to_u64(log.at("logIndex").get<std::string>()),
      normalize_hex(log.at("address").get<std::string>()),
  };
}

TransferLeg parse_transfer_single(const json &log) {
  const json topics = log.at("topics");
  assert(topics.is_array() && topics.size() == 4);
  const std::string data = log.at("data").get<std::string>();
  return TransferLeg{
      .block_number = hex_to_u64(log.at("blockNumber").get<std::string>()),
      .transaction_index = hex_to_u64(log.at("transactionIndex").get<std::string>()),
      .tx_hash = normalize_hex(log.at("transactionHash").get<std::string>()),
      .log_index = static_cast<int64_t>(hex_to_u64(log.at("logIndex").get<std::string>()) * kTransferFlatLogScale),
      .operator_addr = extract_address_from_topic(topics.at(1).get<std::string>()),
      .from_addr = extract_address_from_topic(topics.at(2).get<std::string>()),
      .to_addr = extract_address_from_topic(topics.at(3).get<std::string>()),
      .token_id = big_int_to_string(extract_uint256_word(data, 0)),
      .amount_raw = extract_uint256_word(data, 1),
  };
}

std::vector<TransferLeg> parse_transfer_batch(const json &log) {
  const json topics = log.at("topics");
  assert(topics.is_array() && topics.size() == 4);
  const std::string data = log.at("data").get<std::string>();
  const std::vector<BigInt> ids = extract_uint256_array_from_offset(data, extract_uint256_word(data, 0));
  const std::vector<BigInt> values = extract_uint256_array_from_offset(data, extract_uint256_word(data, 1));
  assert(ids.size() == values.size());

  std::vector<TransferLeg> result;
  result.reserve(ids.size());
  const uint64_t block_number = hex_to_u64(log.at("blockNumber").get<std::string>());
  const uint64_t transaction_index = hex_to_u64(log.at("transactionIndex").get<std::string>());
  const std::string tx_hash = normalize_hex(log.at("transactionHash").get<std::string>());
  const uint64_t raw_log_index = hex_to_u64(log.at("logIndex").get<std::string>());
  const std::string op = extract_address_from_topic(topics.at(1).get<std::string>());
  const std::string from = extract_address_from_topic(topics.at(2).get<std::string>());
  const std::string to = extract_address_from_topic(topics.at(3).get<std::string>());

  for (size_t index = 0; index < ids.size(); ++index) {
    result.push_back(TransferLeg{
        .block_number = block_number,
        .transaction_index = transaction_index,
        .tx_hash = tx_hash,
        .log_index = static_cast<int64_t>(raw_log_index * kTransferFlatLogScale + index),
        .operator_addr = op,
        .from_addr = from,
        .to_addr = to,
        .token_id = big_int_to_string(ids[index]),
        .amount_raw = values[index],
    });
  }
  return result;
}

OrderFillRow parse_order_fill(const json &log) {
  const json topics = log.at("topics");
  assert(topics.is_array() && topics.size() == 4);
  const std::string data = log.at("data").get<std::string>();
  const std::string maker = extract_address_from_topic(topics.at(2).get<std::string>());
  const std::string taker = extract_address_from_topic(topics.at(3).get<std::string>());
  const BigInt maker_asset_id = extract_uint256_word(data, 0);
  const BigInt taker_asset_id = extract_uint256_word(data, 1);
  const BigInt maker_amount = extract_uint256_word(data, 2);
  const BigInt taker_amount = extract_uint256_word(data, 3);
  const BigInt fee = extract_uint256_word(data, 4);
  const bool maker_is_collateral = maker_asset_id == 0;
  assert(maker_is_collateral != (taker_asset_id == 0));

  OrderFillRow row{
      .block_number = hex_to_u64(log.at("blockNumber").get<std::string>()),
      .transaction_index = hex_to_u64(log.at("transactionIndex").get<std::string>()),
      .tx_hash = normalize_hex(log.at("transactionHash").get<std::string>()),
      .log_index = static_cast<int64_t>(hex_to_u64(log.at("logIndex").get<std::string>())),
      .exchange = normalize_hex(log.at("address").get<std::string>()),
      .maker = maker,
      .taker = taker,
      .buyer = maker_is_collateral ? maker : taker,
      .seller = maker_is_collateral ? taker : maker,
      .token_id = maker_is_collateral ? big_int_to_string(taker_asset_id) : big_int_to_string(maker_asset_id),
      .token_amount_raw = maker_is_collateral ? taker_amount : maker_amount,
      .collateral_amount_raw = maker_is_collateral ? maker_amount : taker_amount,
      .fee_raw = fee,
  };
  return row;
}

std::vector<TxContext> build_tx_contexts(const std::vector<json> &raw_logs) {
  std::map<std::string, TxContext> contexts;
  for (const json &log : raw_logs) {
    const std::string tx_hash = normalize_hex(log.at("transactionHash").get<std::string>());
    TxContext &context = contexts[tx_hash];
    if (context.tx_hash.empty()) {
      context.tx_hash = tx_hash;
      context.block_number = hex_to_u64(log.at("blockNumber").get<std::string>());
      context.transaction_index = hex_to_u64(log.at("transactionIndex").get<std::string>());
    }

    const std::string address = normalize_hex(log.at("address").get<std::string>());
    const std::string topic0 = normalize_hex(log.at("topics").at(0).get<std::string>());
    if (address == kConditionalTokens && topic0 == kTransferSingleTopic) {
      context.transfers.push_back(parse_transfer_single(log));
      continue;
    }
    if (address == kConditionalTokens && topic0 == kTransferBatchTopic) {
      std::vector<TransferLeg> legs = parse_transfer_batch(log);
      context.transfers.insert(context.transfers.end(), legs.begin(), legs.end());
      continue;
    }
    if (address == kConditionalTokens && topic0 == kPositionSplitTopic) {
      context.split_users.insert(extract_address_from_topic(log.at("topics").at(1).get<std::string>()));
      continue;
    }
    if (address == kConditionalTokens && topic0 == kPositionMergeTopic) {
      context.merge_users.insert(extract_address_from_topic(log.at("topics").at(1).get<std::string>()));
      continue;
    }
    if (address == kConditionalTokens && topic0 == kPositionRedeemTopic) {
      context.redemption_users.insert(extract_address_from_topic(log.at("topics").at(1).get<std::string>()));
      continue;
    }
    if ((address == kCtfExchange || address == kNegRiskCtfExchange) && topic0 == kOrderFillTopic) {
      context.order_fills.push_back(parse_order_fill(log));
      continue;
    }
    if (address == kNegRiskAdapter && topic0 == kPositionConvertTopic) {
      context.convert_users.insert(extract_address_from_topic(log.at("topics").at(1).get<std::string>()));
      continue;
    }
  }

  std::vector<TxContext> result;
  result.reserve(contexts.size());
  for (auto &[_, context] : contexts) {
    std::sort(context.transfers.begin(), context.transfers.end(), [](const TransferLeg &lhs, const TransferLeg &rhs) {
      return lhs.log_index < rhs.log_index;
    });
    std::sort(context.order_fills.begin(), context.order_fills.end(), [](const OrderFillRow &lhs, const OrderFillRow &rhs) {
      return lhs.log_index < rhs.log_index;
    });
    result.push_back(context);
  }
  std::sort(result.begin(), result.end(), [](const TxContext &lhs, const TxContext &rhs) {
    if (lhs.block_number != rhs.block_number) {
      return lhs.block_number < rhs.block_number;
    }
    if (lhs.transaction_index != rhs.transaction_index) {
      return lhs.transaction_index < rhs.transaction_index;
    }
    const int64_t lhs_min = lhs.transfers.empty() ? 0 : lhs.transfers.front().log_index;
    const int64_t rhs_min = rhs.transfers.empty() ? 0 : rhs.transfers.front().log_index;
    return lhs_min < rhs_min;
  });
  return result;
}

const OrderFillRow *find_matched_buy(const TxContext &context, const TransferLeg &transfer, const std::string &user) {
  for (const OrderFillRow &fill : context.order_fills) {
    if (fill.buyer == user && fill.token_id == transfer.token_id && fill.token_amount_raw == transfer.amount_raw) {
      return &fill;
    }
  }
  return nullptr;
}

const OrderFillRow *find_matched_sell(const TxContext &context, const TransferLeg &transfer, const std::string &user) {
  for (const OrderFillRow &fill : context.order_fills) {
    if (fill.seller == user && fill.token_id == transfer.token_id && fill.token_amount_raw == transfer.amount_raw) {
      return &fill;
    }
  }
  return nullptr;
}

bool is_protocol_addr(const std::string &address) {
  return address == kConditionalTokens
         || address == kCtfExchange
         || address == kNegRiskCtfExchange
         || address == kNegRiskAdapter
         || address == kZeroAddress;
}

json build_event_json(const TxContext &context,
                      const TransferLeg &transfer,
                      const std::string &user,
                      const std::string &direction,
                      const std::string &kind,
                      const OrderFillRow *fill) {
  json result = {
      {"block_number", transfer.block_number},
      {"tx_hash", transfer.tx_hash},
      {"log_index", transfer.log_index},
      {"user", user},
      {"direction", direction},
      {"kind", kind},
      {"token_id", transfer.token_id},
      {"amount_raw", big_int_to_string(transfer.amount_raw)},
      {"counterparty", direction == "in" ? transfer.from_addr : transfer.to_addr},
      {"operator", transfer.operator_addr},
      {"tx_transfer_count", context.transfers.size()},
      {"tx_order_fill_count", context.order_fills.size()},
  };
  if (fill != nullptr) {
    result["exchange"] = fill->exchange;
    result["collateral_amount_raw"] = big_int_to_string(fill->collateral_amount_raw);
    result["price"] = format_decimal(fill->price(), 10);
    result["fee_raw"] = big_int_to_string(fill->fee_raw);
  }
  return result;
}

} // namespace

TrackerService::TrackerService(AppConfig config)
    : config_(std::move(config)),
      snapshot_root_(json::object()),
      history_root_(json::object()) {
  // 初始化logger
  sync_logger().init(config_.log_file);
}

json TrackerService::rpc_call(const std::string &method, const json &params) {
  const json payload = {
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", method},
      {"params", params},
  };
  const HttpResponse response = http_post_json(config_.rpc_http_url, payload);
  assert(response.status == 200);
  const json body = safe_json_parse(response.body);
  assert(body.contains("result"));
  {
    std::lock_guard<std::mutex> guard(mutex_);
    query_counters_.rpc_http_calls += 1;
  }
  return body.at("result");
}

json TrackerService::rpc_batch_call(const std::vector<json> &requests) {
  json payload = json::array();
  int next_id = 1;
  for (const json &request : requests) {
    json item = request;
    item["jsonrpc"] = "2.0";
    item["id"] = next_id++;
    payload.push_back(std::move(item));
  }
  const HttpResponse response = http_post_json(config_.rpc_http_url, payload);
  assert(response.status == 200);
  json body = safe_json_parse(response.body);
  assert(body.is_array());
  std::sort(body.begin(), body.end(), [](const json &lhs, const json &rhs) {
    return lhs.at("id").get<int>() < rhs.at("id").get<int>();
  });
  {
    std::lock_guard<std::mutex> guard(mutex_);
    query_counters_.rpc_http_calls += requests.size();
  }
  return body;
}

json TrackerService::graph_query(const std::string &subgraph_id, const std::string &query, const json &variables, const std::string &label) {
  assert(!config_.graph_api_key.empty());
  const std::string url = "https://gateway.thegraph.com/api/" + config_.graph_api_key + "/subgraphs/id/" + subgraph_id;
  const json payload = {
      {"query", query},
      {"variables", variables},
  };
  size_t retry = 0;
  for (;;) {
    const HttpResponse response = http_post_json(url, payload);
    assert(response.status == 200);
    const json body = safe_json_parse(response.body);
    if (!body.contains("errors") && body.contains("data")) {
      {
        std::lock_guard<std::mutex> guard(mutex_);
        query_counters_.subgraph_queries += 1;
      }
      return body.at("data");
    }
    ++retry;
    sync_logger().warn("subgraph retry " + std::to_string(retry) + ": " + label + " - " + body.dump());
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

uint64_t TrackerService::rpc_block_number() {
  return hex_to_u64(rpc_call("eth_blockNumber", json::array()).get<std::string>());
}

void TrackerService::load_seed_meta() {
  if (!std::filesystem::exists(config_.seed_rebuild_file)) {
    return;
  }
  const json payload = load_json_file(config_.seed_rebuild_file, json::object());
  if (!payload.is_object()) {
    return;
  }

  std::lock_guard<std::mutex> guard(mutex_);
  if (payload.contains("conditions") && payload.at("conditions").is_array()) {
    for (const json &row : payload.at("conditions")) {
      const std::string condition_id = json_string_or_empty(row, "condition_id");
      if (condition_id.empty()) {
        continue;
      }
      ConditionMeta incoming;
      incoming.condition_id = condition_id;
      incoming.question_id = json_string_or_empty(row, "question_id");
      incoming.outcome_slot_count = json_int_or(row, "outcome_slot_count", -1);
      incoming.resolution_timestamp = json_i64_or(row, "resolution_timestamp", -1);
      incoming.token_ids = json_string_array_or(row, "token_ids");
      incoming.payout_numerators = json_bigint_array_or(row, "payout_numerators");
      if (row.contains("payout_denominator") && !row.at("payout_denominator").is_null()) {
        incoming.payout_denominator = json_bigint_or(row, "payout_denominator");
        incoming.has_payout_denominator = true;
      }
      incoming.market_question = json_string_or_empty(row, "market_question");
      incoming.market_description = json_string_or_empty(row, "market_description");
      incoming.market_event_title = json_string_or_empty(row, "market_event_title");
      incoming.market_slug = json_string_or_empty(row, "market_slug");
      incoming.market_url = json_string_or_empty(row, "market_url");
      incoming.market_outcomes = json_string_array_or(row, "market_outcomes");

      ConditionMeta &target = conditions_[condition_id];
      merge_condition_meta(target, incoming);
      target.condition_id = condition_id;
    }
  }

  if (payload.contains("tokens") && payload.at("tokens").is_array()) {
    for (const json &row : payload.at("tokens")) {
      const std::string token_id = json_string_or_empty(row, "token_id");
      if (token_id.empty()) {
        continue;
      }
      TokenMeta incoming;
      incoming.token_id = token_id;
      incoming.condition_id = json_string_or_empty(row, "condition_id");
      if (row.contains("price") && row.at("price").is_string()) {
        incoming.price = parse_decimal_string(row.at("price").get<std::string>());
        incoming.price_source = "seed";
      }
      TokenMeta &target = tokens_[token_id];
      merge_token_meta(target, incoming);
      target.token_id = token_id;
    }
  }
}

void TrackerService::restore_current_state_from_aggregate(const json &payload) {
  assert(payload.is_object());
  if (payload.contains("summary") && payload.at("summary").is_object()) {
    const json &summary = payload.at("summary");
    last_snapshot_block_ = static_cast<uint64_t>(json_i64_or(summary, "snapshot_block", 0));
    last_applied_block_ = static_cast<uint64_t>(json_i64_or(summary, "last_applied_block", 0));
    head_block_ = static_cast<uint64_t>(json_i64_or(summary, "head_block", 0));
    last_resync_started_at_ = json_i64_or(summary, "last_resync_started_at_unix_sec", 0);
    last_resync_finished_at_ = json_i64_or(summary, "last_resync_finished_at_unix_sec", 0);
  }

  if (payload.contains("recent_events") && payload.at("recent_events").is_array()) {
    recent_events_.clear();
    for (const json &item : payload.at("recent_events")) {
      recent_events_.push_back(item);
    }
  }

  if (payload.contains("users") && payload.at("users").is_array()) {
    for (const json &row : payload.at("users")) {
      const std::string user = json_string_or_empty(row, "user");
      if (user.empty()) {
        continue;
      }
      UserRuntimeState &state = users_[user];
      state.user = user;
      state.token_amounts.clear();
      if (row.contains("positions") && row.at("positions").is_array()) {
        for (const json &position : row.at("positions")) {
          const std::string token_id = json_string_or_empty(position, "token_id");
          if (token_id.empty()) {
            continue;
          }
          if (json_string_or_empty(position, "asset_type") == "stable") {
            continue;
          }
          state.token_amounts[token_id] = json_bigint_or(position, "amount_raw");
        }
      }
      if (row.contains("stable_balances") && row.at("stable_balances").is_object()) {
        const json &stable = row.at("stable_balances");
        state.stable_balances.usdc_e_raw = json_bigint_or(stable, "usdc_e_raw");
        state.stable_balances.wrapped_collateral_raw = json_bigint_or(stable, "wrapped_collateral_raw");
      }
    }
  }
}

void TrackerService::load_persisted_files() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    const json meta_payload = load_json_file(config_.meta_file, json::object());
    if (meta_payload.contains("conditions") && meta_payload.at("conditions").is_object()) {
      for (auto it = meta_payload.at("conditions").begin(); it != meta_payload.at("conditions").end(); ++it) {
        ConditionMeta incoming;
        incoming.condition_id = it.key();
        incoming.question_id = json_string_or_empty(it.value(), "question_id");
        incoming.outcome_slot_count = json_int_or(it.value(), "outcome_slot_count", -1);
        incoming.resolution_timestamp = json_i64_or(it.value(), "resolution_timestamp", -1);
        incoming.token_ids = json_string_array_or(it.value(), "token_ids");
        incoming.payout_numerators = json_bigint_array_or(it.value(), "payout_numerators");
        if (it.value().contains("payout_denominator") && !it.value().at("payout_denominator").is_null()) {
          incoming.payout_denominator = json_bigint_or(it.value(), "payout_denominator");
          incoming.has_payout_denominator = true;
        }
        incoming.market_question = json_string_or_empty(it.value(), "market_question");
        incoming.market_description = json_string_or_empty(it.value(), "market_description");
        incoming.market_event_title = json_string_or_empty(it.value(), "market_event_title");
        incoming.market_slug = json_string_or_empty(it.value(), "market_slug");
        incoming.market_url = json_string_or_empty(it.value(), "market_url");
        incoming.market_outcomes = json_string_array_or(it.value(), "market_outcomes");
        ConditionMeta &target = conditions_[incoming.condition_id];
        merge_condition_meta(target, incoming);
        target.condition_id = incoming.condition_id;
      }
    }

    if (meta_payload.contains("tokens") && meta_payload.at("tokens").is_object()) {
      for (auto it = meta_payload.at("tokens").begin(); it != meta_payload.at("tokens").end(); ++it) {
        TokenMeta incoming;
        incoming.token_id = it.key();
        incoming.condition_id = json_string_or_empty(it.value(), "condition_id");
        incoming.question_id = json_string_or_empty(it.value(), "question_id");
        incoming.outcome_index = json_int_or(it.value(), "outcome_index", -1);
        incoming.outcome_slot_count = json_int_or(it.value(), "outcome_slot_count", -1);
        incoming.resolution_timestamp = json_i64_or(it.value(), "resolution_timestamp", -1);
        incoming.payout_numerators = json_bigint_array_or(it.value(), "payout_numerators");
        if (it.value().contains("payout_denominator") && !it.value().at("payout_denominator").is_null()) {
          incoming.payout_denominator = json_bigint_or(it.value(), "payout_denominator");
          incoming.has_payout_denominator = true;
        }
        if (it.value().contains("price") && it.value().at("price").is_string()) {
          incoming.price = parse_decimal_string(it.value().at("price").get<std::string>());
          incoming.price_source = json_string_or_empty(it.value(), "price_source");
        }
        TokenMeta &target = tokens_[incoming.token_id];
        merge_token_meta(target, incoming);
        target.token_id = incoming.token_id;
      }
    }
  }

  {
    std::lock_guard<std::mutex> guard(mutex_);
    // 加载snapshot.json (用户持仓快照)
    snapshot_root_ = load_json_file(config_.snapshot_file, json::object());
    // 加载history.json (交易记录)
    history_root_ = load_json_file(config_.history_file, json::object());
    // 加载aggregate.json (聚合仓位)
    const json aggregate_payload = load_json_file(config_.aggregate_file, json::object());
    if (!aggregate_payload.is_object()) {
      return;
    }
    restore_current_state_from_aggregate(aggregate_payload);
  }
}

bool TrackerService::has_current_state_locked() const {
  return !users_.empty() && last_applied_block_ > 0;
}

bool TrackerService::watched_users_changed_locked(const std::vector<std::string> &users) const {
  if (users.size() != watched_users_.size()) {
    return true;
  }
  for (const std::string &user : users) {
    if (!watched_user_set_.contains(user)) {
      return true;
    }
  }
  return false;
}

void TrackerService::set_watched_users_locked(const std::vector<std::string> &users) {
  watched_users_ = users;
  watched_user_set_.clear();
  for (const std::string &user : users) {
    watched_user_set_.insert(user);
    if (!users_.contains(user)) {
      users_[user].user = user;
    }
  }
  for (auto it = users_.begin(); it != users_.end();) {
    if (!watched_user_set_.contains(it->first)) {
      it = users_.erase(it);
      continue;
    }
    ++it;
  }
}

void TrackerService::append_full_snapshot_locked(uint64_t block_number) {
  const int64_t now_unix = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  for (const std::string &user : watched_users_) {
    json rows = json::array();
    for (const auto &[token_id, amount_raw] : users_.at(user).token_amounts) {
      rows.push_back({
          {"token_id", token_id},
          {"amount_raw", big_int_to_string(amount_raw)},
      });
    }
    // 快照写入snapshot_root_ (key: user -> block_key -> snapshot)
    snapshot_root_[user][block_key(block_number)] = {
        {"block_number", block_number},
        {"captured_at_unix_sec", now_unix},
        {"stable_balances", {
             {"usdc_e_raw", big_int_to_string(users_.at(user).stable_balances.usdc_e_raw)},
             {"wrapped_collateral_raw", big_int_to_string(users_.at(user).stable_balances.wrapped_collateral_raw)},
         }},
        {"positions", rows},
    };
  }
}

json TrackerService::build_meta_json_locked() {
  json result = {
      {"updated_at_unix_sec", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
      {"tokens", json::object()},
      {"conditions", json::object()},
  };
  for (const auto &[token_id, token] : tokens_) {
    json row = {
        {"token_id", token_id},
        {"condition_id", token.condition_id.empty() ? json(nullptr) : json(token.condition_id)},
        {"question_id", token.question_id.empty() ? json(nullptr) : json(token.question_id)},
        {"outcome_index", token.outcome_index < 0 ? json(nullptr) : json(token.outcome_index)},
        {"outcome_slot_count", token.outcome_slot_count < 0 ? json(nullptr) : json(token.outcome_slot_count)},
        {"resolution_timestamp", token.resolution_timestamp < 0 ? json(nullptr) : json(token.resolution_timestamp)},
        {"payout_numerators", bigint_vector_to_json_string_array(token.payout_numerators)},
        {"payout_denominator", token.has_payout_denominator ? json(big_int_to_string(token.payout_denominator)) : json(nullptr)},
        {"price", token.price >= 0.0L ? json(format_decimal(token.price, 10)) : json(nullptr)},
        {"price_source", token.price_source.empty() ? json(nullptr) : json(token.price_source)},
    };
    result["tokens"][token_id] = row;
  }
  for (const auto &[condition_id, condition] : conditions_) {
    json row = {
        {"condition_id", condition_id},
        {"question_id", condition.question_id.empty() ? json(nullptr) : json(condition.question_id)},
        {"outcome_slot_count", condition.outcome_slot_count < 0 ? json(nullptr) : json(condition.outcome_slot_count)},
        {"resolution_timestamp", condition.resolution_timestamp < 0 ? json(nullptr) : json(condition.resolution_timestamp)},
        {"token_ids", condition.token_ids},
        {"payout_numerators", bigint_vector_to_json_string_array(condition.payout_numerators)},
        {"payout_denominator", condition.has_payout_denominator ? json(big_int_to_string(condition.payout_denominator)) : json(nullptr)},
        {"market_question", condition.market_question},
        {"market_description", condition.market_description},
        {"market_event_title", condition.market_event_title},
        {"market_slug", condition.market_slug},
        {"market_url", condition.market_url},
        {"market_outcomes", condition.market_outcomes},
    };
    result["conditions"][condition_id] = row;
  }
  return result;
}

json TrackerService::build_current_state_json_locked() {
  json result = {
      {"summary", json::object()},
      {"users", json::array()},
      {"aggregate", json::array()},
      {"recent_events", json::array()},
      {"history_index", json::object()},
  };

  struct AggregateBucket {
    BigInt amount_raw = 0;
    long double value_usd = 0.0L;
    int holder_count = 0;
    bool stable = false;
    std::string label;
  };

  std::map<std::string, AggregateBucket> aggregate_buckets;
  size_t token_position_count = 0;

  for (const std::string &user : watched_users_) {
    const UserRuntimeState &state = users_.at(user);
    struct RowWithValue {
      long double value = 0.0L;
      json row;
    };
    std::vector<RowWithValue> display_rows;
    long double token_value_usd = 0.0L;
    long double stable_value_usd = 0.0L;

    for (const auto &[token_id, amount_raw] : state.token_amounts) {
      token_position_count += 1;
      const TokenMeta *token = tokens_.contains(token_id) ? &tokens_.at(token_id) : nullptr;
      const ConditionMeta *condition = (token != nullptr && !token->condition_id.empty() && conditions_.contains(token->condition_id))
                                           ? &conditions_.at(token->condition_id)
                                           : nullptr;
      const long double price = (token != nullptr && token->price >= 0.0L) ? token->price : 0.0L;
      const long double value_usd = big_int_to_units(amount_raw) * price;
      token_value_usd += value_usd;
      const std::string outcome_text = (token != nullptr && condition != nullptr
                                        && token->outcome_index >= 0
                                        && static_cast<size_t>(token->outcome_index) < condition->market_outcomes.size())
                                           ? condition->market_outcomes[static_cast<size_t>(token->outcome_index)]
                                           : "";
      display_rows.push_back(RowWithValue{
          .value = value_usd,
          .row = {
              {"asset_type", "token"},
              {"token_id", token_id},
              {"amount_raw", big_int_to_string(amount_raw)},
              {"condition_id", token != nullptr && !token->condition_id.empty() ? json(token->condition_id) : json(nullptr)},
              {"question_id", token != nullptr && !token->question_id.empty() ? json(token->question_id) : json(nullptr)},
              {"outcome_index", token != nullptr && token->outcome_index >= 0 ? json(token->outcome_index) : json(nullptr)},
              {"outcome_text", outcome_text},
              {"market_question", condition != nullptr ? condition->market_question : ""},
              {"market_description", condition != nullptr ? condition->market_description : ""},
              {"price", token != nullptr && token->price >= 0.0L ? json(format_decimal(token->price, 10)) : json(nullptr)},
              {"price_source", token != nullptr ? token->price_source : ""},
              {"value_usd", value_usd},
          },
      });
      AggregateBucket &bucket = aggregate_buckets[token_id];
      bucket.amount_raw += amount_raw;
      bucket.value_usd += value_usd;
      bucket.holder_count += 1;
    }

    if (state.stable_balances.usdc_e_raw > 0) {
      const long double value_usd = big_int_to_units(state.stable_balances.usdc_e_raw);
      stable_value_usd += value_usd;
      display_rows.push_back(RowWithValue{
          .value = value_usd,
          .row = {
              {"asset_type", "stable"},
              {"token_id", "stable:usdc_e"},
              {"label", "USDC.e"},
              {"amount_raw", big_int_to_string(state.stable_balances.usdc_e_raw)},
              {"price", "1.0000000000"},
              {"value_usd", value_usd},
          },
      });
      AggregateBucket &bucket = aggregate_buckets["stable:usdc_e"];
      bucket.amount_raw += state.stable_balances.usdc_e_raw;
      bucket.value_usd += value_usd;
      bucket.holder_count += 1;
      bucket.stable = true;
      bucket.label = "USDC.e";
    }

    if (state.stable_balances.wrapped_collateral_raw > 0) {
      const long double value_usd = big_int_to_units(state.stable_balances.wrapped_collateral_raw);
      stable_value_usd += value_usd;
      display_rows.push_back(RowWithValue{
          .value = value_usd,
          .row = {
              {"asset_type", "stable"},
              {"token_id", "stable:wrapped_collateral"},
              {"label", "Wrapped Collateral"},
              {"amount_raw", big_int_to_string(state.stable_balances.wrapped_collateral_raw)},
              {"price", "1.0000000000"},
              {"value_usd", value_usd},
          },
      });
      AggregateBucket &bucket = aggregate_buckets["stable:wrapped_collateral"];
      bucket.amount_raw += state.stable_balances.wrapped_collateral_raw;
      bucket.value_usd += value_usd;
      bucket.holder_count += 1;
      bucket.stable = true;
      bucket.label = "Wrapped Collateral";
    }

    std::sort(display_rows.begin(), display_rows.end(), [](const RowWithValue &lhs, const RowWithValue &rhs) {
      if (lhs.value != rhs.value) {
        return lhs.value > rhs.value;
      }
      return lhs.row.at("token_id").get<std::string>() < rhs.row.at("token_id").get<std::string>();
    });

    const long double total_value_usd = token_value_usd + stable_value_usd;
    json positions = json::array();
    for (RowWithValue &item : display_rows) {
      item.row["weight"] = total_value_usd > 0.0L ? item.value / total_value_usd : 0.0L;
      positions.push_back(std::move(item.row));
    }

    result["users"].push_back({
        {"user", user},
        {"stable_balances", {
             {"usdc_e_raw", big_int_to_string(state.stable_balances.usdc_e_raw)},
             {"wrapped_collateral_raw", big_int_to_string(state.stable_balances.wrapped_collateral_raw)},
         }},
        {"token_value_usd", token_value_usd},
        {"stable_value_usd", stable_value_usd},
        {"total_value_usd", total_value_usd},
        {"token_value_usd_ratio", total_value_usd > 0.0L ? token_value_usd / total_value_usd : 0.0L},
        {"positions", positions},
    });

    // 从snapshot_root_获取快照信息
    const json &user_snapshots = snapshot_root_.contains(user) ? snapshot_root_.at(user) : json::object();
    json snapshot_rows = json::array();
    for (auto it = user_snapshots.begin(); it != user_snapshots.end(); ++it) {
      snapshot_rows.push_back({
          {"key", it.key()},
          {"block_number", it.value().at("block_number")},
      });
    }
    result["history_index"][user] = {
        {"snapshots", snapshot_rows},
    };
  }

  long double aggregate_total_value_usd = 0.0L;
  for (const auto &[_, bucket] : aggregate_buckets) {
    aggregate_total_value_usd += bucket.value_usd;
  }
  for (const auto &[token_id, bucket] : aggregate_buckets) {
    json row = {
        {"token_id", token_id},
        {"total_amount_raw", big_int_to_string(bucket.amount_raw)},
        {"holder_count", bucket.holder_count},
        {"total_value_usd", bucket.value_usd},
        {"weight", aggregate_total_value_usd > 0.0L ? bucket.value_usd / aggregate_total_value_usd : 0.0L},
    };
    if (bucket.stable) {
      row["asset_type"] = "stable";
      row["label"] = bucket.label;
      row["price"] = "1.0000000000";
    } else if (tokens_.contains(token_id)) {
      const TokenMeta &token = tokens_.at(token_id);
      row["asset_type"] = "token";
      row["condition_id"] = token.condition_id.empty() ? json(nullptr) : json(token.condition_id);
      row["outcome_index"] = token.outcome_index < 0 ? json(nullptr) : json(token.outcome_index);
      row["price"] = token.price >= 0.0L ? json(format_decimal(token.price, 10)) : json(nullptr);
      if (!token.condition_id.empty() && conditions_.contains(token.condition_id)) {
        const ConditionMeta &condition = conditions_.at(token.condition_id);
        row["market_question"] = condition.market_question;
        row["market_description"] = condition.market_description;
        if (token.outcome_index >= 0 && static_cast<size_t>(token.outcome_index) < condition.market_outcomes.size()) {
          row["outcome_text"] = condition.market_outcomes[static_cast<size_t>(token.outcome_index)];
        }
      }
    }
    result["aggregate"].push_back(row);
  }

  std::sort(result["aggregate"].begin(), result["aggregate"].end(), [](const json &lhs, const json &rhs) {
    const long double lv = lhs.at("total_value_usd").get<long double>();
    const long double rv = rhs.at("total_value_usd").get<long double>();
    if (lv != rv) {
      return lv > rv;
    }
    return lhs.at("token_id").get<std::string>() < rhs.at("token_id").get<std::string>();
  });

  for (auto it = recent_events_.rbegin(); it != recent_events_.rend(); ++it) {
    result["recent_events"].push_back(*it);
  }

  result["summary"] = {
      {"user_count", watched_users_.size()},
      {"position_count", token_position_count},
      {"token_count", tokens_.size()},
      {"condition_count", conditions_.size()},
      {"recent_event_count", recent_events_.size()},
      {"snapshot_block", last_snapshot_block_},
      {"last_applied_block", last_applied_block_},
      {"head_block", head_block_},
      {"behind_blocks", head_block_ >= last_applied_block_ ? head_block_ - last_applied_block_ : 0},
      {"last_resync_started_at_unix_sec", last_resync_started_at_},
      {"last_resync_finished_at_unix_sec", last_resync_finished_at_},
      {"query_counts", {
           {"rpc_http_calls", query_counters_.rpc_http_calls},
           {"rpc_ws_messages", query_counters_.rpc_ws_messages},
           {"rpc_ws_subscriptions", query_counters_.rpc_ws_subscriptions},
           {"subgraph_queries", query_counters_.subgraph_queries},
           {"gamma_queries", query_counters_.gamma_queries},
       }},
      {"watched_users", watched_users_},
  };
  return result;
}

void TrackerService::persist_meta_locked() {
  write_json_atomic(config_.meta_file, build_meta_json_locked());
}

void TrackerService::persist_aggregate_locked() {
  write_json_atomic(config_.aggregate_file, build_current_state_json_locked());
}

void TrackerService::persist_snapshot_locked() {
  write_json_atomic(config_.snapshot_file, snapshot_root_);
}

void TrackerService::persist_history_locked() {
  write_json_atomic(config_.history_file, history_root_);
}

void TrackerService::persist_all_locked() {
  persist_meta_locked();
  persist_aggregate_locked();
  persist_snapshot_locked();
  persist_history_locked();
}

json TrackerService::current_state_json() {
  std::lock_guard<std::mutex> guard(mutex_);
  return build_current_state_json_locked();
}

json TrackerService::meta_json() {
  std::lock_guard<std::mutex> guard(mutex_);
  return build_meta_json_locked();
}

json TrackerService::history_json_for_user(const std::string &user) {
  const std::string normalized = normalize_address(user);
  std::lock_guard<std::mutex> guard(mutex_);
  // 从snapshot_root_获取快照，从history_root_获取交易记录
  json snapshots = snapshot_root_.contains(normalized) ? snapshot_root_.at(normalized) : json::object();
  json events = history_root_.contains(normalized) ? history_root_.at(normalized) : json::object();
  return {
      {"user", normalized},
      {"snapshots", snapshots},
      {"events", events},
  };
}

json TrackerService::health_json() {
  std::lock_guard<std::mutex> guard(mutex_);
  return {
      {"ok", true},
      {"snapshot_block", last_snapshot_block_},
      {"last_applied_block", last_applied_block_},
      {"head_block", head_block_},
  };
}

void TrackerService::refresh_stable_balances(const std::string &block_tag) {
  std::vector<std::string> users;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    users = watched_users_;
  }
  std::vector<json> requests;
  struct CallRef {
    std::string user;
    bool wrapped = false;
  };
  std::vector<CallRef> refs;
  requests.reserve(users.size() * 2);
  refs.reserve(users.size() * 2);
  const std::string selector = "0x70a08231";
  for (const std::string &user : users) {
    const std::string encoded_user = selector + std::string(24, '0') + strip_hex_prefix(user);
    requests.push_back({
        {"method", "eth_call"},
        {"params", json::array({json{{"to", kUsdcE}, {"data", encoded_user}}, block_tag})},
    });
    refs.push_back({user, false});
    requests.push_back({
        {"method", "eth_call"},
        {"params", json::array({json{{"to", kWrappedCollateral}, {"data", encoded_user}}, block_tag})},
    });
    refs.push_back({user, true});
  }
  if (requests.empty()) {
    return;
  }
  const json responses = rpc_batch_call(requests);
  assert(responses.size() == refs.size());
  std::lock_guard<std::mutex> guard(mutex_);
  for (size_t index = 0; index < refs.size(); ++index) {
    const BigInt balance = big_int_from_hex(responses.at(index).at("result").get<std::string>());
    UserRuntimeState &state = users_.at(refs[index].user);
    if (refs[index].wrapped) {
      state.stable_balances.wrapped_collateral_raw = balance;
    } else {
      state.stable_balances.usdc_e_raw = balance;
    }
  }
}

void TrackerService::refresh_reference_data(bool missing_only) {
  std::vector<std::string> active_token_ids;
  std::vector<std::string> active_condition_ids;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    std::set<std::string> tokens_to_refresh;
    std::set<std::string> conditions_to_refresh;
    for (const std::string &user : watched_users_) {
      for (const auto &[token_id, _] : users_.at(user).token_amounts) {
        if (!missing_only || !tokens_.contains(token_id) || tokens_.at(token_id).price < 0.0L) {
          tokens_to_refresh.insert(token_id);
        }
        if (tokens_.contains(token_id) && !tokens_.at(token_id).condition_id.empty()) {
          conditions_to_refresh.insert(tokens_.at(token_id).condition_id);
        }
      }
    }
    active_token_ids.assign(tokens_to_refresh.begin(), tokens_to_refresh.end());
    active_condition_ids.assign(conditions_to_refresh.begin(), conditions_to_refresh.end());
  }

  if (!active_token_ids.empty()) {
    std::map<std::string, MarketDataRow> markets;
    for (const auto &group : chunked(active_token_ids, config_.graph_id_batch_limit)) {
      const json data = graph_query(
          kPolymarketSubgraphId,
          kMarketDataLatestQuery,
          {{"ids", group}},
          "marketDatas.latest");
      assert(data.contains("marketDatas") && data.at("marketDatas").is_array());
      for (const json &item : data.at("marketDatas")) {
        MarketDataRow row;
        row.token_id = item.at("id").get<std::string>();
        row.outcome_index = json_int_or(item, "outcomeIndex", -1);
        if (item.contains("priceOrderbook") && item.at("priceOrderbook").is_string()) {
          row.price_orderbook = parse_decimal_string(item.at("priceOrderbook").get<std::string>());
        }
        if (item.contains("condition") && item.at("condition").is_object()) {
          const json &condition = item.at("condition");
          row.condition_id = json_string_or_empty(condition, "id");
          row.question_id = json_string_or_empty(condition, "questionId");
          row.outcome_slot_count = json_int_or(condition, "outcomeSlotCount", -1);
          row.resolution_timestamp = json_i64_or(condition, "resolutionTimestamp", -1);
          row.payout_numerators = json_bigint_array_or(condition, "payoutNumerators");
          if (condition.contains("payoutDenominator") && !condition.at("payoutDenominator").is_null()) {
            row.payout_denominator = json_bigint_or(condition, "payoutDenominator");
            row.has_payout_denominator = true;
          }
        }
        markets[row.token_id] = row;
      }
    }

    std::set<std::string> condition_ids_for_rows;
    for (const auto &[token_id, row] : markets) {
      TokenMeta incoming;
      incoming.token_id = token_id;
      incoming.condition_id = row.condition_id;
      incoming.question_id = row.question_id;
      incoming.outcome_index = row.outcome_index;
      incoming.outcome_slot_count = row.outcome_slot_count;
      incoming.resolution_timestamp = row.resolution_timestamp;
      incoming.payout_numerators = row.payout_numerators;
      if (row.has_payout_denominator) {
        incoming.payout_denominator = row.payout_denominator;
        incoming.has_payout_denominator = true;
      }
      const std::optional<long double> resolved_price = resolved_price_from_payouts(
          row.payout_numerators,
          row.payout_denominator,
          row.has_payout_denominator,
          row.outcome_index);
      if (resolved_price.has_value()) {
        incoming.price = *resolved_price;
        incoming.price_source = "resolution";
      } else if (row.price_orderbook.has_value()) {
        incoming.price = *row.price_orderbook;
        incoming.price_source = "orderbook";
      }

      ConditionMeta condition_incoming;
      condition_incoming.condition_id = row.condition_id;
      condition_incoming.question_id = row.question_id;
      condition_incoming.outcome_slot_count = row.outcome_slot_count;
      condition_incoming.resolution_timestamp = row.resolution_timestamp;
      condition_incoming.payout_numerators = row.payout_numerators;
      if (row.has_payout_denominator) {
        condition_incoming.payout_denominator = row.payout_denominator;
        condition_incoming.has_payout_denominator = true;
      }

      std::lock_guard<std::mutex> guard(mutex_);
      TokenMeta &target = tokens_[token_id];
      merge_token_meta(target, incoming);
      target.token_id = token_id;
      if (!row.condition_id.empty()) {
        ConditionMeta &condition_target = conditions_[row.condition_id];
        merge_condition_meta(condition_target, condition_incoming);
        condition_target.condition_id = row.condition_id;
        condition_ids_for_rows.insert(row.condition_id);
      }
    }

    std::vector<std::string> missing_condition_ids(condition_ids_for_rows.begin(), condition_ids_for_rows.end());
    if (!missing_condition_ids.empty()) {
      for (const auto &group : chunked(missing_condition_ids, config_.graph_id_batch_limit)) {
        const json data = graph_query(
            kPnlSubgraphId,
            kConditionsLatestQuery,
            {{"ids", group}},
            "conditions.latest");
        assert(data.contains("conditions") && data.at("conditions").is_array());
        for (const json &item : data.at("conditions")) {
          ConditionMeta incoming;
          incoming.condition_id = item.at("id").get<std::string>();
          incoming.token_ids = json_string_array_or(item, "positionIds");
          incoming.payout_numerators = json_bigint_array_or(item, "payoutNumerators");
          if (item.contains("payoutDenominator") && !item.at("payoutDenominator").is_null()) {
            incoming.payout_denominator = json_bigint_or(item, "payoutDenominator");
            incoming.has_payout_denominator = true;
          }
          std::lock_guard<std::mutex> guard(mutex_);
          ConditionMeta &target = conditions_[incoming.condition_id];
          merge_condition_meta(target, incoming);
          target.condition_id = incoming.condition_id;
        }
      }
    }

    std::vector<std::string> missing_market_texts;
    {
      std::lock_guard<std::mutex> guard(mutex_);
      std::set<std::string> missing_set;
      for (const std::string &user : watched_users_) {
        for (const auto &[token_id, _] : users_.at(user).token_amounts) {
          if (!tokens_.contains(token_id)) {
            continue;
          }
          const std::string &condition_id = tokens_.at(token_id).condition_id;
          if (condition_id.empty()) {
            continue;
          }
          if (!conditions_.contains(condition_id) || conditions_.at(condition_id).market_question.empty()) {
            missing_set.insert(condition_id);
          }
        }
      }
      missing_market_texts.assign(missing_set.begin(), missing_set.end());
    }

    if (!missing_market_texts.empty()) {
      std::vector<HttpRequest> requests;
      requests.reserve(missing_market_texts.size());
      for (const std::string &condition_id : missing_market_texts) {
        requests.push_back({
            .url = std::string(kGammaApiBase) + "/markets?condition_ids=" + condition_id + "&include_tag=true",
            .method = "GET",
            .body = "",
        });
      }
      const std::vector<HttpResponse> responses = http_batch(requests, config_.http_concurrency);

      std::lock_guard<std::mutex> guard(mutex_);
      query_counters_.gamma_queries += responses.size();
      for (size_t i = 0; i < responses.size(); ++i) {
        const std::string &condition_id = missing_market_texts[i];
        const HttpResponse &response = responses[i];
        assert(response.status == 200);
        const json arr = safe_json_parse(response.body);
        assert(arr.is_array());
        if (arr.empty()) {
          continue;
        }
        json market = arr.front();
        for (const json &item : arr) {
          const std::string candidate = item.contains("conditionId")
                                            ? json_string_or_empty(item, "conditionId")
                                            : json_string_or_empty(item, "condition_id");
          if (candidate.empty()) {
            continue;
          }
          if (normalize_hex(candidate) == normalize_hex(condition_id)) {
            market = item;
            break;
          }
        }
        ConditionMeta incoming;
        incoming.condition_id = condition_id;
        const json events = market.contains("events") && market.at("events").is_array()
                                ? market.at("events")
                                : json::array();
        const json event0 = events.empty() ? json::object() : events.front();
        incoming.market_question = json_string_or_empty(market, "question");
        if (incoming.market_question.empty()) {
          incoming.market_question = json_string_or_empty(event0, "title");
        }
        incoming.market_description = json_string_or_empty(market, "description");
        incoming.market_event_title = json_string_or_empty(event0, "title");
        incoming.market_slug = json_string_or_empty(event0, "slug");
        if (incoming.market_slug.empty()) {
          incoming.market_slug = json_string_or_empty(market, "slug");
        }
        incoming.market_url = incoming.market_slug.empty() ? "" : "https://polymarket.com/event/" + incoming.market_slug;
        if (market.contains("outcomes")) {
          if (market.at("outcomes").is_string()) {
            const json outcomes = json::parse(market.at("outcomes").get<std::string>());
            if (outcomes.is_array()) {
              for (const json &value : outcomes) {
                incoming.market_outcomes.push_back(json_string_or_integer_string(value));
              }
            }
          } else if (market.at("outcomes").is_array()) {
            for (const json &value : market.at("outcomes")) {
              incoming.market_outcomes.push_back(json_string_or_integer_string(value));
            }
          }
        }
        ConditionMeta &target = conditions_[condition_id];
        merge_condition_meta(target, incoming);
        target.condition_id = condition_id;
      }
    }
  }
}

void TrackerService::bootstrap_from_persisted() {
  const std::vector<std::string> users = load_address_file_lines(config_.address_file);
  bool need_full_sync = false;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    need_full_sync = !has_current_state_locked() || watched_users_changed_locked(users);
    set_watched_users_locked(users);
  }
  if (need_full_sync) {
    full_resync();
    return;
  }

  const uint64_t head_block = rpc_block_number();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    head_block_ = std::max(head_block_, head_block);
  }
  uint64_t from_block = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    from_block = last_applied_block_ + 1;
  }
  if (from_block <= head_block) {
    backfill_range(from_block, head_block);
  }
  refresh_stable_balances("latest");
  refresh_reference_data(false);
  {
    std::lock_guard<std::mutex> guard(mutex_);
    persist_all_locked();
  }
}

void TrackerService::bootstrap() {
  load_seed_meta();
  load_persisted_files();
  bootstrap_from_persisted();
}

void TrackerService::full_resync() {
  force_resync_.store(false);
  // 初始化进度显示
  progress_board().init(SYNC_STAGES);
  {
    std::lock_guard<std::mutex> guard(mutex_);
    last_resync_started_at_ = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  }

  const std::vector<std::string> users = load_address_file_lines(config_.address_file);
  progress_board().update("fetch_positions", 0, users.size());

  // 先查 _meta 获取 block，减去 lag 得到稳定的 snapshot_block
  const uint64_t pnl_block = static_cast<uint64_t>(std::stoull(
      json_string_or_integer_string(graph_query(kPnlSubgraphId, kMetaQuery, json::object(), "pnl.meta")
                                        .at("_meta")
                                        .at("block")
                                        .at("number"))));
  const uint64_t poly_block = static_cast<uint64_t>(std::stoull(
      json_string_or_integer_string(graph_query(kPolymarketSubgraphId, kMetaQuery, json::object(), "poly.meta")
                                        .at("_meta")
                                        .at("block")
                                        .at("number"))));
  const uint64_t snapshot_block = std::min(pnl_block, poly_block) - static_cast<uint64_t>(kSnapshotBlockLag);
  sync_logger().info("_meta: pnl=" + std::to_string(pnl_block) + " poly=" + std::to_string(poly_block) +
                     " snapshot=" + std::to_string(snapshot_block));

  std::map<std::string, std::map<std::string, BigInt>> snapshot_positions;
  for (const std::string &user : users) {
    snapshot_positions[user] = {};
  }
  size_t users_processed = 0;
  for (const auto &group : chunked(users, config_.user_query_batch_limit)) {
    std::string after;
    while (true) {
      const json data = graph_query(
          kPnlSubgraphId,
          kUserPositionsQuery,
          {
              {"users", group},
              {"after", after},
              {"block", snapshot_block},
              {"first", config_.graph_page_limit},
          },
          "userPositions");
      assert(data.contains("userPositions") && data.at("userPositions").is_array());
      const json rows = data.at("userPositions");
      for (const json &row : rows) {
        const std::string user = normalize_address(row.at("user").get<std::string>());
        const std::string token_id = row.at("tokenId").get<std::string>();
        snapshot_positions[user][token_id] += big_int_from_dec(json_string_or_integer_string(row.at("amount")));
      }
      if (rows.size() < config_.graph_page_limit) {
        break;
      }
      after = rows.back().at("id").get<std::string>();
    }
    users_processed += group.size();
    progress_board().update("fetch_positions", users_processed, users.size());
  }

  {
    std::lock_guard<std::mutex> guard(mutex_);
    set_watched_users_locked(users);
    for (const std::string &user : watched_users_) {
      users_[user].token_amounts = snapshot_positions[user];
    }
    last_snapshot_block_ = snapshot_block;
    last_applied_block_ = snapshot_block;
    head_block_ = std::max(head_block_, snapshot_block);
  }

  refresh_stable_balances(int_to_hex(snapshot_block));
  {
    std::lock_guard<std::mutex> guard(mutex_);
    append_full_snapshot_locked(snapshot_block);
    persist_snapshot_locked();  // 保存快照到snapshot.json
  }

  const uint64_t head_block = rpc_block_number();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    head_block_ = std::max(head_block_, head_block);
  }
  if (snapshot_block + 1 <= head_block) {
    backfill_range(snapshot_block + 1, head_block);
  }
  refresh_stable_balances("latest");
  refresh_reference_data(false);
  {
    std::lock_guard<std::mutex> guard(mutex_);
    last_resync_finished_at_ = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    persist_all_locked();
  }
  progress_board().finish();
}

std::vector<json> TrackerService::build_log_filters(const std::vector<std::string> &users,
                                                    const std::optional<uint64_t> &from_block,
                                                    const std::optional<uint64_t> &to_block) const {
  std::vector<json> filters;
  for (const auto &group : chunked(users, config_.topic_group_size)) {
    json topic_group = json::array();
    for (const std::string &user : group) {
      topic_group.push_back(address_to_topic(user));
    }
    std::vector<json> group_filters = {
        {
            {"address", kConditionalTokens},
            {"topics", json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}), nullptr, topic_group})},
        },
        {
            {"address", kConditionalTokens},
            {"topics", json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}), nullptr, nullptr, topic_group})},
        },
        {
            {"address", kConditionalTokens},
            {"topics", json::array({json::array({kPositionSplitTopic, kPositionMergeTopic, kPositionRedeemTopic}), topic_group})},
        },
        {
            {"address", json::array({kCtfExchange, kNegRiskCtfExchange})},
            {"topics", json::array({json::array({kOrderFillTopic}), nullptr, topic_group})},
        },
        {
            {"address", json::array({kCtfExchange, kNegRiskCtfExchange})},
            {"topics", json::array({json::array({kOrderFillTopic}), nullptr, nullptr, topic_group})},
        },
        {
            {"address", kNegRiskAdapter},
            {"topics", json::array({json::array({kPositionConvertTopic}), topic_group})},
        },
    };
    for (json &item : group_filters) {
      if (from_block.has_value()) {
        item["fromBlock"] = int_to_hex(*from_block);
      }
      if (to_block.has_value()) {
        item["toBlock"] = int_to_hex(*to_block);
      }
      filters.push_back(std::move(item));
    }
  }

  json resolve_filter = {
      {"address", kConditionalTokens},
      {"topics", json::array({json::array({kConditionResolveTopic})})},
  };
  if (from_block.has_value()) {
    resolve_filter["fromBlock"] = int_to_hex(*from_block);
  }
  if (to_block.has_value()) {
    resolve_filter["toBlock"] = int_to_hex(*to_block);
  }
  filters.push_back(std::move(resolve_filter));
  return filters;
}

bool TrackerService::apply_block_logs(const std::vector<json> &raw_logs, std::set<std::string> &touched_token_ids) {
  bool changed = false;
  const std::vector<TxContext> contexts = build_tx_contexts(raw_logs);
  std::lock_guard<std::mutex> guard(mutex_);
  for (const TxContext &context : contexts) {
    for (const TransferLeg &transfer : context.transfers) {
      if (watched_user_set_.contains(transfer.from_addr)) {
        UserRuntimeState &user = users_.at(transfer.from_addr);
        const OrderFillRow *fill = find_matched_sell(context, transfer, transfer.from_addr);
        std::string kind = "transfer_out";
        if (fill != nullptr) {
          kind = "order_sell";
          user.stable_balances.usdc_e_raw += fill->collateral_amount_raw;
        } else if (context.merge_users.contains(transfer.from_addr)) {
          kind = "merge_out";
        } else if (context.redemption_users.contains(transfer.from_addr)) {
          kind = "redeem_out";
        } else if (context.convert_users.contains(transfer.from_addr)) {
          kind = "convert_out";
        } else if (transfer.to_addr == kZeroAddress) {
          kind = "burn_out";
        } else if (watched_user_set_.contains(transfer.to_addr)) {
          kind = "tracked_out";
        } else if (is_protocol_addr(transfer.to_addr) || is_protocol_addr(transfer.operator_addr)) {
          kind = "protocol_out";
        }
        const BigInt current = user.token_amounts.contains(transfer.token_id) ? user.token_amounts.at(transfer.token_id) : BigInt{0};
        assert(current >= transfer.amount_raw);
        const BigInt next = current - transfer.amount_raw;
        if (next == 0) {
          user.token_amounts.erase(transfer.token_id);
        } else {
          user.token_amounts[transfer.token_id] = next;
        }
        json event = build_event_json(context, transfer, transfer.from_addr, "out", kind, fill);
        // 交易记录写入history_root_ (key: user -> block_key -> events array)
        json &event_bucket = history_root_[transfer.from_addr][block_key(transfer.block_number)];
        if (!event_bucket.is_array()) {
          event_bucket = json::array();
        }
        event_bucket.push_back(event);
        recent_events_.push_back(event);
        while (recent_events_.size() > config_.recent_event_limit) {
          recent_events_.pop_front();
        }
        touched_token_ids.insert(transfer.token_id);
        changed = true;
      }

      if (watched_user_set_.contains(transfer.to_addr)) {
        UserRuntimeState &user = users_.at(transfer.to_addr);
        const OrderFillRow *fill = find_matched_buy(context, transfer, transfer.to_addr);
        std::string kind = "transfer_in";
        if (fill != nullptr) {
          kind = "order_buy";
          user.stable_balances.usdc_e_raw -= fill->collateral_amount_raw;
        } else if (context.split_users.contains(transfer.to_addr)) {
          kind = "split_in";
        } else if (context.convert_users.contains(transfer.to_addr)) {
          kind = "convert_in";
        } else if (transfer.from_addr == kZeroAddress) {
          kind = "mint_in";
        } else if (watched_user_set_.contains(transfer.from_addr)) {
          kind = "tracked_in";
        } else if (is_protocol_addr(transfer.from_addr) || is_protocol_addr(transfer.operator_addr)) {
          kind = "protocol_in";
        }
        user.token_amounts[transfer.token_id] += transfer.amount_raw;
        json event = build_event_json(context, transfer, transfer.to_addr, "in", kind, fill);
        // 交易记录写入history_root_ (key: user -> block_key -> events array)
        json &event_bucket = history_root_[transfer.to_addr][block_key(transfer.block_number)];
        if (!event_bucket.is_array()) {
          event_bucket = json::array();
        }
        event_bucket.push_back(event);
        recent_events_.push_back(event);
        while (recent_events_.size() > config_.recent_event_limit) {
          recent_events_.pop_front();
        }
        touched_token_ids.insert(transfer.token_id);
        changed = true;
      }
    }
  }
  return changed;
}

void TrackerService::apply_log_blocks(std::map<uint64_t, std::map<std::string, json>> blocks) {
  for (auto &[block_number, logs_by_key] : blocks) {
    std::vector<json> raw_logs;
    raw_logs.reserve(logs_by_key.size());
    for (auto &[_, row] : logs_by_key) {
      raw_logs.push_back(std::move(row));
    }
    std::sort(raw_logs.begin(), raw_logs.end(), [](const json &lhs, const json &rhs) {
      return log_sort_key(lhs) < log_sort_key(rhs);
    });
    std::set<std::string> touched_token_ids;
    const bool changed = apply_block_logs(raw_logs, touched_token_ids);
    if (!touched_token_ids.empty()) {
      refresh_reference_data(true);
    }
    if (changed || !touched_token_ids.empty()) {
      std::lock_guard<std::mutex> guard(mutex_);
      persist_all_locked();
    }
    {
      std::lock_guard<std::mutex> guard(mutex_);
      last_applied_block_ = std::max(last_applied_block_, block_number);
      head_block_ = std::max(head_block_, block_number);
    }
  }
}

void TrackerService::backfill_range(uint64_t from_block, uint64_t to_block) {
  if (from_block > to_block) {
    return;
  }
  std::vector<std::string> users;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    users = watched_users_;
  }

  uint64_t start_block = from_block;
  while (start_block <= to_block) {
    const uint64_t end_block = std::min<uint64_t>(to_block, start_block + config_.get_logs_block_span - 1);
    const std::vector<json> filters = build_log_filters(users, start_block, end_block);
    std::vector<json> requests;
    requests.reserve(filters.size());
    for (const json &filter : filters) {
      requests.push_back({
          {"method", "eth_getLogs"},
          {"params", json::array({filter})},
      });
    }
    const json responses = rpc_batch_call(requests);
    std::map<uint64_t, std::map<std::string, json>> blocks;
    for (const json &response : responses) {
      assert(response.contains("result") && response.at("result").is_array());
      for (const json &log : response.at("result")) {
        blocks[hex_to_u64(log.at("blockNumber").get<std::string>())][log_unique_key(log)] = log;
      }
    }
    apply_log_blocks(std::move(blocks));

    {
      std::lock_guard<std::mutex> guard(mutex_);
      last_applied_block_ = std::max(last_applied_block_, end_block);
      head_block_ = std::max(head_block_, end_block);
    }
    start_block = end_block + 1;
  }
}

void TrackerService::run_live_until(std::chrono::steady_clock::time_point deadline) {
  std::vector<std::string> users;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    users = watched_users_;
  }
  const std::vector<json> live_filters = build_log_filters(users, std::nullopt, std::nullopt);

  RpcWebSocketStream ws_stream(config_.rpc_ws_url);
  ws_stream.start();
  const std::string new_heads_subscription_id = ws_stream.subscribe_new_heads();
  std::set<std::string> log_subscription_ids;
  for (const json &filter : live_filters) {
    log_subscription_ids.insert(ws_stream.subscribe_logs(filter));
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    query_counters_.rpc_ws_subscriptions += 1 + live_filters.size();
  }

  // 订阅全部建立完成后，只补一次 gap。后续实时事件只走 ws。
  const uint64_t gap_end_block = rpc_block_number();
  uint64_t resume_from = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    head_block_ = std::max(head_block_, gap_end_block);
    resume_from = last_applied_block_ + 1;
  }
  if (resume_from <= gap_end_block) {
    backfill_range(resume_from, gap_end_block);
  }

  std::map<uint64_t, std::map<std::string, json>> pending_blocks;
  while (std::chrono::steady_clock::now() < deadline && !force_resync_.load()) {
    const std::optional<json> payload_opt = ws_stream.try_read_message();
    if (!payload_opt.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    {
      std::lock_guard<std::mutex> guard(mutex_);
      query_counters_.rpc_ws_messages += 1;
    }

    const json &payload = *payload_opt;
    assert(payload.contains("method"));
    assert(payload.at("method").get<std::string>() == "eth_subscription");
    assert(payload.contains("params") && payload.at("params").is_object());
    const json &params = payload.at("params");
    const std::string subscription_id = normalize_hex(params.at("subscription").get<std::string>());
    assert(params.contains("result"));

    if (subscription_id == new_heads_subscription_id) {
      const uint64_t head_block = hex_to_u64(params.at("result").at("number").get<std::string>());
      {
        std::lock_guard<std::mutex> guard(mutex_);
        head_block_ = std::max(head_block_, head_block);
      }
      std::map<uint64_t, std::map<std::string, json>> ready_blocks;
      while (!pending_blocks.empty() && pending_blocks.begin()->first < head_block) {
        auto node = pending_blocks.extract(pending_blocks.begin());
        ready_blocks.insert(std::move(node));
      }
      if (!ready_blocks.empty()) {
        apply_log_blocks(std::move(ready_blocks));
      }
      continue;
    }

    assert(log_subscription_ids.contains(subscription_id));
    const json &log = params.at("result");
    assert(log.is_object());
    const uint64_t block_number = hex_to_u64(log.at("blockNumber").get<std::string>());
    if (block_number <= gap_end_block) {
      continue;
    }
    pending_blocks[block_number][log_unique_key(log)] = log;
  }
}

void TrackerService::request_resync() {
  force_resync_.store(true);
}

void TrackerService::run_forever() {
  auto next_resync = std::chrono::steady_clock::now() + std::chrono::seconds(config_.resync_interval_sec);
  while (true) {
    if (force_resync_.load() || std::chrono::steady_clock::now() >= next_resync) {
      full_resync();
      next_resync = std::chrono::steady_clock::now() + std::chrono::seconds(config_.resync_interval_sec);
      continue;
    }
    run_live_until(next_resync);
  }
}

} // namespace tracker
