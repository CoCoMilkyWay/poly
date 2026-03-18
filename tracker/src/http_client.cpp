#include "tracker/http_client.hpp"
#include "tracker/common.hpp"
#include "tracker/config.hpp"

#include <iostream>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <openssl/ssl.h>

#include <array>
#include <cctype>
#include <istream>
#include <sstream>

namespace tracker {
namespace {

namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

std::string trim_ascii(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  return value;
}

std::string build_http_request(const UrlParts &parts,
                               std::string_view method,
                               std::string_view body) {
  std::string request;
  request += std::string(method);
  request += " ";
  request += parts.target;
  request += " HTTP/1.1\r\n";
  request += "Host: " + parts.host + "\r\n";
  request += "User-Agent: tracker\r\n";
  request += "Accept: application/json\r\n";
  request += "Connection: close\r\n";
  if (!body.empty()) {
    request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "\r\n";
  request += std::string(body);
  return request;
}

template <typename TStream>
std::string read_chunked_body(TStream &stream, std::string &pending) {
  std::string body;
  auto ensure_pending = [&](size_t need) {
    while (pending.size() < need) {
      std::array<char, 4096> tmp{};
      const size_t n = stream.read_some(asio::buffer(tmp));
      pending.append(tmp.data(), n);
    }
  };
  auto read_line = [&]() {
    while (true) {
      const size_t pos = pending.find("\r\n");
      if (pos != std::string::npos) {
        std::string line = pending.substr(0, pos);
        pending.erase(0, pos + 2);
        return line;
      }
      std::array<char, 4096> tmp{};
      const size_t n = stream.read_some(asio::buffer(tmp));
      pending.append(tmp.data(), n);
    }
  };
  while (true) {
    const std::string chunk_size_line = read_line();
    const size_t chunk_size = std::stoull(chunk_size_line, nullptr, 16);
    if (chunk_size == 0) {
      read_line(); // trailing empty line
      break;
    }
    ensure_pending(chunk_size + 2);
    body.append(pending.data(), chunk_size);
    pending.erase(0, chunk_size + 2);
  }
  return body;
}

template <typename TStream>
HttpResponse read_http_response(TStream &stream) {
  asio::streambuf buffer;
  asio::read_until(stream, buffer, "\r\n\r\n");

  std::istream in(&buffer);
  std::string status_line;
  std::getline(in, status_line);
  if (!status_line.empty() && status_line.back() == '\r') {
    status_line.pop_back();
  }
  std::istringstream status_stream(status_line);
  std::string http_version;
  int status_code = 0;
  status_stream >> http_version >> status_code;
  assert(http_version.starts_with("HTTP/"));
  assert(status_code > 0);

  size_t content_length = 0;
  bool has_content_length = false;
  bool is_chunked = false;
  while (true) {
    std::string header_line;
    std::getline(in, header_line);
    if (!header_line.empty() && header_line.back() == '\r') {
      header_line.pop_back();
    }
    if (header_line.empty()) {
      break;
    }
    const size_t colon = header_line.find(':');
    assert(colon != std::string::npos);
    const std::string key = to_lower_ascii(header_line.substr(0, colon));
    const std::string value = trim_ascii(header_line.substr(colon + 1));
    if (key == "content-length") {
      content_length = static_cast<size_t>(std::stoull(value));
      has_content_length = true;
    } else if (key == "transfer-encoding" && value == "chunked") {
      is_chunked = true;
    }
  }

  std::string body;
  if (is_chunked) {
    std::string pending;
    pending.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    body = read_chunked_body(stream, pending);
  } else {
    body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (has_content_length) {
      while (body.size() < content_length) {
        std::array<char, 4096> chunk{};
        const size_t need = std::min(chunk.size(), content_length - body.size());
        const size_t n = asio::read(stream, asio::buffer(chunk.data(), need));
        body.append(chunk.data(), n);
      }
      if (body.size() > content_length) {
        body.resize(content_length);
      }
    }
  }

  return HttpResponse{
      .status = status_code,
      .body = std::move(body),
  };
}

HttpResponse http_request(const std::string &url, std::string_view method, const std::string &body) {
  const UrlParts parts = parse_url(url);
  std::cerr << "[DEBUG] HTTP " << method << " scheme=" << parts.scheme << " host=" << parts.host << " port=" << parts.port << " target=" << parts.target << " secure=" << parts.secure() << std::endl;
  asio::io_context ioc;
  tcp::resolver resolver(ioc);
  const auto endpoints = resolver.resolve(parts.host, parts.port);
  const std::string request = build_http_request(parts, method, body);

  if (parts.secure()) {
    std::cerr << "[DEBUG] Creating SSL context (sslv23_client)..." << std::endl;
    ssl::context ssl_ctx(ssl::context::sslv23_client);
    ssl_ctx.set_options(ssl::context::default_workarounds | ssl::context::no_sslv2 | ssl::context::no_sslv3);
    ssl_ctx.set_default_verify_paths();
    ssl_ctx.set_verify_mode(ssl::verify_peer);
    ssl::stream<tcp::socket> stream(ioc, ssl_ctx);
    std::cerr << "[DEBUG] Setting SNI: " << parts.host << std::endl;
    const int sni_result = SSL_set_tlsext_host_name(stream.native_handle(), parts.host.c_str());
    std::cerr << "[DEBUG] SNI result: " << sni_result << std::endl;
    assert(sni_result == 1);
    std::cerr << "[DEBUG] Connecting..." << std::endl;
    asio::connect(stream.next_layer(), endpoints);
    std::cerr << "[DEBUG] Handshaking..." << std::endl;
    stream.handshake(ssl::stream_base::client);
    std::cerr << "[DEBUG] Handshake SUCCESS, version=" << SSL_get_version(stream.native_handle()) << std::endl;
    asio::write(stream, asio::buffer(request));
    HttpResponse response = read_http_response(stream);
    boost::system::error_code ec;
    const boost::system::error_code ignored = stream.shutdown(ec);
    (void)ignored;
    return response;
  }

  tcp::socket socket(ioc);
  asio::connect(socket, endpoints);
  asio::write(socket, asio::buffer(request));
  HttpResponse response = read_http_response(socket);
  boost::system::error_code ec;
  const boost::system::error_code ignored = socket.shutdown(tcp::socket::shutdown_both, ec);
  (void)ignored;
  return response;
}

struct IWsConnection {
  virtual ~IWsConnection() = default;
  virtual void connect() = 0;
  virtual void write(const std::string &payload) = 0;
  virtual std::string read() = 0;
  virtual void close() = 0;
};

struct PlainWsConnection final : IWsConnection {
  explicit PlainWsConnection(UrlParts parts)
      : parts_(std::move(parts)) {}

  void connect() override {
    assert(false);
  }

  void write(const std::string &payload) override {
    (void)payload;
    assert(false);
  }

  std::string read() override {
    assert(false);
    return "";
  }

  void close() override {
    assert(false);
  }

  UrlParts parts_;
};

struct TlsWsConnection final : IWsConnection {
  explicit TlsWsConnection(UrlParts parts)
      : parts_(std::move(parts)) {}

  void connect() override {
    assert(false);
  }

  void write(const std::string &payload) override {
    (void)payload;
    assert(false);
  }

  std::string read() override {
    assert(false);
    return "";
  }

  void close() override {
    assert(false);
  }

  UrlParts parts_;
};

} // namespace

HttpResponse http_get(const std::string &url) {
  return http_request(url, "GET", "");
}

HttpResponse http_post_json(const std::string &url, const json &payload) {
  return http_request(url, "POST", payload.dump());
}

struct SyncWebSocketClient::Impl {
  explicit Impl(const std::string &input_url)
      : url(input_url), parts(parse_url(input_url)) {
    assert(parts.websocket());
    if (parts.secure()) {
      connection = std::make_unique<TlsWsConnection>(parts);
    } else {
      connection = std::make_unique<PlainWsConnection>(parts);
    }
  }

  std::string url;
  UrlParts parts;
  std::unique_ptr<IWsConnection> connection;
};

SyncWebSocketClient::SyncWebSocketClient(const std::string &url)
    : impl_(std::make_unique<Impl>(url)) {}

SyncWebSocketClient::~SyncWebSocketClient() = default;

void SyncWebSocketClient::connect() {
  impl_->connection->connect();
}

void SyncWebSocketClient::send_json(const json &payload) {
  impl_->connection->write(payload.dump());
}

json SyncWebSocketClient::recv_json() {
  return json::parse(impl_->connection->read());
}

void SyncWebSocketClient::close() {
  impl_->connection->close();
}

} // namespace tracker
