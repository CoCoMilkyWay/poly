#include "stage0_sync_http.hpp"

#include <cassert>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/ssl.h>
#include <algorithm>

namespace stage0 {
namespace {

constexpr int kFetchTimeoutMs = 5000;
constexpr int kFetchRetryDelayMs = 200;

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

std::string to_lower_ascii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string strip_hex_prefix(std::string s) {
  if (s.starts_with("0x") || s.starts_with("0X")) {
    return s.substr(2);
  }
  return s;
}

std::string normalize_hex_id_no_prefix(std::string s) {
  s = to_lower_ascii(strip_hex_prefix(std::move(s)));
  assert(!s.empty());
  size_t first_non_zero = s.find_first_not_of('0');
  if (first_non_zero == std::string::npos) {
    return "0";
  }
  return s.substr(first_non_zero);
}

std::string compact_for_log(std::string s, size_t max_len = 480) {
  for (char &c : s) {
    if (c == '\n' || c == '\r' || c == '\t') {
      c = ' ';
    }
  }
  if (s.size() <= max_len) {
    return s;
  }
  return s.substr(0, max_len) + "...";
}

std::string build_gamma_target(std::string base_target, const std::string &condition_hex_lower) {
  if (base_target.empty()) {
    base_target = "/";
  }
  if (!base_target.ends_with('/')) {
    base_target.push_back('/');
  }
  return base_target + "markets?condition_ids=" + condition_hex_lower + "&include_tag=true";
}

struct HttpGetOutcome {
  bool ok = false;
  long status_code = 0;
  std::string body;
  std::string error;
  std::string target;
};

class AsyncHttpsGetOp final : public std::enable_shared_from_this<AsyncHttpsGetOp> {
public:
  using DoneFn = std::function<void(HttpGetOutcome)>;

  AsyncHttpsGetOp(asio::io_context &ioc, asio::ssl::context &ssl_ctx, std::string host, std::string port,
                  std::string target, std::string proxy_url, std::chrono::milliseconds timeout, DoneFn done)
      : resolver_(ioc), stream_(ioc, ssl_ctx), timer_(ioc), host_(std::move(host)), port_(std::move(port)),
        target_(std::move(target)), timeout_(timeout), done_(std::move(done)) {
    if (proxy_url.starts_with("socks5://")) {
      proxy_scheme_ = ProxyScheme::kSocks5;
    } else if (!proxy_url.empty()) {
      proxy_scheme_ = ProxyScheme::kHttpConnect;
    }
    auto [proxy_host, proxy_port] = parse_proxy_host_port(proxy_url);
    proxy_host_ = std::move(proxy_host);
    proxy_port_ = std::move(proxy_port);
    use_proxy_ = !proxy_host_.empty();
  }

  void start() {
    const int sni_ok = SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str());
    assert(sni_ok == 1);
    req_.version(11);
    req_.method(http::verb::get);
    req_.target(target_);
    req_.set(http::field::host, host_);
    req_.set(http::field::user_agent, "PolySync/1.0");
    req_.set(http::field::accept, "application/json");
    req_.set(http::field::connection, "close");
    arm_timeout();
    auto self = shared_from_this();
    const std::string &resolve_host = use_proxy_ ? proxy_host_ : host_;
    const std::string &resolve_port = use_proxy_ ? proxy_port_ : port_;
    resolver_.async_resolve(resolve_host, resolve_port,
                            [self](const boost::system::error_code &ec, const tcp::resolver::results_type &results) {
                              self->on_resolve(ec, results);
                            });
  }

private:
  void arm_timeout() {
    timer_.expires_after(timeout_);
    auto self = shared_from_this();
    timer_.async_wait([self](const boost::system::error_code &ec) {
      if (ec || self->finished_) {
        return;
      }
      self->resolver_.cancel();
      beast::error_code ignored;
      auto &sock = beast::get_lowest_layer(self->stream_).socket();
      [[maybe_unused]] auto _ = sock.cancel(ignored);
      [[maybe_unused]] auto __ = sock.shutdown(tcp::socket::shutdown_both, ignored);
      [[maybe_unused]] auto ___ = sock.close(ignored);
    });
  }

  void finish(HttpGetOutcome out) {
    if (finished_) {
      return;
    }
    finished_ = true;
    timer_.cancel();
    done_(std::move(out));
  }

  void on_resolve(const boost::system::error_code &ec, const tcp::resolver::results_type &results) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "resolve: " + ec.message(),
          .target = target_,
      });
      return;
    }
    beast::get_lowest_layer(stream_).expires_after(timeout_);
    auto self = shared_from_this();
    beast::get_lowest_layer(stream_).async_connect(
        results, [self](const boost::system::error_code &ec2, const tcp::resolver::results_type::endpoint_type &) {
          self->on_connect(ec2);
        });
  }

  void on_connect(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "connect: " + ec.message(),
          .target = target_,
      });
      return;
    }
    if (use_proxy_) {
      if (proxy_scheme_ == ProxyScheme::kSocks5) {
        auto self = shared_from_this();
        static constexpr uint8_t kGreeting[3] = {0x05, 0x01, 0x00};
        asio::async_write(stream_.next_layer(), asio::buffer(kGreeting, sizeof(kGreeting)),
                          [self](const boost::system::error_code &ec2, std::size_t) {
                            self->on_socks_greeting_write(ec2);
                          });
        return;
      }
      connect_req_.version(11);
      connect_req_.method(http::verb::connect);
      connect_req_.target(host_ + ":" + port_);
      connect_req_.set(http::field::host, host_ + ":" + port_);
      connect_req_.set(http::field::user_agent, "PolySync/1.0");
      auto self = shared_from_this();
      http::async_write(stream_.next_layer(), connect_req_,
                        [self](const boost::system::error_code &ec2, std::size_t) {
                          self->on_proxy_write(ec2);
                        });
      return;
    }
    auto self = shared_from_this();
    stream_.async_handshake(asio::ssl::stream_base::client,
                            [self](const boost::system::error_code &ec2) { self->on_handshake(ec2); });
  }

  void on_socks_greeting_write(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_greeting_write: " + ec.message(),
          .target = target_,
      });
      return;
    }
    auto self = shared_from_this();
    asio::async_read(stream_.next_layer(), asio::buffer(socks_method_resp_),
                     [self](const boost::system::error_code &ec2, std::size_t) {
                       self->on_socks_greeting_read(ec2);
                     });
  }

  void on_socks_greeting_read(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_greeting_read: " + ec.message(),
          .target = target_,
      });
      return;
    }
    if (socks_method_resp_[0] != 0x05 || socks_method_resp_[1] != 0x00) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_method_not_supported",
          .target = target_,
      });
      return;
    }
    socks_connect_req_.clear();
    socks_connect_req_.push_back(0x05);
    socks_connect_req_.push_back(0x01);
    socks_connect_req_.push_back(0x00);
    socks_connect_req_.push_back(0x03);
    socks_connect_req_.push_back(static_cast<uint8_t>(host_.size()));
    socks_connect_req_.insert(socks_connect_req_.end(), host_.begin(), host_.end());
    const int dst_port = std::stoi(port_);
    socks_connect_req_.push_back(static_cast<uint8_t>((dst_port >> 8) & 0xff));
    socks_connect_req_.push_back(static_cast<uint8_t>(dst_port & 0xff));
    auto self = shared_from_this();
    asio::async_write(stream_.next_layer(), asio::buffer(socks_connect_req_),
                      [self](const boost::system::error_code &ec2, std::size_t) {
                        self->on_socks_connect_write(ec2);
                      });
  }

  void on_socks_connect_write(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_connect_write: " + ec.message(),
          .target = target_,
      });
      return;
    }
    auto self = shared_from_this();
    asio::async_read(stream_.next_layer(), asio::buffer(socks_connect_head_),
                     [self](const boost::system::error_code &ec2, std::size_t) {
                       self->on_socks_connect_head_read(ec2);
                     });
  }

  void on_socks_connect_head_read(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_connect_head_read: " + ec.message(),
          .target = target_,
      });
      return;
    }
    if (socks_connect_head_[0] != 0x05 || socks_connect_head_[1] != 0x00) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_connect_failed",
          .target = target_,
      });
      return;
    }
    size_t rest_len = 0;
    if (socks_connect_head_[3] == 0x01) {
      rest_len = 6;
    } else if (socks_connect_head_[3] == 0x03) {
      rest_len = 1;
    } else if (socks_connect_head_[3] == 0x04) {
      rest_len = 18;
    } else {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_atyp_invalid",
          .target = target_,
      });
      return;
    }
    socks_connect_tail_.assign(rest_len, 0);
    auto self = shared_from_this();
    asio::async_read(stream_.next_layer(), asio::buffer(socks_connect_tail_),
                     [self](const boost::system::error_code &ec2, std::size_t) {
                       self->on_socks_connect_tail_read(ec2);
                     });
  }

  void on_socks_connect_tail_read(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_connect_tail_read: " + ec.message(),
          .target = target_,
      });
      return;
    }
    if (socks_connect_head_[3] == 0x03) {
      const size_t host_len = static_cast<size_t>(socks_connect_tail_[0]);
      socks_connect_domain_tail_.assign(host_len + 2, 0);
      auto self = shared_from_this();
      asio::async_read(stream_.next_layer(), asio::buffer(socks_connect_domain_tail_),
                       [self](const boost::system::error_code &ec2, std::size_t) {
                         self->on_socks_connect_domain_tail_read(ec2);
                       });
      return;
    }
    auto self = shared_from_this();
    stream_.async_handshake(asio::ssl::stream_base::client,
                            [self](const boost::system::error_code &ec2) { self->on_handshake(ec2); });
  }

  void on_socks_connect_domain_tail_read(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "socks_connect_domain_tail_read: " + ec.message(),
          .target = target_,
      });
      return;
    }
    auto self = shared_from_this();
    stream_.async_handshake(asio::ssl::stream_base::client,
                            [self](const boost::system::error_code &ec2) { self->on_handshake(ec2); });
  }

  void on_proxy_write(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "proxy_connect_write: " + ec.message(),
          .target = target_,
      });
      return;
    }
    auto self = shared_from_this();
    http::async_read(stream_.next_layer(), connect_buffer_, connect_res_,
                     [self](const boost::system::error_code &ec2, std::size_t) {
                       self->on_proxy_read(ec2);
                     });
  }

  void on_proxy_read(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "proxy_connect_read: " + ec.message(),
          .target = target_,
      });
      return;
    }
    if (connect_res_.result_int() != 200) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = connect_res_.result_int(),
          .body = std::move(connect_res_.body()),
          .error = "proxy_connect_non_200",
          .target = target_,
      });
      return;
    }
    auto self = shared_from_this();
    stream_.async_handshake(asio::ssl::stream_base::client,
                            [self](const boost::system::error_code &ec2) { self->on_handshake(ec2); });
  }

  void on_handshake(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "handshake: " + ec.message(),
          .target = target_,
      });
      return;
    }
    auto self = shared_from_this();
    http::async_write(stream_, req_, [self](const boost::system::error_code &ec2, std::size_t) {
      self->on_write(ec2);
    });
  }

  void on_write(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "write: " + ec.message(),
          .target = target_,
      });
      return;
    }
    auto self = shared_from_this();
    http::async_read(stream_, buffer_, res_, [self](const boost::system::error_code &ec2, std::size_t) {
      self->on_read(ec2);
    });
  }

  void on_read(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{
          .ok = false,
          .status_code = 0,
          .body = "",
          .error = "read: " + ec.message(),
          .target = target_,
      });
      return;
    }
    finish(HttpGetOutcome{
        .ok = true,
        .status_code = res_.result_int(),
        .body = std::move(res_.body()),
        .error = "",
        .target = target_,
    });
  }

  tcp::resolver resolver_;
  beast::ssl_stream<beast::tcp_stream> stream_;
  beast::flat_buffer buffer_;
  beast::flat_buffer connect_buffer_;
  http::request<http::empty_body> req_;
  http::request<http::empty_body> connect_req_;
  http::response<http::string_body> res_;
  http::response<http::string_body> connect_res_;
  asio::steady_timer timer_;
  std::string host_;
  std::string port_;
  std::string target_;
  std::string proxy_host_;
  std::string proxy_port_;
  bool use_proxy_ = false;
  enum class ProxyScheme {
    kHttpConnect,
    kSocks5,
  };
  ProxyScheme proxy_scheme_ = ProxyScheme::kHttpConnect;
  uint8_t socks_method_resp_[2] = {0, 0};
  uint8_t socks_connect_head_[4] = {0, 0, 0, 0};
  std::vector<uint8_t> socks_connect_req_;
  std::vector<uint8_t> socks_connect_tail_;
  std::vector<uint8_t> socks_connect_domain_tail_;
  std::chrono::milliseconds timeout_;
  DoneFn done_;
  bool finished_ = false;
};

void async_https_get(asio::io_context &ioc, asio::ssl::context &ssl_ctx, const std::string &host,
                     const std::string &port, const std::string &target, const std::string &proxy_url,
                     std::chrono::milliseconds timeout,
                     std::function<void(HttpGetOutcome)> done) {
  auto op = std::make_shared<AsyncHttpsGetOp>(ioc, ssl_ctx, host, port, target, proxy_url, timeout, std::move(done));
  op->start();
}

FetchSeedOutcome parse_seed_outcome(const std::string &condition_hex_lower, HttpGetOutcome out) {
  assert(condition_hex_lower.starts_with("0x"));
  const std::string condition_norm = normalize_hex_id_no_prefix(condition_hex_lower);
  if (!out.ok) {
    return FetchSeedOutcome{
        .state = FetchSeedState::kFailed,
        .market = json::object(),
        .detail = "http_error err=" + compact_for_log(out.error) +
                  " target=" + out.target +
                  " status=" + std::to_string(out.status_code) +
                  " body=" + compact_for_log(out.body),
    };
  }
  if (out.status_code != 200) {
    return FetchSeedOutcome{
        .state = FetchSeedState::kFailed,
        .market = json::object(),
        .detail = "http_non_200 target=" + out.target +
                  " status=" + std::to_string(out.status_code) +
                  " body=" + compact_for_log(out.body),
    };
  }
  json arr = json::parse(out.body, nullptr, false);
  if (arr.is_discarded()) {
    return FetchSeedOutcome{
        .state = FetchSeedState::kFailed,
        .market = json::object(),
        .detail = "json_parse_failed target=" + out.target +
                  " body=" + compact_for_log(out.body),
    };
  }
  if (!arr.is_array()) {
    return FetchSeedOutcome{
        .state = FetchSeedState::kFailed,
        .market = json::object(),
        .detail = "json_not_array target=" + out.target +
                  " body=" + compact_for_log(out.body),
    };
  }
  for (const auto &item : arr) {
    assert(item.is_object());
    std::string cid_raw;
    if (item.contains("conditionId") && item.at("conditionId").is_string()) {
      cid_raw = item.at("conditionId").get<std::string>();
    } else if (item.contains("condition_id") && item.at("condition_id").is_string()) {
      cid_raw = item.at("condition_id").get<std::string>();
    } else {
      continue;
    }
    if (normalize_hex_id_no_prefix(cid_raw) == condition_norm) {
      return FetchSeedOutcome{
          .state = FetchSeedState::kFound,
          .market = item,
          .detail = "ok",
      };
    }
  }
  return FetchSeedOutcome{
      .state = FetchSeedState::kEmpty,
      .market = json::object(),
      .detail = "no_matching_condition target=" + out.target +
                " markets=" + std::to_string(arr.size()) +
                " body=" + compact_for_log(out.body),
  };
}

class AsyncSeedFetchOp final : public std::enable_shared_from_this<AsyncSeedFetchOp> {
public:
  using DoneFn = std::function<void(FetchSeedOutcome)>;
  using RetryFn = std::function<void(int, const std::string &)>;

  AsyncSeedFetchOp(asio::io_context &ioc, asio::ssl::context &ssl_ctx, RpcEndpoint endpoint,
                   std::string proxy_url, std::string condition_hex_lower, DoneFn done, RetryFn on_retry)
      : ioc_(ioc), ssl_ctx_(ssl_ctx), retry_timer_(ioc), endpoint_(std::move(endpoint)),
        proxy_url_(std::move(proxy_url)), condition_hex_lower_(std::move(condition_hex_lower)),
        done_(std::move(done)), on_retry_(std::move(on_retry)) {}

  void start() {
    attempt_once();
  }

private:
  void attempt_once() {
    attempts_ += 1;
    std::string target = build_gamma_target(endpoint_.target, condition_hex_lower_);
    auto self = shared_from_this();
    async_https_get(ioc_, ssl_ctx_, endpoint_.host, endpoint_.port, target, proxy_url_,
                    std::chrono::milliseconds(kFetchTimeoutMs),
                    [self](HttpGetOutcome out) { self->on_http(std::move(out)); });
  }

  void on_http(HttpGetOutcome out) {
    FetchSeedOutcome parsed = parse_seed_outcome(condition_hex_lower_, std::move(out));
    if (parsed.state == FetchSeedState::kFailed) {
      if (on_retry_) {
        on_retry_(attempts_, parsed.detail);
      }
      if (kFetchRetryDelayMs <= 0) {
        attempt_once();
        return;
      }
      auto self = shared_from_this();
      retry_timer_.expires_after(std::chrono::milliseconds(kFetchRetryDelayMs));
      retry_timer_.async_wait([self](const boost::system::error_code &ec) {
        if (ec || self->finished_) {
          return;
        }
        self->attempt_once();
      });
      return;
    }
    finish(std::move(parsed));
  }

  void finish(FetchSeedOutcome out) {
    if (finished_) {
      return;
    }
    finished_ = true;
    retry_timer_.cancel();
    done_(std::move(out));
  }

  asio::io_context &ioc_;
  asio::ssl::context &ssl_ctx_;
  asio::steady_timer retry_timer_;
  RpcEndpoint endpoint_;
  std::string proxy_url_;
  std::string condition_hex_lower_;
  DoneFn done_;
  RetryFn on_retry_;
  int attempts_ = 0;
  bool finished_ = false;
};

} // namespace

void async_seed_fetch(asio::io_context &ioc, asio::ssl::context &ssl_ctx, const RpcEndpoint &endpoint,
                      const std::string &proxy_url, const std::string &condition_hex_lower,
                      std::function<void(FetchSeedOutcome)> done,
                      std::function<void(int, const std::string &)> on_retry) {
  std::make_shared<AsyncSeedFetchOp>(ioc, ssl_ctx, endpoint, proxy_url, condition_hex_lower, std::move(done),
                                     std::move(on_retry))
      ->start();
}

} // namespace stage0
