#include "tracker/api.hpp"
#include "tracker/codec.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cassert>

namespace tracker {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

bool is_peer_closed(const beast::error_code &ec) {
  return ec == http::error::end_of_stream ||
         ec == asio::error::eof ||
         ec == asio::error::connection_reset ||
         ec == asio::error::broken_pipe;
}

std::pair<std::string, std::string> split_target(const std::string &target) {
  size_t pos = target.find('?');
  if (pos == std::string::npos) return {target, ""};
  return {target.substr(0, pos), target.substr(pos + 1)};
}

std::string query_param(const std::string &query, const std::string &key) {
  size_t start = 0;
  while (start < query.size()) {
    size_t end = query.find('&', start);
    std::string part = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
    size_t eq = part.find('=');
    std::string k = eq == std::string::npos ? part : part.substr(0, eq);
    if (k == key) return eq == std::string::npos ? "" : part.substr(eq + 1);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return "";
}

std::string http_response(unsigned version, unsigned status, std::string_view content_type, std::string_view body) {
  std::string r;
  r += "HTTP/" + std::to_string(version / 10) + "." + std::to_string(version % 10) + " ";
  r += std::to_string(status) + " ";
  r += status == 200 ? "OK" : "No Content";
  r += "\r\n";
  r += "Content-Type: " + std::string(content_type) + "\r\n";
  r += "Access-Control-Allow-Origin: *\r\n";
  r += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  r += "Access-Control-Allow-Headers: Content-Type\r\n";
  r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  r += "Connection: close\r\n\r\n";
  r += body;
  return r;
}

void write_json(tcp::socket &socket, unsigned version, const json &data) {
  std::string r = http_response(version, 200, "application/json; charset=utf-8", data.dump());
  beast::error_code ec;
  asio::write(socket, asio::buffer(r), ec);
  if (is_peer_closed(ec)) return;
  assert(!ec);
}

void write_empty(tcp::socket &socket, unsigned version, unsigned status) {
  std::string r = http_response(version, status, "text/plain; charset=utf-8", "");
  beast::error_code ec;
  asio::write(socket, asio::buffer(r), ec);
  if (is_peer_closed(ec)) return;
  assert(!ec);
}

void handle_request(AppState &state, ApiThread::ResyncCallback on_resync, tcp::socket socket) {
  beast::flat_buffer buf;
  http::request<http::string_body> req;
  beast::error_code ec;
  http::read(socket, buf, req, ec);
  if (is_peer_closed(ec)) return;
  assert(!ec);

  if (req.method() == http::verb::options) {
    write_empty(socket, req.version(), 204);
    return;
  }

  auto [path, query] = split_target(std::string(req.target()));

  if (req.method() == http::verb::get && (path == "/" || path == "/api/state")) {
    write_json(socket, req.version(), build_state_json(state));
    return;
  }
  if (req.method() == http::verb::get && path == "/api/meta") {
    write_json(socket, req.version(), build_meta_json(state));
    return;
  }
  if (req.method() == http::verb::get && path == "/api/history") {
    std::string user = query_param(query, "user");
    assert(!user.empty());
    write_json(socket, req.version(), build_history_json(state, user));
    return;
  }
  if (req.method() == http::verb::get && path == "/api/health") {
    write_json(socket, req.version(), build_health_json(state));
    return;
  }
  if (req.method() == http::verb::post && path == "/api/resync") {
    if (on_resync) on_resync();
    write_json(socket, req.version(), json{{"ok", true}});
    return;
  }

  assert(false);
}

} // namespace

ApiThread::ApiThread(const AppConfig &cfg, AppState &state, ResyncCallback on_resync)
    : cfg_(cfg), state_(state), on_resync_(std::move(on_resync)) {}

void ApiThread::start() {
  assert(!running_);
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void ApiThread::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.detach();
}

void ApiThread::run() {
  asio::io_context ioc;
  tcp::acceptor acceptor(ioc, tcp::endpoint(asio::ip::make_address(cfg_.backend_host), cfg_.backend_port));
  while (running_) {
    tcp::socket socket = acceptor.accept();
    std::thread(handle_request, std::ref(state_), on_resync_, std::move(socket)).detach();
  }
}

// ============================================================================
// JSON Builders
// ============================================================================

json build_state_json(AppState &state) {
  std::lock_guard<std::mutex> lock(state.mu);

  json result = {
      {"summary", json::object()},
      {"users", json::array()},
      {"aggregate", json::array()},
      {"recent_events", json::array()},
      {"history_index", json::object()},
  };

  struct Bucket {
    BigInt amount = 0;
    long double value = 0.0L;
    int holders = 0;
    bool stable = false;
    std::string label;
  };
  std::map<std::string, Bucket> agg;
  size_t position_count = 0;

  for (const auto &user : state.users) {
    const UserState &us = state.user_states.at(user);

    struct Row { long double val; json data; };
    std::vector<Row> rows;
    long double token_val = 0.0L, stable_val = 0.0L;

    for (const auto &[token_id, amount] : us.positions) {
      ++position_count;
      const TokenMeta *tm = state.tokens.contains(token_id) ? &state.tokens.at(token_id) : nullptr;
      const ConditionMeta *cm = (tm && !tm->condition_id.empty() && state.conditions.contains(tm->condition_id))
          ? &state.conditions.at(tm->condition_id) : nullptr;
      long double price = (tm && tm->price >= 0.0L) ? tm->price : 0.0L;
      long double val = bigint_to_units(amount) * price;
      token_val += val;

      std::string outcome_text;
      if (tm && cm && tm->outcome_index >= 0 && static_cast<size_t>(tm->outcome_index) < cm->market_outcomes.size()) {
        outcome_text = cm->market_outcomes[static_cast<size_t>(tm->outcome_index)];
      }

      rows.push_back({val, {
          {"asset_type", "token"},
          {"token_id", token_id},
          {"amount_raw", bigint_to_str(amount)},
          {"condition_id", tm && !tm->condition_id.empty() ? json(tm->condition_id) : json(nullptr)},
          {"question_id", tm && !tm->question_id.empty() ? json(tm->question_id) : json(nullptr)},
          {"outcome_index", tm && tm->outcome_index >= 0 ? json(tm->outcome_index) : json(nullptr)},
          {"outcome_text", outcome_text},
          {"market_question", cm ? cm->market_question : ""},
          {"market_description", cm ? cm->market_description : ""},
          {"price", tm && tm->price >= 0.0L ? json(fmt_decimal(tm->price, 10)) : json(nullptr)},
          {"price_source", tm ? tm->price_source : ""},
          {"value_usd", val},
      }});

      Bucket &b = agg[token_id];
      b.amount += amount;
      b.value += val;
      b.holders++;
    }

    if (us.stable.usdc_e > 0) {
      long double val = bigint_to_units(us.stable.usdc_e);
      stable_val += val;
      rows.push_back({val, {
          {"asset_type", "stable"}, {"token_id", "stable:usdc_e"}, {"label", "USDC.e"},
          {"amount_raw", bigint_to_str(us.stable.usdc_e)}, {"price", "1.0"}, {"value_usd", val},
      }});
      Bucket &b = agg["stable:usdc_e"];
      b.amount += us.stable.usdc_e;
      b.value += val;
      b.holders++;
      b.stable = true;
      b.label = "USDC.e";
    }

    if (us.stable.wrapped > 0) {
      long double val = bigint_to_units(us.stable.wrapped);
      stable_val += val;
      rows.push_back({val, {
          {"asset_type", "stable"}, {"token_id", "stable:wrapped"}, {"label", "Wrapped"},
          {"amount_raw", bigint_to_str(us.stable.wrapped)}, {"price", "1.0"}, {"value_usd", val},
      }});
      Bucket &b = agg["stable:wrapped"];
      b.amount += us.stable.wrapped;
      b.value += val;
      b.holders++;
      b.stable = true;
      b.label = "Wrapped";
    }

    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
      return a.val != b.val ? a.val > b.val : a.data.at("token_id") < b.data.at("token_id");
    });

    long double total = token_val + stable_val;
    json positions = json::array();
    for (auto &r : rows) {
      r.data["weight"] = total > 0.0L ? r.val / total : 0.0L;
      positions.push_back(std::move(r.data));
    }

    result["users"].push_back({
        {"user", user},
        {"stable_balances", {{"usdc_e_raw", bigint_to_str(us.stable.usdc_e)}, {"wrapped_raw", bigint_to_str(us.stable.wrapped)}}},
        {"token_value_usd", token_val},
        {"stable_value_usd", stable_val},
        {"total_value_usd", total},
        {"token_value_usd_ratio", total > 0.0L ? token_val / total : 0.0L},
        {"positions", positions},
    });

    const json &snaps = state.snapshot_root.contains(user) ? state.snapshot_root.at(user) : json::object();
    json snap_rows = json::array();
    for (auto it = snaps.begin(); it != snaps.end(); ++it) {
      snap_rows.push_back({{"key", it.key()}, {"block_number", it.value().at("block_number")}});
    }
    result["history_index"][user] = {{"snapshots", snap_rows}};
  }

  long double agg_total = 0.0L;
  for (const auto &[_, b] : agg) agg_total += b.value;

  for (const auto &[token_id, b] : agg) {
    json row = {
        {"token_id", token_id},
        {"total_amount_raw", bigint_to_str(b.amount)},
        {"holder_count", b.holders},
        {"total_value_usd", b.value},
        {"weight", agg_total > 0.0L ? b.value / agg_total : 0.0L},
    };
    if (b.stable) {
      row["asset_type"] = "stable";
      row["label"] = b.label;
      row["price"] = "1.0";
    } else if (state.tokens.contains(token_id)) {
      const TokenMeta &tm = state.tokens.at(token_id);
      row["asset_type"] = "token";
      row["condition_id"] = tm.condition_id.empty() ? json(nullptr) : json(tm.condition_id);
      row["outcome_index"] = tm.outcome_index < 0 ? json(nullptr) : json(tm.outcome_index);
      row["price"] = tm.price >= 0.0L ? json(fmt_decimal(tm.price, 10)) : json(nullptr);
      if (!tm.condition_id.empty() && state.conditions.contains(tm.condition_id)) {
        const ConditionMeta &cm = state.conditions.at(tm.condition_id);
        row["market_question"] = cm.market_question;
        row["market_description"] = cm.market_description;
        if (tm.outcome_index >= 0 && static_cast<size_t>(tm.outcome_index) < cm.market_outcomes.size()) {
          row["outcome_text"] = cm.market_outcomes[static_cast<size_t>(tm.outcome_index)];
        }
      }
    }
    result["aggregate"].push_back(row);
  }

  std::sort(result["aggregate"].begin(), result["aggregate"].end(), [](const json &a, const json &b) {
    long double av = a.at("total_value_usd").get<long double>();
    long double bv = b.at("total_value_usd").get<long double>();
    return av != bv ? av > bv : a.at("token_id") < b.at("token_id");
  });

  for (auto it = state.recent_events.rbegin(); it != state.recent_events.rend(); ++it) {
    result["recent_events"].push_back(*it);
  }

  result["summary"] = {
      {"user_count", state.users.size()},
      {"position_count", position_count},
      {"token_count", state.tokens.size()},
      {"condition_count", state.conditions.size()},
      {"recent_event_count", state.recent_events.size()},
      {"snapshot_block", state.snapshot_block},
      {"last_applied_block", state.applied_block},
      {"head_block", state.head_block},
      {"behind_blocks", state.head_block >= state.applied_block ? state.head_block - state.applied_block : 0},
      {"last_resync_started_at_unix_sec", state.resync_started_at},
      {"last_resync_finished_at_unix_sec", state.resync_finished_at},
      {"query_counts", {
          {"rpc_http", state.counters.rpc_http},
          {"rpc_ws_msg", state.counters.rpc_ws_msg},
          {"rpc_ws_sub", state.counters.rpc_ws_sub},
          {"subgraph", state.counters.subgraph},
          {"gamma", state.counters.gamma},
      }},
      {"watched_users", state.users},
  };

  return result;
}

json build_meta_json(AppState &state) {
  std::lock_guard<std::mutex> lock(state.mu);

  json result = {
      {"updated_at_unix_sec", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())},
      {"tokens", json::object()},
      {"conditions", json::object()},
  };

  for (const auto &[id, t] : state.tokens) {
    result["tokens"][id] = {
        {"token_id", id},
        {"condition_id", t.condition_id.empty() ? json(nullptr) : json(t.condition_id)},
        {"question_id", t.question_id.empty() ? json(nullptr) : json(t.question_id)},
        {"outcome_index", t.outcome_index < 0 ? json(nullptr) : json(t.outcome_index)},
        {"outcome_slot_count", t.outcome_slot_count < 0 ? json(nullptr) : json(t.outcome_slot_count)},
        {"resolution_timestamp", t.resolution_timestamp < 0 ? json(nullptr) : json(t.resolution_timestamp)},
        {"payout_numerators", bigint_vec_to_json(t.payout_numerators)},
        {"payout_denominator", t.has_payout_denominator ? json(bigint_to_str(t.payout_denominator)) : json(nullptr)},
        {"price", t.price >= 0.0L ? json(fmt_decimal(t.price, 10)) : json(nullptr)},
        {"price_source", t.price_source.empty() ? json(nullptr) : json(t.price_source)},
    };
  }

  for (const auto &[id, c] : state.conditions) {
    result["conditions"][id] = {
        {"condition_id", id},
        {"question_id", c.question_id.empty() ? json(nullptr) : json(c.question_id)},
        {"outcome_slot_count", c.outcome_slot_count < 0 ? json(nullptr) : json(c.outcome_slot_count)},
        {"resolution_timestamp", c.resolution_timestamp < 0 ? json(nullptr) : json(c.resolution_timestamp)},
        {"token_ids", c.token_ids},
        {"payout_numerators", bigint_vec_to_json(c.payout_numerators)},
        {"payout_denominator", c.has_payout_denominator ? json(bigint_to_str(c.payout_denominator)) : json(nullptr)},
        {"market_question", c.market_question},
        {"market_description", c.market_description},
        {"market_event_title", c.market_event_title},
        {"market_slug", c.market_slug},
        {"market_url", c.market_url},
        {"market_outcomes", c.market_outcomes},
    };
  }

  return result;
}

json build_history_json(AppState &state, const std::string &user) {
  std::string u = norm_addr(user);
  std::lock_guard<std::mutex> lock(state.mu);
  json snaps = state.snapshot_root.contains(u) ? state.snapshot_root.at(u) : json::object();
  json events = state.history_root.contains(u) ? state.history_root.at(u) : json::object();
  return {{"user", u}, {"snapshots", snaps}, {"events", events}};
}

json build_health_json(AppState &state) {
  std::lock_guard<std::mutex> lock(state.mu);
  return {
      {"ok", true},
      {"snapshot_block", state.snapshot_block},
      {"last_applied_block", state.applied_block},
      {"head_block", state.head_block},
  };
}

} // namespace tracker
