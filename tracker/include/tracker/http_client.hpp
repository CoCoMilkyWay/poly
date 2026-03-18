#pragma once

#include "tracker/json.hpp"

#include <string>
#include <vector>

namespace tracker {

struct HttpResponse {
  int status = 0;
  std::string body;
};

struct HttpRequest {
  std::string url;
  std::string method;
  std::string body;
};

// 单次请求 (同步接口，内部异步实现，无限重试)
HttpResponse http_get(const std::string &url);
HttpResponse http_post_json(const std::string &url, const json &payload);

// 批量并发请求 (单线程异步并发，无限重试)
std::vector<HttpResponse> http_batch(const std::vector<HttpRequest> &requests, size_t concurrency);

} // namespace tracker
