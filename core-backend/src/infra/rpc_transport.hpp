#pragma once

#include <cassert>
#include <string>
#include <utility>

struct RpcEndpoint {
  std::string host;
  std::string port;
  std::string target;
  bool use_ssl = false;
};

class RpcTransport {
public:
  virtual ~RpcTransport() = default;
  virtual std::string post_json(const std::string &body) = 0;
  virtual size_t last_response_size() const = 0;
  virtual void cancel() = 0;
};

inline RpcEndpoint parse_rpc_endpoint(const std::string &url) {
  std::string u = url;
  RpcEndpoint ep;
  if (u.starts_with("https://")) {
    ep.use_ssl = true;
    u = u.substr(8);
  } else if (u.starts_with("http://")) {
    ep.use_ssl = false;
    u = u.substr(7);
  } else {
    assert(false && "RPC URL 必须以 http:// 或 https:// 开头");
  }

  auto slash_pos = u.find('/');
  if (slash_pos != std::string::npos) {
    ep.target = u.substr(slash_pos);
    u = u.substr(0, slash_pos);
  } else {
    ep.target = "/";
  }

  auto colon_pos = u.find(':');
  if (colon_pos != std::string::npos) {
    ep.host = u.substr(0, colon_pos);
    ep.port = u.substr(colon_pos + 1);
  } else {
    ep.host = u;
    ep.port = ep.use_ssl ? "443" : "80";
  }
  return ep;
}

inline std::pair<std::string, std::string> parse_proxy_host_port(const std::string &proxy_url) {
  if (proxy_url.empty()) {
    return {"", ""};
  }
  std::string u = proxy_url;
  if (u.starts_with("socks5://")) {
    u = u.substr(9);
  } else if (u.starts_with("http://")) {
    u = u.substr(7);
  }
  auto colon_pos = u.rfind(':');
  if (colon_pos != std::string::npos) {
    return {u.substr(0, colon_pos), u.substr(colon_pos + 1)};
  }
  return {u, "8080"};
}
