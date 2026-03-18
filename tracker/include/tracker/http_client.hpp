#pragma once

#include "tracker/json.hpp"

#include <memory>
#include <string>

namespace tracker {

struct HttpResponse {
  int status = 0;
  std::string body;
};

HttpResponse http_get(const std::string &url);
HttpResponse http_post_json(const std::string &url, const json &payload);

class SyncWebSocketClient {
public:
  explicit SyncWebSocketClient(const std::string &url);
  ~SyncWebSocketClient();

  void connect();
  void send_json(const json &payload);
  json recv_json();
  void close();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace tracker
