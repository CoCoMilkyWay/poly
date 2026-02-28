#include "rpc_transport_beast.hpp"

#include <stdexcept>
#include <vector>

namespace http = beast::http;

RpcTransportBeast::RpcTransportBeast(const std::string &url, const std::string &proxy_url)
    : ssl_ctx_(asio::ssl::context::tls_client) {
  RpcEndpoint ep = parse_rpc_endpoint(url);
  host_ = std::move(ep.host);
  port_ = std::move(ep.port);
  target_ = std::move(ep.target);
  use_ssl_ = ep.use_ssl;

  auto [proxy_host, proxy_port] = parse_proxy_host_port(proxy_url);
  proxy_host_ = std::move(proxy_host);
  proxy_port_ = std::move(proxy_port);

  ssl_ctx_.set_verify_mode(asio::ssl::verify_none);
}

RpcTransportBeast::~RpcTransportBeast() {
  disconnect();
}

size_t RpcTransportBeast::last_response_size() const {
  return last_response_size_;
}

void RpcTransportBeast::cancel() {
  cancel_requested_.store(true);
  std::lock_guard<std::mutex> lock(conn_mutex_);
  beast::error_code ec;
  if (use_ssl_ && ssl_stream_) {
    auto &sock = beast::get_lowest_layer(*ssl_stream_).socket();
    [[maybe_unused]] auto _ = sock.cancel(ec);
    [[maybe_unused]] auto __ = sock.shutdown(tcp::socket::shutdown_both, ec);
    [[maybe_unused]] auto ___ = sock.close(ec);
  } else if (tcp_stream_) {
    auto &sock = tcp_stream_->socket();
    [[maybe_unused]] auto _ = sock.cancel(ec);
    [[maybe_unused]] auto __ = sock.shutdown(tcp::socket::shutdown_both, ec);
    [[maybe_unused]] auto ___ = sock.close(ec);
  }
}

void RpcTransportBeast::socks5_handshake(asio::ip::tcp::socket &sock, const std::string &dst_host, int dst_port) {
  uint8_t greeting[3] = {0x05, 0x01, 0x00};
  asio::write(sock, asio::buffer(greeting, 3));

  uint8_t method_resp[2];
  asio::read(sock, asio::buffer(method_resp, 2));
  assert(method_resp[0] == 0x05 && method_resp[1] == 0x00 && "SOCKS5 不支持无认证");

  std::vector<uint8_t> req;
  req.push_back(0x05);
  req.push_back(0x01);
  req.push_back(0x00);
  req.push_back(0x03);
  req.push_back(static_cast<uint8_t>(dst_host.size()));
  req.insert(req.end(), dst_host.begin(), dst_host.end());
  req.push_back(static_cast<uint8_t>((dst_port >> 8) & 0xff));
  req.push_back(static_cast<uint8_t>(dst_port & 0xff));
  asio::write(sock, asio::buffer(req));

  uint8_t resp[4];
  asio::read(sock, asio::buffer(resp, 4));
  assert(resp[0] == 0x05 && resp[1] == 0x00 && "SOCKS5 CONNECT 失败");

  if (resp[3] == 0x01) {
    uint8_t buf[6];
    asio::read(sock, asio::buffer(buf, 6));
  } else if (resp[3] == 0x03) {
    uint8_t len;
    asio::read(sock, asio::buffer(&len, 1));
    std::vector<uint8_t> buf(len + 2);
    asio::read(sock, asio::buffer(buf));
  } else if (resp[3] == 0x04) {
    uint8_t buf[18];
    asio::read(sock, asio::buffer(buf, 18));
  }
}

void RpcTransportBeast::ensure_connected() {
  std::lock_guard<std::mutex> lock(conn_mutex_);
  if (connected_) {
    return;
  }
  if (cancel_requested_.load()) {
    throw std::runtime_error("RPC cancelled");
  }

  constexpr auto kConnectTimeout = std::chrono::seconds(30);
  tcp::resolver resolver(ioc_);

  if (use_ssl_) {
    ssl_stream_ = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc_, ssl_ctx_);
    SSL_set_tlsext_host_name(ssl_stream_->native_handle(), host_.c_str());

    if (!proxy_host_.empty()) {
      auto proxy_endpoints = resolver.resolve(proxy_host_, proxy_port_);
      beast::get_lowest_layer(*ssl_stream_).expires_after(kConnectTimeout);
      beast::get_lowest_layer(*ssl_stream_).connect(proxy_endpoints);
      socks5_handshake(beast::get_lowest_layer(*ssl_stream_).socket(), host_, std::stoi(port_));
    } else {
      auto const endpoints = resolver.resolve(host_, port_);
      beast::get_lowest_layer(*ssl_stream_).expires_after(kConnectTimeout);
      beast::get_lowest_layer(*ssl_stream_).connect(endpoints);
    }

    ssl_stream_->handshake(asio::ssl::stream_base::client);
  } else {
    tcp_stream_ = std::make_unique<beast::tcp_stream>(ioc_);
    auto const endpoints = resolver.resolve(host_, port_);
    tcp_stream_->expires_after(kConnectTimeout);
    tcp_stream_->connect(endpoints);
  }
  connected_ = true;
}

void RpcTransportBeast::disconnect() {
  std::lock_guard<std::mutex> lock(conn_mutex_);
  if (!connected_) {
    return;
  }

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

std::string RpcTransportBeast::post_json(const std::string &body) {
  http::request<http::string_body> req{http::verb::post, target_, 11};
  req.set(http::field::host, host_);
  req.set(http::field::content_type, "application/json");
  req.set(http::field::user_agent, "PolySync/1.0");
  req.set(http::field::connection, "keep-alive");
  req.body() = body;
  req.prepare_payload();

  constexpr auto kTimeout = std::chrono::seconds(120);
  for (int retry = 0; retry < 2; ++retry) {
    try {
      if (cancel_requested_.load()) {
        throw std::runtime_error("RPC cancelled");
      }
      ensure_connected();

      beast::flat_buffer buffer;
      http::response_parser<http::string_body> parser;
      parser.body_limit(1024 * 1024 * 1024);

      if (use_ssl_) {
        beast::get_lowest_layer(*ssl_stream_).expires_after(kTimeout);
        http::write(*ssl_stream_, req);
      } else {
        tcp_stream_->expires_after(kTimeout);
        http::write(*tcp_stream_, req);
      }

      if (use_ssl_) {
        beast::get_lowest_layer(*ssl_stream_).expires_after(kTimeout);
        http::read(*ssl_stream_, buffer, parser);
      } else {
        tcp_stream_->expires_after(kTimeout);
        http::read(*tcp_stream_, buffer, parser);
      }

      std::string result = parser.get().body();
      last_response_size_ = result.size();
      return result;
    } catch (...) {
      disconnect();
      if (cancel_requested_.load()) {
        throw;
      }
      if (retry == 1) {
        throw;
      }
    }
  }
  throw std::runtime_error("beast post_json failed after retries");
}
