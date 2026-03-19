#include "tracker/ws.hpp"

#include "tracker/codec.hpp"
#include "tracker/filter.hpp"
#include "tracker/log.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
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
namespace http = beast::http;
namespace ssl = asio::ssl;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

class WsStream {
public:
  WsStream(const std::string &url, const std::string &proxy_url)
      : parts_(parse_url(url)),
        ssl_ctx_(ssl::context::tlsv12_client),
        resolver_(ioc_) {
    assert(parts_.websocket());
    if (parts_.secure()) {
      ssl_ctx_.set_default_verify_paths();
      ssl_ctx_.set_verify_mode(ssl::verify_peer);
    }
    if (!proxy_url.empty()) {
      proxy_ = parse_url(proxy_url);
    }
  }

  void connect() {
    const std::string &host = proxy_ ? proxy_->host : parts_.host;
    const std::string &port = proxy_ ? proxy_->port : parts_.port;
    tcp::resolver::results_type endpoints = resolver_.resolve(host, port);

    std::string ws_host = parts_.host + ":" + parts_.port;
    if (parts_.secure()) {
      ssl_ws_.emplace(ioc_, ssl_ctx_);
      ssl_ws_->set_option(
          websocket::stream_base::timeout::suggested(beast::role_type::client));
      SSL_set_tlsext_host_name(ssl_ws_->next_layer().native_handle(),
                               parts_.host.c_str());
      beast::get_lowest_layer(*ssl_ws_).connect(endpoints);
      if (proxy_) {
        proxy_connect();
      }
      ssl_ws_->next_layer().handshake(ssl::stream_base::client);
      ssl_ws_->handshake(ws_host, parts_.target);
      return;
    }

    plain_ws_.emplace(ioc_);
    plain_ws_->set_option(
        websocket::stream_base::timeout::suggested(beast::role_type::client));
    beast::get_lowest_layer(*plain_ws_).connect(endpoints);
    plain_ws_->handshake(ws_host, parts_.target);
  }

  std::string subscribe_heads() {
    return subscribe(json::array({"newHeads"}), "newHeads");
  }

  std::string subscribe_logs(const json &filter) {
    return subscribe(json::array({"logs", filter}), "logs");
  }

  std::optional<json> try_read() {
    if (!queued_.empty()) {
      json message = std::move(queued_.front());
      queued_.pop_front();
      return message;
    }
    if (!has_data()) {
      return std::nullopt;
    }
    return read_json();
  }

private:
  void proxy_connect() {
    std::string target = parts_.host + ":" + parts_.port;
    http::request<http::empty_body> req;
    req.version(11);
    req.method(http::verb::connect);
    req.target(target);
    req.set(http::field::host, target);
    req.set(http::field::proxy_connection, "Keep-Alive");

    auto &stream = beast::get_lowest_layer(*ssl_ws_);
    http::write(stream, req);

    beast::flat_buffer buf;
    http::response_parser<http::empty_body> parser;
    parser.skip(true);
    http::read(stream, buf, parser);
    assert(parser.get().result() == http::status::ok);
  }

  std::string subscribe(const json &params, const std::string &label) {
    int id = next_id_++;
    write_json({
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "eth_subscribe"},
        {"params", params},
    });
    while (true) {
      json message = read_json();
      if (message.contains("id")) {
        assert(message.at("id").get<int>() == id);
        assert(message.contains("result"));
        std::string sub_id = norm_hex(message.at("result").get<std::string>());
        log_query("ws", "eth_subscribe:" + label, 1, true, "sub_id=" + sub_id);
        return sub_id;
      }
      queued_.push_back(std::move(message));
    }
  }

  void write_json(const json &msg) {
    std::string body = msg.dump();
    if (parts_.secure()) {
      ssl_ws_->write(asio::buffer(body));
      return;
    }
    plain_ws_->write(asio::buffer(body));
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
  std::optional<UrlParts> proxy_;
  asio::io_context ioc_;
  ssl::context ssl_ctx_;
  tcp::resolver resolver_;
  std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ssl_ws_;
  std::optional<websocket::stream<beast::tcp_stream>> plain_ws_;
  beast::flat_buffer buf_;
  std::deque<json> queued_;
  int next_id_ = 1;
};

} // namespace

WsThread::WsThread(const AppConfig &cfg, EventQueue &queue)
    : cfg_(cfg), queue_(queue) {}

void WsThread::start() {
  assert(!running_);
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void WsThread::stop() {
  running_ = false;
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

WsSessionInfo WsThread::start_session(const std::vector<std::string> &users) {
  assert(running_);
  std::unique_lock<std::mutex> lock(mu_);
  ++desired_session_id_;
  desired_users_ = users;
  ready_session_id_ = 0;
  ready_start_block_ = 0;
  uint64_t session_id = desired_session_id_;
  cv_.notify_all();
  cv_.wait(lock, [this, session_id] {
    return !running_ || ready_session_id_ == session_id;
  });
  assert(running_);
  return {session_id, ready_start_block_};
}

WsCounters WsThread::counters() const {
  return {
      .msg = ws_msg_count_.load(),
      .sub = ws_sub_count_.load(),
  };
}


void WsThread::run() {
  while (running_) {
    uint64_t session_id = 0;
    std::vector<std::string> users;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return !running_ || desired_session_id_ > 0; });
      if (!running_) {
        return;
      }
      session_id = desired_session_id_;
      users = desired_users_;
    }
    if (users.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    size_t connect_attempt = 0;
    while (running_) {
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (session_id != desired_session_id_) {
          break;
        }
      }

      try {
        ++connect_attempt;
        WsStream ws(cfg_.rpc_ws_url, cfg_.proxy_url);
        ws.connect();
        log_query("ws", "connect", connect_attempt, true, "url=" + cfg_.rpc_ws_url);
        connect_attempt = 0;

        std::string head_sub = ws.subscribe_heads();
        std::set<std::string> log_subs;
        for (const auto &filter : build_user_log_filters(users, cfg_.topic_group_size)) {
          log_subs.insert(ws.subscribe_logs(filter));
        }
        ws_sub_count_ += 1 + log_subs.size();

        bool ready = false;
        std::map<uint64_t, std::map<std::string, json>> pending_by_block;
        while (running_) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            if (session_id != desired_session_id_) {
              break;
            }
          }

          auto msg_opt = ws.try_read();
          if (!msg_opt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
          }

          ++ws_msg_count_;
          const json &message = *msg_opt;
          assert(message.contains("method"));
          assert(message.at("method").get<std::string>() == "eth_subscription");

          const json &params = message.at("params");
          std::string sub_id =
              norm_hex(params.at("subscription").get<std::string>());
          if (sub_id == head_sub) {
            uint64_t head =
                hex_to_u64(params.at("result").at("number").get<std::string>());
            if (!ready) {
              std::lock_guard<std::mutex> lock(mu_);
              ready = true;
              ready_session_id_ = session_id;
              ready_start_block_ = head;
              cv_.notify_all();
            }
            queue_.push({
                .session_id = session_id,
                .kind = QueueEventKind::Head,
                .block_number = head,
                .logs = json::array(),
            });
            while (!pending_by_block.empty() &&
                   pending_by_block.begin()->first < head) {
              auto block_it = pending_by_block.begin();
              json logs = json::array();
              for (auto &[_, log] : block_it->second) {
                logs.push_back(std::move(log));
              }
              queue_.push({
                  .session_id = session_id,
                  .kind = QueueEventKind::Logs,
                  .block_number = block_it->first,
                  .logs = std::move(logs),
              });
              pending_by_block.erase(block_it);
            }
            continue;
          }

          assert(log_subs.contains(sub_id));
          const json &log = params.at("result");
          if (log.contains("removed") && log.at("removed").get<bool>()) {
            queue_.push({
                .session_id = session_id,
                .kind = QueueEventKind::Resync,
                .block_number = 0,
                .logs = json::array(),
            });
            break;
          }
          uint64_t block = hex_to_u64(log.at("blockNumber").get<std::string>());
          pending_by_block[block][raw_log_key(log)] = log;
        }
      } catch (const std::exception &e) {
        log_query("ws", "connect", connect_attempt, false, std::string(e.what()));
      }

      if (!running_) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        if (session_id != desired_session_id_) {
          break;
        }
      }
      queue_.push({
          .session_id = session_id,
          .kind = QueueEventKind::Resync,
          .block_number = 0,
          .logs = json::array(),
      });
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
  }
}

} // namespace tracker
