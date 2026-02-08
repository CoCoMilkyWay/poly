#pragma once

// ============================================================================
// API Session - HTTP 会话处理
// ============================================================================

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "../core/database.hpp"
#include "../core/entity_definition.hpp"
#include "../rebuild/rebuilder.hpp"
#include "../replayer/replayer.hpp"
#include "../stats/stats_manager.hpp"
#include "../sync/sync_token_filler.hpp"

namespace fs = std::filesystem;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

// ============================================================================
// ApiSession - HTTP 会话
// ============================================================================
class ApiSession : public std::enable_shared_from_this<ApiSession> {
public:
  ApiSession(tcp::socket socket, Database &db, SyncTokenFiller &token_filler, rebuild::Engine &rebuild_engine)
      : socket_(std::move(socket)), db_(db), token_filler_(token_filler), rebuild_engine_(rebuild_engine) {}

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

    res_.set(http::field::access_control_allow_origin, "*");
    res_.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
    res_.set(http::field::access_control_allow_headers, "Content-Type");

    if (req_.method() == http::verb::options) {
      res_.result(http::status::ok);
      return do_write();
    }

    std::string target(req_.target());

    try {
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
      } else if (target.starts_with("/api/sync-progress")) {
        handle_sync_progress();
      } else if (target.starts_with("/api/sync")) {
        handle_sync_state();
      } else if (target.starts_with("/api/fill-token-ids")) {
        handle_fill_token_ids();
      } else if (target.starts_with("/api/replay-users")) {
        handle_replay_users();
      } else if (target.starts_with("/api/replay")) {
        handle_replay();
      } else if (target.starts_with("/api/rebuild-status")) {
        handle_rebuild_status();
      } else if (target.starts_with("/api/rebuild-all")) {
        handle_rebuild_all();
      } else if (target.starts_with("/api/export-raw")) {
        handle_export_raw();
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
    for (const auto *e : entities::ALL_ENTITIES) {
      stats[e->table] = StatsManager::instance().get_total_count_for_entity(e->name);
    }

    res_.result(http::status::ok);
    res_.body() = stats.dump();
  }

  void handle_sync_state() {
    res_.set(http::field::content_type, "application/json");
    json result = db_.query_json(
        "SELECT source, entity, cursor_value, cursor_skip, last_sync_at "
        "FROM sync_state ORDER BY last_sync_at DESC");
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_entity_stats() {
    res_.set(http::field::content_type, "application/json");
    res_.result(http::status::ok);
    res_.body() = StatsManager::instance().get_all_dump();
  }

  void handle_entity_latest() {
    res_.set(http::field::content_type, "application/json");

    std::string entity_name = get_param("entity");
    assert(!entity_name.empty() && "Missing query parameter 'entity'");

    const entities::EntityDef *e = entities::find_entity_by_name(entity_name.c_str());
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

  void handle_sync_progress() {
    res_.set(http::field::content_type, "application/json");

    int64_t eof_min_ts = db_.query_single_int("SELECT MIN(timestamp) FROM enriched_order_filled");
    auto eof_cursor = db_.get_cursor("Polymarket", "EnrichedOrderFilled");
    int64_t eof_synced_ts = eof_cursor.value.empty() ? 0 : std::stoll(eof_cursor.value);

    int64_t token_min_ts = db_.query_single_int("SELECT MIN(resolutionTimestamp) FROM condition");
    int64_t token_synced_ts = db_.query_single_int(
        "SELECT MIN(resolutionTimestamp) FROM condition WHERE positionIds IS NULL");

    int64_t now_ts = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    // 全部填完时 token_synced_ts == 0 (没有 NULL 行), 显示为 now
    if (token_synced_ts == 0)
      token_synced_ts = now_ts;

    json result = {
        {"eof_min_ts", eof_min_ts},
        {"eof_synced_ts", eof_synced_ts},
        {"token_min_ts", token_min_ts},
        {"token_synced_ts", token_synced_ts},
        {"now_ts", now_ts},
        {"filler_running", token_filler_.is_running()},
        {"filler_processed", token_filler_.processed()},
        {"filler_phase", token_filler_.phase()},
        {"filler_total_null", token_filler_.total_null()},
        {"filler_merged", token_filler_.merged()},
        {"filler_not_found", token_filler_.not_found()},
        {"filler_errors", token_filler_.errors()},
        {"filler_start_ts", token_filler_.start_ts()},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_fill_token_ids() {
    res_.set(http::field::content_type, "application/json");
    std::string status = token_filler_.start();
    res_.result(http::status::ok);
    res_.body() = json{{"status", status}}.dump();
  }

  void handle_replay() {
    res_.set(http::field::content_type, "application/json");
    std::string user = get_param("user");
    assert(!user.empty() && "Missing query parameter 'user'");
    assert(rebuild_engine_.find_user(user) != nullptr && "User not found");
    json result = replayer::serialize_user(rebuild_engine_, user);
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_replay_users() {
    res_.set(http::field::content_type, "application/json");
    std::string limit_str = get_param("limit");
    int limit = limit_str.empty() ? 200 : std::stoi(limit_str);
    json result = replayer::serialize_user_list(rebuild_engine_, limit);
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_rebuild_all() {
    res_.set(http::field::content_type, "application/json");
    auto progress = rebuild_engine_.get_progress();
    if (progress.running) {
      res_.result(http::status::ok);
      res_.body() = json{{"status", "already_running"}}.dump();
      return;
    }
    // 后台线程触发重建
    std::thread([&engine = rebuild_engine_]() {
      engine.rebuild_all();
    }).detach();
    res_.result(http::status::ok);
    res_.body() = json{{"status", "started"}}.dump();
  }

  void handle_rebuild_status() {
    res_.set(http::field::content_type, "application/json");
    auto p = rebuild_engine_.get_progress();
    res_.result(http::status::ok);
    res_.body() = json{
        {"running", p.running},
        {"phase", p.phase},
        {"total_conditions", p.total_conditions},
        {"total_tokens", p.total_tokens},
        {"total_events", p.total_events},
        {"total_users", p.total_users},
        {"processed_users", p.processed_users},
        {"eof_rows", p.eof_rows},
        {"eof_events", p.eof_events},
        {"split_rows", p.split_rows},
        {"split_events", p.split_events},
        {"merge_rows", p.merge_rows},
        {"merge_events", p.merge_events},
        {"redemption_rows", p.redemption_rows},
        {"redemption_events", p.redemption_events},
        {"eof_done", p.eof_done},
        {"split_done", p.split_done},
        {"merge_done", p.merge_done},
        {"redemption_done", p.redemption_done},
        {"phase1_ms", p.phase1_ms},
        {"phase2_ms", p.phase2_ms},
        {"phase3_ms", p.phase3_ms}}
                     .dump();
  }

  void handle_export_raw() {
    res_.set(http::field::content_type, "application/json");

    std::string limit_str = get_param("limit");
    int limit = limit_str.empty() ? 100 : std::stoi(limit_str);
    if (limit > 1000)
      limit = 1000;

    std::string order_str = get_param("order");
    std::string order_dir = (order_str == "asc") ? "ASC" : "DESC";

    std::string export_dir = fs::current_path().string() + "/data/export";
    fs::create_directories(export_dir);

    json j_results = json::object();
    int ok_count = 0;

    auto parse_columns = [](const char *columns) {
      std::vector<std::string> names;
      std::string cols = columns;
      size_t pos = 0;
      while (pos < cols.size()) {
        size_t comma = cols.find(',', pos);
        std::string col = (comma == std::string::npos)
                              ? cols.substr(pos)
                              : cols.substr(pos, comma - pos);
        size_t b = col.find_first_not_of(" ");
        size_t e = col.find_last_not_of(" ");
        if (b != std::string::npos)
          names.push_back(col.substr(b, e - b + 1));
        pos = (comma == std::string::npos) ? cols.size() : comma + 1;
      }
      return names;
    };

    for (const auto *e : entities::ALL_ENTITIES) {
      std::string table = e->table;
      std::string sql = "SELECT " + std::string(e->columns) + " FROM " + table +
                        " ORDER BY id " + order_dir + " LIMIT " + std::to_string(limit);

      json rows = db_.query_json(sql);
      auto col_names = parse_columns(e->columns);

      std::string path = export_dir + "/" + table + ".csv";
      std::ofstream ofs(path);
      assert(ofs.is_open());

      for (size_t k = 0; k < col_names.size(); ++k) {
        if (k > 0)
          ofs << ",";
        ofs << col_names[k];
      }
      ofs << "\n";

      for (const auto &row : rows) {
        for (size_t k = 0; k < col_names.size(); ++k) {
          if (k > 0)
            ofs << ",";
          if (!row.contains(col_names[k]) || row[col_names[k]].is_null())
            continue;
          const auto &v = row[col_names[k]];
          if (v.is_string())
            ofs << escape_csv(v.get<std::string>());
          else
            ofs << v.dump();
        }
        ofs << "\n";
      }

      int row_count = static_cast<int>(rows.size());
      j_results[table] = {{"ok", row_count}};
      if (row_count > 0)
        ++ok_count;
    }

    res_.result(http::status::ok);
    res_.body() = json{
        {"path", export_dir},
        {"exported_tables", ok_count},
        {"results", j_results}}
                      .dump();
  }

  static std::string escape_csv(const std::string &s) {
    if (s.find(',') == std::string::npos &&
        s.find('"') == std::string::npos &&
        s.find('\n') == std::string::npos)
      return s;
    std::string r = "\"";
    for (char c : s) {
      if (c == '"')
        r += "\"\"";
      else
        r += c;
    }
    r += "\"";
    return r;
  }

  void do_write() {
    http::async_write(socket_, res_,
                      [self = shared_from_this()](beast::error_code ec, std::size_t) {
                        beast::error_code shutdown_ec;
                        [[maybe_unused]] auto ret = self->socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
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
  SyncTokenFiller &token_filler_;
  rebuild::Engine &rebuild_engine_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};
