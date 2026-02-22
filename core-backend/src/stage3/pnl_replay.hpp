#pragma once

#include "../stage2/event_build.hpp"
#include "../stage2/types.hpp"
#include "misc/profiler.hpp"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace stage3 {

using namespace stage2;

static constexpr int REPLAY_WORKERS = 16;

struct ReplayProgress {
  int64_t total_users = 0;
  int64_t processed_users = 0;
  bool running = false;
  double replay_ms = 0;
};

class PnlEngine {
public:
  explicit PnlEngine(EventBuilder &builder) : builder_(builder) {}

  // TODO: port to new chunk-based EventBuilder interface
#if 0
  void rebuild_all() {
    if (builder_.progress().running) {
      return;
    }

    builder_.build_all();

    replay_progress_ = ReplayProgress{};
    replay_progress_.running = true;

    auto t0 = std::chrono::steady_clock::now();

    users_ = builder_.users();
    user_events_ = builder_.user_events();
    conditions_ = builder_.conditions();
    cond_ids_ = builder_.condition_ids();

    user_map_.clear();
    for (size_t i = 0; i < users_.size(); ++i) {
      user_map_[users_[i]] = static_cast<uint32_t>(i);
    }

    replay_all();

    auto t1 = std::chrono::steady_clock::now();
    replay_progress_.replay_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    replay_progress_.running = false;
  }
#endif
  void rebuild_all() {}

  const BuildProgress &build_progress() const { return builder_.progress(); }
  const ReplayProgress &replay_progress() const { return replay_progress_; }

  struct RebuildProgress {
    int phase = 0;
    int64_t total_conditions = 0;
    int64_t total_tokens = 0;
    int64_t total_events = 0;
    int64_t total_users = 0;
    int64_t processed_users = 0;
    bool running = false;
    double phase1_ms = 0;
    double phase2_ms = 0;
    double phase3_ms = 0;
    int64_t cnt_split = 0;
    int64_t cnt_merge = 0;
    int64_t cnt_redemption = 0;
    int64_t cnt_convert = 0;
    int64_t cnt_order = 0;
    int64_t cnt_fpmm_trade = 0;
    int64_t cnt_fpmm_funding = 0;
    int64_t cnt_transfer = 0;
  };

  RebuildProgress progress() const {
    const auto &bp = builder_.progress();
    RebuildProgress p;
    p.phase = bp.phase;
    p.total_conditions = bp.total_conditions;
    p.total_tokens = bp.total_tokens;
    p.total_events = bp.total_events;
    p.running = bp.running;
    p.cnt_split = bp.cnt_split;
    p.cnt_merge = bp.cnt_merge;
    p.cnt_redemption = bp.cnt_redemption;
    p.cnt_convert = bp.cnt_convert;
    p.cnt_order = bp.cnt_order;
    p.cnt_fpmm_trade = bp.cnt_fpmm_trade;
    p.cnt_fpmm_funding = bp.cnt_fpmm_funding;
    p.cnt_transfer = bp.cnt_transfer;
    return p;
  }

  const UserState *get_user_state(const std::string &addr) const {
    std::string lower = addr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = user_map_.find(lower);
    if (it == user_map_.end())
      return nullptr;
    return &user_states_[it->second];
  }

  const UserState *find_user(const std::string &addr) const { return get_user_state(addr); }
  const ConditionInfo &get_condition(uint32_t idx) const { return conditions_[idx]; }
  const std::string &get_condition_id(uint32_t idx) const { return cond_ids_[idx]; }
  const std::vector<ConditionInfo> &conditions() const { return conditions_; }
  const std::vector<std::string> &condition_ids() const { return cond_ids_; }
  const std::vector<std::string> &users() const { return users_; }
  const std::vector<UserState> &user_states() const { return user_states_; }

  struct UserSummary {
    std::string addr;
    int64_t event_count;
    int64_t realized_pnl;
  };

  std::vector<UserSummary> get_users_sorted(int64_t limit = 200) const {
    TraceN("s3/users");
    std::vector<UserSummary> result;
    result.reserve(users_.size());
    for (size_t i = 0; i < users_.size(); ++i) {
      int64_t event_count = 0;
      int64_t realized_pnl = 0;
      for (const auto &ch : user_states_[i].conditions) {
        event_count += static_cast<int64_t>(ch.snapshots.size());
        if (!ch.snapshots.empty()) {
          realized_pnl += ch.snapshots.back().realized_pnl;
        }
      }
      if (event_count > 0) {
        result.push_back({users_[i], event_count, realized_pnl});
      }
    }
    std::sort(result.begin(), result.end(), [](const UserSummary &a, const UserSummary &b) {
      return a.event_count > b.event_count;
    });
    if (limit > 0 && static_cast<int64_t>(result.size()) > limit) {
      result.resize(static_cast<size_t>(limit));
    }
    return result;
  }

  struct TimelineEntry {
    int64_t sort_key;
    uint8_t event_type;
    int64_t realized_pnl;
    int64_t delta;
    int64_t price;
    uint32_t cond_idx;
    uint8_t token_idx;
    int token_count;
  };

  std::vector<TimelineEntry> get_user_timeline(const std::string &addr) const {
    TraceN("s3/timeline");
    const auto *state = get_user_state(addr);
    if (!state)
      return {};

    std::vector<TimelineEntry> timeline;
    for (const auto &ch : state->conditions) {
      for (const auto &snap : ch.snapshots) {
        int token_count = 0;
        for (int i = 0; i < snap.outcome_count; ++i) {
          if (snap.positions[i] != 0)
            ++token_count;
        }
        timeline.push_back({snap.sort_key, snap.event_type, snap.realized_pnl, snap.delta,
                            snap.price, ch.cond_idx, snap.token_idx, token_count});
      }
    }
    std::sort(timeline.begin(), timeline.end(),
              [](const TimelineEntry &a, const TimelineEntry &b) { return a.sort_key < b.sort_key; });

    std::unordered_map<uint32_t, int> cond_token_count;
    for (auto &e : timeline) {
      auto &tc = cond_token_count[e.cond_idx];
      tc = e.token_count;
      int cum_tokens = 0;
      for (const auto &[_, c] : cond_token_count) {
        cum_tokens += c;
      }
      e.token_count = cum_tokens;
    }
    return timeline;
  }

  struct PositionAtTime {
    std::string condition_id;
    int64_t positions[MAX_OUTCOMES];
    int64_t cost_basis;
    int64_t realized_pnl;
    int outcome_count;
  };

  std::vector<PositionAtTime> get_positions_at(const std::string &addr, int64_t sort_key) const {
    TraceN("s3/positions");
    const auto *state = get_user_state(addr);
    if (!state)
      return {};

    std::vector<PositionAtTime> result;
    for (const auto &ch : state->conditions) {
      if (ch.snapshots.empty())
        continue;
      auto it = std::upper_bound(ch.snapshots.begin(), ch.snapshots.end(), sort_key,
                                 [](int64_t sk, const Snapshot &s) { return sk < s.sort_key; });
      if (it == ch.snapshots.begin())
        continue;
      --it;
      const auto &snap = *it;
      bool has_pos = false;
      for (int i = 0; i < snap.outcome_count; ++i) {
        if (snap.positions[i] != 0) {
          has_pos = true;
          break;
        }
      }
      if (!has_pos && snap.realized_pnl == 0)
        continue;
      PositionAtTime pos;
      pos.condition_id = cond_ids_[ch.cond_idx];
      std::memcpy(pos.positions, snap.positions, sizeof(snap.positions));
      pos.cost_basis = snap.cost_basis;
      pos.realized_pnl = snap.realized_pnl;
      pos.outcome_count = snap.outcome_count;
      result.push_back(pos);
    }
    return result;
  }

  struct TradeEntry {
    int64_t sort_key;
    uint8_t event_type;
    int64_t delta;
    int64_t price;
    uint32_t cond_idx;
    uint8_t token_idx;
  };

  std::vector<TradeEntry> get_trades_near(const std::string &addr, int64_t sort_key, int radius = 20) const {
    TraceN("s3/trades");
    auto timeline = get_user_timeline(addr);
    if (timeline.empty())
      return {};

    auto it = std::lower_bound(timeline.begin(), timeline.end(), sort_key,
                               [](const TimelineEntry &e, int64_t sk) { return e.sort_key < sk; });
    size_t center = (it == timeline.end()) ? timeline.size() - 1 : static_cast<size_t>(it - timeline.begin());
    size_t start = (center > static_cast<size_t>(radius)) ? center - radius : 0;
    size_t end = std::min(center + radius + 1, timeline.size());

    std::vector<TradeEntry> result;
    for (size_t i = start; i < end; ++i) {
      const auto &e = timeline[i];
      result.push_back({e.sort_key, e.event_type, e.delta, e.price, e.cond_idx, e.token_idx});
    }
    return result;
  }

  size_t get_trades_center_index(const std::string &addr, int64_t sort_key, int radius = 20) const {
    auto timeline = get_user_timeline(addr);
    if (timeline.empty())
      return 0;

    auto it = std::lower_bound(timeline.begin(), timeline.end(), sort_key,
                               [](const TimelineEntry &e, int64_t sk) { return e.sort_key < sk; });
    size_t center = (it == timeline.end()) ? timeline.size() - 1 : static_cast<size_t>(it - timeline.begin());
    size_t start = (center > static_cast<size_t>(radius)) ? center - radius : 0;
    return center - start;
  }

private:
  EventBuilder &builder_;
  ReplayProgress replay_progress_;

  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::vector<std::string> users_;
  std::unordered_map<std::string, uint32_t> user_map_;
  std::vector<std::vector<RawEvent>> user_events_;
  std::vector<UserState> user_states_;

  void replay_all() {
    size_t nu = users_.size();
    user_states_.resize(nu);
    replay_progress_.total_users = static_cast<int64_t>(nu);
    replay_progress_.processed_users = 0;

    int nw = std::min(REPLAY_WORKERS, std::max(1, static_cast<int>(std::thread::hardware_concurrency())));
    std::vector<std::thread> workers;
    std::atomic<size_t> next_user{0};

    for (int w = 0; w < nw; ++w) {
      workers.emplace_back([this, &next_user, nu]() {
        while (true) {
          size_t uid = next_user.fetch_add(1);
          if (uid >= nu)
            break;
          replay_user(uid);
          ++replay_progress_.processed_users;
        }
      });
    }

    for (auto &t : workers) {
      t.join();
    }
  }

  void replay_user(size_t uid) {
    auto &events = user_events_[uid];
    if (events.empty()) {
      user_events_[uid].clear();
      user_events_[uid].shrink_to_fit();
      return;
    }

    std::sort(events.begin(), events.end(),
              [](const RawEvent &a, const RawEvent &b) { return a.sort_key < b.sort_key; });

    std::unordered_map<uint32_t, ReplayState> states;
    std::unordered_map<uint32_t, std::vector<Snapshot>> snaps;

    for (const auto &evt : events) {
      auto &st = states[evt.cond_idx];
      const auto &cond = conditions_[evt.cond_idx];

      apply_event(evt, st, cond);

      Snapshot snap{};
      snap.sort_key = evt.sort_key;
      snap.delta = evt.amount;
      snap.price = evt.price;
      snap.event_type = evt.type;
      snap.token_idx = evt.token_idx;
      snap.outcome_count = cond.outcome_count;
      std::memcpy(snap.positions, st.positions, sizeof(st.positions));
      snap.cost_basis = 0;
      for (int i = 0; i < cond.outcome_count; ++i) {
        snap.cost_basis += st.cost[i];
      }
      snap.realized_pnl = st.realized_pnl;
      snaps[evt.cond_idx].push_back(snap);
    }

    auto &us = user_states_[uid];
    us.conditions.reserve(snaps.size());
    for (auto &[cond_idx, snap_vec] : snaps) {
      us.conditions.push_back({cond_idx, std::move(snap_vec)});
    }

    user_events_[uid].clear();
    user_events_[uid].shrink_to_fit();
  }

  static void apply_event(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    switch (static_cast<EventType>(evt.type)) {
    case EventType::Buy:
    case EventType::FPMMBuy:
      apply_buy(evt, st);
      break;
    case EventType::Sell:
    case EventType::FPMMSell:
      apply_sell(evt, st);
      break;
    case EventType::Split:
      apply_split(evt, st);
      break;
    case EventType::Merge:
      apply_merge(evt, st);
      break;
    case EventType::Redemption:
      apply_redemption(evt, st);
      break;
    case EventType::FPMMLPAdd:
      apply_lp_add(evt, st);
      break;
    case EventType::FPMMLPRemove:
      apply_lp_remove(evt, st);
      break;
    case EventType::Convert:
      apply_convert(evt, st);
      break;
    case EventType::TransferIn:
      apply_transfer_in(evt, st);
      break;
    case EventType::TransferOut:
      apply_transfer_out(evt, st);
      break;
    }
  }

  static void apply_buy(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    st.cost[i] += evt.amount * evt.price;
    st.positions[i] += evt.amount;
  }

  static void apply_sell(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    int64_t pos = st.positions[i];
    if (pos <= 0)
      return;
    int64_t sell = std::min(evt.amount, pos);
    int64_t cost_removed = st.cost[i] * sell / pos;
    st.realized_pnl += (sell * evt.price - cost_removed) / 1000000;
    st.cost[i] -= cost_removed;
    st.positions[i] -= sell;
  }

  static void apply_split(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    st.cost[i] += evt.amount * evt.price;
    st.positions[i] += evt.amount;
  }

  static void apply_merge(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    int64_t pos = st.positions[i];
    if (pos <= 0)
      return;
    int64_t sell = std::min(evt.amount, pos);
    int64_t cost_removed = st.cost[i] * sell / pos;
    st.realized_pnl += (sell * evt.price - cost_removed) / 1000000;
    st.cost[i] -= cost_removed;
    st.positions[i] -= sell;
  }

  static void apply_redemption(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    int64_t pos = st.positions[i];
    if (pos <= 0)
      return;
    int64_t cost_removed = st.cost[i];
    st.realized_pnl += (pos * evt.price - cost_removed) / 1000000;
    st.cost[i] = 0;
    st.positions[i] = 0;
  }

  static void apply_lp_add(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    st.cost[i] += evt.amount * evt.price;
    st.positions[i] += evt.amount;
  }

  static void apply_lp_remove(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    int64_t pos = st.positions[i];
    if (pos <= 0)
      return;
    int64_t actual = std::min(evt.amount, pos);
    int64_t cost_removed = st.cost[i] * actual / pos;
    st.realized_pnl += (actual * evt.price - cost_removed) / 1000000;
    st.cost[i] -= cost_removed;
    st.positions[i] -= actual;
  }

  static void apply_convert(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    int64_t pos = st.positions[i];
    if (pos <= 0)
      return;
    int64_t actual = std::min(evt.amount, pos);
    int64_t cost_removed = st.cost[i] * actual / pos;
    st.realized_pnl += (actual * evt.price - cost_removed) / 1000000;
    st.cost[i] -= cost_removed;
    st.positions[i] -= actual;
  }

  static void apply_transfer_in(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    st.positions[i] += evt.amount;
  }

  static void apply_transfer_out(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    int64_t pos = st.positions[i];
    if (pos <= 0)
      return;
    int64_t actual = std::min(evt.amount, pos);
    int64_t cost_removed = st.cost[i] * actual / pos;
    st.cost[i] -= cost_removed;
    st.positions[i] -= actual;
  }
};

} // namespace stage3
