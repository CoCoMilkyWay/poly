#pragma once

// ============================================================================
// 简单 HTTP 服务器，提供查询 API
// ============================================================================

#include <string>
#include <memory>
#include <functional>
#include <iostream>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "db.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

// ============================================================================
// HTTP Session
// ============================================================================
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
    HttpSession(tcp::socket socket, Database& db)
        : socket_(std::move(socket)), db_(db) {}
    
    void run() {
        do_read();
    }

private:
    void do_read() {
        req_ = {};
        http::async_read(socket_, buffer_, req_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return;
                self->handle_request();
            });
    }
    
    void handle_request() {
        res_ = {};
        res_.version(req_.version());
        res_.keep_alive(req_.keep_alive());
        
        // CORS 头
        res_.set(http::field::access_control_allow_origin, "*");
        res_.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
        res_.set(http::field::access_control_allow_headers, "Content-Type");
        
        // OPTIONS 预检请求
        if (req_.method() == http::verb::options) {
            res_.result(http::status::ok);
            return do_write();
        }
        
        std::string target(req_.target());
        
        try {
            // 路由
            if (target.starts_with("/api/sql")) {
                handle_sql();
            } else if (target.starts_with("/api/stats")) {
                handle_stats();
            } else if (target.starts_with("/api/sync")) {
                handle_sync_state();
            } else {
                res_.result(http::status::not_found);
                res_.set(http::field::content_type, "application/json");
                res_.body() = R"({"error":"Not found"})";
            }
        } catch (const std::exception& e) {
            res_.result(http::status::internal_server_error);
            res_.set(http::field::content_type, "application/json");
            res_.body() = json{{"error", e.what()}}.dump();
        } catch (...) {
            res_.result(http::status::internal_server_error);
            res_.set(http::field::content_type, "application/json");
            res_.body() = R"({"error":"Unknown error"})";
        }
        
        res_.prepare_payload();
        do_write();
    }
    
    void handle_sql() {
        std::string target(req_.target());
        std::string query;
        
        // 从 URL 参数获取 SQL
        auto pos = target.find("q=");
        if (pos != std::string::npos) {
            query = url_decode(target.substr(pos + 2));
            // 去掉后续参数
            auto amp = query.find('&');
            if (amp != std::string::npos) query = query.substr(0, amp);
        }
        
        res_.set(http::field::content_type, "application/json");
        
        if (query.empty()) {
            res_.result(http::status::bad_request);
            res_.body() = R"({"error":"Missing query parameter 'q'"})";
            return;
        }
        
        // 只允许 SELECT
        std::string upper = query;
        for (auto& c : upper) c = std::toupper(c);
        if (!upper.starts_with("SELECT")) {
            res_.result(http::status::bad_request);
            res_.body() = R"({"error":"Only SELECT queries allowed"})";
            return;
        }
        
        try {
            json result = db_.query_json(query);
            res_.result(http::status::ok);
            res_.body() = result.dump();
        } catch (const std::exception& e) {
            res_.result(http::status::internal_server_error);
            res_.body() = json{{"error", e.what()}}.dump();
        }
    }
    
    void handle_stats() {
        res_.set(http::field::content_type, "application/json");
        
        try {
            json stats = json::object();
            std::vector<std::string> tables = {
                "condition", "split", "merge", "redemption",
                "account", "enriched_order_filled", "orderbook",
                "market_data", "market_position", "market_profit",
                "fpmm", "fpmm_transaction", "global",
                "user_position", "pnl_condition", "neg_risk_event", "pnl_fpmm"
            };
            
            for (const auto& table : tables) {
                try {
                    auto result = db_.query_json("SELECT COUNT(*) as cnt FROM " + table);
                    if (!result.empty()) {
                        stats[table] = result[0]["cnt"];
                    }
                } catch (...) {
                    stats[table] = 0;
                }
            }
            
            res_.result(http::status::ok);
            res_.body() = stats.dump();
        } catch (const std::exception& e) {
            res_.result(http::status::internal_server_error);
            res_.body() = json{{"error", e.what()}}.dump();
        }
    }
    
    void handle_sync_state() {
        res_.set(http::field::content_type, "application/json");
        
        try {
            json result = db_.query_json(
                "SELECT source, entity, last_id, last_sync_at, total_synced "
                "FROM sync_state ORDER BY last_sync_at DESC"
            );
            res_.result(http::status::ok);
            res_.body() = result.dump();
        } catch (const std::exception& e) {
            res_.result(http::status::internal_server_error);
            res_.body() = json{{"error", e.what()}}.dump();
        }
    }
    
    void do_write() {
        http::async_write(socket_, res_,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                self->socket_.shutdown(tcp::socket::shutdown_send, ec);
            });
    }
    
    static std::string url_decode(const std::string& str) {
        std::string result;
        for (size_t i = 0; i < str.size(); ++i) {
            if (str[i] == '%' && i + 2 < str.size()) {
                int hex = std::stoi(str.substr(i + 1, 2), nullptr, 16);
                result += static_cast<char>(hex);
                i += 2;
            } else if (str[i] == '+') {
                result += ' ';
            } else {
                result += str[i];
            }
        }
        return result;
    }
    
    tcp::socket socket_;
    Database& db_;
    beast::flat_buffer buffer_;
    http::request<http::string_body> req_;
    http::response<http::string_body> res_;
};

// ============================================================================
// HTTP Server
// ============================================================================
class HttpServer {
public:
    HttpServer(asio::io_context& ioc, Database& db, unsigned short port)
        : ioc_(ioc)
        , acceptor_(ioc, tcp::endpoint(tcp::v4(), port))
        , db_(db)
    {
        std::cout << "[HTTP] 监听端口 " << port << std::endl;
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](beast::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<HttpSession>(std::move(socket), db_)->run();
                }
                do_accept();
            });
    }
    
    asio::io_context& ioc_;
    tcp::acceptor acceptor_;
    Database& db_;
};
