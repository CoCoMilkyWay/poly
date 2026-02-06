#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define HTTPS_TIMEOUT_SEC 30 // 请求超时
#define HTTPS_HOST "gateway.thegraph.com"
#define HTTPS_PORT "443"

#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

class HttpsPool;

// ============================================================================
// HttpsSession - 可复用的 HTTPS 连接会话
// ============================================================================
class HttpsSession : public std::enable_shared_from_this<HttpsSession> {
public:
  using Callback = std::function<void(std::string, bool)>; // (response, success)

  HttpsSession(asio::io_context &ioc, ssl::context &ssl_ctx,
               const std::string &api_key, HttpsPool *pool)
      : resolver_(ioc), stream_(ioc, ssl_ctx), api_key_(api_key), pool_(pool) {}

  void run(const std::string &target, const std::string &body, Callback cb) {
    cb_ = std::move(cb);
    target_ = target;
    body_ = body;

    if (connected_) {
      do_write();
    } else {
      SSL_set_tlsext_host_name(stream_.native_handle(), HTTPS_HOST);
      resolver_.async_resolve(
          HTTPS_HOST, HTTPS_PORT,
          [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type results) {
            if (ec) {
              self->fail("DNS resolve");
              return;
            }
            self->on_resolve(results);
          });
    }
  }

  bool is_connected() const { return connected_; }
  void mark_disconnected() { connected_ = false; }

private:
  void fail(const char *what);
  void return_to_pool();

  // ========================================================================
  // 连接建立流程
  // ========================================================================

  void on_resolve(tcp::resolver::results_type results) {
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
    beast::get_lowest_layer(stream_).async_connect(
        results,
        [self = shared_from_this()](beast::error_code ec, tcp::endpoint) {
          if (ec) {
            self->fail("TCP connect");
            return;
          }
          self->on_connect();
        });
  }

  void on_connect() {
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
    stream_.async_handshake(
        ssl::stream_base::client,
        [self = shared_from_this()](beast::error_code ec) {
          if (ec) {
            self->fail("SSL handshake");
            return;
          }
          self->connected_ = true;
          self->do_write();
        });
  }

  // ========================================================================
  // 请求/响应流程
  // ========================================================================

  void do_write() {
    req_ = {};
    res_ = {};
    buffer_.clear();

    req_.method(http::verb::post);
    req_.target(target_);
    req_.version(11);
    req_.set(http::field::host, HTTPS_HOST);
    req_.set(http::field::content_type, "application/json");
    req_.set(http::field::authorization, "Bearer " + api_key_);
    req_.set(http::field::connection, "keep-alive");
    req_.body() = body_;
    req_.prepare_payload();

    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
    http::async_write(
        stream_, req_,
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
          if (ec) {
            self->connected_ = false;
            self->fail("HTTP write");
            return;
          }
          self->on_write();
        });
  }

  void on_write() {
    beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
    http::async_read(
        stream_, buffer_, res_,
        [self = shared_from_this()](beast::error_code ec, std::size_t) {
          if (ec) {
            self->connected_ = false;
            self->fail("HTTP read");
            return;
          }
          self->on_read();
        });
  }

  void on_read() {
    cb_(res_.body(), true);
    return_to_pool();
  }

  // 网络组件
  tcp::resolver resolver_;
  beast::ssl_stream<beast::tcp_stream> stream_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;

  // 配置
  std::string api_key_;
  HttpsPool *pool_;

  // 请求状态
  std::string target_;
  std::string body_;
  Callback cb_;
  bool connected_ = false;
};
