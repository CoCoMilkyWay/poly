#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

class RpcClient {
public:
  RpcClient(const std::string &url, const std::string &api_key = "")
      : api_key_(api_key) {
    parse_url(url);
  }

  int64_t eth_blockNumber() {
    json request = {
        {"jsonrpc", "2.0"},
        {"id", ++request_id_},
        {"method", "eth_blockNumber"},
        {"params", json::array()}};

    std::string response_body = http_post(request.dump());
    json response = json::parse(response_body);
    if (response.contains("error")) {
      throw std::runtime_error("RPC error: " + response["error"].dump());
    }
    return from_hex(response["result"].get<std::string>());
  }

  std::vector<json> eth_getLogs_batch(
      const std::vector<std::tuple<std::string, int64_t, int64_t, std::vector<std::string>>> &queries) {
    json batch = json::array();

    for (const auto &[address, from_block, to_block, topic0_list] : queries) {
      json filter = {
          {"address", address},
          {"fromBlock", to_hex(from_block)},
          {"toBlock", to_hex(to_block)}};

      if (!topic0_list.empty()) {
        filter["topics"] = json::array({topic0_list});
      }

      batch.push_back({{"jsonrpc", "2.0"},
                       {"id", batch.size()},
                       {"method", "eth_getLogs"},
                       {"params", json::array({filter})}});
    }

    std::string response_body = http_post(batch.dump());
    json responses = json::parse(response_body);

    std::vector<json> results(queries.size());
    for (const auto &resp : responses) {
      if (resp.contains("error")) {
        throw std::runtime_error("RPC error: " + resp["error"].dump());
      }
      size_t id = resp["id"].get<size_t>();
      results[id] = resp["result"];
    }
    return results;
  }

private:
  void parse_url(const std::string &url) {
    std::string u = url;

    if (u.starts_with("https://")) {
      use_ssl_ = true;
      u = u.substr(8);
    } else if (u.starts_with("http://")) {
      use_ssl_ = false;
      u = u.substr(7);
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

  std::string http_post(const std::string &body) {
    asio::io_context ioc;
    tcp::resolver resolver(ioc);
    beast::tcp_stream stream(ioc);

    auto const results = resolver.resolve(host_, port_);
    stream.connect(results);

    http::request<http::string_body> req{http::verb::post, target_, 11};
    req.set(http::field::host, host_);
    req.set(http::field::content_type, "application/json");
    req.set(http::field::user_agent, "PolySync/1.0");

    if (!api_key_.empty()) {
      req.set(http::field::authorization, "Bearer " + api_key_);
    }

    req.body() = body;
    req.prepare_payload();

    http::write(stream, req);

    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(256 * 1024 * 1024);  // 256MB
    http::read(stream, buffer, parser);

    beast::error_code ec;
    [[maybe_unused]] auto ret = stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    return parser.get().body();
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
  bool use_ssl_ = false;
  int request_id_ = 0;
};
