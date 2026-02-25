#pragma once

#include <cassert>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include "misc/profiler.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

class RpcClient {
public:
  using LogsQuery = std::tuple<std::optional<std::string>, int64_t, int64_t, std::vector<std::string>>;

  struct BatchResult {
    std::vector<json> results;
    std::string raw_body;
    size_t response_bytes = 0;
    bool success = false;
    std::string error_msg;
  };

  RpcClient(const std::string &url, const std::string &api_key = "", const std::string &worker_name = "RPC-Worker",
            const std::string &proxy_url = "")
      : api_key_(api_key), worker_name_(worker_name), ioc_(), ssl_ctx_(asio::ssl::context::tls_client) {
    parse_url(url);
    parse_proxy_url(proxy_url);
    ssl_ctx_.set_verify_mode(asio::ssl::verify_none);
    start_worker();
  }

  ~RpcClient() {
    stop_worker();
    disconnect();
  }

  int64_t eth_blockNumber() {
    json request = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "eth_blockNumber"},
        {"params", json::array()}};

    std::string response_body = execute_request(request.dump());
    json response = json::parse(response_body);
    if (response.contains("error")) {
      throw std::runtime_error("eth_blockNumber RPC error: " + response["error"].dump());
    }
    return from_hex(response["result"].get<std::string>());
  }

  std::future<BatchResult> eth_getLogs_batch_async(const std::vector<LogsQuery> &queries) {
    auto promise = std::make_shared<std::promise<BatchResult>>();
    auto future = promise->get_future();
    std::string body = build_batch_request(queries);
    size_t count = queries.size();

    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      request_queue_.push_back(PendingRequest{
          .body = std::move(body),
          .promise = promise,
          .count = count,
      });
      worker_cv_.notify_one();
    }

    return future;
  }

private:
  struct PendingRequest {
    std::string body;
    std::shared_ptr<std::promise<BatchResult>> promise;
    size_t count = 0;
  };

  void start_worker() {
    worker_running_ = true;
    worker_thread_ = std::thread([this]() {
      TraceThread(worker_name_.c_str());
      TraceN("rpc/thread_start");
      worker_loop();
    });
  }

  void stop_worker() {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      worker_running_ = false;
      // Graceful stop: do not feed workers with queued requests anymore.
      // Keep only currently executing request; queued promises will break immediately.
      request_queue_.clear();
      worker_cv_.notify_one();
    }
    if (worker_thread_.joinable()) {
      worker_thread_.join();
    }
  }

  void worker_loop() {
    while (true) {
      std::string body;
      std::shared_ptr<std::promise<BatchResult>> promise;
      size_t count = 0;

      {
        std::unique_lock<std::mutex> lock(worker_mutex_);
        TraceN("rpc/wait");
        worker_cv_.wait(lock, [this] { return !worker_running_ || !request_queue_.empty(); });

        if (!worker_running_ && request_queue_.empty()) {
          break;
        }

        if (!request_queue_.empty()) {
          auto req = std::move(request_queue_.front());
          request_queue_.pop_front();
          body = std::move(req.body);
          promise = std::move(req.promise);
          count = req.count;
        }
      }

      if (promise) {
        TraceN("rpc/dispatch");
        BatchResult result;
        try {
          std::string response_body = http_post(body);
          result.response_bytes = last_response_size_;
          if (count > 0) {
            try {
              result.results = parse_batch_response(response_body, count);
            } catch (const std::exception &e) {
              throw std::runtime_error(std::string(e.what()) + " | response=" + response_body);
            }
          } else {
            result.raw_body = std::move(response_body);
          }
          result.success = true;
          TraceN("rpc/success");
        } catch (const std::exception &e) {
          result.success = false;
          result.error_msg = e.what();
          TraceN("rpc/fail");
        }
        promise->set_value(std::move(result));
      }
    }
  }

  std::string execute_request(const std::string &body) {
    auto promise = std::make_shared<std::promise<BatchResult>>();
    auto future = promise->get_future();

    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      request_queue_.push_back(PendingRequest{
          .body = body,
          .promise = promise,
          .count = 0,
      });
      worker_cv_.notify_one();
    }

    auto result = future.get();
    if (!result.success) {
      std::cerr << "[RPC] Request failed: " << result.error_msg << std::endl;
      throw std::runtime_error(result.error_msg);
    }
    return result.raw_body;
  }

  std::string build_batch_request(const std::vector<LogsQuery> &queries) {
    json batch = json::array();
    for (const auto &[address, from_block, to_block, topic0_list] : queries) {
      json filter = {
          {"fromBlock", to_hex(from_block)},
          {"toBlock", to_hex(to_block)}};
      if (address.has_value()) {
        filter["address"] = address.value();
      }
      if (!topic0_list.empty()) {
        filter["topics"] = json::array({topic0_list});
      }
      batch.push_back({{"jsonrpc", "2.0"},
                       {"id", batch.size()},
                       {"method", "eth_getLogs"},
                       {"params", json::array({filter})}});
    }
    return batch.dump();
  }

  std::vector<json> parse_batch_response(const std::string &response_body, size_t count) {
    json responses = json::parse(response_body);
    std::vector<json> results(count);
    for (const auto &resp : responses) {
      if (resp.contains("error")) {
        size_t id = resp.value("id", 0);
        throw std::runtime_error("RPC batch response error, id=" + std::to_string(id) + ": " + resp["error"].dump());
      }
      size_t id = resp["id"].get<size_t>();
      results[id] = resp["result"];
    }
    return results;
  }

  void parse_proxy_url(const std::string &proxy_url) {
    if (proxy_url.empty())
      return;
    std::string u = proxy_url;
    if (u.starts_with("socks5://"))
      u = u.substr(9);
    else if (u.starts_with("http://"))
      u = u.substr(7);
    auto colon_pos = u.rfind(':');
    if (colon_pos != std::string::npos) {
      proxy_host_ = u.substr(0, colon_pos);
      proxy_port_ = u.substr(colon_pos + 1);
    } else {
      proxy_host_ = u;
      proxy_port_ = "8080";
    }
  }

  static void socks5_handshake(asio::ip::tcp::socket &sock, const std::string &dst_host, int dst_port) {
    // 1. 发送 greeting: version=5, nmethods=1, method=no-auth
    uint8_t greeting[3] = {0x05, 0x01, 0x00};
    asio::write(sock, asio::buffer(greeting, 3));

    // 2. 读取代理选择的 method
    uint8_t method_resp[2];
    asio::read(sock, asio::buffer(method_resp, 2));
    assert(method_resp[0] == 0x05 && method_resp[1] == 0x00 && "SOCKS5 不支持无认证");

    // 3. 发送 CONNECT 请求 (ATYP=0x03 域名)
    std::vector<uint8_t> req;
    req.push_back(0x05);                          // version
    req.push_back(0x01);                          // cmd=CONNECT
    req.push_back(0x00);                          // reserved
    req.push_back(0x03);                          // atyp=domain
    req.push_back(static_cast<uint8_t>(dst_host.size()));
    req.insert(req.end(), dst_host.begin(), dst_host.end());
    req.push_back(static_cast<uint8_t>((dst_port >> 8) & 0xff));
    req.push_back(static_cast<uint8_t>(dst_port & 0xff));
    asio::write(sock, asio::buffer(req));

    // 4. 读取响应头 (固定 4 字节)
    uint8_t resp[4];
    asio::read(sock, asio::buffer(resp, 4));
    assert(resp[0] == 0x05 && resp[1] == 0x00 && "SOCKS5 CONNECT 失败");

    // 5. 读取并丢弃 BND.ADDR + BND.PORT
    if (resp[3] == 0x01) {          // IPv4: 4 + 2
      uint8_t buf[6]; asio::read(sock, asio::buffer(buf, 6));
    } else if (resp[3] == 0x03) {   // domain: len + domain + 2
      uint8_t len; asio::read(sock, asio::buffer(&len, 1));
      std::vector<uint8_t> buf(len + 2); asio::read(sock, asio::buffer(buf));
    } else if (resp[3] == 0x04) {   // IPv6: 16 + 2
      uint8_t buf[18]; asio::read(sock, asio::buffer(buf, 18));
    }
    // 隧道建立完毕，后续直接走 TLS
  }

  void parse_url(const std::string &url) {
    std::string u = url;
    if (u.starts_with("https://")) {
      use_ssl_ = true;
      u = u.substr(8);
    } else if (u.starts_with("http://")) {
      use_ssl_ = false;
      u = u.substr(7);
    } else {
      assert(false && "RPC URL 必须以 http:// 或 https:// 开头");
    }

    auto slash_pos = u.find('/');
    if (slash_pos != std::string::npos) {
      target_ = u.substr(slash_pos);
      u = u.substr(0, slash_pos);
    } else {
      target_ = "/";
    }

    auto colon_pos = u.find(':');
    if (colon_pos != std::string::npos) {
      host_ = u.substr(0, colon_pos);
      port_ = u.substr(colon_pos + 1);
    } else {
      host_ = u;
      port_ = use_ssl_ ? "443" : "80";
    }
  }

  void ensure_connected() {
    if (connected_)
      return;

    tcp::resolver resolver(ioc_);

    if (use_ssl_) {
      ssl_stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc_, ssl_ctx_);
      SSL_set_tlsext_host_name(ssl_stream_->native_handle(), host_.c_str());

      if (!proxy_host_.empty()) {
        // 通过 SOCKS5 代理连接
        auto proxy_endpoints = resolver.resolve(proxy_host_, proxy_port_);
        beast::get_lowest_layer(*ssl_stream_).connect(proxy_endpoints);
        socks5_handshake(beast::get_lowest_layer(*ssl_stream_).socket(), host_, std::stoi(port_));
      } else {
        auto const endpoints = resolver.resolve(host_, port_);
        beast::get_lowest_layer(*ssl_stream_).connect(endpoints);
      }

      ssl_stream_->handshake(asio::ssl::stream_base::client);
    } else {
      tcp_stream_ = std::make_unique<beast::tcp_stream>(ioc_);
      auto const endpoints = resolver.resolve(host_, port_);
      tcp_stream_->connect(endpoints);
    }
    connected_ = true;
  }

  void disconnect() {
    if (!connected_)
      return;

    beast::error_code ec;
    if (use_ssl_ && ssl_stream_) {
      [[maybe_unused]] auto _ = ssl_stream_->shutdown(ec);
      ssl_stream_.reset();
    } else if (tcp_stream_) {
      [[maybe_unused]] auto _ = tcp_stream_->socket().shutdown(tcp::socket::shutdown_both, ec);
      tcp_stream_.reset();
    }
    connected_ = false;
  }

  std::string http_post(const std::string &body) {
    http::request<http::string_body> req{http::verb::post, target_, 11};
    req.set(http::field::host, host_);
    req.set(http::field::content_type, "application/json");
    req.set(http::field::user_agent, "PolySync/1.0");
    req.set(http::field::connection, "keep-alive");
    if (!api_key_.empty()) {
      req.set(http::field::authorization, "Bearer " + api_key_);
    }
    req.body() = body;
    req.prepare_payload();

    for (int retry = 0; retry < 2; ++retry) {
      try {
        ensure_connected();

        beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        parser.body_limit(1024 * 1024 * 1024); // 1GB

        if (use_ssl_) {
          http::write(*ssl_stream_, req);
        } else {
          http::write(*tcp_stream_, req);
        }

        if (use_ssl_) {
          http::read(*ssl_stream_, buffer, parser);
        } else {
          http::read(*tcp_stream_, buffer, parser);
        }

        std::string result = parser.get().body();
        last_response_size_ = result.size();
        return result;
      } catch (...) {
        disconnect();
        if (retry == 1)
          throw;
      }
    }
    throw std::runtime_error("http_post failed after retries");
  }

  static std::string to_hex(int64_t value) {
    std::stringstream ss;
    ss << "0x" << std::hex << value;
    return ss.str();
  }

  static int64_t from_hex(const std::string &hex) {
    return std::stoll(hex, nullptr, 16);
  }

  std::string host_;
  std::string port_;
  std::string target_;
  std::string api_key_;
  std::string worker_name_;
  std::string proxy_host_;
  std::string proxy_port_;
  bool use_ssl_ = false;
  size_t last_response_size_ = 0;

  asio::io_context ioc_;
  asio::ssl::context ssl_ctx_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> ssl_stream_;
  std::unique_ptr<beast::tcp_stream> tcp_stream_;
  bool connected_ = false;

  std::thread worker_thread_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  bool worker_running_ = false;
  std::deque<PendingRequest> request_queue_;
};
