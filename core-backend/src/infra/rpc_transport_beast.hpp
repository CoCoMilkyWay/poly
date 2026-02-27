#pragma once

#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include "rpc_transport.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

class RpcTransportBeast final : public RpcTransport {
public:
  RpcTransportBeast(const std::string &url, const std::string &proxy_url);
  ~RpcTransportBeast() override;

  std::string post_json(const std::string &body) override;
  size_t last_response_size() const override;

private:
  static void socks5_handshake(asio::ip::tcp::socket &sock, const std::string &dst_host, int dst_port);
  void ensure_connected();
  void disconnect();

  std::string host_;
  std::string port_;
  std::string target_;
  std::string proxy_host_;
  std::string proxy_port_;
  bool use_ssl_ = false;
  size_t last_response_size_ = 0;

  asio::io_context ioc_;
  asio::ssl::context ssl_ctx_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> ssl_stream_;
  std::unique_ptr<beast::tcp_stream> tcp_stream_;
  bool connected_ = false;
};
