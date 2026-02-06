#pragma once

// ============================================================================
// 简单 HTTP 服务器，提供查询 API
// ============================================================================

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "db.hpp"
#include "entities.hpp"
#include "entity_stats.hpp"
#include "introspector.hpp"
#include "rebuilder.hpp"

namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

// 前向声明
class HttpServer;

// ============================================================================
// HTTP Session
// ============================================================================
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(tcp::socket socket, Database &db, rebuilder::Rebuilder &rebuilder,
              HttpsPool &pool, const Config &config, const std::string &export_dir)
      : socket_(std::move(socket)), db_(db), rebuilder_(rebuilder),
        pool_(pool), config_(config), export_dir_(export_dir) {}

  void run() {
    do_read();
  }

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
      } else if (target.starts_with("/api/indexer-fails")) {
        handle_indexer_fails();
      } else if (target.starts_with("/api/entity-latest")) {
        handle_entity_latest();
      } else if (target.starts_with("/api/entity-stats")) {
        handle_entity_stats();
      } else if (target.starts_with("/api/stats")) {
        handle_stats();
      } else if (target.starts_with("/api/sync")) {
        handle_sync_state();
      } else if (target.starts_with("/api/rebuild-status")) {
        handle_rebuild_status();
      } else if (target.starts_with("/api/rebuild-all")) {
        handle_rebuild_all();
      } else if (target.starts_with("/api/rebuild")) {
        handle_rebuild_user();
      } else if (target.starts_with("/api/export-raw")) {
        handle_export_raw();
        return;  // async handler, do_write() called in callback
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

  void handle_sql() {
    res_.set(http::field::content_type, "application/json");

    std::string query = get_param("q");
    assert(!query.empty() && "Missing query parameter 'q'");

    // SQL 注入防护
    std::string upper = query;
    for (auto &c : upper)
      c = std::toupper(c);
    assert(upper.starts_with("SELECT") && "Only SELECT queries allowed");
    assert(query.find(';') == std::string::npos && "Semicolon not allowed");
    assert(query.find("--") == std::string::npos && "SQL comment not allowed");
    assert(query.find("/*") == std::string::npos && "SQL comment not allowed");
    assert(upper.find("INSERT") == std::string::npos && "INSERT not allowed");
    assert(upper.find("UPDATE") == std::string::npos && "UPDATE not allowed");
    assert(upper.find("DELETE") == std::string::npos && "DELETE not allowed");
    assert(upper.find("DROP") == std::string::npos && "DROP not allowed");
    assert(upper.find("CREATE") == std::string::npos && "CREATE not allowed");
    assert(upper.find("ALTER") == std::string::npos && "ALTER not allowed");
    assert(upper.find("TRUNCATE") == std::string::npos && "TRUNCATE not allowed");

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

  void handle_stats() {
    res_.set(http::field::content_type, "application/json");

    json stats = json::object();
    // 从 entities 定义动态获取表名
    auto collect_stats = [&](const entities::EntityDef *const *list, size_t count) {
      for (size_t i = 0; i < count; ++i) {
        const auto *e = list[i];
        assert(e != nullptr);
        // 极限优化：不做 COUNT(*) 扫表，直接复用 puller 维护的内存计数(跨 source 汇总)
        stats[e->table] = EntityStatsManager::instance().get_total_count_for_entity(e->name);
      }
    };
    collect_stats(entities::MAIN_ENTITIES, entities::MAIN_ENTITY_COUNT);
    collect_stats(entities::PNL_ENTITIES, entities::PNL_ENTITY_COUNT);

    res_.result(http::status::ok);
    res_.body() = stats.dump();
  }

  void handle_sync_state() {
    res_.set(http::field::content_type, "application/json");
    json result = db_.query_json(
        "SELECT source, entity, last_id, last_sync_at "
        "FROM sync_state ORDER BY last_sync_at DESC");
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_entity_stats() {
    res_.set(http::field::content_type, "application/json");
    res_.result(http::status::ok);
    res_.body() = EntityStatsManager::instance().get_all_dump();
  }

  void handle_entity_latest() {
    res_.set(http::field::content_type, "application/json");

    std::string entity = get_param("entity");
    assert(!entity.empty() && "Missing query parameter 'entity'");

    const entities::EntityDef *e =
        entities::find_entity(entity.c_str(), entities::MAIN_ENTITIES, entities::MAIN_ENTITY_COUNT);
    if (!e) {
      e = entities::find_entity(entity.c_str(), entities::PNL_ENTITIES, entities::PNL_ENTITY_COUNT);
    }
    assert(e && "Unknown entity");

    json schema = db_.query_json(std::string("PRAGMA table_info('") + e->table + "')");
    json rows = db_.query_json(std::string("SELECT * FROM ") + e->table + " ORDER BY id DESC LIMIT 1");
    json row = rows.empty() ? json(nullptr) : rows[0];

    json result = {
        {"entity", e->name},
        {"table", e->table},
        {"columns", schema},
        {"row", row},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_indexer_fails() {
    res_.set(http::field::content_type, "application/json");
    std::string source = get_param("source");
    std::string entity = get_param("entity");
    assert(!source.empty() && "Missing query parameter 'source'");
    assert(!entity.empty() && "Missing query parameter 'entity'");

    std::string sql =
        "SELECT indexer, fail_requests "
        "FROM indexer_fail_meta "
        "WHERE source = " +
        entities::escape_sql(source) +
        " AND entity = " + entities::escape_sql(entity) +
        " ORDER BY fail_requests DESC";
    json rows = db_.query_json(sql);
    res_.result(http::status::ok);
    res_.body() = rows.dump();
  }

  void handle_rebuild_user() {
    res_.set(http::field::content_type, "application/json");
    std::string user = get_param("user");
    assert(!user.empty() && "Missing query parameter 'user'");

    auto result = rebuilder_.rebuild_user(user);
    res_.result(http::status::ok);
    res_.body() = rebuilder::Rebuilder::result_to_json(result).dump();
  }

  void handle_rebuild_all() {
    res_.set(http::field::content_type, "application/json");

    // 检查是否已经在运行
    if (rebuilder_.get_progress().running) {
      res_.result(http::status::ok);
      res_.body() = R"({"status":"already_running"})";
      return;
    }

    // 异步执行全量重建
    std::thread([&rebuilder = rebuilder_]() {
      rebuilder.rebuild_all();
    }).detach();

    res_.result(http::status::ok);
    res_.body() = R"({"status":"started"})";
  }

  void handle_rebuild_status() {
    res_.set(http::field::content_type, "application/json");
    auto progress = rebuilder_.get_progress();

    json result = {
        {"running", progress.running},
        {"total_users", progress.total_users},
        {"processed_users", progress.processed_users},
        {"total_events", progress.total_events},
        {"processed_events", progress.processed_events},
        {"error", progress.error}};

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_export_raw() {
    res_.set(http::field::content_type, "application/json");

    std::string limit_str = get_param("limit");
    int limit = limit_str.empty() ? 100 : std::stoi(limit_str);
    if (limit > 1000)
      limit = 1000;

    // order=desc(默认,最新数据) 或 order=asc(最早数据)
    std::string order_str = get_param("order");
    bool order_desc = (order_str != "asc");

    // 创建 introspector 并启动导出
    auto intro = std::make_shared<introspector::Introspector>(pool_, config_);
    intro->start(export_dir_, limit, order_desc);

    // 异步等待完成(通过定时器轮询)
    auto self = shared_from_this();
    auto timer = std::make_shared<asio::steady_timer>(socket_.get_executor());

    auto check_done = std::make_shared<std::function<void()>>();
    *check_done = [this, self, intro, timer, check_done]() mutable {
      if (intro->is_done()) {
        const auto &results = intro->get_results();
        json j_results = json::object();
        int ok_count = 0;
        for (const auto &r : results) {
          std::string key = r.source + "/" + r.entity;
          if (r.error.empty()) {
            j_results[key] = {{"ok", r.rows}};
            if (r.rows > 0)
              ++ok_count;
          } else {
            j_results[key] = {{"error", r.error}};
          }
        }

        res_.result(http::status::ok);
        res_.body() = json{
            {"path", export_dir_},
            {"exported_tables", ok_count},
            {"results", j_results}}
                          .dump();
        res_.prepare_payload();
        do_write();

        // 打破循环引用
        *check_done = nullptr;
      } else {
        timer->expires_after(std::chrono::milliseconds(100));
        timer->async_wait([check_done](boost::system::error_code) {
          if (*check_done) (*check_done)();
        });
      }
    };

    timer->expires_after(std::chrono::milliseconds(100));
    timer->async_wait([check_done](boost::system::error_code) {
      (*check_done)();
    });
  }

  void do_write() {
    http::async_write(socket_, res_,
                      [self = shared_from_this()](beast::error_code ec, std::size_t) {
                        beast::error_code shutdown_ec;
                        [[maybe_unused]] auto ret = self->socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                        // shutdown 失败不是致命错误，显式忽略
                      });
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

  tcp::socket socket_;
  Database &db_;
  rebuilder::Rebuilder &rebuilder_;
  HttpsPool &pool_;
  const Config &config_;
  std::string export_dir_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};

// ============================================================================
// HTTP Server
// ============================================================================
class HttpServer {
public:
  HttpServer(asio::io_context &ioc, Database &db, HttpsPool &pool,
             const Config &config, unsigned short port)
      : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), db_(db),
        pool_(pool), config_(config), rebuilder_(db.get_duckdb()) {
    // 设置导出目录
    export_dir_ = fs::current_path().string() + "/data/export";
    fs::create_directories(export_dir_);
    std::cout << "[HTTP] 监听端口 " << port << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<HttpSession>(std::move(socket), db_, rebuilder_,
                                          pool_, config_, export_dir_)
                ->run();
          }
          do_accept();
        });
  }

  asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  Database &db_;
  HttpsPool &pool_;
  const Config &config_;
  std::string export_dir_;
  rebuilder::Rebuilder rebuilder_;
};
