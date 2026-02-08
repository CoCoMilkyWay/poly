#pragma once

// ============================================================================
// Replayer — 从 rebuild 内存数据序列化单用户完整交易时间线
//
// serialize_user()      — 返回 timeline + per-condition snapshots JSON
// serialize_user_list() — 返回按事件数排序的用户列表 JSON
// ============================================================================

#include "../rebuild/rebuilder.hpp"

#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_map>

namespace replayer {

using json = nlohmann::json;

// ============================================================================
// 序列化单用户数据 → JSON
// ============================================================================
inline json serialize_user(const rebuild::Engine &engine, const std::string &user_id) {
  const auto *state = engine.find_user(user_id);
  assert(state != nullptr && "user not found");

  const auto &cond_ids = engine.condition_ids();
  const auto &conditions = engine.conditions();

  // -- 收集所有 condition 的 snapshots 到扁平 timeline --
  struct TimelineEntry {
    int64_t timestamp;
    uint32_t cond_idx;
    uint8_t event_type;
    uint8_t token_idx;
    int64_t delta;
    int64_t price;
    int64_t cond_rpnl; // per-condition cumulative
  };

  std::vector<TimelineEntry> timeline;
  for (const auto &ch : state->conditions) {
    for (const auto &snap : ch.snapshots) {
      timeline.push_back({
          snap.timestamp,
          ch.cond_idx,
          snap.event_type,
          snap.token_idx,
          snap.delta,
          snap.price,
          snap.realized_pnl,
      });
    }
  }

  std::sort(timeline.begin(), timeline.end(),
            [](const TimelineEntry &a, const TimelineEntry &b) {
              return a.timestamp < b.timestamp;
            });

  // -- 计算逐事件的全局累计 realized PnL --
  std::unordered_map<uint32_t, int64_t> cond_rpnl;
  json j_timeline = json::array();

  for (const auto &e : timeline) {
    cond_rpnl[e.cond_idx] = e.cond_rpnl;
    int64_t global_rpnl = 0;
    for (const auto &[ci, rpnl] : cond_rpnl)
      global_rpnl += rpnl;

    j_timeline.push_back({
        {"ts", e.timestamp},
        {"ty", e.event_type},
        {"ti", e.token_idx},
        {"ci", e.cond_idx},
        {"d", e.delta},
        {"p", e.price},
        {"rpnl", global_rpnl},
    });
  }

  // -- per-condition snapshot chains (用于 cursor 查持仓) --
  json j_conditions = json::object();
  for (const auto &ch : state->conditions) {
    uint32_t ci = ch.cond_idx;
    const auto &cond = conditions[ci];

    json j_snaps = json::array();
    for (const auto &snap : ch.snapshots) {
      json j_pos = json::array();
      for (int k = 0; k < cond.outcome_count; ++k)
        j_pos.push_back(snap.positions[k]);

      j_snaps.push_back({
          {"ts", snap.timestamp},
          {"ty", snap.event_type},
          {"ti", snap.token_idx},
          {"d", snap.delta},
          {"p", snap.price},
          {"pos", j_pos},
          {"cost", snap.cost_basis},
          {"rpnl", snap.realized_pnl},
      });
    }

    j_conditions[std::to_string(ci)] = {
        {"id", cond_ids[ci]},
        {"oc", cond.outcome_count},
        {"snaps", j_snaps},
    };
  }

  int64_t first_ts = timeline.empty() ? 0 : timeline.front().timestamp;
  int64_t last_ts = timeline.empty() ? 0 : timeline.back().timestamp;

  return {
      {"user", user_id},
      {"total_events", (int64_t)timeline.size()},
      {"first_ts", first_ts},
      {"last_ts", last_ts},
      {"conditions", j_conditions},
      {"timeline", j_timeline},
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
