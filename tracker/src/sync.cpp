#include "tracker/sync.hpp"
#include "tracker/const.hpp"
#include "tracker/http.hpp"
#include "tracker/log.hpp"
#include "tracker/store.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <set>

namespace tracker {
namespace {

// ============================================================================
// GraphQL Queries
// ============================================================================

const char *kUserPositionsQuery = R"(
query UserPositions($user: String!, $after: String!, $first: Int!) {
  _meta { block { number } }
  userPositions(
    first: $first
    orderBy: id
    orderDirection: asc
    where: {user: $user, amount_gt: "0", id_gt: $after}
  ) { id tokenId amount }
}
)";

const char *kMarketDataQuery = R"(
query MarketDatas($ids: [ID!]!) {
  marketDatas(where: {id_in: $ids}, orderBy: id) {
    id outcomeIndex priceOrderbook
    condition { id questionId outcomeSlotCount resolutionTimestamp payoutNumerators payoutDenominator }
  }
}
)";

const char *kConditionsQuery = R"(
query Conditions($ids: [ID!]!) {
  conditions(where: {id_in: $ids}, orderBy: id) {
    id positionIds payoutNumerators payoutDenominator
  }
}
)";

// ============================================================================
// JSON Helpers
// ============================================================================

std::string json_str(const json &row, const char *key) {
  if (!row.contains(key) || row.at(key).is_null()) return "";
  if (row.at(key).is_string()) return row.at(key).get<std::string>();
  return row.at(key).dump();
}

int json_int(const json &row, const char *key, int fallback = -1) {
  if (!row.contains(key) || row.at(key).is_null()) return fallback;
  if (row.at(key).is_number_integer()) return row.at(key).get<int>();
  if (row.at(key).is_string()) return std::stoi(row.at(key).get<std::string>());
  return fallback;
}

int64_t json_i64(const json &row, const char *key, int64_t fallback = -1) {
  if (!row.contains(key) || row.at(key).is_null()) return fallback;
  if (row.at(key).is_number_integer()) return row.at(key).get<int64_t>();
  if (row.at(key).is_string()) return std::stoll(row.at(key).get<std::string>());
  return fallback;
}

BigInt json_bigint(const json &row, const char *key) {
  if (!row.contains(key) || row.at(key).is_null()) return 0;
  if (row.at(key).is_string()) return bigint_from_dec(row.at(key).get<std::string>());
  if (row.at(key).is_number_integer()) return bigint_from_dec(std::to_string(row.at(key).get<int64_t>()));
  return 0;
}

std::vector<BigInt> json_bigint_arr(const json &row, const char *key) {
  std::vector<BigInt> r;
  if (!row.contains(key) || !row.at(key).is_array()) return r;
  for (const auto &v : row.at(key)) {
    if (v.is_string()) r.push_back(bigint_from_dec(v.get<std::string>()));
    else if (v.is_number_integer()) r.push_back(bigint_from_dec(std::to_string(v.get<int64_t>())));
  }
  return r;
}

std::vector<std::string> json_str_arr(const json &row, const char *key) {
  std::vector<std::string> r;
  if (!row.contains(key) || !row.at(key).is_array()) return r;
  for (const auto &v : row.at(key)) {
    if (v.is_string()) r.push_back(v.get<std::string>());
    else if (v.is_number()) r.push_back(std::to_string(v.get<int64_t>()));
  }
  return r;
}

std::string json_str_or_int(const json &v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number()) return std::to_string(v.get<int64_t>());
  return "";
}

std::optional<long double> resolved_price(const std::vector<BigInt> &nums, const BigInt &denom, bool has, int idx) {
  if (!has || idx < 0 || static_cast<size_t>(idx) >= nums.size()) return std::nullopt;
  long double d = denom.convert_to<long double>();
  if (d <= 0.0L) return std::nullopt;
  return nums[static_cast<size_t>(idx)].convert_to<long double>() / d;
}

// ============================================================================
// Log Parsing
// ============================================================================

struct TransferLeg {
  uint64_t block = 0;
  uint64_t tx_idx = 0;
  std::string tx_hash;
  int64_t log_idx = 0;
  std::string op, from, to;
  std::string token_id;
  BigInt amount = 0;
};

struct OrderFill {
  uint64_t block = 0;
  std::string tx_hash;
  int64_t log_idx = 0;
  std::string exchange;
  std::string maker, taker, buyer, seller;
  std::string token_id;
  BigInt token_amount = 0;
  BigInt collateral_amount = 0;
  BigInt fee = 0;
  [[nodiscard]] long double price() const {
    long double t = token_amount.convert_to<long double>();
    return t > 0.0L ? collateral_amount.convert_to<long double>() / t : 0.0L;
  }
};

struct TxContext {
  uint64_t block = 0;
  uint64_t tx_idx = 0;
  std::string tx_hash;
  std::vector<TransferLeg> transfers;
  std::set<std::string> split_users, merge_users, redeem_users, convert_users;
  std::vector<OrderFill> fills;
};

TransferLeg parse_transfer_single(const json &log) {
  const json &topics = log.at("topics");
  std::string data = log.at("data").get<std::string>();
  return {
      hex_to_u64(log.at("blockNumber").get<std::string>()),
      hex_to_u64(log.at("transactionIndex").get<std::string>()),
      norm_hex(log.at("transactionHash").get<std::string>()),
      static_cast<int64_t>(hex_to_u64(log.at("logIndex").get<std::string>()) * kTransferFlatLogScale),
      topic_to_addr(topics.at(1).get<std::string>()),
      topic_to_addr(topics.at(2).get<std::string>()),
      topic_to_addr(topics.at(3).get<std::string>()),
      bigint_to_str(extract_u256(data, 0)),
      extract_u256(data, 1),
  };
}

std::vector<TransferLeg> parse_transfer_batch(const json &log) {
  const json &topics = log.at("topics");
  std::string data = log.at("data").get<std::string>();
  std::vector<BigInt> ids = extract_u256_array(data, extract_u256(data, 0));
  std::vector<BigInt> vals = extract_u256_array(data, extract_u256(data, 1));
  assert(ids.size() == vals.size());

  uint64_t block = hex_to_u64(log.at("blockNumber").get<std::string>());
  uint64_t tx_idx = hex_to_u64(log.at("transactionIndex").get<std::string>());
  std::string tx_hash = norm_hex(log.at("transactionHash").get<std::string>());
  uint64_t log_idx = hex_to_u64(log.at("logIndex").get<std::string>());
  std::string op = topic_to_addr(topics.at(1).get<std::string>());
  std::string from = topic_to_addr(topics.at(2).get<std::string>());
  std::string to = topic_to_addr(topics.at(3).get<std::string>());

  std::vector<TransferLeg> r;
  for (size_t i = 0; i < ids.size(); ++i) {
    r.push_back({block, tx_idx, tx_hash, static_cast<int64_t>(log_idx * kTransferFlatLogScale + i),
                 op, from, to, bigint_to_str(ids[i]), vals[i]});
  }
  return r;
}

OrderFill parse_order_fill(const json &log) {
  const json &topics = log.at("topics");
  std::string data = log.at("data").get<std::string>();
  std::string maker = topic_to_addr(topics.at(2).get<std::string>());
  std::string taker = topic_to_addr(topics.at(3).get<std::string>());
  BigInt maker_asset = extract_u256(data, 0);
  BigInt taker_asset = extract_u256(data, 1);
  BigInt maker_amt = extract_u256(data, 2);
  BigInt taker_amt = extract_u256(data, 3);
  BigInt fee = extract_u256(data, 4);
  bool maker_is_coll = maker_asset == 0;
  return {
      hex_to_u64(log.at("blockNumber").get<std::string>()),
      norm_hex(log.at("transactionHash").get<std::string>()),
      static_cast<int64_t>(hex_to_u64(log.at("logIndex").get<std::string>())),
      norm_hex(log.at("address").get<std::string>()),
      maker, taker,
      maker_is_coll ? maker : taker,
      maker_is_coll ? taker : maker,
      bigint_to_str(maker_is_coll ? taker_asset : maker_asset),
      maker_is_coll ? taker_amt : maker_amt,
      maker_is_coll ? maker_amt : taker_amt,
      fee,
  };
}

std::string log_key(const json &log) {
  return log.at("blockNumber").get<std::string>() + "|" +
         norm_hex(log.at("transactionHash").get<std::string>()) + "|" +
         log.at("logIndex").get<std::string>() + "|" +
         norm_hex(log.at("address").get<std::string>());
}

std::vector<TxContext> build_tx_contexts(const std::vector<json> &logs) {
  std::map<std::string, TxContext> map;
  for (const auto &log : logs) {
    std::string tx_hash = norm_hex(log.at("transactionHash").get<std::string>());
    TxContext &ctx = map[tx_hash];
    if (ctx.tx_hash.empty()) {
      ctx.tx_hash = tx_hash;
      ctx.block = hex_to_u64(log.at("blockNumber").get<std::string>());
      ctx.tx_idx = hex_to_u64(log.at("transactionIndex").get<std::string>());
    }

    std::string addr = norm_hex(log.at("address").get<std::string>());
    std::string topic0 = norm_hex(log.at("topics").at(0).get<std::string>());

    if (addr == kConditionalTokens && topic0 == kTransferSingleTopic) {
      ctx.transfers.push_back(parse_transfer_single(log));
    } else if (addr == kConditionalTokens && topic0 == kTransferBatchTopic) {
      auto legs = parse_transfer_batch(log);
      ctx.transfers.insert(ctx.transfers.end(), legs.begin(), legs.end());
    } else if (addr == kConditionalTokens && topic0 == kPositionSplitTopic) {
      ctx.split_users.insert(topic_to_addr(log.at("topics").at(1).get<std::string>()));
    } else if (addr == kConditionalTokens && topic0 == kPositionMergeTopic) {
      ctx.merge_users.insert(topic_to_addr(log.at("topics").at(1).get<std::string>()));
    } else if (addr == kConditionalTokens && topic0 == kPositionRedeemTopic) {
      ctx.redeem_users.insert(topic_to_addr(log.at("topics").at(1).get<std::string>()));
    } else if ((addr == kCtfExchange || addr == kNegRiskCtfExchange) && topic0 == kOrderFillTopic) {
      ctx.fills.push_back(parse_order_fill(log));
    } else if (addr == kNegRiskAdapter && topic0 == kPositionConvertTopic) {
      ctx.convert_users.insert(topic_to_addr(log.at("topics").at(1).get<std::string>()));
    }
  }

  std::vector<TxContext> r;
  for (auto &[_, ctx] : map) {
    std::sort(ctx.transfers.begin(), ctx.transfers.end(), [](const auto &a, const auto &b) { return a.log_idx < b.log_idx; });
    std::sort(ctx.fills.begin(), ctx.fills.end(), [](const auto &a, const auto &b) { return a.log_idx < b.log_idx; });
    r.push_back(std::move(ctx));
  }
  std::sort(r.begin(), r.end(), [](const auto &a, const auto &b) {
    if (a.block != b.block) return a.block < b.block;
    if (a.tx_idx != b.tx_idx) return a.tx_idx < b.tx_idx;
    return (a.transfers.empty() ? 0 : a.transfers[0].log_idx) < (b.transfers.empty() ? 0 : b.transfers[0].log_idx);
  });
  return r;
}

const OrderFill *find_buy(const TxContext &ctx, const TransferLeg &t, const std::string &u) {
  for (const auto &f : ctx.fills) {
    if (f.buyer == u && f.token_id == t.token_id && f.token_amount == t.amount) return &f;
  }
  return nullptr;
}

const OrderFill *find_sell(const TxContext &ctx, const TransferLeg &t, const std::string &u) {
  for (const auto &f : ctx.fills) {
    if (f.seller == u && f.token_id == t.token_id && f.token_amount == t.amount) return &f;
  }
  return nullptr;
}

bool is_protocol(const std::string &addr) {
  return addr == kConditionalTokens || addr == kCtfExchange ||
         addr == kNegRiskCtfExchange || addr == kNegRiskAdapter || addr == kZeroAddress;
}

json build_event(const TxContext &ctx, const TransferLeg &t, const std::string &user,
                 const std::string &dir, const std::string &kind, const OrderFill *fill) {
  json r = {
      {"block_number", t.block}, {"tx_hash", t.tx_hash}, {"log_index", t.log_idx},
      {"user", user}, {"direction", dir}, {"kind", kind},
      {"token_id", t.token_id}, {"amount_raw", bigint_to_str(t.amount)},
      {"counterparty", dir == "in" ? t.from : t.to},
      {"operator", t.op},
      {"tx_transfer_count", ctx.transfers.size()},
      {"tx_order_fill_count", ctx.fills.size()},
  };
  if (fill) {
    r["exchange"] = fill->exchange;
    r["collateral_amount_raw"] = bigint_to_str(fill->collateral_amount);
    r["price"] = fmt_decimal(fill->price(), 10);
    r["fee_raw"] = bigint_to_str(fill->fee);
  }
  return r;
}

} // namespace

// ============================================================================
// SyncThread Implementation
// ============================================================================

SyncThread::SyncThread(const AppConfig &cfg, AppState &state, EventQueue &queue, WsThread &ws)
    : cfg_(cfg), state_(state), queue_(queue), ws_(ws) {}

void SyncThread::request_resync() {
  resync_flag_ = true;
}

void SyncThread::run() {
  logger().init(cfg_.log_file);
  load_seed();
  load_files();

  auto next_resync = std::chrono::steady_clock::now();
  while (true) {
    if (resync_flag_.exchange(false) || std::chrono::steady_clock::now() >= next_resync) {
      full_resync();
      next_resync = std::chrono::steady_clock::now() + std::chrono::seconds(cfg_.resync_interval_sec);
      ws_.request_reconnect();
      continue;
    }
    drain_queue();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void SyncThread::full_resync() {
  progress().init(SYNC_STAGES);
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.resync_started_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  }

  fetch_positions();

  uint64_t snap_block;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    snap_block = state_.snapshot_block;
  }
  fetch_balances(u64_to_hex(snap_block));
  append_snapshot(snap_block);

  fetch_market_data(false);

  uint64_t head = rpc_block_number();
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.head_block = std::max(state_.head_block, head);
  }

  if (snap_block + 1 <= head) {
    backfill_range(snap_block + 1, head);
  }

  fetch_balances("latest");
  fetch_market_data(false);

  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.resync_finished_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  }
  persist_all();
  progress().finish();
  logger().info("resync done");
}

void SyncThread::fetch_positions() {
  std::vector<std::string> users = load_addr_file(cfg_.address_file);
  progress().update("fetch_positions", 0, users.size());

  struct Snap { std::string user; uint64_t block = 0; std::map<std::string, BigInt> pos; std::string cursor; bool done = false; };
  std::vector<Snap> snaps;
  for (const auto &u : users) snaps.push_back({u, 0, {}, "", false});

  std::string url = "https://gateway.thegraph.com/api/" + cfg_.graph_api_key + "/subgraphs/id/" + std::string(kPnlSubgraphId);
  size_t done_count = 0;

  while (done_count < users.size()) {
    std::vector<HttpReq> reqs;
    std::vector<size_t> idx_map;
    for (size_t i = 0; i < snaps.size(); ++i) {
      if (snaps[i].done) continue;
      json vars = {{"user", snaps[i].user}, {"after", snaps[i].cursor}, {"first", static_cast<int>(cfg_.graph_page_limit)}};
      reqs.push_back({url, "POST", json{{"query", kUserPositionsQuery}, {"variables", vars}}.dump()});
      idx_map.push_back(i);
    }
    if (reqs.empty()) break;

    auto resps = http_batch(reqs, cfg_.http_concurrency);
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.counters.subgraph += resps.size();
    }

    for (size_t r = 0; r < resps.size(); ++r) {
      size_t i = idx_map[r];
      Snap &s = snaps[i];
      assert(resps[r].status == 200);
      json body = safe_parse(resps[r].body);
      assert(!body.contains("errors") && body.contains("data"));
      const json &data = body.at("data");

      if (s.block == 0) {
        s.block = static_cast<uint64_t>(std::stoull(json_str_or_int(data.at("_meta").at("block").at("number"))));
      }

      const json &rows = data.at("userPositions");
      for (const auto &row : rows) {
        std::string token_id = row.at("tokenId").get<std::string>();
        s.pos[token_id] += bigint_from_dec(json_str_or_int(row.at("amount")));
      }

      if (rows.size() < cfg_.graph_page_limit) {
        s.done = true;
        ++done_count;
        progress().update("fetch_positions", done_count, users.size());
      } else {
        s.cursor = rows.back().at("id").get<std::string>();
      }
    }
  }

  uint64_t min_block = UINT64_MAX;
  for (const auto &s : snaps) {
    if (s.block > 0 && s.block < min_block) min_block = s.block;
  }
  assert(min_block != UINT64_MAX);
  logger().info("fetch_positions done, min_block=" + std::to_string(min_block));

  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.users = users;
    state_.user_set.clear();
    for (const auto &u : users) state_.user_set.insert(u);
    state_.user_states.clear();
    for (const auto &s : snaps) {
      UserState &us = state_.user_states[s.user];
      us.user = s.user;
      us.positions = s.pos;
    }
    state_.snapshot_block = min_block;
    state_.applied_block = min_block;
    state_.head_block = std::max(state_.head_block, min_block);
  }
}

void SyncThread::fetch_balances(const std::string &block_tag) {
  std::vector<std::string> users;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    users = state_.users;
  }
  progress().update("fetch_balances", 0, 0);

  std::vector<json> reqs;
  struct Ref { std::string user; bool wrapped; };
  std::vector<Ref> refs;

  std::string selector = "0x70a08231";
  for (const auto &u : users) {
    std::string data = selector + std::string(24, '0') + strip_0x(u);
    reqs.push_back({{"method", "eth_call"}, {"params", json::array({json{{"to", kUsdcE}, {"data", data}}, block_tag})}});
    refs.push_back({u, false});
    reqs.push_back({{"method", "eth_call"}, {"params", json::array({json{{"to", kWrappedCollateral}, {"data", data}}, block_tag})}});
    refs.push_back({u, true});
  }

  if (reqs.empty()) return;
  json resps = rpc_batch(reqs);
  logger().info("fetch_balances batch(" + std::to_string(reqs.size()) + ")");

  std::lock_guard<std::mutex> lock(state_.mu);
  for (size_t i = 0; i < refs.size(); ++i) {
    BigInt bal = bigint_from_hex(resps.at(i).at("result").get<std::string>());
    UserState &us = state_.user_states.at(refs[i].user);
    if (refs[i].wrapped) us.stable.wrapped = bal;
    else us.stable.usdc_e = bal;
  }
}

void SyncThread::fetch_market_data(bool missing_only) {
  std::vector<std::string> token_ids;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    std::set<std::string> set;
    for (const auto &u : state_.users) {
      for (const auto &[tid, _] : state_.user_states.at(u).positions) {
        if (!missing_only || !state_.tokens.contains(tid) || state_.tokens.at(tid).price < 0.0L) {
          set.insert(tid);
        }
      }
    }
    token_ids.assign(set.begin(), set.end());
  }
  if (token_ids.empty()) return;
  progress().update("fetch_market_data", 0, token_ids.size());

  std::string url = "https://gateway.thegraph.com/api/" + cfg_.graph_api_key + "/subgraphs/id/" + std::string(kPolymarketSubgraphId);
  std::vector<HttpReq> reqs;
  for (const auto &chunk : chunked(token_ids, cfg_.graph_id_batch_limit)) {
    json vars = {{"ids", std::vector<std::string>(chunk.begin(), chunk.end())}};
    reqs.push_back({url, "POST", json{{"query", kMarketDataQuery}, {"variables", vars}}.dump()});
  }

  auto resps = http_batch(reqs, cfg_.http_concurrency);
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.counters.subgraph += resps.size();
  }
  logger().info("fetch_market_data batch(" + std::to_string(resps.size()) + ")");

  std::set<std::string> cond_ids;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    for (const auto &resp : resps) {
      assert(resp.status == 200);
      json body = safe_parse(resp.body);
      assert(!body.contains("errors") && body.contains("data"));
      for (const auto &row : body.at("data").at("marketDatas")) {
        TokenMeta tm;
        tm.token_id = row.at("id").get<std::string>();
        tm.outcome_index = json_int(row, "outcomeIndex");
        if (row.contains("priceOrderbook") && row.at("priceOrderbook").is_string()) {
          tm.price = parse_decimal(row.at("priceOrderbook").get<std::string>());
          tm.price_source = "orderbook";
        }
        if (row.contains("condition") && row.at("condition").is_object()) {
          const json &c = row.at("condition");
          tm.condition_id = json_str(c, "id");
          tm.question_id = json_str(c, "questionId");
          tm.outcome_slot_count = json_int(c, "outcomeSlotCount");
          tm.resolution_timestamp = json_i64(c, "resolutionTimestamp");
          tm.payout_numerators = json_bigint_arr(c, "payoutNumerators");
          if (c.contains("payoutDenominator") && !c.at("payoutDenominator").is_null()) {
            tm.payout_denominator = json_bigint(c, "payoutDenominator");
            tm.has_payout_denominator = true;
          }
          auto rp = resolved_price(tm.payout_numerators, tm.payout_denominator, tm.has_payout_denominator, tm.outcome_index);
          if (rp) { tm.price = *rp; tm.price_source = "resolution"; }

          ConditionMeta cm;
          cm.condition_id = tm.condition_id;
          cm.question_id = tm.question_id;
          cm.outcome_slot_count = tm.outcome_slot_count;
          cm.resolution_timestamp = tm.resolution_timestamp;
          cm.payout_numerators = tm.payout_numerators;
          cm.payout_denominator = tm.payout_denominator;
          cm.has_payout_denominator = tm.has_payout_denominator;
          merge_condition(state_.conditions[tm.condition_id], cm);
          cond_ids.insert(tm.condition_id);
        }
        merge_token(state_.tokens[tm.token_id], tm);
      }
    }
  }
  progress().update("fetch_market_data", token_ids.size(), token_ids.size());

  std::vector<std::string> cond_vec(cond_ids.begin(), cond_ids.end());
  if (!cond_vec.empty()) fetch_conditions(cond_vec);

  std::vector<std::string> missing_gamma;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    for (const auto &u : state_.users) {
      for (const auto &[tid, _] : state_.user_states.at(u).positions) {
        if (!state_.tokens.contains(tid)) continue;
        const std::string &cid = state_.tokens.at(tid).condition_id;
        if (cid.empty()) continue;
        if (!state_.conditions.contains(cid) || state_.conditions.at(cid).market_question.empty()) {
          missing_gamma.push_back(cid);
        }
      }
    }
    std::sort(missing_gamma.begin(), missing_gamma.end());
    missing_gamma.erase(std::unique(missing_gamma.begin(), missing_gamma.end()), missing_gamma.end());
  }
  if (!missing_gamma.empty()) fetch_gamma(missing_gamma);
}

void SyncThread::fetch_conditions(const std::vector<std::string> &ids) {
  if (ids.empty()) return;
  progress().update("fetch_conditions", 0, ids.size());

  std::string url = "https://gateway.thegraph.com/api/" + cfg_.graph_api_key + "/subgraphs/id/" + std::string(kPnlSubgraphId);
  std::vector<HttpReq> reqs;
  for (const auto &chunk : chunked(ids, cfg_.graph_id_batch_limit)) {
    json vars = {{"ids", std::vector<std::string>(chunk.begin(), chunk.end())}};
    reqs.push_back({url, "POST", json{{"query", kConditionsQuery}, {"variables", vars}}.dump()});
  }

  auto resps = http_batch(reqs, cfg_.http_concurrency);
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.counters.subgraph += resps.size();
  }
  logger().info("fetch_conditions batch(" + std::to_string(resps.size()) + ")");

  std::lock_guard<std::mutex> lock(state_.mu);
  for (const auto &resp : resps) {
    assert(resp.status == 200);
    json body = safe_parse(resp.body);
    assert(!body.contains("errors") && body.contains("data"));
    for (const auto &row : body.at("data").at("conditions")) {
      ConditionMeta cm;
      cm.condition_id = row.at("id").get<std::string>();
      cm.token_ids = json_str_arr(row, "positionIds");
      cm.payout_numerators = json_bigint_arr(row, "payoutNumerators");
      if (row.contains("payoutDenominator") && !row.at("payoutDenominator").is_null()) {
        cm.payout_denominator = json_bigint(row, "payoutDenominator");
        cm.has_payout_denominator = true;
      }
      merge_condition(state_.conditions[cm.condition_id], cm);
    }
  }
  progress().update("fetch_conditions", ids.size(), ids.size());
}

void SyncThread::fetch_gamma(const std::vector<std::string> &ids) {
  if (ids.empty()) return;
  progress().update("fetch_gamma", 0, ids.size());

  std::vector<HttpReq> reqs;
  for (const auto &cid : ids) {
    reqs.push_back({std::string(kGammaApiBase) + "/markets?condition_ids=" + cid + "&include_tag=true", "GET", ""});
  }

  auto resps = http_batch(reqs, cfg_.http_concurrency);
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.counters.gamma += resps.size();
  }
  logger().info("fetch_gamma batch(" + std::to_string(resps.size()) + ")");

  std::lock_guard<std::mutex> lock(state_.mu);
  for (size_t i = 0; i < resps.size(); ++i) {
    const std::string &cid = ids[i];
    assert(resps[i].status == 200);
    json arr = safe_parse(resps[i].body);
    if (!arr.is_array() || arr.empty()) continue;

    json market = arr.front();
    for (const auto &item : arr) {
      std::string c = item.contains("conditionId") ? json_str(item, "conditionId") : json_str(item, "condition_id");
      if (!c.empty() && norm_hex(c) == norm_hex(cid)) { market = item; break; }
    }

    ConditionMeta cm;
    cm.condition_id = cid;
    json events = market.contains("events") && market.at("events").is_array() ? market.at("events") : json::array();
    json event0 = events.empty() ? json::object() : events.front();
    cm.market_question = json_str(market, "question");
    if (cm.market_question.empty()) cm.market_question = json_str(event0, "title");
    cm.market_description = json_str(market, "description");
    cm.market_event_title = json_str(event0, "title");
    cm.market_slug = json_str(event0, "slug");
    if (cm.market_slug.empty()) cm.market_slug = json_str(market, "slug");
    cm.market_url = cm.market_slug.empty() ? "" : "https://polymarket.com/event/" + cm.market_slug;
    if (market.contains("outcomes")) {
      json outcomes = market.at("outcomes");
      if (outcomes.is_string()) outcomes = json::parse(outcomes.get<std::string>());
      if (outcomes.is_array()) {
        for (const auto &v : outcomes) cm.market_outcomes.push_back(json_str_or_int(v));
      }
    }
    merge_condition(state_.conditions[cid], cm);
  }
  progress().update("fetch_gamma", ids.size(), ids.size());
}

std::vector<json> SyncThread::build_log_filters(uint64_t from, uint64_t to) const {
  std::vector<std::string> users;
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    users = state_.users;
  }

  std::vector<json> filters;
  for (const auto &group : chunked(users, cfg_.topic_group_size)) {
    json topics = json::array();
    for (const auto &u : group) topics.push_back(addr_to_topic(u));

    std::vector<json> group_filters = {
        {{"address", kConditionalTokens}, {"topics", json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}), nullptr, topics})}},
        {{"address", kConditionalTokens}, {"topics", json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}), nullptr, nullptr, topics})}},
        {{"address", kConditionalTokens}, {"topics", json::array({json::array({kPositionSplitTopic, kPositionMergeTopic, kPositionRedeemTopic}), topics})}},
        {{"address", json::array({kCtfExchange, kNegRiskCtfExchange})}, {"topics", json::array({json::array({kOrderFillTopic}), nullptr, topics})}},
        {{"address", json::array({kCtfExchange, kNegRiskCtfExchange})}, {"topics", json::array({json::array({kOrderFillTopic}), nullptr, nullptr, topics})}},
        {{"address", kNegRiskAdapter}, {"topics", json::array({json::array({kPositionConvertTopic}), topics})}},
    };
    for (auto &f : group_filters) {
      f["fromBlock"] = u64_to_hex(from);
      f["toBlock"] = u64_to_hex(to);
      filters.push_back(std::move(f));
    }
  }

  filters.push_back({
      {"address", kConditionalTokens},
      {"topics", json::array({json::array({kConditionResolveTopic})})},
      {"fromBlock", u64_to_hex(from)},
      {"toBlock", u64_to_hex(to)},
  });
  return filters;
}

void SyncThread::backfill_range(uint64_t from, uint64_t to) {
  if (from > to) return;
  progress().update("backfill_logs", 0, to - from + 1);

  uint64_t start = from;
  while (start <= to) {
    uint64_t end = std::min(to, start + cfg_.get_logs_block_span - 1);
    auto filters = build_log_filters(start, end);

    std::vector<json> reqs;
    for (const auto &f : filters) {
      reqs.push_back({{"method", "eth_getLogs"}, {"params", json::array({f})}});
    }
    json resps = rpc_batch(reqs);
    logger().info("backfill_logs batch(" + std::to_string(reqs.size()) + ") blocks=" + std::to_string(start) + "-" + std::to_string(end));

    std::map<uint64_t, std::map<std::string, json>> blocks;
    for (const auto &resp : resps) {
      assert(resp.contains("result") && resp.at("result").is_array());
      for (const auto &log : resp.at("result")) {
        blocks[hex_to_u64(log.at("blockNumber").get<std::string>())][log_key(log)] = log;
      }
    }

    for (auto &[blk, logs_map] : blocks) {
      std::vector<json> logs;
      for (auto &[_, log] : logs_map) logs.push_back(std::move(log));
      std::sort(logs.begin(), logs.end(), [](const json &a, const json &b) {
        return std::make_tuple(hex_to_u64(a.at("transactionIndex").get<std::string>()), hex_to_u64(a.at("logIndex").get<std::string>()))
             < std::make_tuple(hex_to_u64(b.at("transactionIndex").get<std::string>()), hex_to_u64(b.at("logIndex").get<std::string>()));
      });
      std::set<std::string> touched;
      apply_logs(logs, touched);
      if (!touched.empty()) fetch_market_data(true);
      {
        std::lock_guard<std::mutex> lock(state_.mu);
        state_.applied_block = std::max(state_.applied_block, blk);
        state_.head_block = std::max(state_.head_block, blk);
      }
    }

    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.applied_block = std::max(state_.applied_block, end);
      state_.head_block = std::max(state_.head_block, end);
    }
    progress().update("backfill_logs", end - from + 1, to - from + 1);
    start = end + 1;
  }
}

void SyncThread::drain_queue() {
  while (auto ev = queue_.try_pop()) {
    std::set<std::string> touched;
    std::vector<json> logs;
    for (const auto &log : ev->logs) logs.push_back(log);
    bool changed = apply_logs(logs, touched);
    if (!touched.empty()) fetch_market_data(true);
    if (changed || !touched.empty()) persist_all();
    {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.applied_block = std::max(state_.applied_block, ev->block_number);
    }
  }
}

bool SyncThread::apply_logs(const std::vector<json> &logs, std::set<std::string> &touched) {
  bool changed = false;
  auto ctxs = build_tx_contexts(logs);

  std::lock_guard<std::mutex> lock(state_.mu);
  for (const auto &ctx : ctxs) {
    for (const auto &t : ctx.transfers) {
      // OUT
      if (state_.user_set.contains(t.from)) {
        UserState &us = state_.user_states.at(t.from);
        const OrderFill *fill = find_sell(ctx, t, t.from);
        std::string kind = "transfer_out";
        if (fill) { kind = "order_sell"; us.stable.usdc_e += fill->collateral_amount; }
        else if (ctx.merge_users.contains(t.from)) kind = "merge_out";
        else if (ctx.redeem_users.contains(t.from)) kind = "redeem_out";
        else if (ctx.convert_users.contains(t.from)) kind = "convert_out";
        else if (t.to == kZeroAddress) kind = "burn_out";
        else if (state_.user_set.contains(t.to)) kind = "tracked_out";
        else if (is_protocol(t.to) || is_protocol(t.op)) kind = "protocol_out";

        BigInt cur = us.positions.contains(t.token_id) ? us.positions.at(t.token_id) : BigInt{0};
        assert(cur >= t.amount);
        BigInt next = cur - t.amount;
        if (next == 0) us.positions.erase(t.token_id);
        else us.positions[t.token_id] = next;

        json ev = build_event(ctx, t, t.from, "out", kind, fill);
        json &bucket = state_.history_root[t.from][block_key(t.block)];
        if (!bucket.is_array()) bucket = json::array();
        bucket.push_back(ev);
        state_.recent_events.push_back(ev);
        while (state_.recent_events.size() > cfg_.recent_event_limit) state_.recent_events.pop_front();
        touched.insert(t.token_id);
        changed = true;
      }

      // IN
      if (state_.user_set.contains(t.to)) {
        UserState &us = state_.user_states.at(t.to);
        const OrderFill *fill = find_buy(ctx, t, t.to);
        std::string kind = "transfer_in";
        if (fill) { kind = "order_buy"; us.stable.usdc_e -= fill->collateral_amount; }
        else if (ctx.split_users.contains(t.to)) kind = "split_in";
        else if (ctx.convert_users.contains(t.to)) kind = "convert_in";
        else if (t.from == kZeroAddress) kind = "mint_in";
        else if (state_.user_set.contains(t.from)) kind = "tracked_in";
        else if (is_protocol(t.from) || is_protocol(t.op)) kind = "protocol_in";

        us.positions[t.token_id] += t.amount;

        json ev = build_event(ctx, t, t.to, "in", kind, fill);
        json &bucket = state_.history_root[t.to][block_key(t.block)];
        if (!bucket.is_array()) bucket = json::array();
        bucket.push_back(ev);
        state_.recent_events.push_back(ev);
        while (state_.recent_events.size() > cfg_.recent_event_limit) state_.recent_events.pop_front();
        touched.insert(t.token_id);
        changed = true;
      }
    }
  }
  return changed;
}

void SyncThread::load_files() {
  std::lock_guard<std::mutex> lock(state_.mu);

  json meta = load_json(cfg_.meta_file);
  if (meta.contains("conditions") && meta.at("conditions").is_object()) {
    for (auto it = meta.at("conditions").begin(); it != meta.at("conditions").end(); ++it) {
      ConditionMeta cm;
      cm.condition_id = it.key();
      cm.question_id = json_str(it.value(), "question_id");
      cm.outcome_slot_count = json_int(it.value(), "outcome_slot_count");
      cm.resolution_timestamp = json_i64(it.value(), "resolution_timestamp");
      cm.token_ids = json_str_arr(it.value(), "token_ids");
      cm.payout_numerators = json_bigint_arr(it.value(), "payout_numerators");
      if (it.value().contains("payout_denominator") && !it.value().at("payout_denominator").is_null()) {
        cm.payout_denominator = json_bigint(it.value(), "payout_denominator");
        cm.has_payout_denominator = true;
      }
      cm.market_question = json_str(it.value(), "market_question");
      cm.market_description = json_str(it.value(), "market_description");
      cm.market_event_title = json_str(it.value(), "market_event_title");
      cm.market_slug = json_str(it.value(), "market_slug");
      cm.market_url = json_str(it.value(), "market_url");
      cm.market_outcomes = json_str_arr(it.value(), "market_outcomes");
      merge_condition(state_.conditions[cm.condition_id], cm);
    }
  }
  if (meta.contains("tokens") && meta.at("tokens").is_object()) {
    for (auto it = meta.at("tokens").begin(); it != meta.at("tokens").end(); ++it) {
      TokenMeta tm;
      tm.token_id = it.key();
      tm.condition_id = json_str(it.value(), "condition_id");
      tm.question_id = json_str(it.value(), "question_id");
      tm.outcome_index = json_int(it.value(), "outcome_index");
      tm.outcome_slot_count = json_int(it.value(), "outcome_slot_count");
      tm.resolution_timestamp = json_i64(it.value(), "resolution_timestamp");
      tm.payout_numerators = json_bigint_arr(it.value(), "payout_numerators");
      if (it.value().contains("payout_denominator") && !it.value().at("payout_denominator").is_null()) {
        tm.payout_denominator = json_bigint(it.value(), "payout_denominator");
        tm.has_payout_denominator = true;
      }
      if (it.value().contains("price") && it.value().at("price").is_string()) {
        tm.price = parse_decimal(it.value().at("price").get<std::string>());
        tm.price_source = json_str(it.value(), "price_source");
      }
      merge_token(state_.tokens[tm.token_id], tm);
    }
  }

  state_.snapshot_root = load_json(cfg_.snapshot_file);
  state_.history_root = load_json(cfg_.history_file);

  json agg = load_json(cfg_.aggregate_file);
  if (agg.contains("summary") && agg.at("summary").is_object()) {
    const json &s = agg.at("summary");
    state_.snapshot_block = static_cast<uint64_t>(json_i64(s, "snapshot_block", 0));
    state_.applied_block = static_cast<uint64_t>(json_i64(s, "last_applied_block", 0));
    state_.head_block = static_cast<uint64_t>(json_i64(s, "head_block", 0));
    state_.resync_started_at = json_i64(s, "last_resync_started_at_unix_sec", 0);
    state_.resync_finished_at = json_i64(s, "last_resync_finished_at_unix_sec", 0);
  }
  if (agg.contains("recent_events") && agg.at("recent_events").is_array()) {
    for (const auto &ev : agg.at("recent_events")) state_.recent_events.push_back(ev);
  }
}

void SyncThread::load_seed() {
  if (!std::filesystem::exists(cfg_.seed_file)) return;
  json seed = load_json(cfg_.seed_file);
  if (!seed.is_object()) return;

  std::lock_guard<std::mutex> lock(state_.mu);
  if (seed.contains("conditions") && seed.at("conditions").is_array()) {
    for (const auto &row : seed.at("conditions")) {
      std::string cid = json_str(row, "condition_id");
      if (cid.empty()) continue;
      ConditionMeta cm;
      cm.condition_id = cid;
      cm.question_id = json_str(row, "question_id");
      cm.outcome_slot_count = json_int(row, "outcome_slot_count");
      cm.resolution_timestamp = json_i64(row, "resolution_timestamp");
      cm.token_ids = json_str_arr(row, "token_ids");
      cm.payout_numerators = json_bigint_arr(row, "payout_numerators");
      if (row.contains("payout_denominator") && !row.at("payout_denominator").is_null()) {
        cm.payout_denominator = json_bigint(row, "payout_denominator");
        cm.has_payout_denominator = true;
      }
      cm.market_question = json_str(row, "market_question");
      cm.market_description = json_str(row, "market_description");
      cm.market_event_title = json_str(row, "market_event_title");
      cm.market_slug = json_str(row, "market_slug");
      cm.market_url = json_str(row, "market_url");
      cm.market_outcomes = json_str_arr(row, "market_outcomes");
      merge_condition(state_.conditions[cid], cm);
    }
  }
  if (seed.contains("tokens") && seed.at("tokens").is_array()) {
    for (const auto &row : seed.at("tokens")) {
      std::string tid = json_str(row, "token_id");
      if (tid.empty()) continue;
      TokenMeta tm;
      tm.token_id = tid;
      tm.condition_id = json_str(row, "condition_id");
      if (row.contains("price") && row.at("price").is_string()) {
        tm.price = parse_decimal(row.at("price").get<std::string>());
        tm.price_source = "seed";
      }
      merge_token(state_.tokens[tid], tm);
    }
  }
}

void SyncThread::append_snapshot(uint64_t block) {
  int64_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::lock_guard<std::mutex> lock(state_.mu);
  for (const auto &u : state_.users) {
    const UserState &us = state_.user_states.at(u);
    json positions = json::array();
    for (const auto &[tid, amt] : us.positions) {
      positions.push_back({{"token_id", tid}, {"amount_raw", bigint_to_str(amt)}});
    }
    state_.snapshot_root[u][block_key(block)] = {
        {"block_number", block},
        {"captured_at_unix_sec", now},
        {"stable_balances", {{"usdc_e_raw", bigint_to_str(us.stable.usdc_e)}, {"wrapped_raw", bigint_to_str(us.stable.wrapped)}}},
        {"positions", positions},
    };
  }
}

void SyncThread::persist_all() {
  progress().update("persist", 0, 0);
  std::lock_guard<std::mutex> lock(state_.mu);
  save_json(cfg_.meta_file, build_meta_json(state_));
  save_json(cfg_.aggregate_file, build_state_json(state_));
  save_json(cfg_.snapshot_file, state_.snapshot_root);
  save_json(cfg_.history_file, state_.history_root);
}

uint64_t SyncThread::rpc_block_number() {
  json r = rpc_call("eth_blockNumber", json::array());
  return hex_to_u64(r.get<std::string>());
}

json SyncThread::rpc_call(const std::string &method, const json &params) {
  json payload = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
  HttpRes resp = http_post(cfg_.rpc_http_url, payload);
  assert(resp.status == 200);
  json body = safe_parse(resp.body);
  assert(body.contains("result"));
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.counters.rpc_http++;
  }
  return body.at("result");
}

json SyncThread::rpc_batch(const std::vector<json> &reqs) {
  json payload = json::array();
  int id = 1;
  for (const auto &r : reqs) {
    json item = r;
    item["jsonrpc"] = "2.0";
    item["id"] = id++;
    payload.push_back(std::move(item));
  }
  HttpRes resp = http_post(cfg_.rpc_http_url, payload);
  assert(resp.status == 200);
  json body = safe_parse(resp.body);
  assert(body.is_array());
  std::sort(body.begin(), body.end(), [](const json &a, const json &b) {
    return a.at("id").get<int>() < b.at("id").get<int>();
  });
  {
    std::lock_guard<std::mutex> lock(state_.mu);
    state_.counters.rpc_http += reqs.size();
  }
  return body;
}

json SyncThread::graph_query(const std::string &subgraph_id, const std::string &query, const json &vars, const std::string &label) {
  std::string url = "https://gateway.thegraph.com/api/" + cfg_.graph_api_key + "/subgraphs/id/" + subgraph_id;
  json payload = {{"query", query}, {"variables", vars}};
  for (size_t retry = 0;; ++retry) {
    HttpRes resp = http_post(url, payload);
    assert(resp.status == 200);
    json body = safe_parse(resp.body);
    if (!body.contains("errors") && body.contains("data")) {
      std::lock_guard<std::mutex> lock(state_.mu);
      state_.counters.subgraph++;
      return body.at("data");
    }
    logger().warn("subgraph retry " + std::to_string(retry + 1) + ": " + label + " - " + body.dump());
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

} // namespace tracker
