#pragma once

#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "../core/database.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

struct SyncStatus {
  bool is_syncing = false;
  int64_t head_block = 0;
  double blocks_per_second = 0.0;
};

class ApiSession : public std::enable_shared_from_this<ApiSession> {
public:
  using SyncStatusGetter = std::function<SyncStatus()>;

  ApiSession(tcp::socket socket, Database &db, SyncStatusGetter sync_getter = nullptr)
      : socket_(std::move(socket)), db_(db), sync_getter_(std::move(sync_getter)) {}

  void run() { do_read(); }

private:
  void do_read() {
    req_ = {};
    http::async_read(socket_, buffer_, req_,
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                       if (ec)
                         return;
                       self->handle_request();
                     });
  }

  void handle_request() {
    res_ = {};
    res_.version(req_.version());
    res_.keep_alive(req_.keep_alive());

    res_.set(http::field::access_control_allow_origin, "*");
    res_.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res_.set(http::field::access_control_allow_headers, "Content-Type");

    if (req_.method() == http::verb::options) {
      res_.result(http::status::ok);
      return do_write();
    }

    std::string target(req_.target());

    try {
      if (target.starts_with("/api/health")) {
        handle_health();
      } else if (target.starts_with("/api/tables")) {
        handle_tables();
      } else if (target.starts_with("/api/sync-state")) {
        handle_sync_state();
      } else if (target.starts_with("/api/query")) {
        handle_query();
      } else {
        res_.result(http::status::not_found);
        res_.set(http::field::content_type, "application/json");
        res_.body() = R"({"error":"Not found"})";
      }
    } catch (const std::exception &e) {
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

  void handle_health() {
    res_.result(http::status::ok);
    res_.set(http::field::content_type, "application/json");
    res_.body() = R"({"status":"ok"})";
  }

  void handle_tables() {
    res_.set(http::field::content_type, "application/json");

    json tables_info = json::array();
    auto tables = db_.get_tables();
    for (const auto &t : tables) {
      std::string name = t["table_name"].get<std::string>();
      int64_t count = db_.get_table_count(name);
      tables_info.push_back({{"name", name}, {"count", count}});
    }

    res_.result(http::status::ok);
    res_.body() = tables_info.dump();
  }

  void handle_sync_state() {
    res_.set(http::field::content_type, "application/json");

    int64_t last_block = db_.get_last_block();
    json result = {{"last_block", last_block}};

    if (sync_getter_) {
      SyncStatus status = sync_getter_();
      result["head_block"] = status.head_block;
      result["is_syncing"] = status.is_syncing;
      result["blocks_per_second"] = status.blocks_per_second;
    }

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_query() {
    res_.set(http::field::content_type, "application/json");

    std::string query = get_param("q");
    if (query.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing query parameter 'q'"})";
      return;
    }

    std::string upper = query;
    for (auto &c : upper)
      c = std::toupper(c);

    if (!upper.starts_with("SELECT")) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Only SELECT queries allowed"})";
      return;
    }

    json result = db_.query_json(query);
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  std::string get_param(const char *name) {
    std::string target(req_.target());
    std::string key = std::string(name) + "=";
    auto pos = target.find(key);
    if (pos == std::string::npos)
      return "";
    std::string value = url_decode(target.substr(pos + key.size()));
    auto amp = value.find('&');
    return (amp != std::string::npos) ? value.substr(0, amp) : value;
  }

  static std::string url_decode(const std::string &str) {
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

  void do_write() {
    http::async_write(socket_, res_,
                      [self = shared_from_this()](beast::error_code ec, std::size_t) {
                        if (!ec && self->res_.keep_alive()) {
                          self->do_read();
                        } else {
                          beast::error_code shutdown_ec;
                          [[maybe_unused]] auto ret = self->socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                        }
                      });
  }

  tcp::socket socket_;
  Database &db_;
  SyncStatusGetter sync_getter_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};
