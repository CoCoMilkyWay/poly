#include "tracker/http.hpp"
#include "tracker/config.hpp"
#include "tracker/log.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>

#include <atomic>
#include <cassert>
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

struct PendingReq {
  HttpReq req;
  UrlParts parts;
  size_t idx = 0;
};

class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(asio::io_context &ioc, ssl::context &ssl_ctx,
              std::deque<PendingReq> &queue, std::vector<HttpRes> &results,
              std::atomic<size_t> &active, std::mutex &mu,
              const std::optional<UrlParts> &proxy)
      : resolver_(ioc), ssl_ctx_(ssl_ctx), queue_(queue),
        results_(results), active_(active), mu_(mu), proxy_(proxy) {}

  void start() { next(); }

private:
  void next() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (queue_.empty()) {
        --active_;
        return;
      }
      cur_ = std::move(queue_.front());
      queue_.pop_front();
    }
    retries_ = 0;
    resolve();
  }

  void resolve() {
    // 有代理时解析代理地址,否则解析目标地址
    const std::string &host = proxy_ ? proxy_->host : cur_.parts.host;
    const std::string &port = proxy_ ? proxy_->port : cur_.parts.port;
    resolver_.async_resolve(host, port,
                            [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type r) {
                              if (ec) {
                                self->retry();
                                return;
                              }
                              self->endpoints_ = std::move(r);
                              self->connect();
                            });
  }

  void connect() {
    if (cur_.parts.secure()) {
      ssl_stream_.emplace(resolver_.get_executor(), ssl_ctx_);
      SSL_set_tlsext_host_name(ssl_stream_->native_handle(), cur_.parts.host.c_str());
      beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(30));
      beast::get_lowest_layer(*ssl_stream_).async_connect(endpoints_, [self = shared_from_this()](beast::error_code ec, const tcp::endpoint &) {
        if (ec) {
          self->retry();
          return;
        }
        if (self->proxy_)
          self->proxy_connect();
        else
          self->handshake();
      });
    } else {
      plain_stream_.emplace(resolver_.get_executor());
      plain_stream_->expires_after(std::chrono::seconds(30));
      plain_stream_->async_connect(endpoints_,
                                   [self = shared_from_this()](beast::error_code ec, const tcp::endpoint &) {
                                     if (ec) {
                                       self->retry();
                                       return;
                                     }
                                     self->write_plain();
                                   });
    }
  }

  void proxy_connect() {
    // 发送 HTTP CONNECT 请求建立隧道
    std::string target = cur_.parts.host + ":" + cur_.parts.port;
    proxy_req_ = {};
    proxy_req_.version(11);
    proxy_req_.method(http::verb::connect);
    proxy_req_.target(target);
    proxy_req_.set(http::field::host, target);
    proxy_req_.set(http::field::proxy_connection, "Keep-Alive");

    auto &stream = beast::get_lowest_layer(*ssl_stream_);
    http::async_write(stream, proxy_req_,
                      [self = shared_from_this()](beast::error_code ec, size_t) {
                        if (ec) {
                          self->retry();
                          return;
                        }
                        self->proxy_read();
                      });
  }

  void proxy_read() {
    proxy_parser_.emplace();
    proxy_parser_->skip(true); // CONNECT 响应没有 body,跳过 body 解析
    auto &stream = beast::get_lowest_layer(*ssl_stream_);
    http::async_read(stream, proxy_buf_, *proxy_parser_,
                     [self = shared_from_this()](beast::error_code ec, size_t) {
                       if (ec) {
                         self->retry();
                         return;
                       }
                       // CONNECT 成功返回 200
                       assert(self->proxy_parser_->get().result() == http::status::ok);
                       self->proxy_buf_.consume(self->proxy_buf_.size());
                       self->handshake();
                     });
  }

  void handshake() {
    ssl_stream_->async_handshake(ssl::stream_base::client,
                                 [self = shared_from_this()](beast::error_code ec) {
                                   if (ec) {
                                     self->retry();
                                     return;
                                   }
                                   self->write_ssl();
                                 });
  }

  void write_ssl() {
    build_request();
    http::async_write(*ssl_stream_, req_,
                      [self = shared_from_this()](beast::error_code ec, size_t) {
                        if (ec) {
                          self->retry();
                          return;
                        }
                        self->read_ssl();
                      });
  }

  void read_ssl() {
    res_ = {};
    http::async_read(*ssl_stream_, buf_, res_,
                     [self = shared_from_this()](beast::error_code ec, size_t) {
                       if (ec) {
                         self->retry();
                         return;
                       }
                       self->done();
                       self->shutdown_ssl();
                     });
  }

  void shutdown_ssl() {
    beast::get_lowest_layer(*ssl_stream_).expires_after(std::chrono::seconds(5));
    ssl_stream_->async_shutdown([self = shared_from_this()](beast::error_code) {
      self->ssl_stream_.reset();
      self->next();
    });
  }

  void write_plain() {
    build_request();
    http::async_write(*plain_stream_, req_,
                      [self = shared_from_this()](beast::error_code ec, size_t) {
                        if (ec) {
                          self->retry();
                          return;
                        }
                        self->read_plain();
                      });
  }

  void read_plain() {
    res_ = {};
    http::async_read(*plain_stream_, buf_, res_,
                     [self = shared_from_this()](beast::error_code ec, size_t) {
                       if (ec) {
                         self->retry();
                         return;
                       }
                       self->done();
                       self->shutdown_plain();
                     });
  }

  void shutdown_plain() {
    boost::system::error_code ec;
    [[maybe_unused]] boost::system::error_code shutdown_ec =
        plain_stream_->socket().shutdown(tcp::socket::shutdown_both, ec);
    assert(!shutdown_ec);
    assert(!ec);
    plain_stream_.reset();
    next();
  }

  void build_request() {
    req_ = {};
    req_.version(11);
    req_.method(cur_.req.method == "POST" ? http::verb::post : http::verb::get);
    req_.target(cur_.parts.target);
    req_.set(http::field::host, cur_.parts.host);
    req_.set(http::field::user_agent, "tracker");
    req_.set(http::field::accept, "application/json");
    req_.set(http::field::connection, "close");
    if (!cur_.req.body.empty()) {
      req_.set(http::field::content_type, "application/json");
      req_.body() = cur_.req.body;
      req_.prepare_payload();
    }
  }

  void done() {
    results_[cur_.idx] = HttpRes{
        .status = static_cast<int>(res_.result_int()),
        .body = std::move(res_.body()),
    };
  }

  void retry() {
    ssl_stream_.reset();
    plain_stream_.reset();
    buf_.clear();
    proxy_buf_.clear();
    ++retries_;
    sync_logger().warn("http retry " + std::to_string(retries_) + ": " + cur_.req.url);
    auto timer = std::make_shared<asio::steady_timer>(resolver_.get_executor());
    timer->expires_after(std::chrono::milliseconds(500));
    timer->async_wait([self = shared_from_this(), timer](beast::error_code) {
      self->resolve();
    });
  }

  size_t retries_ = 0;
  tcp::resolver resolver_;
  ssl::context &ssl_ctx_;
  std::deque<PendingReq> &queue_;
  std::vector<HttpRes> &results_;
  std::atomic<size_t> &active_;
  std::mutex &mu_;
  std::optional<UrlParts> proxy_;

  PendingReq cur_;
  tcp::resolver::results_type endpoints_;
  std::optional<beast::ssl_stream<beast::tcp_stream>> ssl_stream_;
  std::optional<beast::tcp_stream> plain_stream_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
  beast::flat_buffer buf_;
  // proxy CONNECT
  http::request<http::empty_body> proxy_req_;
  std::optional<http::response_parser<http::empty_body>> proxy_parser_;
  beast::flat_buffer proxy_buf_;
};

} // namespace

std::vector<HttpRes> http_batch(const std::vector<HttpReq> &reqs, size_t concurrency, const std::string &proxy_url) {
  assert(!reqs.empty());
  assert(concurrency > 0);

  std::optional<UrlParts> proxy;
  if (!proxy_url.empty())
    proxy = parse_url(proxy_url);

  std::deque<PendingReq> queue;
  std::vector<HttpRes> results(reqs.size());
  std::mutex mu;

  for (size_t i = 0; i < reqs.size(); ++i) {
    queue.push_back({reqs[i], parse_url(reqs[i].url), i});
  }

  asio::io_context ioc;
  ssl::context ssl_ctx(ssl::context::tlsv12_client);
  ssl_ctx.set_default_verify_paths();
  ssl_ctx.set_verify_mode(ssl::verify_peer);

  std::atomic<size_t> active = std::min(concurrency, reqs.size());
  for (size_t i = 0; i < active; ++i) {
    std::make_shared<HttpSession>(ioc, ssl_ctx, queue, results, active, mu, proxy)->start();
  }
  ioc.run();
  return results;
}

HttpRes http_get(const std::string &url, const std::string &proxy_url) {
  return http_batch({{url, "GET", ""}}, 1, proxy_url)[0];
}

HttpRes http_post(const std::string &url, const json &payload, const std::string &proxy_url) {
  return http_batch({{url, "POST", payload.dump()}}, 1, proxy_url)[0];
}

} // namespace tracker
