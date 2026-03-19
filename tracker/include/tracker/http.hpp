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
// HTTP Client
// ============================================================================

HttpRes http_get(const std::string &url);
HttpRes http_post(const std::string &url, const json &payload);
std::vector<HttpRes> http_batch(const std::vector<HttpReq> &reqs, size_t concurrency);

} // namespace tracker
