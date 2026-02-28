#pragma once

#include <atomic>
#include <string>

#include <curl/curl.h>

#include "rpc_transport.hpp"

class RpcTransportCurl final : public RpcTransport {
public:
  RpcTransportCurl(const std::string &url, const std::string &proxy_url);
  ~RpcTransportCurl() override;

  std::string post_json(const std::string &body) override;
  size_t last_response_size() const override;
  void cancel() override;

private:
  static size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
  static int progress_callback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t);

  std::string url_;
  std::string proxy_url_;
  size_t last_response_size_ = 0;
  CURL *easy_ = nullptr;
  curl_slist *headers_ = nullptr;
  std::atomic<bool> cancel_requested_{false};
};
