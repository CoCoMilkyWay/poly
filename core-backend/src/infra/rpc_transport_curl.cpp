#include "rpc_transport_curl.hpp"

#include <mutex>
#include <stdexcept>

RpcTransportCurl::RpcTransportCurl(const std::string &url, const std::string &proxy_url)
    : url_(url), proxy_url_(proxy_url) {
  static std::once_flag curl_init_once;
  std::call_once(curl_init_once, []() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
  });

  easy_ = curl_easy_init();
  if (!easy_) {
    throw std::runtime_error("curl_easy_init failed");
  }

  headers_ = curl_slist_append(headers_, "Content-Type: application/json");
  headers_ = curl_slist_append(headers_, "User-Agent: PolySync/1.0");
  headers_ = curl_slist_append(headers_, "Connection: close");
}

RpcTransportCurl::~RpcTransportCurl() {
  if (headers_) {
    curl_slist_free_all(headers_);
    headers_ = nullptr;
  }
  if (easy_) {
    curl_easy_cleanup(easy_);
    easy_ = nullptr;
  }
}

size_t RpcTransportCurl::last_response_size() const {
  return last_response_size_;
}

void RpcTransportCurl::cancel() {
  cancel_requested_.store(true);
}

size_t RpcTransportCurl::write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t bytes = size * nmemb;
  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, bytes);
  return bytes;
}

int RpcTransportCurl::progress_callback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto *self = static_cast<RpcTransportCurl *>(clientp);
  return self->cancel_requested_.load() ? 1 : 0;
}

std::string RpcTransportCurl::post_json(const std::string &body) {
  if (cancel_requested_.load()) {
    throw std::runtime_error("RPC cancelled");
  }
  std::string response;
  char errbuf[CURL_ERROR_SIZE] = {0};

  curl_easy_reset(easy_);
  curl_easy_setopt(easy_, CURLOPT_URL, url_.c_str());
  curl_easy_setopt(easy_, CURLOPT_POST, 1L);
  curl_easy_setopt(easy_, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(easy_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(easy_, CURLOPT_HTTPHEADER, headers_);
  curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, &RpcTransportCurl::write_callback);
  curl_easy_setopt(easy_, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(easy_, CURLOPT_TIMEOUT, 120L);
  curl_easy_setopt(easy_, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(easy_, CURLOPT_ERRORBUFFER, errbuf);
  curl_easy_setopt(easy_, CURLOPT_FRESH_CONNECT, 1L);
  curl_easy_setopt(easy_, CURLOPT_FORBID_REUSE, 1L);
  curl_easy_setopt(easy_, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(easy_, CURLOPT_XFERINFOFUNCTION, &RpcTransportCurl::progress_callback);
  curl_easy_setopt(easy_, CURLOPT_XFERINFODATA, this);

  if (!proxy_url_.empty()) {
    curl_easy_setopt(easy_, CURLOPT_PROXY, proxy_url_.c_str());
  }

  CURLcode rc = curl_easy_perform(easy_);
  if (rc != CURLE_OK) {
    if (cancel_requested_.load()) {
      throw std::runtime_error("RPC cancelled");
    }
    std::string msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    throw std::runtime_error("curl_easy_perform failed: " + msg);
  }

  long http_code = 0;
  curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code >= 400) {
    throw std::runtime_error("curl http status: " + std::to_string(http_code));
  }

  last_response_size_ = response.size();
  return response;
}
