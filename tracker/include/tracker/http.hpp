#pragma once

#include "tracker/json.hpp"

#include <string>
#include <vector>

namespace tracker {

// ============================================================================
// HTTP Request / Response
// ============================================================================

struct HttpReq {
  std::string url;
  std::string method;  // "GET" or "POST"
  std::string body;
};

struct HttpRes {
  int status = 0;
  std::string body;
};

// ============================================================================
// HTTP Client (proxy_url empty = no proxy)
// ============================================================================

HttpRes http_get(const std::string &url, const std::string &proxy_url = "");
HttpRes http_post(const std::string &url, const json &payload, const std::string &proxy_url = "");
std::vector<HttpRes> http_batch(const std::vector<HttpReq> &reqs, size_t concurrency, const std::string &proxy_url = "");

} // namespace tracker
