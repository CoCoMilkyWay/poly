#pragma once

#include "tracker/codec.hpp"
#include "tracker/json.hpp"

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace tracker {

enum class Collateral : uint8_t {
  Unknown = 0,
  USDC = 1,
  USDCe = 2,
  USDT = 3,
  WrappedUSDCe = 4,
};

enum class EventType : uint8_t {
  OrderBuy = 0,
  OrderSell = 1,
  Split = 2,
  Merge = 3,
  Redeem = 4,
  Convert = 5,
};

inline uint8_t to_u8(Collateral value) {
  return static_cast<uint8_t>(value);
}

inline uint8_t to_u8(EventType value) {
  return static_cast<uint8_t>(value);
}

inline const char *collateral_label(Collateral collateral) {
  switch (collateral) {
    case Collateral::USDC:
      return "USDC";
    case Collateral::USDCe:
      return "USDC.e";
    case Collateral::USDT:
      return "USDT";
    case Collateral::WrappedUSDCe:
      return "WrappedUSDCe";
    case Collateral::Unknown:
      break;
  }
  return "";
}

inline const char *collateral_addr(Collateral collateral) {
  switch (collateral) {
    case Collateral::USDC:
      return kUsdc;
    case Collateral::USDCe:
      return kUsdcE;
    case Collateral::USDT:
      return kUsdt;
    case Collateral::WrappedUSDCe:
      return kWrappedUsdcE;
    case Collateral::Unknown:
      break;
  }
  return kZeroAddress;
}

inline Collateral collateral_from_addr(const std::string &addr) {
  std::string lower = norm_addr(addr);
  if (lower == kUsdc) {
    return Collateral::USDC;
  }
  if (lower == kUsdcE) {
    return Collateral::USDCe;
  }
  if (lower == kUsdt) {
    return Collateral::USDT;
  }
  if (lower == kWrappedUsdcE) {
    return Collateral::WrappedUSDCe;
  }
  return Collateral::Unknown;
}

inline bool is_usd_collateral(Collateral collateral) {
  return collateral != Collateral::Unknown;
}

struct StableBalances {
  BigInt usdc = 0;
  BigInt usdc_e = 0;
  BigInt usdt = 0;
  BigInt wrapped = 0;
};

struct TokenMeta {
  std::string cond;
  uint8_t idx = 0xFF;
  int64_t price = -1;
  std::string price_src;
};

struct ConditionMeta {
  std::string qid;
  uint8_t oc = 0;
  uint8_t coll = 0;
  std::vector<std::string> tids;
  bool resolved = false;
  std::vector<BigInt> payout;
  BigInt payout_d = 0;
  bool has_payout_d = false;
  std::string q;
  std::string desc;
  std::string slug;
  std::string url;
  std::vector<std::string> outcomes;
};

struct MarketMeta {
  std::vector<std::string> qids;
};

struct UserSnapshotState {
  uint64_t snapshot_block = 0;
  StableBalances stable;
  std::map<std::string, BigInt> positions;
};

struct UserLiveState {
  std::string user;
  StableBalances stable;
  std::map<std::string, BigInt> positions;
};

struct QueryCounters {
  uint64_t rpc_http = 0;
  uint64_t rpc_ws_msg = 0;
  uint64_t rpc_ws_sub = 0;
  uint64_t subgraph = 0;
  uint64_t gamma = 0;
};

struct RuntimeState {
  std::vector<std::string> users;
  std::unordered_set<std::string> user_set;
  std::map<std::string, UserSnapshotState> user_snapshots;
  std::map<std::string, UserLiveState> user_states;
  std::map<std::string, TokenMeta> tokens;
  std::map<std::string, ConditionMeta> conditions;
  std::map<std::string, MarketMeta> markets;
  json snapshot_root = json::object();
  json history_root = json::object();
  std::unordered_set<std::string> history_event_ids;
  std::deque<json> recent_events;
  uint64_t last_applied_block = 0;
  uint64_t head_block = 0;
  int64_t resync_started_at = 0;
  int64_t resync_finished_at = 0;
  QueryCounters counters;
};

struct AppState {
  std::shared_ptr<const json> state_ptr = std::make_shared<json>(json::object());
  std::shared_ptr<const json> meta_ptr = std::make_shared<json>(json::object());
  std::shared_ptr<const json> snapshot_ptr = std::make_shared<json>(json::object());
  std::shared_ptr<const json> history_ptr = std::make_shared<json>(json::object());
  std::atomic<uint64_t> version{0};
};

inline void publish_json(std::shared_ptr<const json> &slot, json value) {
  std::atomic_store(
      &slot, std::shared_ptr<const json>(std::make_shared<json>(std::move(value))));
}

inline std::shared_ptr<const json> load_published(
    const std::shared_ptr<const json> &slot) {
  return std::atomic_load(&slot);
}

inline void merge_token(TokenMeta &dst, const TokenMeta &src) {
  if (dst.cond.empty()) {
    dst.cond = src.cond;
  }
  if (dst.idx == 0xFF && src.idx != 0xFF) {
    dst.idx = src.idx;
  }
  if (src.price >= 0) {
    dst.price = src.price;
    dst.price_src = src.price_src;
  }
}

inline void merge_condition(ConditionMeta &dst, const ConditionMeta &src) {
  if (dst.qid.empty()) {
    dst.qid = src.qid;
  }
  if (dst.oc == 0 && src.oc > 0) {
    dst.oc = src.oc;
  }
  if (dst.coll == 0 && src.coll > 0) {
    dst.coll = src.coll;
  }
  size_t target_size = std::max(dst.tids.size(), src.tids.size());
  target_size = std::max(target_size, static_cast<size_t>(dst.oc));
  if (target_size > dst.tids.size()) {
    dst.tids.resize(target_size);
  }
  for (size_t i = 0; i < src.tids.size(); ++i) {
    if (!src.tids[i].empty()) {
      dst.tids[i] = src.tids[i];
    }
  }
  if (!src.payout.empty()) {
    dst.payout = src.payout;
  }
  if (src.has_payout_d) {
    dst.payout_d = src.payout_d;
    dst.has_payout_d = true;
    dst.resolved = true;
  }
  dst.resolved = dst.resolved || src.resolved;
  if (dst.q.empty()) {
    dst.q = src.q;
  }
  if (dst.desc.empty()) {
    dst.desc = src.desc;
  }
  if (dst.slug.empty()) {
    dst.slug = src.slug;
  }
  if (dst.url.empty()) {
    dst.url = src.url;
  }
  if (dst.outcomes.empty()) {
    dst.outcomes = src.outcomes;
  }
}

inline void merge_market(MarketMeta &dst, const MarketMeta &src) {
  std::unordered_set<std::string> seen(dst.qids.begin(), dst.qids.end());
  for (const auto &qid : src.qids) {
    if (!qid.empty() && seen.insert(qid).second) {
      dst.qids.push_back(qid);
    }
  }
}

inline void push_recent_event(RuntimeState &state, json row, size_t limit) {
  state.recent_events.push_back(std::move(row));
  while (state.recent_events.size() > limit) {
    state.recent_events.pop_front();
  }
}

} // namespace tracker
