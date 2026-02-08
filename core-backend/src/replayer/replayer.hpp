#pragma once

// ============================================================================
// Replayer — 从 rebuild 内存数据序列化单用户完整交易时间线
//
// serialize_user_timeline() — 返回 timeline JSON (快速首屏)
// serialize_positions_at()  — 返回指定时刻的持仓快照 JSON (按需)
// serialize_user_list()     — 返回按事件数排序的用户列表 JSON
// ============================================================================

#include "../rebuild/rebuilder.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>

#define REPLAY_DUST_THRESHOLD_USDC 50

namespace replayer {

using json = nlohmann::json;
static constexpr int64_t DUST_THRESHOLD = REPLAY_DUST_THRESHOLD_USDC * 1000000LL;

// ============================================================================
// 序列化单用户 timeline → JSON (瘦身: 仅 PnL 曲线 + #token 曲线, 用于快速首屏)
// ============================================================================
inline std::string serialize_user_timeline(const rebuild::Engine &engine, const std::string &user_id) {
  const auto *state = engine.find_user(user_id);
  assert(state != nullptr && "user not found");

  // -- 收集所有 condition 的 snapshots 到扁平 timeline --
  struct TimelineEntry {
    int64_t timestamp;
    uint32_t cond_idx;
    uint8_t event_type;
    uint8_t outcome_count;
    int64_t cond_rpnl; // per-condition cumulative
    int64_t positions[rebuild::MAX_OUTCOMES];
  };

  std::vector<TimelineEntry> timeline;
  for (const auto &ch : state->conditions) {
    for (const auto &snap : ch.snapshots) {
      TimelineEntry te{};
      te.timestamp = snap.timestamp;
      te.cond_idx = ch.cond_idx;
      te.event_type = snap.event_type;
      te.outcome_count = snap.outcome_count;
      te.cond_rpnl = snap.realized_pnl;
      std::memcpy(te.positions, snap.positions, sizeof(snap.positions));
      timeline.push_back(te);
    }
  }

  std::sort(timeline.begin(), timeline.end(),
            [](const TimelineEntry &a, const TimelineEntry &b) {
              return a.timestamp < b.timestamp;
            });

  // -- 计算逐事件的全局累计 realized PnL + 活跃 condition 数 (按 condition 维度 dust 过滤) --
  // -- 直接写 JSON 字符串, 跳过 nlohmann::json 避免百万级堆分配 --
  std::unordered_map<uint32_t, int64_t> cond_rpnl_map;
  std::unordered_map<uint32_t, bool> cond_non_dust; // per-condition non-dust flag
  int64_t global_rpnl = 0;
  int64_t total_tokens = 0;

  std::string buf;
  buf.reserve(timeline.size() * 40 + 256);
  buf += '[';
  bool first = true;

  for (const auto &e : timeline) {
    global_rpnl += e.cond_rpnl - cond_rpnl_map[e.cond_idx];
    cond_rpnl_map[e.cond_idx] = e.cond_rpnl;

    // per-condition: sum of abs(positions) across all outcomes
    int64_t abs_sum = 0;
    for (int k = 0; k < e.outcome_count; ++k)
      abs_sum += std::abs(e.positions[k]);

    bool old_non_dust = cond_non_dust[e.cond_idx];
    bool new_non_dust = (abs_sum >= DUST_THRESHOLD);
    cond_non_dust[e.cond_idx] = new_non_dust;

    if (new_non_dust && !old_non_dust) total_tokens++;
    else if (!new_non_dust && old_non_dust) total_tokens--;

    if (!first) buf += ',';
    first = false;
    buf += "{\"ts\":";   buf += std::to_string(e.timestamp);
    buf += ",\"ty\":";   buf += std::to_string((int)e.event_type);
    buf += ",\"rpnl\":"; buf += std::to_string(global_rpnl);
    buf += ",\"tk\":";   buf += std::to_string(total_tokens);
    buf += '}';
  }
  buf += ']';

  int64_t first_ts = timeline.empty() ? 0 : timeline.front().timestamp;
  int64_t last_ts = timeline.empty() ? 0 : timeline.back().timestamp;

  return "{\"user\":\"" + user_id + "\""
       + ",\"total_events\":" + std::to_string((int64_t)timeline.size())
       + ",\"first_ts\":" + std::to_string(first_ts)
       + ",\"last_ts\":" + std::to_string(last_ts)
       + ",\"dust_threshold\":" + std::to_string(DUST_THRESHOLD)
       + ",\"timeline\":" + buf + "}";
}

// ============================================================================
// 查询指定时刻附近的交易记录 → JSON (按需调用, cursor 联动)
// ============================================================================
inline json serialize_trades_at(const rebuild::Engine &engine, const std::string &user_id,
                                int64_t ts, int radius = 20) {
  const auto *state = engine.find_user(user_id);
  assert(state != nullptr && "user not found");

  const auto &cond_ids = engine.condition_ids();

  struct TradeEntry {
    int64_t timestamp;
    uint32_t cond_idx;
    uint8_t event_type;
    uint8_t token_idx;
    int64_t delta;
    int64_t price;
  };

  std::vector<TradeEntry> trades;
  for (const auto &ch : state->conditions) {
    for (const auto &snap : ch.snapshots) {
      trades.push_back({snap.timestamp, ch.cond_idx, snap.event_type,
                         snap.token_idx, snap.delta, snap.price});
    }
  }

  std::sort(trades.begin(), trades.end(),
            [](const TradeEntry &a, const TradeEntry &b) { return a.timestamp < b.timestamp; });

  // 二分查找最接近 ts 的事件
  int lo = 0, hi = (int)trades.size();
  while (lo < hi) {
    int mid = (lo + hi) >> 1;
    if (trades[mid].timestamp < ts) lo = mid + 1;
    else hi = mid;
  }
  int center = lo;
  if (center > 0 && center < (int)trades.size()) {
    if (std::abs(trades[center - 1].timestamp - ts) <= std::abs(trades[center].timestamp - ts))
      center = center - 1;
  } else if (center >= (int)trades.size()) {
    center = (int)trades.size() - 1;
  }

  int start = std::max(0, center - radius);
  int end = std::min((int)trades.size() - 1, center + radius);

  json events = json::array();
  for (int i = start; i <= end; ++i) {
    const auto &t = trades[i];
    events.push_back({
        {"ts", t.timestamp},
        {"ty", (int)t.event_type},
        {"ti", (int)t.token_idx},
        {"ci", t.cond_idx},
        {"cid", cond_ids[t.cond_idx]},
        {"d", t.delta},
        {"p", t.price},
    });
  }

  return {
      {"ts", ts},
      {"center", center - start},
      {"events", events},
  };
}

// ============================================================================
// 查询指定时刻的持仓快照 → JSON (服务端二分查找, 按需调用)
// ============================================================================
inline json serialize_positions_at(const rebuild::Engine &engine, const std::string &user_id, int64_t ts) {
  const auto *state = engine.find_user(user_id);
  assert(state != nullptr && "user not found");

  const auto &cond_ids = engine.condition_ids();
  const auto &conditions = engine.conditions();

  // Step 1: 收集每个 condition 在 ts 时刻的 snapshot, 按 condition 维度 dust 过滤
  struct CondSnap {
    uint32_t cond_idx;
    const rebuild::Snapshot *snap;
  };
  std::vector<CondSnap> cond_snaps;

  for (const auto &ch : state->conditions) {
    const auto &snaps = ch.snapshots;
    if (snaps.empty()) continue;
    // 二分查找: 最后一个 snap.timestamp <= ts
    int lo = 0, hi = (int)snaps.size() - 1, best = -1;
    while (lo <= hi) {
      int mid = (lo + hi) >> 1;
      if (snaps[mid].timestamp <= ts) { best = mid; lo = mid + 1; }
      else hi = mid - 1;
    }
    if (best < 0) continue;
    const auto &snap = snaps[best];
    // per-condition dust check: sum of abs(positions) across all outcomes
    int64_t abs_sum = 0;
    for (int k = 0; k < snap.outcome_count; ++k)
      abs_sum += std::abs(snap.positions[k]);
    if (abs_sum < DUST_THRESHOLD) continue;
    cond_snaps.push_back({ch.cond_idx, &snap});
  }

  // Step 2: 构造 JSON (已按 condition 维度过滤)
  json j_positions = json::array();
  for (const auto &cs : cond_snaps) {
    const auto &cond = conditions[cs.cond_idx];
    json j_pos = json::array();
    for (int k = 0; k < cond.outcome_count; ++k)
      j_pos.push_back(cs.snap->positions[k]);

    j_positions.push_back({
        {"ci", cs.cond_idx},
        {"id", cond_ids[cs.cond_idx]},
        {"oc", cond.outcome_count},
        {"pos", j_pos},
        {"cost", cs.snap->cost_basis},
        {"rpnl", cs.snap->realized_pnl},
    });
  }

  // 按 |rpnl| 降序
  std::sort(j_positions.begin(), j_positions.end(),
            [](const json &a, const json &b) {
              return std::abs(a["rpnl"].get<int64_t>()) > std::abs(b["rpnl"].get<int64_t>());
            });

  return {
      {"ts", ts},
      {"count", (int64_t)j_positions.size()},
      {"dust_threshold", DUST_THRESHOLD},
      {"positions", j_positions},
  };
}

// ============================================================================
// 序列化用户列表 (按事件数降序, 前 limit 个)
// ============================================================================
inline json serialize_user_list(const rebuild::Engine &engine, int limit) {
  const auto &users = engine.users();
  const auto &states = engine.user_states();

  struct UserInfo {
    size_t idx;
    size_t event_count;
  };

  std::vector<UserInfo> infos;
  infos.reserve(users.size());

  for (size_t i = 0; i < users.size(); ++i) {
    size_t count = 0;
    for (const auto &ch : states[i].conditions)
      count += ch.snapshots.size();
    infos.push_back({i, count});
  }

  std::sort(infos.begin(), infos.end(),
            [](const UserInfo &a, const UserInfo &b) {
              return a.event_count > b.event_count;
            });

  json result = json::array();
  int n = std::min((int)infos.size(), limit);
  for (int i = 0; i < n; ++i) {
    result.push_back({
        {"user_addr", users[infos[i].idx]},
        {"event_count", (int64_t)infos[i].event_count},
    });
  }

  return result;
}

} // namespace replayer
