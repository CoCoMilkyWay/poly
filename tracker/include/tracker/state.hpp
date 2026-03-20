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

struct StableBalances {
  BigInt usdc = 0;
  BigInt usdc_e = 0;
  BigInt usdt = 0;
  BigInt wrapped = 0;
};

struct TokenMeta {
  std::string cond; // token_id → condition_id 映射
};

struct ConditionMeta {
  // 身份标识
  std::string qid;                    // question_id (NegRisk 专用)
  uint8_t oc = 0;                     // outcome_count
  uint8_t coll = 0;                   // collateral enum
  std::vector<std::string> tids;      // token_ids[idx]

  // 市场信息 (gamma)
  std::string q;                      // question
  std::string desc;                   // description
  std::string slug;                   // slug
  std::vector<std::string> outcomes;  // outcomes[]

  // 价格
  std::vector<int64_t> prices;        // 价格 * 1e6 (per outcome)
  std::vector<int64_t> price_ts;      // 价格时间 unix sec (per outcome)

  // 时间
  std::string start;                  // 开始时间 ISO8601
  std::string end;                    // 结束时间 ISO8601

  // 结算
  std::vector<BigInt> payout;         // payout_numerators[]
  BigInt payout_d = 0;                // payout_denominator
  bool has_payout_d = false;

  // 状态
  bool updated = false;               // 已从 gamma 成功更新
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

struct VisibleTokenState {
  BigInt amount = 0;
  long double value_usd = 0.0L;
};

struct UserViewState {
  size_t raw_position_count = 0;
  size_t filtered_dust_count = 0;
  size_t filtered_settled_count = 0;
  long double token_value_usd = 0.0L;
  long double stable_value_usd = 0.0L;
  long double total_value_usd = 0.0L;
  bool qualifies_for_aggregate = false;
  std::map<std::string, VisibleTokenState> visible_tokens;
};

struct AggregateTokenState {
  BigInt amount = 0;
  long double value_usd = 0.0L;
  size_t holder_count = 0;
};

struct QueryCounters {
  uint64_t rpc_http = 0;
  uint64_t rpc_ws_msg = 0;
  uint64_t rpc_ws_sub = 0;
  uint64_t snapshot_api = 0;
  uint64_t gamma = 0;
  uint64_t clob = 0;
};

struct RuntimeState {
  std::vector<std::string> users;
  std::unordered_set<std::string> user_set;
  std::map<std::string, UserSnapshotState> user_snapshots;
  std::map<std::string, UserLiveState> user_states;
  std::map<std::string, UserViewState> user_views; // raw user_states 的派生有效视图
  std::map<std::string, TokenMeta> tokens;
  std::map<std::string, ConditionMeta> conditions;
  std::map<std::string, MarketMeta> markets;
  std::map<std::string, AggregateTokenState> aggregate_tokens; // 有效用户贡献的聚合桶
  std::map<std::string, std::unordered_set<std::string>> token_holders; // token -> 持有用户
  long double aggregate_value_usd = 0.0L; // aggregate_tokens 的总价值
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

// 从 condition.tids 推断 token 的 idx
inline uint8_t get_token_idx(const std::map<std::string, ConditionMeta> &conditions,
                             const std::string &cond_id,
                             const std::string &token_id) {
  auto it = conditions.find(cond_id);
  if (it == conditions.end()) {
    return 0xFF;
  }
  for (size_t i = 0; i < it->second.tids.size(); ++i) {
    if (it->second.tids[i] == token_id) {
      return static_cast<uint8_t>(i);
    }
  }
  return 0xFF;
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
  // prices / price_ts 按索引更新 (非负值覆盖)
  if (dst.prices.size() < target_size) {
    dst.prices.resize(target_size, -1);
  }
  if (dst.price_ts.size() < target_size) {
    dst.price_ts.resize(target_size, 0);
  }
  for (size_t i = 0; i < src.prices.size(); ++i) {
    if (src.prices[i] >= 0) {
      dst.prices[i] = src.prices[i];
    }
  }
  for (size_t i = 0; i < src.price_ts.size(); ++i) {
    if (src.price_ts[i] > 0) {
      dst.price_ts[i] = src.price_ts[i];
    }
  }
  if (dst.start.empty()) {
    dst.start = src.start;
  }
  if (dst.end.empty()) {
    dst.end = src.end;
  }
  if (!src.payout.empty()) {
    dst.payout = src.payout;
  }
  if (src.has_payout_d) {
    dst.payout_d = src.payout_d;
    dst.has_payout_d = true;
  }
  if (dst.q.empty()) {
    dst.q = src.q;
  }
  if (dst.desc.empty()) {
    dst.desc = src.desc;
  }
  if (dst.slug.empty()) {
    dst.slug = src.slug;
  }
  if (dst.outcomes.empty()) {
    dst.outcomes = src.outcomes;
  }
  dst.updated = dst.updated || src.updated;
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

inline long double stable_value_usd(const StableBalances &stable) {
  return bigint_to_units(stable.usdc) + bigint_to_units(stable.usdc_e) +
         bigint_to_units(stable.usdt) + bigint_to_units(stable.wrapped);
}

inline long double token_value_usd(const BigInt &amount, int64_t price) {
  if (price < 0) {
    return 0.0L;
  }
  return bigint_to_units(amount) * static_cast<long double>(price) /
         static_cast<long double>(kPriceScale);
}

inline bool is_settled(const ConditionMeta &cond) {
  return cond.has_payout_d && cond.payout_d > 0;
}

} // namespace tracker
