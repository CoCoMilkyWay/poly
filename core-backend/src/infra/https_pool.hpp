#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define HTTPS_POOL_SIZE 16 // 连接池大小(>= PARALLEL_TOTAL)

#include "https_session.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace asio = boost::asio;
namespace ssl = asio::ssl;

// ============================================================================
// HttpsPool - HTTPS 连接池(连接复用 + 重试)
// ============================================================================
class HttpsPool {
public:
  using Callback = std::function<void(std::string)>;

  HttpsPool(asio::io_context &ioc, const std::string &api_key)
      : ioc_(ioc), ssl_ctx_(ssl::context::tlsv12_client), api_key_(api_key) {
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
  }

  void async_post(const std::string &target, const std::string &body, Callback cb) {
    do_request(target, body, std::move(cb));
  }

  void return_session(std::shared_ptr<HttpsSession> session) {
    --active_count_;
    if (session && session->is_connected()) {
      idle_sessions_.push(session);
    }
    process_pending();
  }

  void on_request_failed(Callback cb) {
    --active_count_;
    cb("");
    process_pending();
  }

  int active_count() const { return active_count_; }

  // 延迟执行回调(用于重试)
  template <typename Func>
  void schedule_retry(Func &&func, int delay_ms) {
    auto timer = std::make_shared<asio::steady_timer>(ioc_);
    timer->expires_after(std::chrono::milliseconds(delay_ms));
    timer->async_wait([func = std::forward<Func>(func), timer](boost::system::error_code) {
      func();
    });
  }

private:
  struct PendingRequest {
    std::string target;
    std::string body;
    Callback cb;
  };

  void do_request(const std::string &target, const std::string &body,
                  Callback cb) {
    if (active_count_ < HTTPS_POOL_SIZE) {
      start_request(target, body, std::move(cb));
    } else {
      pending_.push({target, body, std::move(cb)});
    }
  }

  void start_request(const std::string &target, const std::string &body,
                     Callback cb) {
    ++active_count_;

    std::shared_ptr<HttpsSession> session;
    if (!idle_sessions_.empty()) {
      session = idle_sessions_.front();
      idle_sessions_.pop();
    } else {
      session = std::make_shared<HttpsSession>(ioc_, ssl_ctx_, api_key_, this);
    }

    session->run(target, body,
                 [this, cb = std::move(cb)](std::string response, bool success) mutable {
                   if (success) {
                     cb(std::move(response));
                   } else {
                     on_request_failed(std::move(cb));
                   }
                 });
  }

  void process_pending() {
    while (!pending_.empty() && active_count_ < HTTPS_POOL_SIZE) {
      auto req = std::move(pending_.front());
      pending_.pop();
      start_request(req.target, req.body, std::move(req.cb));
    }
  }

  // 配置
  asio::io_context &ioc_;
  ssl::context ssl_ctx_;
  std::string api_key_;

  // 状态
  int active_count_ = 0;
  std::queue<PendingRequest> pending_;
  std::queue<std::shared_ptr<HttpsSession>> idle_sessions_;
};

// ============================================================================
// HttpsSession 成员函数实现(需要 HttpsPool 完整定义)
// ============================================================================

inline void HttpsSession::fail(const char *what) {
  std::cerr << "[HTTPS] " << what << " failed" << std::endl;
  connected_ = false;
  auto cb = std::move(cb_);
  cb("", false);
}

inline void HttpsSession::return_to_pool() {
  pool_->return_session(shared_from_this());
}
