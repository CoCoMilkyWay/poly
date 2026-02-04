#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define HTTPS_POOL_SIZE 16          // 连接池大小(>= PARALLEL_TOTAL)
#define HTTPS_TIMEOUT_SEC 30        // 请求超时
#define HTTPS_HOST "gateway.thegraph.com"
#define HTTPS_PORT "443"
#define HTTPS_MAX_RETRY 3           // 最大重试次数
#define HTTPS_RETRY_DELAY_MS 500    // 重试延迟基数

#include <string>
#include <queue>
#include <vector>
#include <memory>
#include <functional>
#include <cassert>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

class HttpsPool;

// ============================================================================
// HttpsSession - 可复用的 HTTPS 连接会话
// ============================================================================
class HttpsSession : public std::enable_shared_from_this<HttpsSession> {
public:
    using Callback = std::function<void(std::string, bool)>;  // (response, success)
    
    HttpsSession(asio::io_context& ioc, ssl::context& ssl_ctx, 
                 const std::string& api_key, HttpsPool* pool)
        : resolver_(ioc)
        , stream_(ioc, ssl_ctx)
        , api_key_(api_key)
        , pool_(pool) {}
    
    void run(const std::string& target, const std::string& body, Callback cb) {
        cb_ = std::move(cb);
        target_ = target;
        body_ = body;
        
        if (connected_) {
            do_write();
        } else {
            SSL_set_tlsext_host_name(stream_.native_handle(), HTTPS_HOST);
            resolver_.async_resolve(
                HTTPS_HOST, HTTPS_PORT,
                [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type results) {
                    if (ec) { self->fail("DNS resolve"); return; }
                    self->on_resolve(results);
                });
        }
    }
    
    bool is_connected() const { return connected_; }
    void mark_disconnected() { connected_ = false; }

private:
    void fail(const char* what);
    void return_to_pool();
    
    // ========================================================================
    // 连接建立流程
    // ========================================================================
    
    void on_resolve(tcp::resolver::results_type results) {
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        beast::get_lowest_layer(stream_).async_connect(
            results,
            [self = shared_from_this()](beast::error_code ec, tcp::endpoint) {
                if (ec) { self->fail("TCP connect"); return; }
                self->on_connect();
            });
    }
    
    void on_connect() {
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        stream_.async_handshake(
            ssl::stream_base::client,
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) { self->fail("SSL handshake"); return; }
                self->connected_ = true;
                self->do_write();
            });
    }
    
    // ========================================================================
    // 请求/响应流程
    // ========================================================================
    
    void do_write() {
        req_ = {};
        res_ = {};
        buffer_.clear();
        
        req_.method(http::verb::post);
        req_.target(target_);
        req_.version(11);
        req_.set(http::field::host, HTTPS_HOST);
        req_.set(http::field::content_type, "application/json");
        req_.set(http::field::authorization, "Bearer " + api_key_);
        req_.set(http::field::connection, "keep-alive");
        req_.body() = body_;
        req_.prepare_payload();
        
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        http::async_write(
            stream_, req_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) { self->connected_ = false; self->fail("HTTP write"); return; }
                self->on_write();
            });
    }
    
    void on_write() {
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        http::async_read(
            stream_, buffer_, res_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) { self->connected_ = false; self->fail("HTTP read"); return; }
                self->on_read();
            });
    }
    
    void on_read() {
        cb_(res_.body(), true);
        return_to_pool();
    }
    
    // 网络组件
    tcp::resolver resolver_;
    beast::ssl_stream<beast::tcp_stream> stream_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    http::response<http::string_body> res_;
    
    // 配置
    std::string api_key_;
    HttpsPool* pool_;
    
    // 请求状态
    std::string target_;
    std::string body_;
    Callback cb_;
    bool connected_ = false;
};

// ============================================================================
// HttpsPool - HTTPS 连接池(连接复用 + 重试)
// ============================================================================
class HttpsPool {
public:
    using Callback = std::function<void(std::string)>;
    
    HttpsPool(asio::io_context& ioc, const std::string& api_key)
        : ioc_(ioc)
        , ssl_ctx_(ssl::context::tlsv12_client)
        , api_key_(api_key) {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);
    }
    
    void async_post(const std::string& target, const std::string& body, Callback cb) {
        do_request(target, body, std::move(cb), 0);
    }
    
    void return_session(std::shared_ptr<HttpsSession> session) {
        --active_count_;
        if (session && session->is_connected()) {
            idle_sessions_.push(session);
        }
        process_pending();
    }
    
    void on_request_failed(const std::string& target, const std::string& body, 
                           Callback cb, int retry_count) {
        --active_count_;
        
        if (retry_count < HTTPS_MAX_RETRY) {
            int delay = HTTPS_RETRY_DELAY_MS * (1 << retry_count);  // 指数退避
            std::cerr << "[HTTPS] 重试 " << (retry_count + 1) << "/" << HTTPS_MAX_RETRY 
                      << "，延迟 " << delay << "ms" << std::endl;
            
            auto timer = std::make_shared<asio::steady_timer>(ioc_);
            timer->expires_after(std::chrono::milliseconds(delay));
            timer->async_wait([this, target, body, cb = std::move(cb), retry_count, timer]
                              (boost::system::error_code) mutable {
                do_request(target, body, std::move(cb), retry_count + 1);
            });
        } else {
            std::cerr << "[HTTPS] 重试耗尽" << std::endl;
            cb("");
            process_pending();
        }
    }
    
    int active_count() const { return active_count_; }

private:
    struct PendingRequest {
        std::string target;
        std::string body;
        Callback cb;
        int retry_count;
    };
    
    void do_request(const std::string& target, const std::string& body, 
                    Callback cb, int retry_count) {
        if (active_count_ < HTTPS_POOL_SIZE) {
            start_request(target, body, std::move(cb), retry_count);
        } else {
            pending_.push({target, body, std::move(cb), retry_count});
        }
    }
    
    void start_request(const std::string& target, const std::string& body, 
                       Callback cb, int retry_count) {
        ++active_count_;
        
        std::shared_ptr<HttpsSession> session;
        if (!idle_sessions_.empty()) {
            session = idle_sessions_.front();
            idle_sessions_.pop();
        } else {
            session = std::make_shared<HttpsSession>(ioc_, ssl_ctx_, api_key_, this);
        }
        
        session->run(target, body, 
            [this, target, body, cb = std::move(cb), retry_count]
            (std::string response, bool success) mutable {
                if (success) {
                    cb(std::move(response));
                } else {
                    on_request_failed(target, body, std::move(cb), retry_count);
                }
            });
    }
    
    void process_pending() {
        while (!pending_.empty() && active_count_ < HTTPS_POOL_SIZE) {
            auto req = std::move(pending_.front());
            pending_.pop();
            start_request(req.target, req.body, std::move(req.cb), req.retry_count);
        }
    }
    
    // 配置
    asio::io_context& ioc_;
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

inline void HttpsSession::fail(const char* what) {
    std::cerr << "[HTTPS] " << what << " failed" << std::endl;
    connected_ = false;
    auto cb = std::move(cb_);
    cb("", false);
}

inline void HttpsSession::return_to_pool() {
    pool_->return_session(shared_from_this());
}
