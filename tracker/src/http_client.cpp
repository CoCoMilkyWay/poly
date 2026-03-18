#include "tracker/http_client.hpp"
#include "tracker/config.hpp"
#include "tracker/logger.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

namespace tracker {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

struct PendingRequest {
  HttpRequest request;
  UrlParts parts;
  size_t index = 0;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(asio::io_context &ioc, ssl::context &ssl_ctx,
              std::deque<PendingRequest> &queue,
              std::vector<HttpResponse> &results,
              std::atomic<size_t> &active_count,
              std::mutex &queue_mutex)
      : resolver_(ioc),
        ssl_ctx_(ssl_ctx),
        queue_(queue),
        results_(results),
        active_count_(active_count),
        queue_mutex_(queue_mutex) {}

  void start() { start_next(); }

private:
  void start_next() {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (queue_.empty()) {
        --active_count_;
        return;
      }
      current_ = std::move(queue_.front());
      queue_.pop_front();
    }
    retry_count_ = 0;
    do_resolve();
  }

  void do_resolve() {
    resolver_.async_resolve(
        current_.parts.host, current_.parts.port,
        [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type results) {
          if (ec) {
            self->retry_with_delay();
            return;
          }
          self->endpoints_ = std::move(results);
          self->do_connect();
        });
  }

  void do_connect() {
    if (current_.parts.secure()) {
      ssl_stream_.emplace(resolver_.get_executor(), ssl_ctx_);
      [[maybe_unused]] const int sni = SSL_set_tlsext_host_name(ssl_stream_->native_handle(), current_.parts.host.c_str());
      assert(sni == 1);
      beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(30));
      beast::get_lowest_layer(*ssl_stream_).async_connect(
          endpoints_,
          [self = shared_from_this()](beast::error_code ec, const tcp::endpoint &) {
            if (ec) { self->retry_with_delay(); return; }
            self->do_ssl_handshake();
          });
    } else {
      plain_stream_.emplace(resolver_.get_executor());
      plain_stream_->expires_after(std::chrono::seconds(30));
      plain_stream_->async_connect(
          endpoints_,
          [self = shared_from_this()](beast::error_code ec, const tcp::endpoint &) {
            if (ec) { self->retry_with_delay(); return; }
            self->do_write_plain();
          });
    }
  }

  void do_ssl_handshake() {
    ssl_stream_->async_handshake(ssl::stream_base::client,
        [self = shared_from_this()](beast::error_code ec) {
          if (ec) { self->retry_with_delay(); return; }
          self->do_write_ssl();
        });
  }

  void do_write_ssl() {
    build_request();
    http::async_write(*ssl_stream_, request_,
        [self = shared_from_this()](beast::error_code ec, size_t) {
          if (ec) { self->retry_with_delay(); return; }
          self->do_read_ssl();
        });
  }

  void do_read_ssl() {
    response_ = {};
    http::async_read(*ssl_stream_, buffer_, response_,
        [self = shared_from_this()](beast::error_code ec, size_t) {
          if (ec) { self->retry_with_delay(); return; }
          self->handle_response();
          self->do_shutdown_ssl();
        });
  }

  void do_shutdown_ssl() {
    beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(5));
    ssl_stream_->async_shutdown([self = shared_from_this()](beast::error_code) {
      self->ssl_stream_.reset();
      self->start_next();
    });
  }

  void do_write_plain() {
    build_request();
    http::async_write(*plain_stream_, request_,
        [self = shared_from_this()](beast::error_code ec, size_t) {
          if (ec) { self->retry_with_delay(); return; }
          self->do_read_plain();
        });
  }

  void do_read_plain() {
    response_ = {};
    http::async_read(*plain_stream_, buffer_, response_,
        [self = shared_from_this()](beast::error_code ec, size_t) {
          if (ec) { self->retry_with_delay(); return; }
          self->handle_response();
          self->do_shutdown_plain();
        });
  }

  void do_shutdown_plain() {
    boost::system::error_code ec;
    plain_stream_->socket().shutdown(tcp::socket::shutdown_both, ec); // NOLINT
    plain_stream_.reset();
    start_next();
  }

  void build_request() {
    request_ = {};
    request_.version(11);
    request_.method(current_.request.method == "POST" ? http::verb::post : http::verb::get);
    request_.target(current_.parts.target);
    request_.set(http::field::host, current_.parts.host);
    request_.set(http::field::user_agent, "tracker");
    request_.set(http::field::accept, "application/json");
    request_.set(http::field::connection, "close");
    if (!current_.request.body.empty()) {
      request_.set(http::field::content_type, "application/json");
      request_.body() = current_.request.body;
      request_.prepare_payload();
    }
  }

  void handle_response() {
    results_[current_.index] = HttpResponse{
        .status = static_cast<int>(response_.result_int()),
        .body = std::move(response_.body()),
    };
  }

  void retry_with_delay() {
    ssl_stream_.reset();
    plain_stream_.reset();
    buffer_.clear();
    ++retry_count_;
    logger().warn("http retry " + std::to_string(retry_count_) + ": " + current_.request.url);

    auto timer = std::make_shared<asio::steady_timer>(resolver_.get_executor());
    timer->expires_after(std::chrono::milliseconds(500));
    timer->async_wait([self = shared_from_this(), timer](beast::error_code) {
      self->do_resolve();
    });
  }

  size_t retry_count_ = 0;

  tcp::resolver resolver_;
  ssl::context &ssl_ctx_;
  std::deque<PendingRequest> &queue_;
  std::vector<HttpResponse> &results_;
  std::atomic<size_t> &active_count_;
  std::mutex &queue_mutex_;

  PendingRequest current_;
  tcp::resolver::results_type endpoints_;
  std::optional<beast::ssl_stream<beast::tcp_stream>> ssl_stream_;
  std::optional<beast::tcp_stream> plain_stream_;
  http::request<http::string_body> request_;
  http::response<http::string_body> response_;
  beast::flat_buffer buffer_;
};

} // namespace

std::vector<HttpResponse> http_batch(const std::vector<HttpRequest> &requests, size_t concurrency) {
  assert(!requests.empty());
  assert(concurrency > 0);

  std::deque<PendingRequest> queue;
  std::vector<HttpResponse> results(requests.size());
  std::mutex queue_mutex;

  for (size_t i = 0; i < requests.size(); ++i) {
    queue.push_back(PendingRequest{
        .request = requests[i],
        .parts = parse_url(requests[i].url),
        .index = i,
    });
  }

  asio::io_context ioc;
  ssl::context ssl_ctx(ssl::context::tlsv12_client);
  ssl_ctx.set_default_verify_paths();
  ssl_ctx.set_verify_mode(ssl::verify_peer);

  std::atomic<size_t> active_count = std::min(concurrency, requests.size());
  for (size_t i = 0; i < active_count; ++i) {
    std::make_shared<HttpSession>(ioc, ssl_ctx, queue, results, active_count, queue_mutex)->start();
  }

  ioc.run();
  return results;
}

HttpResponse http_get(const std::string &url) {
  return http_batch({{url, "GET", ""}}, 1)[0];
}

HttpResponse http_post_json(const std::string &url, const json &payload) {
  return http_batch({{url, "POST", payload.dump()}}, 1)[0];
}

} // namespace tracker
