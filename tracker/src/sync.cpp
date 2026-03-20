#include "tracker/sync.hpp"

#include "tracker/api.hpp"
#include "tracker/filter.hpp"
#include "tracker/http.hpp"
#include "tracker/log.hpp"
#include "tracker/store.hpp"

#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace tracker {
namespace {

const char *kUserPositionsQuery = R"(
query UserPositions($user: String!, $after: String!, $first: Int!) {
  _meta { block { number } }
  userPositions(
    first: $first
    orderBy: id
    orderDirection: asc
    where: {user: $user, amount_gt: "0", id_gt: $after}
  ) {
    id
    tokenId
    amount
  }
}
)";

const std::string &zero_b32() {
  static const std::string value = "0x" + std::string(64, '0');
  return value;
}

std::string clip_text(const std::string &s, size_t n = 256) {
  if (s.size() <= n) {
    return s;
  }
  return s.substr(0, n) + "...";
}

int64_t scaled_price(const BigInt &quote_amount, const BigInt &token_amount) {
  assert(token_amount > 0);
  return bigint_to_i64((quote_amount * kPriceScale) / token_amount);
}

void apply_resolved_prices(RuntimeState &state, const std::string &condition_id) {
  auto cond_it = state.conditions.find(condition_id);
  if (cond_it == state.conditions.end()) {
    return;
  }
  ConditionMeta &condition = cond_it->second;
  if (!condition.has_payout_d || condition.payout_d == 0) {
    return;
  }
  size_t n = std::max(condition.tids.size(), condition.payout.size());
  if (condition.prices.size() < n) {
    condition.prices.resize(n, -1);
  }
  if (condition.price_ts.size() < n) {
    condition.price_ts.resize(n, 0);
  }
  int64_t ts = now_unix_sec();
  for (size_t i = 0; i < condition.payout.size(); ++i) {
    condition.prices[i] = scaled_price(condition.payout[i], condition.payout_d);
    condition.price_ts[i] = ts;
  }
  // 确保 token 映射存在
  for (size_t i = 0; i < condition.tids.size(); ++i) {
    const std::string &token_id = condition.tids[i];
    if (!token_id.empty()) {
      TokenMeta &token = state.tokens[token_id];
      token.cond = condition_id;
    }
  }
}

Collateral infer_collateral_from_token(const std::string &condition_id,
                                       uint8_t token_idx,
                                       const std::string &token_id) {
  for (Collateral collateral :
       {Collateral::USDC, Collateral::USDCe, Collateral::USDT,
        Collateral::WrappedUSDCe}) {
    if (condition_token_id(condition_id, collateral_addr(collateral), token_idx) ==
        norm_hex(token_id)) {
      return collateral;
    }
  }
  return Collateral::Unknown;
}

json graph_data_with_retry(RuntimeState &state,
                           const std::string &name,
                           const std::string &detail,
                           const std::string &url,
                           const json &payload,
                           const std::string &proxy_url,
                           std::optional<HttpRes> first_resp = std::nullopt) {
  HttpRes resp = first_resp ? *first_resp : http_post(url, payload, proxy_url);
  for (size_t attempt = 1;; ++attempt) {
    ++state.counters.subgraph;
    if (resp.status == 200) {
      json body = safe_parse(resp.body);
      if (!body.contains("errors") && body.contains("data")) {
        log_query("graph", name, attempt, true, detail);
        return body.at("data");
      }
      log_query("graph", name, attempt, false,
                detail + " body=" + clip_text(body.dump()));
    } else {
      log_query("graph", name, attempt, false,
                detail + " status=" + std::to_string(resp.status));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    resp = http_post(url, payload, proxy_url);
  }
}

struct TransferLeg {
  uint64_t block_number = 0;
  uint64_t transaction_index = 0;
  std::string tx_hash;
  int64_t log_index = 0;
  std::string op;
  std::string from;
  std::string to;
  std::string token_id;
  BigInt amount = 0;
};

struct TxContext {
  uint64_t block_number = 0;
  uint64_t transaction_index = 0;
  std::string tx_hash;
  std::vector<json> raw_logs;
};

struct PendingEmit {
  std::string user;
  std::string token_id;
  std::string condition_id;
  uint8_t token_idx = 0xFF;
  uint8_t collateral = 0;
  EventType type = EventType::OrderBuy;
  int64_t amount = 0;
  int64_t price = 0;
};

struct SnapshotFetch {
  std::string user;
  uint64_t snapshot_block = 0;
  std::map<std::string, BigInt> positions;
  std::string cursor;
  bool done = false;
};

TransferLeg parse_transfer_single(const json &log) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  return {
      .block_number = hex_to_u64(log.at("blockNumber").get<std::string>()),
      .transaction_index =
          hex_to_u64(log.at("transactionIndex").get<std::string>()),
      .tx_hash = norm_hex(log.at("transactionHash").get<std::string>()),
      .log_index = static_cast<int64_t>(
          hex_to_u64(log.at("logIndex").get<std::string>()) *
          kTransferFlatLogScale),
      .op = topic_to_addr(topics.at(1).get<std::string>()),
      .from = topic_to_addr(topics.at(2).get<std::string>()),
      .to = topic_to_addr(topics.at(3).get<std::string>()),
      .token_id = bigint_to_str(extract_u256(data, 0)),
      .amount = extract_u256(data, 1),
  };
}

std::vector<TransferLeg> parse_transfer_batch(const json &log) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  std::vector<BigInt> ids = extract_u256_array(data, extract_u256(data, 0));
  std::vector<BigInt> values = extract_u256_array(data, extract_u256(data, 1));
  assert(ids.size() == values.size());

  uint64_t block_number = hex_to_u64(log.at("blockNumber").get<std::string>());
  uint64_t transaction_index =
      hex_to_u64(log.at("transactionIndex").get<std::string>());
  std::string tx_hash = norm_hex(log.at("transactionHash").get<std::string>());
  uint64_t raw_log_index = hex_to_u64(log.at("logIndex").get<std::string>());
  std::string op = topic_to_addr(topics.at(1).get<std::string>());
  std::string from = topic_to_addr(topics.at(2).get<std::string>());
  std::string to = topic_to_addr(topics.at(3).get<std::string>());

  std::vector<TransferLeg> out;
  for (size_t i = 0; i < ids.size(); ++i) {
    out.push_back({
        .block_number = block_number,
        .transaction_index = transaction_index,
        .tx_hash = tx_hash,
        .log_index = static_cast<int64_t>(raw_log_index * kTransferFlatLogScale +
                                          i),
        .op = op,
        .from = from,
        .to = to,
        .token_id = bigint_to_str(ids[i]),
        .amount = values[i],
    });
  }
  return out;
}

std::vector<TxContext> build_tx_contexts(const std::vector<json> &logs) {
  std::map<std::string, TxContext> txs;
  for (const auto &log : logs) {
    const std::string tx_hash =
        norm_hex(log.at("transactionHash").get<std::string>());
    TxContext &ctx = txs[tx_hash];
    if (ctx.tx_hash.empty()) {
      ctx.tx_hash = tx_hash;
      ctx.block_number = hex_to_u64(log.at("blockNumber").get<std::string>());
      ctx.transaction_index =
          hex_to_u64(log.at("transactionIndex").get<std::string>());
    }
    ctx.raw_logs.push_back(log);
  }

  std::vector<TxContext> out;
  for (auto &[_, ctx] : txs) {
    std::sort(ctx.raw_logs.begin(), ctx.raw_logs.end(),
              [](const json &a, const json &b) {
                return raw_log_sort_key(a) < raw_log_sort_key(b);
              });
    out.push_back(std::move(ctx));
  }
  std::sort(out.begin(), out.end(), [](const TxContext &a, const TxContext &b) {
    if (a.block_number != b.block_number) {
      return a.block_number < b.block_number;
    }
    return a.transaction_index < b.transaction_index;
  });
  return out;
}

std::string op_key_from_log(const json &log) {
  return std::to_string(hex_to_u64(log.at("blockNumber").get<std::string>())) +
         "|" + norm_hex(log.at("transactionHash").get<std::string>()) + "|" +
         std::to_string(hex_to_u64(log.at("logIndex").get<std::string>())) +
         "|" + norm_hex(log.at("address").get<std::string>());
}

void commit_pending_events(RuntimeState &state,
                           const json &root_log,
                           const std::vector<PendingEmit> &events,
                           size_t recent_limit,
                           const std::function<bool(const std::string &, uint64_t)>
                               &visible_at) {
  const uint64_t block_number =
      hex_to_u64(root_log.at("blockNumber").get<std::string>());
  const int64_t log_index =
      static_cast<int64_t>(hex_to_u64(root_log.at("logIndex").get<std::string>()));
  const std::string op_key = op_key_from_log(root_log);

  std::map<std::string, int64_t> next_leg;
  for (const auto &event : events) {
    if (!visible_at(event.user, block_number)) {
      continue;
    }

    int64_t leg_index = next_leg[event.user]++;
    std::string event_id =
        op_key + "|" + event.user + "|" + std::to_string(leg_index);
    if (!state.history_event_ids.insert(event_id).second) {
      continue;
    }

    BigInt delta = bigint_from_dec(std::to_string(event.amount));
    UserLiveState &user_state = state.user_states.at(event.user);
    if (delta >= 0) {
      user_state.positions[event.token_id] += delta;
    } else {
      BigInt current = 0;
      auto current_it = user_state.positions.find(event.token_id);
      if (current_it != user_state.positions.end()) {
        current = current_it->second;
      }
      assert(current >= -delta);
      BigInt next = current + delta;
      if (next == 0) {
        user_state.positions.erase(event.token_id);
      } else {
        user_state.positions[event.token_id] = next;
      }
    }

    json row = {
        {"event_id", event_id},
        {"op_key", op_key},
        {"log_index", log_index},
        {"leg_index", leg_index},
        {"type", to_u8(event.type)},
        {"condition_id", event.condition_id},
        {"token_idx", event.token_idx},
        {"collateral", event.collateral},
        {"amount", event.amount},
        {"price", event.price},
    };
    json &bucket = state.history_root[event.user][block_key(block_number)];
    if (!bucket.is_array()) {
      bucket = json::array();
    }
    bucket.push_back(row);

    json recent = row;
    recent["user"] = event.user;
    recent["block_number"] = block_number;
    push_recent_event(state, std::move(recent), recent_limit);
  }
}

} // namespace

SyncThread::SyncThread(const AppConfig &cfg,
                       AppState &shared,
                       EventQueue &queue,
                       WsThread &ws)
    : cfg_(cfg), shared_(shared), queue_(queue), ws_(ws) {
  resync_flag_ = true;
}

void SyncThread::request_resync() {
  resync_flag_ = true;
}

void SyncThread::run() {
  logger().init(cfg_.log_file);
  load_seed();
  load_files();
  publish_all();

  auto next_resync = std::chrono::steady_clock::now();
  while (true) {
    if (resync_flag_.exchange(false) ||
        std::chrono::steady_clock::now() >= next_resync) {
      full_resync();
      next_resync = std::chrono::steady_clock::now() +
                    std::chrono::seconds(cfg_.resync_interval_sec);
      continue;
    }
    drain_queue();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void SyncThread::full_resync() {
  progress().init();
  rt_.resync_started_at = now_unix_sec();

  // Clear history state - new snapshot will have positions up to snapshot_block,
  // and we only want to track events from snapshot_block+1 onwards
  rt_.history_root = json::object();
  rt_.history_event_ids.clear();
  rt_.recent_events.clear();

  // [a] snapshot + [b] stables
  fetch_user_snapshots();
  fetch_snapshot_balances();
  append_snapshot_roots();
  persist_snapshot(); // 阶段完成,立即落地 S

  // [c] meta (仅 updated=0)
  std::vector<std::string> token_ids = collect_active_token_ids();
  progress()[API::meta].total = token_ids.size();
  progress().stage("meta");
  fetch_gamma_by_token_ids(token_ids);

  // [d] prices (仅 price_ts 过期)
  progress()[API::prices].total = token_ids.size();
  progress().stage("prices");
  refresh_prices(token_ids);
  persist_meta(); // 阶段完成,立即落地 M

  // [e] ws_sub
  queue_.clear();
  deferred_.clear();
  progress()[API::ws_sub].total = rt_.users.size();
  progress().stage("ws_sub");
  WsSessionInfo ws_session = ws_.start_session(rt_.users);
  current_session_id_ = ws_session.session_id;
  progress()[API::ws_sub].done = rt_.users.size();
  progress().flush();

  // [f] head
  progress().stage("head");
  progress()[API::head].total = 1;
  uint64_t head_block = std::max(ws_session.start_block, rpc_block_number());
  rt_.head_block = std::max(rt_.head_block, head_block);
  progress()[API::head].done = 1;

  // [g] backfill
  uint64_t from_block = head_block + 1;
  for (const auto &user : rt_.users) {
    uint64_t user_from = rt_.user_snapshots.at(user).snapshot_block + 1;
    if (user_from < from_block) {
      from_block = user_from;
    }
  }
  if (from_block <= head_block) {
    backfill_range(from_block, head_block);
  } else {
    rt_.last_applied_block = head_block;
  }

  handle_overlap_queue(ws_session.session_id, head_block);
  rt_.last_applied_block = std::max(rt_.last_applied_block, head_block);
  rt_.resync_finished_at = now_unix_sec();

  publish_all();
  progress().finish();
  logger().info("resync done");
}

void SyncThread::drain_queue() {
  while (true) {
    if (!deferred_.empty()) {
      QueueEvent ev = std::move(deferred_.front());
      deferred_.pop_front();
      handle_queue_event(std::move(ev));
      continue;
    }
    auto ev = queue_.try_pop();
    if (!ev) {
      break;
    }
    handle_queue_event(std::move(*ev));
  }
}

void SyncThread::handle_queue_event(QueueEvent ev) {
  if (ev.session_id != current_session_id_) {
    return;
  }
  if (ev.kind == QueueEventKind::Resync) {
    resync_flag_ = true;
    return;
  }
  if (ev.kind == QueueEventKind::Head) {
    rt_.head_block = std::max(rt_.head_block, ev.block_number);
    publish_all();
    return;
  }
  if (ev.kind == QueueEventKind::Logs) {
    std::vector<json> logs;
    for (const auto &log : ev.logs) {
      logs.push_back(log);
    }
    std::sort(logs.begin(), logs.end(), [](const json &a, const json &b) {
      return raw_log_sort_key(a) < raw_log_sort_key(b);
    });
    apply_block_logs(logs);
    rt_.last_applied_block = std::max(rt_.last_applied_block, ev.block_number);
    rt_.head_block = std::max(rt_.head_block, ev.block_number);
    publish_all();
    persist_history(); // ws 增量落地 H
    return;
  }
  assert(false);
}

void SyncThread::handle_overlap_queue(uint64_t session_id, uint64_t overlap_block) {
  std::map<uint64_t, std::map<std::string, json>> overlap;
  while (auto ev = queue_.try_pop()) {
    if (ev->session_id != session_id) {
      continue;
    }
    if (ev->kind == QueueEventKind::Resync) {
      resync_flag_ = true;
      continue;
    }
    if (ev->kind == QueueEventKind::Head) {
      rt_.head_block = std::max(rt_.head_block, ev->block_number);
      if (ev->block_number > overlap_block) {
        deferred_.push_back(std::move(*ev));
      }
      continue;
    }
    if (ev->kind == QueueEventKind::Logs) {
      if (ev->block_number > overlap_block) {
        deferred_.push_back(std::move(*ev));
        continue;
      }
      for (const auto &log : ev->logs) {
        overlap[ev->block_number][raw_log_key(log)] = log;
      }
    }
  }

  for (auto &[block_number, by_key] : overlap) {
    std::vector<json> logs;
    for (auto &[_, log] : by_key) {
      logs.push_back(std::move(log));
    }
    std::sort(logs.begin(), logs.end(), [](const json &a, const json &b) {
      return raw_log_sort_key(a) < raw_log_sort_key(b);
    });
    apply_block_logs(logs);
    rt_.last_applied_block = std::max(rt_.last_applied_block, block_number);
  }
}

void SyncThread::load_files() {
  json meta = load_json(cfg_.meta_file);
  // tokens: token_id → condition_id 映射
  if (meta.contains("tokens") && meta.at("tokens").is_object()) {
    for (auto it = meta.at("tokens").begin(); it != meta.at("tokens").end(); ++it) {
      if (it.value().is_string()) {
        rt_.tokens[it.key()].cond = it.value().get<std::string>();
      }
    }
  }
  if (meta.contains("conditions") && meta.at("conditions").is_object()) {
    for (auto it = meta.at("conditions").begin(); it != meta.at("conditions").end();
         ++it) {
      ConditionMeta condition;
      condition.qid = json_str(it.value(), "qid");
      int oc = json_int(it.value(), "oc", 0);
      condition.oc = oc <= 0 ? 0 : static_cast<uint8_t>(oc);
      int coll = json_int(it.value(), "coll", 0);
      condition.coll = coll <= 0 ? 0 : static_cast<uint8_t>(coll);
      condition.tids = json_str_arr(it.value(), "tids");
      condition.prices = json_i64_arr(it.value(), "prices");
      condition.price_ts = json_i64_arr(it.value(), "price_ts");
      condition.start = json_str(it.value(), "start");
      condition.end = json_str(it.value(), "end");
      condition.payout = json_bigint_arr(it.value(), "payout");
      if (it.value().contains("payout_d") && !it.value().at("payout_d").is_null()) {
        if (it.value().at("payout_d").is_string()) {
          condition.payout_d =
              bigint_from_dec(it.value().at("payout_d").get<std::string>());
        } else {
          condition.payout_d =
              bigint_from_dec(std::to_string(it.value().at("payout_d").get<int64_t>()));
        }
        condition.has_payout_d = true;
      }
      condition.q = json_str(it.value(), "q");
      condition.desc = json_str(it.value(), "desc");
      condition.slug = json_str(it.value(), "slug");
      condition.outcomes = json_str_arr(it.value(), "outcomes");
      condition.updated = json_int(it.value(), "updated", 0) != 0;
      merge_condition(rt_.conditions[it.key()], condition);
    }
  }
  if (meta.contains("markets") && meta.at("markets").is_object()) {
    for (auto it = meta.at("markets").begin(); it != meta.at("markets").end(); ++it) {
      MarketMeta market;
      market.qids = json_str_arr(it.value(), "qids");
      merge_market(rt_.markets[it.key()], market);
    }
  }

  rt_.snapshot_root = load_json(cfg_.snapshot_file);
  rt_.history_root = load_json(cfg_.history_file);

  struct RecentRow {
    uint64_t block_number = 0;
    int64_t log_index = 0;
    json row;
  };
  std::vector<RecentRow> recent_rows;
  if (rt_.history_root.is_object()) {
    for (auto user_it = rt_.history_root.begin(); user_it != rt_.history_root.end();
         ++user_it) {
      const std::string user = user_it.key();
      if (!user_it.value().is_object()) {
        continue;
      }
      for (auto block_it = user_it.value().begin(); block_it != user_it.value().end();
           ++block_it) {
        uint64_t block_number = std::stoull(block_it.key());
        if (!block_it.value().is_array()) {
          continue;
        }
        for (const auto &event : block_it.value()) {
          if (event.contains("event_id") && event.at("event_id").is_string()) {
            rt_.history_event_ids.insert(event.at("event_id").get<std::string>());
          }
          json recent = event;
          recent["user"] = user;
          recent["block_number"] = block_number;
          recent_rows.push_back(
              {block_number, json_i64(event, "log_index", 0), std::move(recent)});
        }
      }
    }
  }
  std::sort(recent_rows.begin(), recent_rows.end(),
            [](const RecentRow &a, const RecentRow &b) {
              if (a.block_number != b.block_number) {
                return a.block_number < b.block_number;
              }
              return a.log_index < b.log_index;
            });
  for (const auto &recent : recent_rows) {
    push_recent_event(rt_, recent.row, cfg_.recent_event_limit);
  }
  // 不再从 aggregate.json 恢复进度, full_resync 会重算
}

void SyncThread::load_seed() {
  if (!std::filesystem::exists(cfg_.seed_file)) {
    return;
  }
  json seed = load_json(cfg_.seed_file);
  if (!seed.is_object()) {
    return;
  }

  // tokens: token_id → condition_id 映射
  if (seed.contains("tokens") && seed.at("tokens").is_object()) {
    for (auto it = seed.at("tokens").begin(); it != seed.at("tokens").end(); ++it) {
      if (it.value().is_string()) {
        rt_.tokens[it.key()].cond = it.value().get<std::string>();
      }
    }
  }

  auto load_condition_row = [&](const std::string &condition_id, const json &row) {
    ConditionMeta condition;
    condition.qid = json_str(row, "qid");
    int oc = json_int(row, "oc", 0);
    condition.oc = oc <= 0 ? 0 : static_cast<uint8_t>(oc);
    int coll = json_int(row, "coll", 0);
    condition.coll = coll <= 0 ? 0 : static_cast<uint8_t>(coll);
    condition.tids = json_str_arr(row, "tids");
    condition.prices = json_i64_arr(row, "prices");
    condition.price_ts = json_i64_arr(row, "price_ts");
    condition.start = json_str(row, "start");
    condition.end = json_str(row, "end");
    condition.payout = json_bigint_arr(row, "payout");
    if (row.contains("payout_d") && !row.at("payout_d").is_null()) {
      condition.payout_d = json_bigint(row, "payout_d");
      condition.has_payout_d = true;
    }
    condition.q = json_str(row, "q");
    condition.desc = json_str(row, "desc");
    condition.slug = json_str(row, "slug");
    condition.outcomes = json_str_arr(row, "outcomes");
    condition.updated = json_int(row, "updated", 0) != 0;
    merge_condition(rt_.conditions[condition_id], condition);
  };

  if (seed.contains("conditions") && seed.at("conditions").is_object()) {
    for (auto it = seed.at("conditions").begin(); it != seed.at("conditions").end();
         ++it) {
      load_condition_row(it.key(), it.value());
    }
  }

  if (seed.contains("markets") && seed.at("markets").is_object()) {
    for (auto it = seed.at("markets").begin(); it != seed.at("markets").end(); ++it) {
      MarketMeta market;
      market.qids = json_str_arr(it.value(), "qids");
      merge_market(rt_.markets[it.key()], market);
    }
  }
}

void SyncThread::publish_all() {
  WsCounters ws_counters = ws_.counters();
  rt_.counters.rpc_ws_msg = ws_counters.msg;
  rt_.counters.rpc_ws_sub = ws_counters.sub;
  publish_json(shared_.state_ptr, build_state_json(rt_));
  publish_json(shared_.meta_ptr, build_meta_json(rt_));
  publish_json(shared_.snapshot_ptr, rt_.snapshot_root);
  publish_json(shared_.history_ptr, rt_.history_root);
  ++shared_.version;
}

void SyncThread::persist_snapshot() {
  save_json(cfg_.snapshot_file, rt_.snapshot_root);
}

void SyncThread::persist_meta() {
  publish_json(shared_.meta_ptr, build_meta_json(rt_));
  save_json(cfg_.meta_file, *load_published(shared_.meta_ptr));
}

void SyncThread::persist_history() {
  save_json(cfg_.history_file, rt_.history_root);
}

void SyncThread::fetch_user_snapshots() {
  std::vector<std::string> users = load_addr_file(cfg_.address_file);
  auto &pa = progress()[API::snapshot];
  pa.total = users.size();
  progress().stage("snapshot");

  rt_.users = users;
  rt_.user_set.clear();
  rt_.user_snapshots.clear();
  rt_.user_states.clear();
  stale_users_.clear();

  const int64_t now = now_unix_sec();
  size_t cached_count = 0;

  // [a] try_reuse_snapshots: 检查现有 snapshot 是否可复用
  for (const auto &user : users) {
    rt_.user_set.insert(user);
    bool cached = false;

    if (rt_.snapshot_root.contains(user) && rt_.snapshot_root.at(user).is_object()) {
      // 找最新的 snapshot (最大 block_number)
      const json &user_snapshots = rt_.snapshot_root.at(user);
      std::string best_block_key;
      int64_t best_captured_at = 0;
      for (auto it = user_snapshots.begin(); it != user_snapshots.end(); ++it) {
        if (!it.value().is_object())
          continue;
        int64_t captured = json_i64(it.value(), "captured_at_unix_sec", 0);
        if (captured > best_captured_at) {
          best_captured_at = captured;
          best_block_key = it.key();
        }
      }

      // 判断是否在有效期内
      if (!best_block_key.empty() &&
          now - best_captured_at < static_cast<int64_t>(cfg_.resync_interval_sec)) {
        const json &snap = user_snapshots.at(best_block_key);
        uint64_t block_num = static_cast<uint64_t>(json_i64(snap, "block_number", 0));

        // 恢复 positions
        std::map<std::string, BigInt> positions;
        if (snap.contains("positions") && snap.at("positions").is_array()) {
          for (const auto &pos : snap.at("positions")) {
            std::string tid = json_str(pos, "token_id");
            if (!tid.empty()) {
              positions[tid] = bigint_from_dec(json_str(pos, "amount_raw"));
            }
          }
        }

        // 恢复 stables
        StableBalances stable;
        if (snap.contains("stable_balances") && snap.at("stable_balances").is_object()) {
          const json &sb = snap.at("stable_balances");
          stable.usdc = bigint_from_dec(json_str(sb, "usdc_raw"));
          stable.usdc_e = bigint_from_dec(json_str(sb, "usdc_e_raw"));
          stable.usdt = bigint_from_dec(json_str(sb, "usdt_raw"));
          stable.wrapped = bigint_from_dec(json_str(sb, "wrapped_raw"));
        }

        rt_.user_snapshots[user] = {
            .snapshot_block = block_num,
            .stable = stable,
            .positions = positions,
        };
        rt_.user_states[user] = {
            .user = user,
            .stable = stable,
            .positions = positions,
        };
        cached = true;
        ++cached_count;
      }
    }

    if (!cached) {
      stale_users_.push_back(user);
      // 先初始化空状态,后续从 Graph 填充
      rt_.user_snapshots[user] = {};
      rt_.user_states[user] = {.user = user, .stable = {}, .positions = {}};
    }
  }

  pa.done = cached_count;
  progress().flush();

  // [a'] fetch_user_snapshots: 仅对 stale_users 从 Graph 抓取
  if (!stale_users_.empty()) {
    std::vector<SnapshotFetch> snapshots;
    for (const auto &user : stale_users_) {
      snapshots.push_back({
          .user = user,
          .snapshot_block = 0,
          .positions = {},
          .cursor = "",
          .done = false,
      });
    }

    std::string url = "https://gateway.thegraph.com/api/" + cfg_.graph_api_key +
                      "/subgraphs/id/" + std::string(kPnlSubgraphId);
    size_t done_count = 0;
    while (done_count < snapshots.size()) {
      std::vector<HttpReq> reqs;
      std::vector<size_t> refs;
      for (size_t i = 0; i < snapshots.size(); ++i) {
        if (snapshots[i].done) {
          continue;
        }
        json variables = {
            {"user", snapshots[i].user},
            {"after", snapshots[i].cursor},
            {"first", static_cast<int>(cfg_.graph_page_limit)},
        };
        reqs.push_back({
            .url = url,
            .method = "POST",
            .body =
                json{{"query", kUserPositionsQuery}, {"variables", variables}}.dump(),
        });
        refs.push_back(i);
      }
      pa.pending = reqs.size();
      progress().flush();
      auto responses = http_batch(reqs, cfg_.http_concurrency, cfg_.proxy_url);
      pa.pending = 0;
      progress().flush();
      for (size_t i = 0; i < responses.size(); ++i) {
        SnapshotFetch &snapshot = snapshots[refs[i]];
        json variables = {
            {"user", snapshot.user},
            {"after", snapshot.cursor},
            {"first", static_cast<int>(cfg_.graph_page_limit)},
        };
        json data = graph_data_with_retry(
            rt_, "userPositions",
            "user=" + snapshot.user + " after=" + snapshot.cursor, url,
            json{{"query", kUserPositionsQuery}, {"variables", variables}},
            cfg_.proxy_url, responses[i]);
        if (snapshot.snapshot_block == 0) {
          snapshot.snapshot_block = static_cast<uint64_t>(
              std::stoull(json_str_or_int(data.at("_meta").at("block").at("number"))));
        }
        const json &rows = data.at("userPositions");
        for (const auto &row : rows) {
          std::string token_id = row.at("tokenId").get<std::string>();
          // 跳过负数 token_id（graph subgraph 返回的溢出值）
          if (token_id.empty() || token_id[0] == '-') {
            continue;
          }
          snapshot.positions[token_id] +=
              bigint_from_dec(json_str_or_int(row.at("amount")));
        }
        if (rows.size() < cfg_.graph_page_limit) {
          snapshot.done = true;
          ++done_count;
          pa.done = cached_count + done_count;
          progress().flush();
        } else {
          snapshot.cursor = rows.back().at("id").get<std::string>();
        }
      }
    }

    // 更新 stale_users 的 snapshot
    for (const auto &snapshot : snapshots) {
      rt_.user_snapshots[snapshot.user] = {
          .snapshot_block = snapshot.snapshot_block,
          .stable = {},
          .positions = snapshot.positions,
      };
      rt_.user_states[snapshot.user] = {
          .user = snapshot.user,
          .stable = {},
          .positions = snapshot.positions,
      };
    }
  }

  // 计算 min_snapshot_block
  uint64_t min_snapshot_block = 0;
  bool have_min_snapshot_block = false;
  for (const auto &user : users) {
    uint64_t block = rt_.user_snapshots.at(user).snapshot_block;
    if (!have_min_snapshot_block || block < min_snapshot_block) {
      min_snapshot_block = block;
      have_min_snapshot_block = true;
    }
  }
  assert(have_min_snapshot_block);
  rt_.last_applied_block = min_snapshot_block;
  rt_.head_block = std::max(rt_.head_block, min_snapshot_block);
}

void SyncThread::fetch_snapshot_balances() {
  auto &pb = progress()[API::stables];
  pb.total = stale_users_.size() * 4; // 仅 stale_users 需要获取余额
  progress().stage("stables");

  if (stale_users_.empty()) {
    pb.done = 0;
    progress().flush();
    return;
  }

  std::vector<json> reqs;
  struct BalanceRef {
    std::string user;
    Collateral collateral = Collateral::Unknown;
  };
  std::vector<BalanceRef> refs;

  const std::string selector = "0x70a08231";
  for (const auto &user : stale_users_) {
    const uint64_t block = rt_.user_snapshots.at(user).snapshot_block;
    const std::string block_tag = u64_to_hex(block);
    const std::string data = selector + std::string(24, '0') + strip_0x(user);
    auto push_call = [&](const char *token_addr, Collateral collateral) {
      reqs.push_back({
          {"method", "eth_call"},
          {"params",
           json::array({json{{"to", token_addr}, {"data", data}}, block_tag})},
      });
      refs.push_back({user, collateral});
    };
    push_call(kUsdc, Collateral::USDC);
    push_call(kUsdcE, Collateral::USDCe);
    push_call(kUsdt, Collateral::USDT);
    push_call(kWrappedUsdcE, Collateral::WrappedUSDCe);
  }

  pb.pending = reqs.size();
  progress().flush();
  json responses = rpc_batch(reqs);
  pb.pending = 0;
  progress().flush();
  for (size_t i = 0; i < refs.size(); ++i) {
    BigInt balance =
        bigint_from_hex(responses.at(i).at("result").get<std::string>());
    UserSnapshotState &snapshot = rt_.user_snapshots.at(refs[i].user);
    UserLiveState &live = rt_.user_states.at(refs[i].user);
    switch (refs[i].collateral) {
    case Collateral::USDC:
      snapshot.stable.usdc = balance;
      live.stable.usdc = balance;
      break;
    case Collateral::USDCe:
      snapshot.stable.usdc_e = balance;
      live.stable.usdc_e = balance;
      break;
    case Collateral::USDT:
      snapshot.stable.usdt = balance;
      live.stable.usdt = balance;
      break;
    case Collateral::WrappedUSDCe:
      snapshot.stable.wrapped = balance;
      live.stable.wrapped = balance;
      break;
    case Collateral::Unknown:
      assert(false);
    }
    pb.done = i + 1;
    progress().flush();
  }
}

void SyncThread::append_snapshot_roots() {
  // 仅更新 stale_users 的 snapshot,保留 cached users 的旧数据
  const int64_t now = now_unix_sec();
  for (const auto &user : stale_users_) {
    const UserSnapshotState &snapshot = rt_.user_snapshots.at(user);
    json positions = json::array();
    for (const auto &[token_id, amount] : snapshot.positions) {
      positions.push_back({
          {"token_id", token_id},
          {"amount_raw", bigint_to_str(amount)},
      });
    }
    rt_.snapshot_root[user][block_key(snapshot.snapshot_block)] = {
        {"block_number", snapshot.snapshot_block},
        {"captured_at_unix_sec", now},
        {"stable_balances",
         {
             {"usdc_raw", bigint_to_str(snapshot.stable.usdc)},
             {"usdc_e_raw", bigint_to_str(snapshot.stable.usdc_e)},
             {"usdt_raw", bigint_to_str(snapshot.stable.usdt)},
             {"wrapped_raw", bigint_to_str(snapshot.stable.wrapped)},
         }},
        {"positions", positions},
    };
  }
}

std::vector<std::string> SyncThread::collect_active_token_ids() const {
  std::set<std::string> token_ids;
  for (const auto &user : rt_.users) {
    const UserLiveState &live = rt_.user_states.at(user);
    for (const auto &[token_id, _] : live.positions) {
      // 过滤掉负数 token_id（graph subgraph 数据源本身的问题）
      if (!token_id.empty() && token_id[0] != '-') {
        token_ids.insert(token_id);
      }
    }
  }
  return {token_ids.begin(), token_ids.end()};
}

void SyncThread::fetch_gamma_by_token_ids(const std::vector<std::string> &token_ids) {
  auto &pc = progress()[API::meta];
  if (token_ids.empty()) {
    return;
  }

  // 过滤掉已 updated 的 condition 对应的 token
  std::vector<std::string> pending;
  for (const auto &tid : token_ids) {
    auto tok_it = rt_.tokens.find(tid);
    if (tok_it != rt_.tokens.end() && !tok_it->second.cond.empty()) {
      auto cond_it = rt_.conditions.find(tok_it->second.cond);
      if (cond_it != rt_.conditions.end() && cond_it->second.updated) {
        continue; // 跳过已更新
      }
    }
    pending.push_back(tid);
  }

  std::vector<std::string> unique = pending;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  pc.total = token_ids.size();
  pc.done = token_ids.size() - unique.size(); // 已跳过的计入 done

  if (unique.empty()) {
    return;
  }

  // 按 gamma_batch_limit 分 chunk,使用重复参数格式 clob_token_ids=x&clob_token_ids=y
  std::vector<std::vector<std::string>> chunks = chunked(unique, cfg_.gamma_batch_limit);
  std::vector<HttpReq> reqs;
  for (const auto &chunk : chunks) {
    std::string params = "limit=" + std::to_string(chunk.size());
    for (const auto &tid : chunk) {
      params += "&clob_token_ids=" + tid;
    }
    reqs.push_back({
        .url = std::string(kGammaApiBase) + "/markets?" + params,
        .method = "GET",
        .body = "",
    });
  }

  // 跟踪每个 chunk 的结果
  std::vector<std::optional<json>> chunk_results(chunks.size());
  std::vector<size_t> pending_chunk_indices;
  for (size_t i = 0; i < chunks.size(); ++i) {
    pending_chunk_indices.push_back(i);
  }

  // 并发请求 + 并发重试
  size_t done_count = 0;
  for (size_t attempt = 1; !pending_chunk_indices.empty(); ++attempt) {
    std::vector<HttpReq> batch_reqs;
    for (size_t idx : pending_chunk_indices) {
      batch_reqs.push_back(reqs[idx]);
    }

    pc.pending = batch_reqs.size();
    progress().flush();
    auto responses = http_batch(batch_reqs, cfg_.http_concurrency, cfg_.proxy_url);
    pc.pending = 0;
    progress().flush();
    assert(responses.size() == pending_chunk_indices.size());

    std::vector<size_t> still_pending;
    for (size_t i = 0; i < responses.size(); ++i) {
      size_t idx = pending_chunk_indices[i];
      const auto &resp = responses[i];
      ++rt_.counters.gamma;

      if (resp.status == 200) {
        json body = safe_parse(resp.body);
        if (body.is_array()) {
          log_query("gamma", "markets/tokens", attempt, true, "n=" + std::to_string(chunks[idx].size()));
          chunk_results[idx] = std::move(body);
          done_count += chunks[idx].size();
          pc.done = done_count;
          progress().flush();
          continue;
        }
        log_query("gamma", "markets/tokens", attempt, false,
                  "n=" + std::to_string(chunks[idx].size()) + " body=" + clip_text(body.dump()));
      } else {
        log_query("gamma", "markets/tokens", attempt, false,
                  "n=" + std::to_string(chunks[idx].size()) + " status=" + std::to_string(resp.status));
      }
      still_pending.push_back(idx);
    }

    pending_chunk_indices = std::move(still_pending);
    if (!pending_chunk_indices.empty()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // 处理所有结果
  for (size_t chunk_idx = 0; chunk_idx < chunks.size(); ++chunk_idx) {
    const auto &chunk = chunks[chunk_idx];
    const auto &arr_opt = chunk_results[chunk_idx];
    json arr = (arr_opt && arr_opt->is_array()) ? *arr_opt : json::array();

    for (const auto &token_id : chunk) {
      // 在返回的 array 中找到匹配的 market (通过 clobTokenIds 匹配)
      json market = json::object();
      for (const auto &item : arr) {
        std::string clob_token_ids_str = json_str(item, "clobTokenIds");
        if (clob_token_ids_str.empty())
          continue;
        json clob_token_ids = safe_parse(clob_token_ids_str);
        if (!clob_token_ids.is_array())
          continue;
        for (size_t i = 0; i < clob_token_ids.size(); ++i) {
          if (clob_token_ids[i].is_string() && clob_token_ids[i].get<std::string>() == token_id) {
            market = item;
            market["_matched_idx"] = i; // 记录 token 在数组中的位置 (即 idx)
            break;
          }
        }
        if (!market.empty())
          break;
      }

      if (market.empty()) {
        // Gamma 中找不到此 token,标记 cond="?" 避免重复查询
        if (rt_.tokens[token_id].cond.empty()) {
          rt_.tokens[token_id].cond = "?";
        }
        continue;
      }

      // 提取 condition_id
      std::string condition_id = json_str(market, "conditionId");
      if (condition_id.empty()) {
        condition_id = json_str(market, "condition_id");
      }
      if (condition_id.empty()) {
        continue;
      }
      condition_id = norm_hex(condition_id);

      // 更新 token → condition 映射
      rt_.tokens[token_id].cond = condition_id;

      // 更新 condition
      ConditionMeta condition;
      condition.qid = json_str(market, "questionId");
      if (condition.qid.empty()) {
        condition.qid = json_str(market, "question_id");
      }

      // 从 clobTokenIds 提取 tids 和 outcome_count
      std::string clob_token_ids_str = json_str(market, "clobTokenIds");
      json clob_token_ids = safe_parse(clob_token_ids_str);
      if (clob_token_ids.is_array()) {
        condition.oc = static_cast<uint8_t>(clob_token_ids.size());
        for (size_t i = 0; i < clob_token_ids.size(); ++i) {
          if (clob_token_ids[i].is_string()) {
            std::string tid = clob_token_ids[i].get<std::string>();
            if (condition.tids.size() <= i) {
              condition.tids.resize(i + 1);
            }
            condition.tids[i] = tid;
            rt_.tokens[tid].cond = condition_id;
          }
        }
      }

      // 提取 outcomePrices
      std::string outcome_prices_str = json_str(market, "outcomePrices");
      json outcome_prices = safe_parse(outcome_prices_str);
      if (outcome_prices.is_array()) {
        int64_t ts = now_unix_sec();
        for (size_t i = 0; i < outcome_prices.size(); ++i) {
          if (condition.prices.size() <= i) {
            condition.prices.resize(i + 1, -1);
            condition.price_ts.resize(i + 1, 0);
          }
          if (outcome_prices[i].is_string()) {
            double p = std::stod(outcome_prices[i].get<std::string>());
            condition.prices[i] = static_cast<int64_t>(p * 1e6);
            condition.price_ts[i] = ts;
          }
        }
      }

      // 提取 question/desc/slug/outcomes/start/end
      json events = market.contains("events") && market.at("events").is_array()
                        ? market.at("events")
                        : json::array();
      json event0 = events.empty() ? json::object() : events.front();
      condition.q = json_str(market, "question");
      if (condition.q.empty()) {
        condition.q = json_str(event0, "title");
      }
      condition.desc = json_str(market, "description");
      if (condition.desc.empty()) {
        condition.desc = json_str(event0, "description");
      }
      condition.slug = json_str(event0, "slug");
      if (condition.slug.empty()) {
        condition.slug = json_str(market, "slug");
      }
      condition.start = json_str(event0, "startDate");
      if (condition.start.empty()) {
        condition.start = json_str(market, "startDate");
      }
      condition.end = json_str(event0, "endDate");
      if (condition.end.empty()) {
        condition.end = json_str(market, "endDate");
      }
      if (market.contains("outcomes")) {
        json outcomes = market.at("outcomes");
        if (outcomes.is_string()) {
          outcomes = safe_parse(outcomes.get<std::string>());
        }
        if (outcomes.is_array()) {
          for (const auto &outcome : outcomes) {
            if (outcome.is_string()) {
              condition.outcomes.push_back(outcome.get<std::string>());
            }
          }
        }
      }

      // 标记为已从 gamma 成功更新
      condition.updated = true;

      merge_condition(rt_.conditions[condition_id], condition);

      // 推断 collateral
      ConditionMeta &merged = rt_.conditions[condition_id];
      if (merged.coll == 0 && !merged.tids.empty()) {
        merged.coll = to_u8(infer_collateral_from_token(condition_id, 0, merged.tids[0]));
      }

      apply_resolved_prices(rt_, condition_id);
    }
  }
}

void SyncThread::refresh_prices(const std::vector<std::string> &token_ids) {
  auto &pc = progress()[API::prices];
  if (token_ids.empty()) {
    return;
  }

  // 筛选 price_ts 过期的 token
  int64_t now = now_unix_sec();
  std::vector<std::string> stale;
  for (const auto &tid : token_ids) {
    auto tok_it = rt_.tokens.find(tid);
    if (tok_it == rt_.tokens.end() || tok_it->second.cond.empty() ||
        tok_it->second.cond == "?") {
      continue;
    }
    auto cond_it = rt_.conditions.find(tok_it->second.cond);
    if (cond_it == rt_.conditions.end()) {
      continue;
    }
    // 找到 token 在 condition 中的 index
    const auto &tids = cond_it->second.tids;
    size_t idx = std::find(tids.begin(), tids.end(), tid) - tids.begin();
    if (idx >= tids.size()) {
      continue;
    }
    // 检查 price_ts 是否过期
    int64_t ts = (idx < cond_it->second.price_ts.size())
                     ? cond_it->second.price_ts[idx]
                     : 0;
    if (now - ts > static_cast<int64_t>(cfg_.resync_interval_sec)) {
      stale.push_back(tid);
    }
  }

  std::vector<std::string> unique = stale;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  pc.total = token_ids.size();
  pc.done = token_ids.size() - unique.size();

  if (unique.empty()) {
    return;
  }

  // 按 kClobBatchLimit 分 chunk
  std::vector<std::vector<std::string>> chunks = chunked(unique, kClobBatchLimit);
  std::vector<HttpReq> reqs;
  for (const auto &chunk : chunks) {
    // 构建 POST body: [{"token_id":"xxx","side":"BUY"},...]
    json arr = json::array();
    for (const auto &tid : chunk) {
      arr.push_back({{"token_id", tid}, {"side", "BUY"}});
    }
    reqs.push_back({
        .url = std::string(kClobApiBase) + "/prices",
        .method = "POST",
        .body = arr.dump(),
    });
  }

  std::vector<std::optional<json>> chunk_results(chunks.size());
  std::vector<size_t> pending_indices;
  for (size_t i = 0; i < chunks.size(); ++i) {
    pending_indices.push_back(i);
  }

  size_t done_count = pc.done;
  for (size_t attempt = 1; !pending_indices.empty(); ++attempt) {
    std::vector<HttpReq> batch_reqs;
    for (size_t idx : pending_indices) {
      batch_reqs.push_back(reqs[idx]);
    }

    pc.pending = batch_reqs.size();
    progress().flush();
    auto responses = http_batch(batch_reqs, cfg_.http_concurrency, cfg_.proxy_url);
    pc.pending = 0;
    progress().flush();
    assert(responses.size() == pending_indices.size());

    std::vector<size_t> still_pending;
    for (size_t i = 0; i < responses.size(); ++i) {
      size_t idx = pending_indices[i];
      const auto &resp = responses[i];
      ++rt_.counters.clob;

      if (resp.status == 200) {
        json body = safe_parse(resp.body);
        if (body.is_object() && !body.contains("error")) {
          log_query("clob", "prices", attempt, true,
                    "n=" + std::to_string(chunks[idx].size()));
          chunk_results[idx] = std::move(body);
          done_count += chunks[idx].size();
          pc.done = done_count;
          progress().flush();
          continue;
        }
        log_query("clob", "prices", attempt, false,
                  "n=" + std::to_string(chunks[idx].size()) +
                      " body=" + clip_text(body.dump()));
      } else {
        log_query("clob", "prices", attempt, false,
                  "n=" + std::to_string(chunks[idx].size()) +
                      " status=" + std::to_string(resp.status));
      }
      still_pending.push_back(idx);
    }

    pending_indices = std::move(still_pending);
    if (!pending_indices.empty()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // 处理结果: {"tid1":{"BUY":"0.45"},...}
  int64_t ts = now_unix_sec();
  for (size_t chunk_idx = 0; chunk_idx < chunks.size(); ++chunk_idx) {
    const auto &result_opt = chunk_results[chunk_idx];
    if (!result_opt || !result_opt->is_object()) {
      continue;
    }
    const json &result = *result_opt;

    for (const auto &tid : chunks[chunk_idx]) {
      if (!result.contains(tid)) {
        continue;
      }
      const json &price_obj = result.at(tid);
      if (!price_obj.is_object() || !price_obj.contains("BUY")) {
        continue;
      }
      std::string price_str = price_obj.at("BUY").get<std::string>();
      double price = std::stod(price_str);
      int64_t price_scaled = static_cast<int64_t>(price * 1e6);

      // 更新 condition.prices[idx]
      auto tok_it = rt_.tokens.find(tid);
      if (tok_it == rt_.tokens.end()) {
        continue;
      }
      auto cond_it = rt_.conditions.find(tok_it->second.cond);
      if (cond_it == rt_.conditions.end()) {
        continue;
      }
      const auto &tids = cond_it->second.tids;
      size_t idx = std::find(tids.begin(), tids.end(), tid) - tids.begin();
      if (idx >= tids.size()) {
        continue;
      }
      ConditionMeta &cond = cond_it->second;
      if (cond.prices.size() <= idx) {
        cond.prices.resize(idx + 1, -1);
        cond.price_ts.resize(idx + 1, 0);
      }
      cond.prices[idx] = price_scaled;
      cond.price_ts[idx] = ts;
    }
  }
}

void SyncThread::fetch_gamma_by_condition_ids(const std::vector<std::string> &condition_ids) {
  if (condition_ids.empty()) {
    return;
  }

  // 过滤掉已 updated 的 condition
  std::vector<std::string> pending;
  for (const auto &cid : condition_ids) {
    auto it = rt_.conditions.find(cid);
    if (it != rt_.conditions.end() && it->second.updated) {
      continue;
    }
    pending.push_back(cid);
  }

  std::vector<std::string> unique = pending;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

  if (unique.empty()) {
    return;
  }

  // 按 gamma_batch_limit 分 chunk
  std::vector<std::vector<std::string>> chunks = chunked(unique, cfg_.gamma_batch_limit);
  std::vector<HttpReq> reqs;
  for (const auto &chunk : chunks) {
    std::string params = "limit=" + std::to_string(chunk.size());
    for (const auto &cid : chunk) {
      params += "&condition_ids=" + strip_0x(cid);
    }
    reqs.push_back({
        .url = std::string(kGammaApiBase) + "/markets?" + params,
        .method = "GET",
        .body = "",
    });
  }

  std::vector<std::optional<json>> chunk_results(chunks.size());
  std::vector<size_t> pending_chunk_indices;
  for (size_t i = 0; i < chunks.size(); ++i) {
    pending_chunk_indices.push_back(i);
  }

  for (size_t attempt = 1; !pending_chunk_indices.empty(); ++attempt) {
    std::vector<HttpReq> batch_reqs;
    for (size_t idx : pending_chunk_indices) {
      batch_reqs.push_back(reqs[idx]);
    }

    auto responses = http_batch(batch_reqs, cfg_.http_concurrency, cfg_.proxy_url);
    assert(responses.size() == pending_chunk_indices.size());

    std::vector<size_t> still_pending;
    for (size_t i = 0; i < responses.size(); ++i) {
      size_t idx = pending_chunk_indices[i];
      const auto &resp = responses[i];
      ++rt_.counters.gamma;

      if (resp.status == 200) {
        json body = safe_parse(resp.body);
        if (body.is_array()) {
          log_query("gamma", "markets/conds", attempt, true, "n=" + std::to_string(chunks[idx].size()));
          chunk_results[idx] = std::move(body);
          continue;
        }
        log_query("gamma", "markets/conds", attempt, false,
                  "n=" + std::to_string(chunks[idx].size()) + " body=" + clip_text(body.dump()));
      } else {
        log_query("gamma", "markets/conds", attempt, false,
                  "n=" + std::to_string(chunks[idx].size()) + " status=" + std::to_string(resp.status));
      }
      still_pending.push_back(idx);
    }

    pending_chunk_indices = std::move(still_pending);
    if (!pending_chunk_indices.empty()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }

  // 处理所有结果
  for (size_t chunk_idx = 0; chunk_idx < chunks.size(); ++chunk_idx) {
    const auto &chunk = chunks[chunk_idx];
    const auto &arr_opt = chunk_results[chunk_idx];
    json arr = (arr_opt && arr_opt->is_array()) ? *arr_opt : json::array();

    for (const auto &condition_id : chunk) {
      json market = json::object();
      for (const auto &item : arr) {
        std::string current = item.contains("conditionId")
                                  ? json_str(item, "conditionId")
                                  : json_str(item, "condition_id");
        if (!current.empty() && norm_hex(current) == norm_hex(condition_id)) {
          market = item;
          break;
        }
      }

      if (market.empty()) {
        continue;
      }

      ConditionMeta condition;
      condition.qid = json_str(market, "questionId");
      if (condition.qid.empty()) {
        condition.qid = json_str(market, "question_id");
      }

      // 从 clobTokenIds 提取 tids
      std::string clob_token_ids_str = json_str(market, "clobTokenIds");
      json clob_token_ids = safe_parse(clob_token_ids_str);
      if (clob_token_ids.is_array()) {
        condition.oc = static_cast<uint8_t>(clob_token_ids.size());
        for (size_t i = 0; i < clob_token_ids.size(); ++i) {
          if (clob_token_ids[i].is_string()) {
            std::string tid = clob_token_ids[i].get<std::string>();
            if (condition.tids.size() <= i) {
              condition.tids.resize(i + 1);
            }
            condition.tids[i] = tid;
            rt_.tokens[tid].cond = condition_id;
          }
        }
      }

      // 提取 outcomePrices
      std::string outcome_prices_str = json_str(market, "outcomePrices");
      json outcome_prices = safe_parse(outcome_prices_str);
      if (outcome_prices.is_array()) {
        int64_t ts = now_unix_sec();
        for (size_t i = 0; i < outcome_prices.size(); ++i) {
          if (condition.prices.size() <= i) {
            condition.prices.resize(i + 1, -1);
            condition.price_ts.resize(i + 1, 0);
          }
          if (outcome_prices[i].is_string()) {
            double p = std::stod(outcome_prices[i].get<std::string>());
            condition.prices[i] = static_cast<int64_t>(p * 1e6);
            condition.price_ts[i] = ts;
          }
        }
      }

      // 提取 question/desc/slug/outcomes/start/end
      json events = market.contains("events") && market.at("events").is_array()
                        ? market.at("events")
                        : json::array();
      json event0 = events.empty() ? json::object() : events.front();
      condition.q = json_str(market, "question");
      if (condition.q.empty()) {
        condition.q = json_str(event0, "title");
      }
      condition.desc = json_str(market, "description");
      if (condition.desc.empty()) {
        condition.desc = json_str(event0, "description");
      }
      condition.slug = json_str(event0, "slug");
      if (condition.slug.empty()) {
        condition.slug = json_str(market, "slug");
      }
      condition.start = json_str(event0, "startDate");
      if (condition.start.empty()) {
        condition.start = json_str(market, "startDate");
      }
      condition.end = json_str(event0, "endDate");
      if (condition.end.empty()) {
        condition.end = json_str(market, "endDate");
      }
      if (market.contains("outcomes")) {
        json outcomes = market.at("outcomes");
        if (outcomes.is_string()) {
          outcomes = safe_parse(outcomes.get<std::string>());
        }
        if (outcomes.is_array()) {
          for (const auto &outcome : outcomes) {
            if (outcome.is_string()) {
              condition.outcomes.push_back(outcome.get<std::string>());
            }
          }
        }
      }

      condition.updated = true;
      merge_condition(rt_.conditions[condition_id], condition);

      // 推断 collateral
      ConditionMeta &merged = rt_.conditions[condition_id];
      if (merged.coll == 0 && !merged.tids.empty()) {
        merged.coll = to_u8(infer_collateral_from_token(condition_id, 0, merged.tids[0]));
      }
      apply_resolved_prices(rt_, condition_id);
    }
  }
}

void SyncThread::fetch_gamma_market_questions(const std::string &market_id) {
  // NegRisk market_id 查询流程:
  // 1. market_id → first_question_id = market_id[0:31] + "00"
  // 2. Gamma /markets?question_ids={first_question_id} → 获取 slug
  // 3. Gamma /events?slug={slug} → 获取所有 markets[].questionID

  // Step 1: 构建第一个 question_id
  std::string first_question_id = build_negrisk_question_id(market_id, 0);

  // Step 2: 查询第一个 market 获取 slug
  std::string url1 = std::string(kGammaApiBase) + "/markets?question_ids=" + strip_0x(first_question_id);
  std::string slug;
  for (size_t attempt = 1;; ++attempt) {
    HttpRes resp = http_get(url1, cfg_.proxy_url);
    ++rt_.counters.gamma;
    if (resp.status == 200) {
      json body = safe_parse(resp.body);
      if (body.is_array() && !body.empty()) {
        json events = body[0].contains("events") && body[0].at("events").is_array()
                          ? body[0].at("events")
                          : json::array();
        json event0 = events.empty() ? json::object() : events.front();
        slug = json_str(event0, "slug");
        if (slug.empty()) {
          slug = json_str(body[0], "slug");
        }
        if (!slug.empty()) {
          log_query("gamma", "markets/qid", attempt, true, "market_id=" + market_id);
          break;
        }
      }
      log_query("gamma", "markets/qid", attempt, false, "market_id=" + market_id + " no_slug");
    } else {
      log_query("gamma", "markets/qid", attempt, false,
                "market_id=" + market_id + " status=" + std::to_string(resp.status));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  // Step 3: 通过 slug 查询 event 获取所有 markets
  std::string url2 = std::string(kGammaApiBase) + "/events?slug=" + slug;
  json event_data;
  for (size_t attempt = 1;; ++attempt) {
    HttpRes resp = http_get(url2, cfg_.proxy_url);
    ++rt_.counters.gamma;
    if (resp.status == 200) {
      json body = safe_parse(resp.body);
      if (body.is_array() && !body.empty()) {
        event_data = body[0];
        log_query("gamma", "events/slug", attempt, true, "slug=" + slug);
        break;
      }
      log_query("gamma", "events/slug", attempt, false, "slug=" + slug + " empty");
    } else {
      log_query("gamma", "events/slug", attempt, false,
                "slug=" + slug + " status=" + std::to_string(resp.status));
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  // 从 event_data.markets 提取所有 questionID
  MarketMeta market;
  if (event_data.contains("markets") && event_data.at("markets").is_array()) {
    for (const auto &m : event_data.at("markets")) {
      std::string qid = json_str(m, "questionID");
      if (qid.empty()) {
        qid = json_str(m, "question_id");
      }
      if (!qid.empty()) {
        qid = norm_hex(qid);
        market.qids.push_back(qid);

        // 同时创建 condition
        std::string condition_id = build_negrisk_condition_id(qid);
        ConditionMeta condition;
        condition.qid = qid;
        condition.oc = 2;
        condition.coll = to_u8(Collateral::WrappedUSDCe);
        if (condition.tids.size() < 2) {
          condition.tids.resize(2);
        }
        merge_condition(rt_.conditions[condition_id], condition);
      }
    }
  }

  merge_market(rt_.markets[market_id], market);
}

void SyncThread::ensure_token_meta(const std::string &token_id) {
  auto it = rt_.tokens.find(token_id);
  if (it != rt_.tokens.end()) {
    // cond="?" 表示已查询但 Gamma 中不存在,跳过重复查询
    if (it->second.cond == "?") {
      return;
    }
    if (!it->second.cond.empty()) {
      auto cond_it = rt_.conditions.find(it->second.cond);
      if (cond_it != rt_.conditions.end() && cond_it->second.updated) {
        return;
      }
    }
  }
  // 使用 Gamma API 一步获取 token + condition 元数据
  fetch_gamma_by_token_ids({token_id});
  it = rt_.tokens.find(token_id);
  if (it == rt_.tokens.end() || it->second.cond.empty() || it->second.cond == "?") {
    logger().warn("token_meta incomplete token_id=" + token_id);
    return;
  }
}

void SyncThread::ensure_condition_meta(const std::string &condition_id,
                                       Collateral hint_collateral) {
  ConditionMeta &condition = rt_.conditions[condition_id];
  if (hint_collateral != Collateral::Unknown && condition.coll == 0) {
    condition.coll = to_u8(hint_collateral);
  }
  // 使用 Gamma API 一步获取 condition 元数据 (包括 tids, q, outcomes)
  if (condition.oc == 0 || condition.tids.empty() || condition.q.empty()) {
    fetch_gamma_by_condition_ids({condition_id});
  }
  if (hint_collateral != Collateral::Unknown && condition.coll == 0) {
    condition.coll = to_u8(hint_collateral);
  }
  assert(condition.oc > 0);
}

void SyncThread::ensure_market_questions(const std::string &market_id) {
  auto it = rt_.markets.find(market_id);
  if (it != rt_.markets.end() && !it->second.qids.empty()) {
    return;
  }
  // 使用 Gamma API 获取 market 的所有 question_ids
  fetch_gamma_market_questions(market_id);
  it = rt_.markets.find(market_id);
  assert(it != rt_.markets.end());
  assert(!it->second.qids.empty());
}

void SyncThread::backfill_range(uint64_t from_block, uint64_t to_block) {
  if (from_block > to_block) {
    return;
  }
  auto &pe = progress()[API::backfill];
  pe.total = to_block - from_block + 1;
  progress().stage("backfill");

  uint64_t start = from_block;
  while (start <= to_block) {
    uint64_t end = std::min(to_block, start + cfg_.get_logs_block_span - 1);
    auto filters = build_user_log_filters(rt_.users, cfg_.topic_group_size, start, end);
    std::vector<json> reqs;
    for (const auto &filter : filters) {
      reqs.push_back({
          {"method", "eth_getLogs"},
          {"params", json::array({filter})},
      });
    }
    pe.pending = reqs.size();
    progress().flush();
    json responses = rpc_batch(reqs);
    pe.pending = 0;
    progress().flush();
    std::map<uint64_t, std::map<std::string, json>> blocks;
    for (const auto &response : responses) {
      assert(response.contains("result") && response.at("result").is_array());
      for (const auto &log : response.at("result")) {
        blocks[hex_to_u64(log.at("blockNumber").get<std::string>())]
              [raw_log_key(log)] = log;
      }
    }

    for (auto &[block_number, deduped] : blocks) {
      std::vector<json> logs;
      for (auto &[_, log] : deduped) {
        logs.push_back(std::move(log));
      }
      std::sort(logs.begin(), logs.end(), [](const json &a, const json &b) {
        return raw_log_sort_key(a) < raw_log_sort_key(b);
      });
      apply_block_logs(logs);
      rt_.last_applied_block = std::max(rt_.last_applied_block, block_number);
      rt_.head_block = std::max(rt_.head_block, block_number);
    }

    rt_.last_applied_block = std::max(rt_.last_applied_block, end);
    rt_.head_block = std::max(rt_.head_block, end);
    pe.done = end - from_block + 1;
    persist_history(); // 每批完成,立即落地 H
    progress().flush();
    start = end + 1;
  }
}

void SyncThread::apply_block_logs(const std::vector<json> &logs) {
  auto txs = build_tx_contexts(logs);
  for (const auto &tx : txs) {
    for (const auto &log : tx.raw_logs) {
      const std::string address = norm_hex(log.at("address").get<std::string>());
      const std::string topic0 =
          norm_hex(log.at("topics").at(0).get<std::string>());
      if (address == kConditionalTokens && topic0 == kConditionResolveTopic) {
        apply_condition_resolution(log);
      } else if (address == kConditionalTokens && topic0 == kPositionSplitTopic) {
        apply_split(log);
      } else if (address == kConditionalTokens && topic0 == kPositionMergeTopic) {
        apply_merge(log);
      } else if (address == kConditionalTokens && topic0 == kPositionRedeemTopic) {
        apply_redeem(log);
      } else if ((address == kCtfExchange || address == kNegRiskCtfExchange) &&
                 topic0 == kOrderFillTopic) {
        apply_order_fill(log);
      } else if (address == kNegRiskAdapter && topic0 == kPositionConvertTopic) {
        apply_convert(log, tx.raw_logs);
      }
    }
  }
}

void SyncThread::apply_condition_resolution(const json &log) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  const std::string condition_id = norm_b32(topics.at(1).get<std::string>());
  const std::string question_id = norm_b32(topics.at(3).get<std::string>());
  BigInt outcome_count = extract_u256(data, 0);
  std::vector<BigInt> payouts = extract_u256_array(data, extract_u256(data, 1));

  ConditionMeta condition;
  condition.qid = question_id;
  condition.oc = static_cast<uint8_t>(bigint_to_u64(outcome_count));
  condition.payout = payouts;
  condition.payout_d = 0;
  for (const auto &value : payouts) {
    condition.payout_d += value;
  }
  condition.has_payout_d = true;
  merge_condition(rt_.conditions[condition_id], condition);
  apply_resolved_prices(rt_, condition_id);
}

void SyncThread::apply_order_fill(const json &log) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  BigInt maker_asset_id = extract_u256(data, 0);
  BigInt taker_asset_id = extract_u256(data, 1);
  BigInt maker_amount = extract_u256(data, 2);
  BigInt taker_amount = extract_u256(data, 3);
  assert((maker_asset_id == 0) ^ (taker_asset_id == 0));

  const std::string maker = topic_to_addr(topics.at(2).get<std::string>());
  const std::string taker = topic_to_addr(topics.at(3).get<std::string>());
  const std::string buyer = maker_asset_id == 0 ? maker : taker;
  const std::string seller = maker_asset_id == 0 ? taker : maker;
  const std::string token_id =
      bigint_to_str(maker_asset_id == 0 ? taker_asset_id : maker_asset_id);
  const BigInt token_amount = maker_asset_id == 0 ? taker_amount : maker_amount;
  const BigInt collateral_amount =
      maker_asset_id == 0 ? maker_amount : taker_amount;

  ensure_token_meta(token_id);
  auto token_it = rt_.tokens.find(token_id);
  if (token_it == rt_.tokens.end() || token_it->second.cond.empty() ||
      token_it->second.cond == "?") {
    // 静默跳过 Gamma 中找不到的 token (已在 ensure_token_meta 中记录警告)
    if (token_it == rt_.tokens.end() || token_it->second.cond != "?") {
      logger().warn("apply_order_filled skip incomplete token_id=" + token_id);
    }
    return;
  }
  const std::string &cond_id = token_it->second.cond;
  ensure_condition_meta(cond_id, Collateral::Unknown);
  ConditionMeta &condition = rt_.conditions.at(cond_id);
  uint8_t token_idx = get_token_idx(rt_.conditions, cond_id, token_id);
  if (condition.coll == 0) {
    condition.coll = to_u8(infer_collateral_from_token(cond_id, token_idx, token_id));
  }

  std::vector<PendingEmit> events;
  if (rt_.user_set.contains(buyer)) {
    events.push_back({
        .user = buyer,
        .token_id = token_id,
        .condition_id = cond_id,
        .token_idx = token_idx,
        .collateral = condition.coll,
        .type = EventType::OrderBuy,
        .amount = bigint_to_i64(token_amount),
        .price = scaled_price(collateral_amount, token_amount),
    });
  }
  if (rt_.user_set.contains(seller)) {
    events.push_back({
        .user = seller,
        .token_id = token_id,
        .condition_id = cond_id,
        .token_idx = token_idx,
        .collateral = condition.coll,
        .type = EventType::OrderSell,
        .amount = -bigint_to_i64(token_amount),
        .price = scaled_price(collateral_amount, token_amount),
    });
  }

  commit_pending_events(
      rt_, log, events, cfg_.recent_event_limit,
      [this](const std::string &user, uint64_t block_number) {
        return user_visible_at(user, block_number);
      });
}

void SyncThread::apply_split(const json &log) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  const std::string stakeholder = topic_to_addr(topics.at(1).get<std::string>());
  const std::string parent_collection_id =
      norm_b32(topics.at(2).get<std::string>());
  const std::string condition_id = norm_b32(topics.at(3).get<std::string>());
  assert(parent_collection_id == zero_b32());

  const std::string collateral_token = extract_addr_from_word(data, 0);
  Collateral collateral = collateral_from_addr(collateral_token);
  assert(collateral != Collateral::Unknown);
  ensure_condition_meta(condition_id, collateral);
  ConditionMeta &condition = rt_.conditions.at(condition_id);
  assert(condition.oc == 2);
  condition.coll = to_u8(collateral);

  std::vector<BigInt> partition = extract_u256_array(data, extract_u256(data, 1));
  BigInt amount = extract_u256(data, 2);
  std::vector<PendingEmit> events;
  for (const auto &entry : partition) {
    uint8_t token_idx = index_set_to_token_idx(entry);
    assert(token_idx < condition.oc);
    std::string token_id =
        condition_token_id(condition_id, collateral_token, token_idx);
    if (condition.tids.size() <= token_idx) {
      condition.tids.resize(static_cast<size_t>(token_idx) + 1);
    }
    condition.tids[token_idx] = token_id;
    rt_.tokens[token_id].cond = condition_id;

    if (rt_.user_set.contains(stakeholder)) {
      events.push_back({
          .user = stakeholder,
          .token_id = token_id,
          .condition_id = condition_id,
          .token_idx = token_idx,
          .collateral = condition.coll,
          .type = EventType::Split,
          .amount = bigint_to_i64(amount),
          .price = kPriceScale / condition.oc,
      });
    }
  }

  commit_pending_events(
      rt_, log, events, cfg_.recent_event_limit,
      [this](const std::string &user, uint64_t block_number) {
        return user_visible_at(user, block_number);
      });
}

void SyncThread::apply_merge(const json &log) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  const std::string stakeholder = topic_to_addr(topics.at(1).get<std::string>());
  const std::string parent_collection_id =
      norm_b32(topics.at(2).get<std::string>());
  const std::string condition_id = norm_b32(topics.at(3).get<std::string>());
  assert(parent_collection_id == zero_b32());

  const std::string collateral_token = extract_addr_from_word(data, 0);
  Collateral collateral = collateral_from_addr(collateral_token);
  assert(collateral != Collateral::Unknown);
  ensure_condition_meta(condition_id, collateral);
  ConditionMeta &condition = rt_.conditions.at(condition_id);
  assert(condition.oc == 2);
  condition.coll = to_u8(collateral);

  std::vector<BigInt> partition = extract_u256_array(data, extract_u256(data, 1));
  BigInt amount = extract_u256(data, 2);
  std::vector<PendingEmit> events;
  for (const auto &entry : partition) {
    uint8_t token_idx = index_set_to_token_idx(entry);
    assert(token_idx < condition.oc);
    std::string token_id =
        condition_token_id(condition_id, collateral_token, token_idx);
    if (condition.tids.size() <= token_idx) {
      condition.tids.resize(static_cast<size_t>(token_idx) + 1);
    }
    condition.tids[token_idx] = token_id;
    rt_.tokens[token_id].cond = condition_id;

    if (rt_.user_set.contains(stakeholder)) {
      events.push_back({
          .user = stakeholder,
          .token_id = token_id,
          .condition_id = condition_id,
          .token_idx = token_idx,
          .collateral = condition.coll,
          .type = EventType::Merge,
          .amount = -bigint_to_i64(amount),
          .price = kPriceScale / condition.oc,
      });
    }
  }

  commit_pending_events(
      rt_, log, events, cfg_.recent_event_limit,
      [this](const std::string &user, uint64_t block_number) {
        return user_visible_at(user, block_number);
      });
}

void SyncThread::apply_redeem(const json &log) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  const std::string redeemer = topic_to_addr(topics.at(1).get<std::string>());
  const std::string collateral_token = topic_to_addr(topics.at(2).get<std::string>());
  const std::string condition_id = norm_b32(topics.at(3).get<std::string>());
  const std::string parent_collection_id = extract_b32_from_word(data, 0);
  assert(parent_collection_id == zero_b32());

  Collateral collateral = collateral_from_addr(collateral_token);
  assert(collateral != Collateral::Unknown);
  ensure_condition_meta(condition_id, collateral);
  ConditionMeta &condition = rt_.conditions.at(condition_id);
  condition.coll = to_u8(collateral);
  assert(condition.oc == 2);
  assert(condition.has_payout_d);
  assert(condition.payout.size() == condition.oc);

  std::vector<BigInt> index_sets =
      extract_u256_array(data, extract_u256(data, 1));
  BigInt payout = extract_u256(data, 2);

  uint8_t winner_idx = 0;
  for (size_t i = 1; i < condition.payout.size(); ++i) {
    if (condition.payout[i] > condition.payout[winner_idx]) {
      winner_idx = static_cast<uint8_t>(i);
    }
  }
  assert(condition.payout[winner_idx] > 0);
  BigInt winner_holding =
      (payout * condition.payout_d) / condition.payout[winner_idx];

  std::vector<PendingEmit> events;
  for (const auto &index_set : index_sets) {
    uint8_t token_idx = index_set_to_token_idx(index_set);
    assert(token_idx < condition.oc);
    std::string token_id =
        condition_token_id(condition_id, collateral_token, token_idx);
    if (condition.tids.size() <= token_idx) {
      condition.tids.resize(static_cast<size_t>(token_idx) + 1);
    }
    condition.tids[token_idx] = token_id;
    rt_.tokens[token_id].cond = condition_id;

    BigInt holding = 0;
    if (token_idx == winner_idx) {
      holding = winner_holding;
    } else if (rt_.user_states.contains(redeemer) &&
               rt_.user_states.at(redeemer).positions.contains(token_id)) {
      holding = rt_.user_states.at(redeemer).positions.at(token_id);
    }

    events.push_back({
        .user = redeemer,
        .token_id = token_id,
        .condition_id = condition_id,
        .token_idx = token_idx,
        .collateral = condition.coll,
        .type = EventType::Redeem,
        .amount = -bigint_to_i64(holding),
        .price = scaled_price(condition.payout[token_idx], condition.payout_d),
    });
  }

  commit_pending_events(
      rt_, log, events, cfg_.recent_event_limit,
      [this](const std::string &user, uint64_t block_number) {
        return user_visible_at(user, block_number);
      });
}

void SyncThread::apply_convert(const json &log, const std::vector<json> &tx_logs) {
  const json &topics = log.at("topics");
  const std::string data = log.at("data").get<std::string>();
  const std::string stakeholder = topic_to_addr(topics.at(1).get<std::string>());
  const std::string market_id = norm_b32(topics.at(2).get<std::string>());
  (void)extract_u256(data, 0);

  ensure_market_questions(market_id);
  const MarketMeta &market = rt_.markets.at(market_id);
  std::unordered_set<std::string> market_conditions;
  for (const auto &question_id : market.qids) {
    std::string condition_id = build_negrisk_condition_id(question_id);
    ensure_condition_meta(condition_id, Collateral::WrappedUSDCe);
    market_conditions.insert(condition_id);
  }

  std::vector<TransferLeg> transfers;
  for (const auto &tx_log : tx_logs) {
    const std::string address = norm_hex(tx_log.at("address").get<std::string>());
    if (address != kConditionalTokens) {
      continue;
    }
    const std::string topic0 =
        norm_hex(tx_log.at("topics").at(0).get<std::string>());
    if (topic0 == kTransferSingleTopic) {
      transfers.push_back(parse_transfer_single(tx_log));
    } else if (topic0 == kTransferBatchTopic) {
      auto batch = parse_transfer_batch(tx_log);
      transfers.insert(transfers.end(), batch.begin(), batch.end());
    }
  }
  std::sort(transfers.begin(), transfers.end(),
            [](const TransferLeg &a, const TransferLeg &b) {
              return a.log_index < b.log_index;
            });

  std::vector<PendingEmit> events;
  for (const auto &transfer : transfers) {
    ensure_token_meta(transfer.token_id);
    const std::string &cond_id = rt_.tokens.at(transfer.token_id).cond;
    if (!market_conditions.contains(cond_id)) {
      continue;
    }
    ConditionMeta &condition = rt_.conditions.at(cond_id);
    if (condition.coll == 0) {
      condition.coll = to_u8(Collateral::WrappedUSDCe);
    }

    int64_t signed_amount = 0;
    if (transfer.from == stakeholder && transfer.to == kNoTokenBurnAddress) {
      signed_amount = -bigint_to_i64(transfer.amount);
    } else if (transfer.to == stakeholder &&
               (transfer.from == kNegRiskAdapter || transfer.from == kZeroAddress)) {
      signed_amount = bigint_to_i64(transfer.amount);
    } else {
      continue;
    }

    uint8_t token_idx = get_token_idx(rt_.conditions, cond_id, transfer.token_id);
    events.push_back({
        .user = stakeholder,
        .token_id = transfer.token_id,
        .condition_id = cond_id,
        .token_idx = token_idx,
        .collateral = condition.coll,
        .type = EventType::Convert,
        .amount = signed_amount,
        .price = 0,
    });
  }

  assert(!events.empty());
  commit_pending_events(
      rt_, log, events, cfg_.recent_event_limit,
      [this](const std::string &user, uint64_t block_number) {
        return user_visible_at(user, block_number);
      });
}

bool SyncThread::user_visible_at(const std::string &user,
                                 uint64_t block_number) const {
  auto it = rt_.user_snapshots.find(user);
  if (it == rt_.user_snapshots.end()) {
    return false;
  }
  return block_number > it->second.snapshot_block;
}

uint64_t SyncThread::rpc_block_number() {
  json result = rpc_call("eth_blockNumber", json::array());
  return hex_to_u64(result.get<std::string>());
}

json SyncThread::rpc_call(const std::string &method, const json &params) {
  json payload = {
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", method},
      {"params", params},
  };
  for (size_t attempt = 1;; ++attempt) {
    HttpRes response = http_post(cfg_.rpc_http_url, payload, cfg_.proxy_url);
    ++rt_.counters.rpc_http;
    if (response.status != 200) {
      log_query("rpc", method, attempt, false,
                "status=" + std::to_string(response.status));
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    json body = safe_parse(response.body);
    if (body.contains("result")) {
      log_query("rpc", method, attempt, true);
      return body.at("result");
    }
    log_query("rpc", method, attempt, false, "body=" + clip_text(body.dump()));
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

json SyncThread::rpc_batch(const std::vector<json> &reqs) {
  json payload = json::array();
  int id = 1;
  for (const auto &req : reqs) {
    json item = req;
    item["jsonrpc"] = "2.0";
    item["id"] = id++;
    payload.push_back(std::move(item));
  }
  for (size_t attempt = 1;; ++attempt) {
    HttpRes response = http_post(cfg_.rpc_http_url, payload, cfg_.proxy_url);
    rt_.counters.rpc_http += reqs.size();
    if (response.status != 200) {
      log_query("rpc", "batch", attempt, false,
                "status=" + std::to_string(response.status));
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    json body = safe_parse(response.body);
    if (!body.is_array()) {
      log_query("rpc", "batch", attempt, false, "body=" + clip_text(body.dump()));
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }
    std::sort(body.begin(), body.end(), [](const json &a, const json &b) {
      return a.at("id").get<int>() < b.at("id").get<int>();
    });
    log_query("rpc", "batch", attempt, true,
              "size=" + std::to_string(reqs.size()));
    return body;
  }
}

} // namespace tracker
