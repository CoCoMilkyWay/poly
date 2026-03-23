#include "tracker/api.hpp"

#include "tracker/codec.hpp"
#include "tracker/log.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <cassert>
#include <thread>

namespace tracker {

// Forward declaration
json build_progress_json();

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

bool is_peer_closed(const beast::error_code &ec) {
  return ec == http::error::end_of_stream || ec == asio::error::eof ||
         ec == asio::error::connection_reset ||
         ec == asio::error::broken_pipe;
}

std::pair<std::string, std::string> split_target(const std::string &target) {
  size_t pos = target.find('?');
  if (pos == std::string::npos) {
    return {target, ""};
  }
  return {target.substr(0, pos), target.substr(pos + 1)};
}

std::string query_param(const std::string &query, const std::string &key) {
  size_t start = 0;
  while (start < query.size()) {
    size_t end = query.find('&', start);
    std::string part = query.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    size_t eq = part.find('=');
    std::string current_key =
        eq == std::string::npos ? part : part.substr(0, eq);
    if (current_key == key) {
      return eq == std::string::npos ? "" : part.substr(eq + 1);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return "";
}

std::string http_response(unsigned version,
                          unsigned status,
                          std::string_view content_type,
                          std::string_view body) {
  std::string r;
  r += "HTTP/" + std::to_string(version / 10) + "." +
       std::to_string(version % 10) + " ";
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
  std::string response = http_response(version, 200,
                                       "application/json; charset=utf-8",
                                       data.dump());
  beast::error_code ec;
  asio::write(socket, asio::buffer(response), ec);
  if (is_peer_closed(ec)) {
    return;
  }
  assert(!ec);
}

void write_empty(tcp::socket &socket, unsigned version, unsigned status) {
  std::string response =
      http_response(version, status, "text/plain; charset=utf-8", "");
  beast::error_code ec;
  asio::write(socket, asio::buffer(response), ec);
  if (is_peer_closed(ec)) {
    return;
  }
  assert(!ec);
}

const char *event_type_name(uint8_t type) {
  switch (static_cast<EventType>(type)) {
  case EventType::OrderBuy:
    return "order_buy";
  case EventType::OrderSell:
    return "order_sell";
  case EventType::Split:
    return "split";
  case EventType::Merge:
    return "merge";
  case EventType::Redeem:
    return "redeem";
  case EventType::Convert:
    return "convert";
  }
  return "unknown";
}

json stable_balances_json(const StableBalances &stable) {
  return {
      {"usdc_raw", bigint_to_str(stable.usdc)},
      {"usdc_e_raw", bigint_to_str(stable.usdc_e)},
      {"usdt_raw", bigint_to_str(stable.usdt)},
      {"wrapped_raw", bigint_to_str(stable.wrapped)},
  };
}

json payout_json(const std::vector<BigInt> &values) {
  json out = json::array();
  for (const auto &value : values) {
    out.push_back(bigint_to_i64(value));
  }
  return out;
}

struct TokenRowMeta {
  std::string condition_id;
  const ConditionMeta *condition = nullptr;
  uint8_t token_idx = 0xFF;
  int64_t price = -1;
};

TokenRowMeta resolve_token_row_meta(const RuntimeState &state,
                                    const std::string &token_id) {
  TokenRowMeta meta;
  auto token_it = state.tokens.find(token_id);
  if (token_it == state.tokens.end() || token_it->second.cond.empty()) {
    return meta;
  }
  meta.condition_id = token_it->second.cond;
  auto cond_it = state.conditions.find(meta.condition_id);
  if (cond_it == state.conditions.end()) {
    return meta;
  }
  meta.condition = &cond_it->second;
  meta.token_idx = get_token_idx(state.conditions, meta.condition_id, token_id);
  if (meta.token_idx < meta.condition->prices.size()) {
    meta.price = meta.condition->prices[meta.token_idx];
  }
  return meta;
}

void attach_token_row_meta(json &row, const TokenRowMeta &meta) {
  row["condition_id"] =
      meta.condition_id.empty() ? json(nullptr) : json(meta.condition_id);
  row["token_idx"] = meta.token_idx == 0xFF ? json(nullptr) : json(meta.token_idx);
  row["collateral"] =
      meta.condition == nullptr || meta.condition->coll == 0
          ? json(nullptr)
          : json(meta.condition->coll);
  row["price"] = meta.price < 0 ? json(nullptr) : json(meta.price);
  row["q"] = meta.condition == nullptr ? "" : meta.condition->q;
  row["desc"] = meta.condition == nullptr ? "" : meta.condition->desc;
  row["outcomes"] =
      meta.condition == nullptr ? json::array() : json(meta.condition->outcomes);
  if (meta.condition != nullptr && meta.token_idx != 0xFF &&
      meta.token_idx < meta.condition->outcomes.size()) {
    row["outcome_text"] = meta.condition->outcomes[meta.token_idx];
  }
}

json build_token_position_row(const RuntimeState &state,
                              const std::string &token_id,
                              const BigInt &amount,
                              long double value_usd) {
  TokenRowMeta meta = resolve_token_row_meta(state, token_id);
  json row = {
      {"asset_type", "token"},
      {"token_id", token_id},
      {"amount_raw", bigint_to_str(amount)},
      {"value_usd", value_usd},
      {"settled", meta.condition != nullptr && is_settled(*meta.condition)},
  };
  attach_token_row_meta(row, meta);
  return row;
}

json build_stable_position_row(const char *token_id,
                               const char *label,
                               Collateral collateral,
                               const BigInt &amount) {
  return {
      {"asset_type", "stable"},
      {"token_id", token_id},
      {"label", label},
      {"condition_id", nullptr},
      {"token_idx", nullptr},
      {"collateral", to_u8(collateral)},
      {"amount_raw", bigint_to_str(amount)},
      {"price", kPriceScale},
      {"value_usd", bigint_to_units(amount)},
      {"q", label},
      {"desc", ""},
      {"outcomes", json::array()},
      {"settled", false},
  };
}

json build_aggregate_row(const RuntimeState &state,
                         const std::string &token_id,
                         const AggregateTokenState &bucket) {
  TokenRowMeta meta = resolve_token_row_meta(state, token_id);
  json row = {
      {"token_id", token_id},
      {"amount_raw", bigint_to_str(bucket.amount)},
      {"holder_count", bucket.holder_count},
      {"value_usd", bucket.value_usd},
      {"aggregate_weight",
       state.aggregate_value_usd > 0.0L
           ? bucket.value_usd / state.aggregate_value_usd
           : 0.0L},
      {"asset_type", "token"},
  };
  attach_token_row_meta(row, meta);
  return row;
}

json enrich_event(json row, const RuntimeState &state) {
  uint8_t type = row.contains("type") && row.at("type").is_number_integer()
                     ? row.at("type").get<uint8_t>()
                     : 0;
  row["type_name"] = event_type_name(type);
  uint8_t collateral = row.contains("collateral") &&
                               row.at("collateral").is_number_integer()
                           ? row.at("collateral").get<uint8_t>()
                           : 0;
  row["collateral_label"] =
      collateral_label(static_cast<Collateral>(collateral));

  if (!row.contains("condition_id") || !row.at("condition_id").is_string()) {
    return row;
  }

  const std::string condition_id = row.at("condition_id").get<std::string>();
  auto cond_it = state.conditions.find(condition_id);
  if (cond_it == state.conditions.end()) {
    return row;
  }

  const ConditionMeta &condition = cond_it->second;
  row["q"] = condition.q;
  row["desc"] = condition.desc;
  row["outcomes"] = condition.outcomes;

  uint8_t token_idx = row.contains("token_idx") &&
                              row.at("token_idx").is_number_integer()
                          ? row.at("token_idx").get<uint8_t>()
                          : 0xFF;
  if (token_idx != 0xFF && token_idx < condition.outcomes.size()) {
    row["outcome_text"] = condition.outcomes[token_idx];
  }

  return row;
}

void handle_request(AppState &state,
                    ApiThread::ResyncCallback on_resync,
                    tcp::socket socket) {
  beast::flat_buffer buf;
  http::request<http::string_body> req;
  beast::error_code ec;
  http::read(socket, buf, req, ec);
  if (is_peer_closed(ec)) {
    return;
  }
  assert(!ec);

  if (req.method() == http::verb::options) {
    write_empty(socket, req.version(), 204);
    return;
  }

  auto [path, query] = split_target(std::string(req.target()));
  if (req.method() == http::verb::get && (path == "/" || path == "/api/state")) {
    write_json(socket, req.version(), *load_published(state.state_ptr));
    return;
  }
  if (req.method() == http::verb::get && path == "/api/meta") {
    write_json(socket, req.version(), *load_published(state.meta_ptr));
    return;
  }
  if (req.method() == http::verb::get && path == "/api/history") {
    std::string user = query_param(query, "user");
    assert(!user.empty());
    auto snapshot_root = load_published(state.snapshot_ptr);
    auto history_root = load_published(state.history_ptr);
    write_json(socket, req.version(),
               build_history_json(*snapshot_root, *history_root, user));
    return;
  }
  if (req.method() == http::verb::get && path == "/api/health") {
    write_json(socket, req.version(),
               build_health_json(*load_published(state.state_ptr)));
    return;
  }
  if (req.method() == http::verb::get && path == "/api/progress") {
    write_json(socket, req.version(), build_progress_json());
    return;
  }
  if (req.method() == http::verb::post && path == "/api/resync") {
    if (on_resync) {
      on_resync();
    }
    write_json(socket, req.version(), json{{"ok", true}});
    return;
  }
  if (req.method() == http::verb::get && path == "/api/events") {
    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n\r\n";
    beast::error_code ec;
    asio::write(socket, asio::buffer(header), ec);
    if (is_peer_closed(ec))
      return;

    uint64_t last_version = 0;
    while (true) {
      uint64_t current = state.version.load();
      if (current != last_version) {
        last_version = current;
        std::string event = "data: " + std::to_string(current) + "\n\n";
        asio::write(socket, asio::buffer(event), ec);
        if (is_peer_closed(ec))
          return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  if (req.method() == http::verb::get && path == "/api/progress/stream") {
    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: keep-alive\r\n\r\n";
    beast::error_code ec;
    asio::write(socket, asio::buffer(header), ec);
    if (is_peer_closed(ec))
      return;

    std::string last_data;
    while (true) {
      std::string current_data = build_progress_json().dump();
      if (current_data != last_data) {
        last_data = current_data;
        std::string event = "data: " + current_data + "\n\n";
        asio::write(socket, asio::buffer(event), ec);
        if (is_peer_closed(ec))
          return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }

  assert(false);
}

} // namespace

ApiThread::ApiThread(const AppConfig &cfg,
                     AppState &state,
                     ResyncCallback on_resync)
    : cfg_(cfg), state_(state), on_resync_(std::move(on_resync)) {}

void ApiThread::start() {
  assert(!running_);
  running_ = true;
  thread_ = std::thread([this] { run(); });
}

void ApiThread::stop() {
  running_ = false;
  if (thread_.joinable()) {
    thread_.detach();
  }
}

void ApiThread::run() {
  asio::io_context ioc;
  tcp::acceptor acceptor(
      ioc, tcp::endpoint(asio::ip::make_address(cfg_.backend_host),
                         cfg_.backend_port));
  while (running_) {
    tcp::socket socket = acceptor.accept();
    std::thread(handle_request, std::ref(state_), on_resync_, std::move(socket))
        .detach();
  }
}

// ============================================================================
// JSON Builders
// ============================================================================

json build_progress_json() {
  json result = json::array();
  auto &board = progress();
  for (size_t i = 0; i < ProgressBoard::kApiCount; ++i) {
    const auto &api = board.apis[i];
    result.push_back({
        {"name", ProgressBoard::kApiNames[i]},
        {"data",
         {
             {"done", api.data.done.load()},
             {"total", api.data.total.load()},
         }},
        {"query",
         {
             {"pending", api.query.pending.load()},
             {"total", api.query.total.load()},
         }},
    });
  }
  return result;
}

json build_state_json(const RuntimeState &state) {
  json result = {
      {"summary", json::object()},
      {"users", json::array()},
      {"aggregate", json::array()},
      {"recent_events", json::array()},
  };

  uint64_t min_snapshot_block = 0;
  bool have_snapshot_block = false;
  size_t position_count = 0;

  for (const auto &user : state.users) {
    const UserLiveState &live = state.user_states.at(user);
    const UserSnapshotState &snapshot = state.user_snapshots.at(user);
    const UserViewState &view = state.user_views.at(user);
    if (!have_snapshot_block || snapshot.snapshot_block < min_snapshot_block) {
      min_snapshot_block = snapshot.snapshot_block;
      have_snapshot_block = true;
    }
    position_count += view.raw_position_count;

    struct PositionRow {
      long double value_usd = 0.0L;
      json row;
    };
    std::vector<PositionRow> rows;
    for (const auto &[token_id, visible] : view.visible_tokens) {
      rows.push_back({
          .value_usd = visible.value_usd,
          .row = build_token_position_row(state, token_id, visible.amount,
                                          visible.value_usd),
      });
    }

    auto push_stable_row = [&](const char *token_id,
                               const char *label,
                               Collateral collateral,
                               const BigInt &amount) {
      if (amount == 0) {
        return;
      }
      rows.push_back({
          .value_usd = bigint_to_units(amount),
          .row = build_stable_position_row(token_id, label, collateral, amount),
      });
    };
    push_stable_row("stable:usdc", "USDC", Collateral::USDC, live.stable.usdc);
    push_stable_row("stable:usdc_e", "USDC.e", Collateral::USDCe, live.stable.usdc_e);
    push_stable_row("stable:usdt", "USDT", Collateral::USDT, live.stable.usdt);
    push_stable_row("stable:wrapped", "WrappedUSDCe", Collateral::WrappedUSDCe,
                    live.stable.wrapped);

    std::sort(rows.begin(), rows.end(), [](const PositionRow &a, const PositionRow &b) {
      if (a.value_usd != b.value_usd) {
        return a.value_usd > b.value_usd;
      }
      return a.row.at("token_id").get<std::string>() <
             b.row.at("token_id").get<std::string>();
    });

    json positions = json::array();
    for (auto &row : rows) {
      row.row["weight"] = view.total_value_usd > 0.0L
                              ? row.value_usd / view.total_value_usd
                              : 0.0L;
      positions.push_back(row.row);
    }

    result["users"].push_back({
        {"user", user},
        {"snapshot_block", snapshot.snapshot_block},
        {"stable_balances", stable_balances_json(live.stable)},
        {"token_value_usd", view.token_value_usd},
        {"stable_value_usd", view.stable_value_usd},
        {"total_value_usd", view.total_value_usd},
        {"qualifies_for_aggregate", view.qualifies_for_aggregate},
        {"positions", positions},
        {"filtered_dust_count", view.filtered_dust_count},
        {"filtered_settled_count", view.filtered_settled_count},
    });
  }

  long double aggregate_threshold =
      kAggregateValueThreshold * state.aggregate_value_usd;
  for (const auto &[token_id, bucket] : state.aggregate_tokens) {
    if (bucket.value_usd <= aggregate_threshold) {
      continue;
    }
    result["aggregate"].push_back(build_aggregate_row(state, token_id, bucket));
  }

  std::sort(result["aggregate"].begin(), result["aggregate"].end(),
            [](const json &a, const json &b) {
              long double av = a.at("value_usd").get<long double>();
              long double bv = b.at("value_usd").get<long double>();
              if (av != bv) {
                return av > bv;
              }
              return a.at("token_id").get<std::string>() <
                     b.at("token_id").get<std::string>();
            });

  for (auto it = state.recent_events.rbegin(); it != state.recent_events.rend();
       ++it) {
    result["recent_events"].push_back(enrich_event(*it, state));
  }

  json snapshot_blocks = json::object();
  for (const auto &user : state.users) {
    snapshot_blocks[user] = state.user_snapshots.at(user).snapshot_block;
  }

  result["summary"] = {
      {"min_snapshot_block", have_snapshot_block ? json(min_snapshot_block)
                                                 : json(0)},
      {"snapshot_blocks", snapshot_blocks},
      {"last_applied_block", state.last_applied_block},
      {"head_block", state.head_block},
      {"behind_blocks",
       state.head_block >= state.last_applied_block
           ? state.head_block - state.last_applied_block
           : 0},
      {"user_count", state.users.size()},
      {"position_count", position_count},
      {"token_count", state.tokens.size()},
      {"condition_count", state.conditions.size()},
      {"recent_event_count", state.recent_events.size()},
      {"last_resync_started_at_unix_sec", state.resync_started_at},
      {"last_resync_finished_at_unix_sec", state.resync_finished_at},
      {"ws_blocks", state.counters.ws_blocks},
      {"ws_logs", state.counters.ws_logs},
      {"query_counts",
       {
           {"rpc_http", state.counters.rpc_http},
           {"rpc_ws_msg", state.counters.rpc_ws_msg},
           {"rpc_ws_sub", state.counters.rpc_ws_sub},
           {"snapshot_api", state.counters.snapshot_api},
           {"gamma", state.counters.gamma},
           {"clob", state.counters.clob},
       }},
  };

  return result;
}

json build_meta_json(const RuntimeState &state) {
  json result = {
      {"updated_at_unix_sec", now_unix_sec()},
      {"tokens", json::object()},
      {"conditions", json::object()},
      {"markets", json::object()},
  };

  // tokens: token_id → condition_id 映射
  for (const auto &[token_id, token] : state.tokens) {
    result["tokens"][token_id] = token.cond.empty() ? json(nullptr) : json(token.cond);
  }

  for (const auto &[condition_id, condition] : state.conditions) {
    result["conditions"][condition_id] = {
        // 身份标识
        {"qid", condition.qid.empty() ? json(nullptr) : json(condition.qid)},
        {"oc", condition.oc == 0 ? json(nullptr) : json(condition.oc)},
        {"coll", condition.coll == 0 ? json(nullptr) : json(condition.coll)},
        {"tids", condition.tids},
        // 市场信息
        {"q", condition.q},
        {"desc", condition.desc},
        {"slug", condition.slug},
        {"outcomes", condition.outcomes},
        // 价格
        {"prices", condition.prices},
        {"price_ts", condition.price_ts},
        // 时间
        {"start", condition.start.empty() ? json(nullptr) : json(condition.start)},
        {"end", condition.end.empty() ? json(nullptr) : json(condition.end)},
        // 结算
        {"payout", payout_json(condition.payout)},
        {"payout_d", condition.has_payout_d ? json(bigint_to_i64(condition.payout_d))
                                            : json(nullptr)},
        // 状态
        {"updated", condition.updated ? 1 : 0},
    };
  }

  for (const auto &[market_id, market] : state.markets) {
    result["markets"][market_id] = {{"qids", market.qids}};
  }

  return result;
}

json build_history_json(const json &snapshot_root,
                        const json &history_root,
                        const std::string &user) {
  std::string normalized = norm_addr(user);
  json snapshots =
      snapshot_root.contains(normalized) ? snapshot_root.at(normalized)
                                         : json::object();
  json events = history_root.contains(normalized) ? history_root.at(normalized)
                                                  : json::object();
  return {
      {"user", normalized},
      {"snapshots", snapshots},
      {"events", events},
  };
}

json build_health_json(const json &state_root) {
  json summary =
      state_root.contains("summary") ? state_root.at("summary") : json::object();
  return {
      {"ok", true},
      {"summary", summary},
  };
}

} // namespace tracker
