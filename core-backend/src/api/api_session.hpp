#pragma once

#include <functional>
#include <memory>
#include <string>
#include <thread>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "../core/database.hpp"
#include "../rebuild/rebuilder.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

struct SyncStatus {
  bool is_syncing = false;
  int64_t head_block = 0;
  double blocks_per_second = 0.0;
  double bytes_per_block = 0.0;
};

class ApiSession : public std::enable_shared_from_this<ApiSession> {
public:
  using SyncStatusGetter = std::function<SyncStatus()>;

  ApiSession(tcp::socket socket, Database &db, rebuild::Engine &rebuilder,
             SyncStatusGetter sync_getter = nullptr)
      : socket_(std::move(socket)), db_(db), rebuilder_(rebuilder),
        sync_getter_(std::move(sync_getter)) {}

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
      } else if (target == "/api/rebuild" && req_.method() == http::verb::post) {
        handle_rebuild();
      } else if (target.starts_with("/api/rebuild-status")) {
        handle_rebuild_status();
      } else if (target.starts_with("/api/user/") && target.find("/pnl") != std::string::npos) {
        handle_user_pnl(target);
      } else if (target.starts_with("/api/user/") && target.find("/positions") != std::string::npos) {
        handle_user_positions(target);
      } else if (target.starts_with("/api/replay-users")) {
        handle_replay_users();
      } else if (target.starts_with("/api/replay-positions")) {
        handle_replay_positions();
      } else if (target.starts_with("/api/replay-trades")) {
        handle_replay_trades();
      } else if (target.starts_with("/api/replay")) {
        handle_replay();
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
      result["bytes_per_block"] = status.bytes_per_block;
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

  void handle_rebuild() {
    res_.set(http::field::content_type, "application/json");

    const auto &progress = rebuilder_.progress();
    if (progress.running) {
      res_.result(http::status::conflict);
      res_.body() = R"({"error":"Rebuild already in progress"})";
      return;
    }

    std::thread([this]() { rebuilder_.rebuild_all(); }).detach();

    res_.result(http::status::accepted);
    res_.body() = R"({"status":"started"})";
  }

  void handle_rebuild_status() {
    res_.set(http::field::content_type, "application/json");

    const auto &p = rebuilder_.progress();
    json result = {
        {"phase", p.phase},
        {"running", p.running},
        {"total_conditions", p.total_conditions},
        {"total_tokens", p.total_tokens},
        {"total_events", p.total_events},
        {"total_users", p.total_users},
        {"processed_users", p.processed_users},
        {"phase1_ms", p.phase1_ms},
        {"phase2_ms", p.phase2_ms},
        {"phase3_ms", p.phase3_ms},
        {"order_filled", {{"rows", p.order_filled_rows}, {"events", p.order_filled_events}}},
        {"split", {{"rows", p.split_rows}, {"events", p.split_events}}},
        {"merge", {{"rows", p.merge_rows}, {"events", p.merge_events}}},
        {"redemption", {{"rows", p.redemption_rows}, {"events", p.redemption_events}}},
        {"fpmm_trade", {{"rows", p.fpmm_trade_rows}, {"events", p.fpmm_trade_events}}},
        {"fpmm_funding", {{"rows", p.fpmm_funding_rows}, {"events", p.fpmm_funding_events}}},
        {"convert", {{"rows", p.convert_rows}, {"events", p.convert_events}}},
        {"transfer", {{"rows", p.transfer_rows}, {"events", p.transfer_events}}},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_user_pnl(const std::string &target) {
    res_.set(http::field::content_type, "application/json");

    std::string addr = extract_user_addr(target);
    if (addr.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Invalid address"})";
      return;
    }

    const auto *state = rebuilder_.get_user_state(addr);
    if (!state) {
      res_.result(http::status::not_found);
      res_.body() = R"({"error":"User not found"})";
      return;
    }

    int64_t total_realized_pnl = 0;
    int64_t total_cost_basis = 0;
    json conditions = json::array();

    for (const auto &ch : state->conditions) {
      if (ch.snapshots.empty())
        continue;
      const auto &last = ch.snapshots.back();
      total_realized_pnl += last.realized_pnl;
      total_cost_basis += last.cost_basis;

      json cond_obj = {
          {"condition_id", rebuilder_.get_condition_id(ch.cond_idx)},
          {"realized_pnl", last.realized_pnl},
          {"cost_basis", last.cost_basis},
          {"positions", json::array()},
      };
      for (int i = 0; i < last.outcome_count; ++i) {
        cond_obj["positions"].push_back(last.positions[i]);
      }
      conditions.push_back(cond_obj);
    }

    json result = {
        {"address", addr},
        {"total_realized_pnl", total_realized_pnl},
        {"total_cost_basis", total_cost_basis},
        {"conditions_count", state->conditions.size()},
        {"conditions", conditions},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_user_positions(const std::string &target) {
    res_.set(http::field::content_type, "application/json");

    std::string addr = extract_user_addr(target);
    if (addr.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Invalid address"})";
      return;
    }

    const auto *state = rebuilder_.get_user_state(addr);
    if (!state) {
      res_.result(http::status::not_found);
      res_.body() = R"({"error":"User not found"})";
      return;
    }

    json positions = json::array();
    for (const auto &ch : state->conditions) {
      if (ch.snapshots.empty())
        continue;
      const auto &last = ch.snapshots.back();
      bool has_position = false;
      for (int i = 0; i < last.outcome_count; ++i) {
        if (last.positions[i] != 0) {
          has_position = true;
          break;
        }
      }
      if (!has_position)
        continue;

      json pos_obj = {
          {"condition_id", rebuilder_.get_condition_id(ch.cond_idx)},
          {"positions", json::array()},
          {"cost_basis", last.cost_basis},
      };
      for (int i = 0; i < last.outcome_count; ++i) {
        pos_obj["positions"].push_back(last.positions[i]);
      }
      positions.push_back(pos_obj);
    }

    json result = {
        {"address", addr},
        {"active_positions", positions.size()},
        {"positions", positions},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_replay_users() {
    res_.set(http::field::content_type, "application/json");

    std::string limit_str = get_param("limit");
    int64_t limit = limit_str.empty() ? 200 : std::stoll(limit_str);

    auto users = rebuilder_.get_users_sorted(limit);
    json result = json::array();
    for (const auto &u : users) {
      result.push_back({
          {"user_addr", u.addr},
          {"event_count", u.event_count},
          {"realized_pnl", u.realized_pnl},
      });
    }

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_replay() {
    res_.set(http::field::content_type, "application/json");

    std::string user = get_param("user");
    if (user.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing user parameter"})";
      return;
    }

    auto timeline = rebuilder_.get_user_timeline(user);
    if (timeline.empty()) {
      res_.result(http::status::not_found);
      res_.body() = R"({"error":"User not found or no events"})";
      return;
    }

    int64_t first_ts = timeline.front().sort_key / 1000000000LL;
    int64_t last_ts = timeline.back().sort_key / 1000000000LL;

    json timeline_arr = json::array();
    for (const auto &e : timeline) {
      timeline_arr.push_back({
          {"sk", e.sort_key},
          {"ty", e.event_type},
          {"rpnl", e.realized_pnl},
          {"d", e.delta},
          {"p", e.price},
          {"ci", e.cond_idx},
          {"ti", e.token_idx},
          {"tk", e.token_count},
      });
    }

    json result = {
        {"total_events", timeline.size()},
        {"first_ts", first_ts},
        {"last_ts", last_ts},
        {"timeline", timeline_arr},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_replay_positions() {
    res_.set(http::field::content_type, "application/json");

    std::string user = get_param("user");
    std::string sk_str = get_param("sk");
    if (user.empty() || sk_str.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing user or sk parameter"})";
      return;
    }

    int64_t sort_key = std::stoll(sk_str);
    auto positions = rebuilder_.get_positions_at(user, sort_key);

    json pos_arr = json::array();
    for (const auto &p : positions) {
      json pos_obj = {
          {"id", p.condition_id},
          {"pos", json::array()},
          {"cost", p.cost_basis},
          {"rpnl", p.realized_pnl},
      };
      for (int i = 0; i < p.outcome_count; ++i) {
        pos_obj["pos"].push_back(p.positions[i]);
      }
      pos_arr.push_back(pos_obj);
    }

    res_.result(http::status::ok);
    res_.body() = json{{"positions", pos_arr}}.dump();
  }

  void handle_replay_trades() {
    res_.set(http::field::content_type, "application/json");

    std::string user = get_param("user");
    std::string sk_str = get_param("sk");
    std::string radius_str = get_param("radius");
    if (user.empty() || sk_str.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing user or sk parameter"})";
      return;
    }

    int64_t sort_key = std::stoll(sk_str);
    int radius = radius_str.empty() ? 20 : std::stoi(radius_str);

    auto trades = rebuilder_.get_trades_near(user, sort_key, radius);
    size_t center = rebuilder_.get_trades_center_index(user, sort_key, radius);

    json events_arr = json::array();
    for (const auto &t : trades) {
      events_arr.push_back({
          {"sk", t.sort_key},
          {"ty", t.event_type},
          {"d", t.delta},
          {"p", t.price},
          {"ci", t.cond_idx},
          {"ti", t.token_idx},
      });
    }

    res_.result(http::status::ok);
    res_.body() = json{{"events", events_arr}, {"center", center}}.dump();
  }

  static std::string extract_user_addr(const std::string &target) {
    size_t start = target.find("/api/user/");
    if (start == std::string::npos)
      return "";
    start += 10;
    size_t end = target.find('/', start);
    if (end == std::string::npos)
      end = target.find('?', start);
    if (end == std::string::npos)
      end = target.size();
    return target.substr(start, end - start);
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
  rebuild::Engine &rebuilder_;
  SyncStatusGetter sync_getter_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};
