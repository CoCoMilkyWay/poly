#include "tracker/ws.hpp"
#include "tracker/codec.hpp"
#include "tracker/const.hpp"
#include "tracker/log.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <openssl/ssl.h>

#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <thread>

namespace tracker {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class WsStream {
public:
  explicit WsStream(const std::string &url)
      : parts_(parse_url(url)),
        ssl_ctx_(ssl::context::tlsv12_client),
        resolver_(ioc_) {
    assert(parts_.websocket());
    if (parts_.secure()) {
      ssl_ctx_.set_default_verify_paths();
      ssl_ctx_.set_verify_mode(ssl::verify_peer);
    }
  }

  void connect() {
    tcp::resolver::results_type endpoints = resolver_.resolve(parts_.host, parts_.port);
    std::string host = parts_.host + ":" + parts_.port;
    if (parts_.secure()) {
      ssl_ws_.emplace(ioc_, ssl_ctx_);
      ssl_ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
      SSL_set_tlsext_host_name(ssl_ws_->next_layer().native_handle(), parts_.host.c_str());
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

  std::string subscribe_heads() {
    return subscribe(json::array({"newHeads"}), "newHeads");
  }

  std::string subscribe_logs(const json &filter) {
    return subscribe(json::array({"logs", filter}), "logs");
  }

  std::optional<json> try_read() {
    if (!queued_.empty()) {
      json msg = std::move(queued_.front());
      queued_.pop_front();
      return msg;
    }
    if (!has_data()) return std::nullopt;
    return read_json();
  }

private:
  std::string subscribe(const json &params, const std::string &label) {
    int id = next_id_++;
    write_json({{"jsonrpc", "2.0"}, {"id", id}, {"method", "eth_subscribe"}, {"params", params}});
    while (true) {
      json msg = read_json();
      if (msg.contains("id")) {
        assert(msg.at("id").get<int>() == id);
        assert(msg.contains("result"));
        std::string sub_id = norm_hex(msg.at("result").get<std::string>());
        logger().info("ws subscribed " + label + " " + sub_id);
        return sub_id;
      }
      queued_.push_back(msg);
    }
  }

  void write_json(const json &msg) {
    std::string body = msg.dump();
    if (parts_.secure()) ssl_ws_->write(asio::buffer(body));
    else plain_ws_->write(asio::buffer(body));
  }

  json read_json() {
    buf_.consume(buf_.size());
    if (parts_.secure()) {
      ssl_ws_->read(buf_);
      assert(ssl_ws_->got_text());
    } else {
      plain_ws_->read(buf_);
      assert(plain_ws_->got_text());
    }
    std::string body = beast::buffers_to_string(buf_.cdata());
    buf_.consume(buf_.size());
    return safe_parse(body);
  }

  bool has_data() {
    boost::system::error_code ec;
    size_t avail = parts_.secure()
        ? beast::get_lowest_layer(*ssl_ws_).socket().available(ec)
        : beast::get_lowest_layer(*plain_ws_).socket().available(ec);
    assert(!ec);
    return avail > 0;
  }

  UrlParts parts_;
  asio::io_context ioc_;
  ssl::context ssl_ctx_;
  tcp::resolver resolver_;
  std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ssl_ws_;
  std::optional<websocket::stream<beast::tcp_stream>> plain_ws_;
  beast::flat_buffer buf_;
  std::deque<json> queued_;
  int next_id_ = 1;
};

std::string log_key(const json &log) {
  return log.at("blockNumber").get<std::string>() + "|" +
         norm_hex(log.at("transactionHash").get<std::string>()) + "|" +
         log.at("logIndex").get<std::string>() + "|" +
         norm_hex(log.at("address").get<std::string>());
}

} // namespace

WsThread::WsThread(const AppConfig &cfg, AppState &state, EventQueue &queue)
    : cfg_(cfg), state_(state), queue_(queue) {}

void WsThread::start() {
  assert(!running_);
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void WsThread::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

void WsThread::request_reconnect() {
  reconnect_ = true;
}

std::vector<json> WsThread::build_log_filters() const {
  std::vector<std::string> users;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    users = state_.users;
  }

  std::vector<json> filters;
  for (const auto &group : chunked(users, cfg_.topic_group_size)) {
    json topics = json::array();
    for (const auto &u : group) topics.push_back(addr_to_topic(u));

    std::vector<json> group_filters = {
        {{"address", kConditionalTokens}, {"topics", json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}), nullptr, topics})}},
        {{"address", kConditionalTokens}, {"topics", json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}), nullptr, nullptr, topics})}},
        {{"address", kConditionalTokens}, {"topics", json::array({json::array({kPositionSplitTopic, kPositionMergeTopic, kPositionRedeemTopic}), topics})}},
        {{"address", json::array({kCtfExchange, kNegRiskCtfExchange})}, {"topics", json::array({json::array({kOrderFillTopic}), nullptr, topics})}},
        {{"address", json::array({kCtfExchange, kNegRiskCtfExchange})}, {"topics", json::array({json::array({kOrderFillTopic}), nullptr, nullptr, topics})}},
        {{"address", kNegRiskAdapter}, {"topics", json::array({json::array({kPositionConvertTopic}), topics})}},
    };
    for (auto &f : group_filters) filters.push_back(std::move(f));
  }

  filters.push_back({{"address", kConditionalTokens}, {"topics", json::array({json::array({kConditionResolveTopic})})}});
  return filters;
}

void WsThread::run() {
  while (running_) {
    try {
      reconnect_ = false;
      WsStream ws(cfg_.rpc_ws_url);
      ws.connect();

      std::string heads_sub = ws.subscribe_heads();
      std::set<std::string> log_subs;
      for (const auto &f : build_log_filters()) {
        log_subs.insert(ws.subscribe_logs(f));
      }

      {
        std::lock_guard<std::mutex> lock(state_.mu);
        state_.counters.rpc_ws_sub += 1 + log_subs.size();
      }

      std::map<uint64_t, std::map<std::string, json>> pending;

      while (running_ && !reconnect_) {
        auto msg_opt = ws.try_read();
        if (!msg_opt) {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }

        {
          std::lock_guard<std::mutex> lock(state_.mu);
          state_.counters.rpc_ws_msg++;
        }

        const json &msg = *msg_opt;
        assert(msg.contains("method"));
        assert(msg.at("method").get<std::string>() == "eth_subscription");
        const json &params = msg.at("params");
        std::string sub_id = norm_hex(params.at("subscription").get<std::string>());

        if (sub_id == heads_sub) {
          uint64_t head = hex_to_u64(params.at("result").at("number").get<std::string>());
          {
            std::lock_guard<std::mutex> lock(state_.mu);
            state_.head_block = std::max(state_.head_block, head);
          }
          // flush blocks < head
          while (!pending.empty() && pending.begin()->first < head) {
            auto node = pending.extract(pending.begin());
            json logs = json::array();
            for (auto &[_, log] : node.mapped()) logs.push_back(std::move(log));
            queue_.push({node.key(), std::move(logs)});
          }
          continue;
        }

        assert(log_subs.contains(sub_id));
        const json &log = params.at("result");
        uint64_t block = hex_to_u64(log.at("blockNumber").get<std::string>());
        pending[block][log_key(log)] = log;
      }

    } catch (const std::exception &e) {
      logger().error(std::string("ws error: ") + e.what());
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
  }
}

} // namespace tracker
