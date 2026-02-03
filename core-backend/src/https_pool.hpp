#pragma once

// ============================================================================
// 宏配置
// ============================================================================
#define HTTPS_POOL_SIZE 16          // 连接池大小（>= PARALLEL_TOTAL）
#define HTTPS_TIMEOUT_SEC 30        // 请求超时
#define HTTPS_HOST "gateway.thegraph.com"
#define HTTPS_PORT "443"

#include <string>
#include <queue>
#include <vector>
#include <memory>
#include <functional>
#include <cassert>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

// ============================================================================
// HttpsSession - 单个 HTTPS 连接会话
// ============================================================================
class HttpsSession : public std::enable_shared_from_this<HttpsSession> {
public:
    using Callback = std::function<void(std::string)>;
    
    HttpsSession(asio::io_context& ioc, ssl::context& ssl_ctx, const std::string& api_key)
        : resolver_(ioc)
        , stream_(ioc, ssl_ctx)
        , api_key_(api_key) {}
    
    void run(const std::string& target, const std::string& body, Callback cb) {
        cb_ = std::move(cb);
        target_ = target;
        body_ = body;
        
        // 设置 SNI
        SSL_set_tlsext_host_name(stream_.native_handle(), HTTPS_HOST);
        
        resolver_.async_resolve(
            HTTPS_HOST, HTTPS_PORT,
            [self = shared_from_this()](beast::error_code ec, tcp::resolver::results_type results) {
                assert(!ec && "DNS resolve failed");
                self->on_resolve(results);
            });
    }

private:
    void on_resolve(tcp::resolver::results_type results) {
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        beast::get_lowest_layer(stream_).async_connect(
            results,
            [self = shared_from_this()](beast::error_code ec, tcp::endpoint) {
                assert(!ec && "TCP connect failed");
                self->on_connect();
            });
    }
    
    void on_connect() {
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        stream_.async_handshake(
            ssl::stream_base::client,
            [self = shared_from_this()](beast::error_code ec) {
                assert(!ec && "SSL handshake failed");
                self->on_handshake();
            });
    }
    
    void on_handshake() {
        req_.method(http::verb::post);
        req_.target(target_);
        req_.version(11);
        req_.set(http::field::host, HTTPS_HOST);
        req_.set(http::field::content_type, "application/json");
        req_.set(http::field::authorization, "Bearer " + api_key_);
        req_.body() = body_;
        req_.prepare_payload();
        
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        http::async_write(
            stream_, req_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                assert(!ec && "HTTP write failed");
                self->on_write();
            });
    }
    
    void on_write() {
        http::async_read(
            stream_, buffer_, res_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                assert(!ec && "HTTP read failed");
                self->on_read();
            });
    }
    
    void on_read() {
        std::string response = res_.body();
        
        // 关闭连接
        beast::get_lowest_layer(stream_).expires_after(std::chrono::seconds(HTTPS_TIMEOUT_SEC));
        stream_.async_shutdown(
            [self = shared_from_this(), response = std::move(response)](beast::error_code) {
                // 忽略 shutdown 错误（服务器可能已关闭）
                self->cb_(response);
            });
    }
    
    tcp::resolver resolver_;
    beast::ssl_stream<beast::tcp_stream> stream_;
    std::string api_key_;
    std::string target_;
    std::string body_;
    Callback cb_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    http::response<http::string_body> res_;
};

// ============================================================================
// HttpsPool - HTTPS 连接池（实际上是请求调度器，每次创建新连接）
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
        if (active_count_ < HTTPS_POOL_SIZE) {
            start_request(target, body, std::move(cb));
        } else {
            pending_.push({target, body, std::move(cb)});
        }
    }
    
    int active_count() const { return active_count_; }
    int pending_count() const { return static_cast<int>(pending_.size()); }

private:
    struct PendingRequest {
        std::string target;
        std::string body;
        Callback cb;
    };
    
    void start_request(const std::string& target, const std::string& body, Callback cb) {
        ++active_count_;
        
        auto session = std::make_shared<HttpsSession>(ioc_, ssl_ctx_, api_key_);
        session->run(target, body, [this, cb = std::move(cb)](std::string response) {
            --active_count_;
            cb(std::move(response));
            
            // 处理队列中的下一个请求
            if (!pending_.empty()) {
                auto req = std::move(pending_.front());
                pending_.pop();
                start_request(req.target, req.body, std::move(req.cb));
            }
        });
    }
    
    asio::io_context& ioc_;
    ssl::context ssl_ctx_;
    std::string api_key_;
    int active_count_ = 0;
    std::queue<PendingRequest> pending_;
};
