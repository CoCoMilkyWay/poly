#pragma once

#include "../core/database.hpp"
#include "rebuilder_types.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace rebuild {

static constexpr int REBUILD_P3_WORKERS = 16;

class Engine {
public:
  explicit Engine(Database &db) : db_(db) {}

  void rebuild_all() {
    assert(!progress_.running);
    progress_ = RebuildProgress{};
    progress_.running = true;
    progress_.phase = 1;

    auto t0 = std::chrono::steady_clock::now();
    load_metadata();
    auto t1 = std::chrono::steady_clock::now();
    progress_.phase1_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    progress_.phase = 2;
    collect_events();
    auto t2 = std::chrono::steady_clock::now();
    progress_.phase2_ms =
        std::chrono::duration<double, std::milli>(t2 - t1).count();

    progress_.phase = 3;
    replay_all();
    auto t3 = std::chrono::steady_clock::now();
    progress_.phase3_ms =
        std::chrono::duration<double, std::milli>(t3 - t2).count();

    progress_.phase = 7;
    progress_.running = false;
  }

  const RebuildProgress &progress() const { return progress_; }

  const UserState *get_user_state(const std::string &addr) const {
    std::string lower = addr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto it = user_map_.find(lower);
    if (it == user_map_.end())
      return nullptr;
    return &user_states_[it->second];
  }

  const ConditionInfo &get_condition(uint32_t idx) const {
    return conditions_[idx];
  }

  const std::string &get_condition_id(uint32_t idx) const {
    return cond_ids_[idx];
  }

  struct UserSummary {
    std::string addr;
    int64_t event_count;
    int64_t realized_pnl;
  };

  std::vector<UserSummary> get_users_sorted(int64_t limit = 200) const {
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
    std::sort(result.begin(), result.end(),
              [](const UserSummary &a, const UserSummary &b) {
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
        timeline.push_back({snap.sort_key, snap.event_type, snap.realized_pnl,
                            snap.delta, snap.price, ch.cond_idx, snap.token_idx,
                            token_count});
      }
    }
    std::sort(timeline.begin(), timeline.end(),
              [](const TimelineEntry &a, const TimelineEntry &b) {
                return a.sort_key < b.sort_key;
              });

    int64_t cum_pnl = 0;
    int cum_tokens = 0;
    std::unordered_map<uint32_t, int> cond_token_count;
    for (auto &e : timeline) {
      cum_pnl = e.realized_pnl;
      auto &tc = cond_token_count[e.cond_idx];
      tc = e.token_count;
      cum_tokens = 0;
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

  std::vector<PositionAtTime> get_positions_at(const std::string &addr,
                                                int64_t sort_key) const {
    const auto *state = get_user_state(addr);
    if (!state)
      return {};

    std::vector<PositionAtTime> result;
    for (const auto &ch : state->conditions) {
      if (ch.snapshots.empty())
        continue;
      auto it =
          std::upper_bound(ch.snapshots.begin(), ch.snapshots.end(), sort_key,
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

  std::vector<TradeEntry> get_trades_near(const std::string &addr,
                                          int64_t sort_key, int radius = 20) const {
    auto timeline = get_user_timeline(addr);
    if (timeline.empty())
      return {};

    auto it = std::lower_bound(timeline.begin(), timeline.end(), sort_key,
                               [](const TimelineEntry &e, int64_t sk) {
                                 return e.sort_key < sk;
                               });
    size_t center = (it == timeline.end()) ? timeline.size() - 1
                                            : static_cast<size_t>(it - timeline.begin());
    size_t start = (center > static_cast<size_t>(radius)) ? center - radius : 0;
    size_t end = std::min(center + radius + 1, timeline.size());

    std::vector<TradeEntry> result;
    for (size_t i = start; i < end; ++i) {
      const auto &e = timeline[i];
      result.push_back(
          {e.sort_key, e.event_type, e.delta, e.price, e.cond_idx, e.token_idx});
    }
    return result;
  }

  size_t get_trades_center_index(const std::string &addr, int64_t sort_key,
                                 int radius = 20) const {
    auto timeline = get_user_timeline(addr);
    if (timeline.empty())
      return 0;

    auto it = std::lower_bound(timeline.begin(), timeline.end(), sort_key,
                               [](const TimelineEntry &e, int64_t sk) {
                                 return e.sort_key < sk;
                               });
    size_t center = (it == timeline.end()) ? timeline.size() - 1
                                            : static_cast<size_t>(it - timeline.begin());
    size_t start = (center > static_cast<size_t>(radius)) ? center - radius : 0;
    return center - start;
  }

private:
  Database &db_;
  RebuildProgress progress_;

  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<std::string, uint32_t> cond_map_;
  std::unordered_map<std::string, TokenInfo> token_map_;
  std::unordered_map<std::string, uint32_t> fpmm_map_;

  std::vector<std::string> users_;
  std::unordered_map<std::string, uint32_t> user_map_;
  std::vector<std::vector<RawEvent>> user_events_;
  std::vector<UserState> user_states_;

  std::mutex user_mutex_;

  uint32_t intern_user(const std::string &addr) {
    std::string lower = addr;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::lock_guard<std::mutex> lock(user_mutex_);
    auto it = user_map_.find(lower);
    if (it != user_map_.end())
      return it->second;
    uint32_t idx = static_cast<uint32_t>(users_.size());
    users_.push_back(lower);
    user_map_[lower] = idx;
    user_events_.emplace_back();
    return idx;
  }

  void push_event(uint32_t uid, const RawEvent &evt) {
    std::lock_guard<std::mutex> lock(user_mutex_);
    user_events_[uid].push_back(evt);
  }

  static std::string blob_to_hex(const std::string &blob) {
    if (blob.starts_with("0x"))
      return blob;
    return "0x" + blob;
  }

  void load_metadata() {
    conditions_.clear();
    cond_ids_.clear();
    cond_map_.clear();
    token_map_.clear();
    fpmm_map_.clear();

    auto token_rows = db_.query_json(
        "SELECT token_id, condition_id, is_yes FROM token_map");
    for (const auto &row : token_rows) {
      std::string token_id = row["token_id"].get<std::string>();
      std::string cond_id = row["condition_id"].get<std::string>();
      int is_yes = row["is_yes"].get<int>();

      std::transform(token_id.begin(), token_id.end(), token_id.begin(),
                     ::tolower);
      std::transform(cond_id.begin(), cond_id.end(), cond_id.begin(),
                     ::tolower);

      uint32_t cond_idx;
      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end()) {
        cond_idx = static_cast<uint32_t>(conditions_.size());
        cond_map_[cond_id] = cond_idx;
        cond_ids_.push_back(cond_id);
        conditions_.emplace_back();
      } else {
        cond_idx = it->second;
      }

      token_map_[token_id] = {cond_idx, static_cast<uint8_t>(is_yes)};
    }

    auto cond_rows = db_.query_json(
        "SELECT condition_id, payout_numerators FROM condition "
        "WHERE payout_numerators IS NOT NULL");
    for (const auto &row : cond_rows) {
      std::string cond_id = row["condition_id"].get<std::string>();
      std::transform(cond_id.begin(), cond_id.end(), cond_id.begin(),
                     ::tolower);

      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end())
        continue;

      auto &cond = conditions_[it->second];
      std::string payout_str = row["payout_numerators"].get<std::string>();
      auto payout_arr = json::parse(payout_str);
      for (const auto &v : payout_arr) {
        cond.payout_numerators.push_back(v.get<int64_t>());
      }
    }

    auto fpmm_rows =
        db_.query_json("SELECT fpmm_addr, condition_id FROM fpmm");
    for (const auto &row : fpmm_rows) {
      std::string fpmm_addr = row["fpmm_addr"].get<std::string>();
      std::string cond_id = row["condition_id"].get<std::string>();
      std::transform(fpmm_addr.begin(), fpmm_addr.end(), fpmm_addr.begin(),
                     ::tolower);
      std::transform(cond_id.begin(), cond_id.end(), cond_id.begin(),
                     ::tolower);

      auto it = cond_map_.find(cond_id);
      if (it != cond_map_.end()) {
        fpmm_map_[fpmm_addr] = it->second;
      }
    }

    progress_.total_conditions = static_cast<int64_t>(conditions_.size());
    progress_.total_tokens = static_cast<int64_t>(token_map_.size());
  }

  void collect_events() {
    users_.clear();
    user_map_.clear();
    user_events_.clear();

    auto f1 = std::async(std::launch::async, [this]() { scan_order_filled(); });
    auto f2 = std::async(std::launch::async, [this]() { scan_split(); });
    auto f3 = std::async(std::launch::async, [this]() { scan_merge(); });
    auto f4 = std::async(std::launch::async, [this]() { scan_redemption(); });
    auto f5 = std::async(std::launch::async, [this]() { scan_fpmm_trade(); });
    auto f6 = std::async(std::launch::async, [this]() { scan_fpmm_funding(); });
    auto f7 = std::async(std::launch::async, [this]() { scan_convert(); });
    auto f8 = std::async(std::launch::async, [this]() { scan_transfer(); });

    f1.get();
    f2.get();
    f3.get();
    f4.get();
    f5.get();
    f6.get();
    f7.get();
    f8.get();

    int64_t total = 0;
    for (const auto &evts : user_events_) {
      total += static_cast<int64_t>(evts.size());
    }
    progress_.total_events = total;
    progress_.total_users = static_cast<int64_t>(users_.size());
  }

  void scan_order_filled() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, maker, taker, token_id, side, "
        "usdc_amount, token_amount FROM order_filled ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string maker = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      std::string taker = blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r)));
      std::string token_id = blob_to_hex(duckdb::StringValue::Get(result->GetValue(4, r)));
      int side = result->GetValue(5, r).GetValue<int32_t>();
      int64_t usdc = result->GetValue(6, r).GetValue<int64_t>();
      int64_t tokens = result->GetValue(7, r).GetValue<int64_t>();

      std::transform(token_id.begin(), token_id.end(), token_id.begin(), ::tolower);
      auto it = token_map_.find(token_id);
      if (it == token_map_.end())
        continue;

      uint32_t cond_idx = it->second.cond_idx;
      uint8_t token_idx = it->second.is_yes ? 0 : 1;
      int64_t sort_key = block * 1000000000LL + log_idx;
      int64_t price = tokens > 0 ? (usdc * 1000000LL / tokens) : 0;

      uint32_t maker_uid = intern_user(maker);
      uint32_t taker_uid = intern_user(taker);

      RawEvent maker_evt{sort_key, cond_idx, 0, token_idx, 0, tokens, price};
      RawEvent taker_evt{sort_key, cond_idx, 0, token_idx, 0, tokens, price};

      if (side == 1) {
        maker_evt.type = EventType::Buy;
        taker_evt.type = EventType::Sell;
      } else {
        maker_evt.type = EventType::Sell;
        taker_evt.type = EventType::Buy;
      }

      push_event(maker_uid, maker_evt);
      push_event(taker_uid, taker_evt);
      ++rows;
      events += 2;
    }
    progress_.order_filled_rows = rows;
    progress_.order_filled_events = events;
  }

  void scan_split() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, stakeholder, condition_id, amount "
        "FROM split ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string user = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      std::string cond_id = blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r)));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      std::transform(cond_id.begin(), cond_id.end(), cond_id.begin(), ::tolower);
      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end())
        continue;

      uint32_t cond_idx = it->second;
      int64_t sort_key = block * 1000000000LL + log_idx;
      uint32_t uid = intern_user(user);

      RawEvent evt{sort_key, cond_idx, EventType::Split, 0xFF, 0, amount, 0};
      push_event(uid, evt);
      ++rows;
      ++events;
    }
    progress_.split_rows = rows;
    progress_.split_events = events;
  }

  void scan_merge() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, stakeholder, condition_id, amount "
        "FROM merge ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string user = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      std::string cond_id = blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r)));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      std::transform(cond_id.begin(), cond_id.end(), cond_id.begin(), ::tolower);
      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end())
        continue;

      uint32_t cond_idx = it->second;
      int64_t sort_key = block * 1000000000LL + log_idx;
      uint32_t uid = intern_user(user);

      RawEvent evt{sort_key, cond_idx, EventType::Merge, 0xFF, 0, amount, 0};
      push_event(uid, evt);
      ++rows;
      ++events;
    }
    progress_.merge_rows = rows;
    progress_.merge_events = events;
  }

  void scan_redemption() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, redeemer, condition_id, index_sets, payout "
        "FROM redemption ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string user = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      std::string cond_id = blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r)));
      int index_sets = result->GetValue(4, r).GetValue<int32_t>();
      int64_t payout = result->GetValue(5, r).GetValue<int64_t>();

      std::transform(cond_id.begin(), cond_id.end(), cond_id.begin(), ::tolower);
      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end())
        continue;

      uint32_t cond_idx = it->second;
      int64_t sort_key = block * 1000000000LL + log_idx;
      uint32_t uid = intern_user(user);

      RawEvent evt{sort_key, cond_idx, EventType::Redemption,
                   static_cast<uint8_t>(index_sets), 0, payout, 0};
      push_event(uid, evt);
      ++rows;
      ++events;
    }
    progress_.redemption_rows = rows;
    progress_.redemption_events = events;
  }

  void scan_fpmm_trade() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, fpmm_addr, trader, side, outcome_index, "
        "usdc_amount, token_amount FROM fpmm_trade ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string fpmm_addr = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      std::string trader = blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r)));
      int side = result->GetValue(4, r).GetValue<int32_t>();
      int outcome_idx = result->GetValue(5, r).GetValue<int32_t>();
      int64_t usdc = result->GetValue(6, r).GetValue<int64_t>();
      int64_t tokens = result->GetValue(7, r).GetValue<int64_t>();

      std::transform(fpmm_addr.begin(), fpmm_addr.end(), fpmm_addr.begin(), ::tolower);
      auto it = fpmm_map_.find(fpmm_addr);
      if (it == fpmm_map_.end())
        continue;

      uint32_t cond_idx = it->second;
      uint8_t token_idx = (outcome_idx == 0) ? 0 : 1;
      int64_t sort_key = block * 1000000000LL + log_idx;
      int64_t price = tokens > 0 ? (usdc * 1000000LL / tokens) : 0;
      uint32_t uid = intern_user(trader);

      RawEvent evt{sort_key, cond_idx,
                   static_cast<uint8_t>(side == 1 ? EventType::FPMMBuy : EventType::FPMMSell),
                   token_idx, 0, tokens, price};
      push_event(uid, evt);
      ++rows;
      ++events;
    }
    progress_.fpmm_trade_rows = rows;
    progress_.fpmm_trade_events = events;
  }

  void scan_fpmm_funding() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, fpmm_addr, funder, side, amount0, amount1 "
        "FROM fpmm_funding ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string fpmm_addr = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      std::string funder = blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r)));
      int side = result->GetValue(4, r).GetValue<int32_t>();
      int64_t amount0 = result->GetValue(5, r).GetValue<int64_t>();
      int64_t amount1 = result->GetValue(6, r).GetValue<int64_t>();

      std::transform(fpmm_addr.begin(), fpmm_addr.end(), fpmm_addr.begin(), ::tolower);
      auto it = fpmm_map_.find(fpmm_addr);
      if (it == fpmm_map_.end())
        continue;

      uint32_t cond_idx = it->second;
      int64_t sort_key = block * 1000000000LL + log_idx;
      uint32_t uid = intern_user(funder);

      RawEvent evt{sort_key, cond_idx,
                   static_cast<uint8_t>(side == 1 ? EventType::FPMMLPAdd : EventType::FPMMLPRemove),
                   0xFF, 0, amount0, amount1};
      push_event(uid, evt);
      ++rows;
      ++events;
    }
    progress_.fpmm_funding_rows = rows;
    progress_.fpmm_funding_events = events;
  }

  void scan_convert() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, stakeholder, market_id, index_set, amount "
        "FROM convert ORDER BY block_number, log_index");
    assert(!result->HasError());

    std::unordered_map<std::string, uint32_t> market_to_cond;
    auto q_rows = db_.query_json(
        "SELECT nrq.market_id, c.condition_id FROM neg_risk_question nrq "
        "JOIN condition c ON nrq.question_id = c.question_id LIMIT 1");

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string user = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      int64_t index_set = result->GetValue(4, r).GetValue<int64_t>();
      int64_t amount = result->GetValue(5, r).GetValue<int64_t>();

      int64_t sort_key = block * 1000000000LL + log_idx;
      uint32_t uid = intern_user(user);

      RawEvent evt{sort_key, 0, EventType::Convert, 0xFF, 0, amount, index_set};
      push_event(uid, evt);
      ++rows;
      ++events;
    }
    progress_.convert_rows = rows;
    progress_.convert_events = events;
  }

  void scan_transfer() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, log_index, from_addr, to_addr, token_id, amount "
        "FROM transfer ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      int64_t log_idx = result->GetValue(1, r).GetValue<int64_t>();
      std::string from = blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r)));
      std::string to = blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r)));
      std::string token_id = blob_to_hex(duckdb::StringValue::Get(result->GetValue(4, r)));
      int64_t amount = result->GetValue(5, r).GetValue<int64_t>();

      std::transform(token_id.begin(), token_id.end(), token_id.begin(), ::tolower);
      auto it = token_map_.find(token_id);
      if (it == token_map_.end())
        continue;

      uint32_t cond_idx = it->second.cond_idx;
      uint8_t token_idx = it->second.is_yes ? 0 : 1;
      int64_t sort_key = block * 1000000000LL + log_idx;

      uint32_t from_uid = intern_user(from);
      uint32_t to_uid = intern_user(to);

      RawEvent out_evt{sort_key, cond_idx, EventType::TransferOut, token_idx, 0, amount, 0};
      RawEvent in_evt{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0};

      push_event(from_uid, out_evt);
      push_event(to_uid, in_evt);
      ++rows;
      events += 2;
    }
    progress_.transfer_rows = rows;
    progress_.transfer_events = events;
  }

  void replay_all() {
    size_t nu = users_.size();
    user_states_.resize(nu);
    progress_.processed_users = 0;

    int nw = std::min(REBUILD_P3_WORKERS,
                      std::max(1, static_cast<int>(std::thread::hardware_concurrency())));
    std::vector<std::thread> workers;
    std::atomic<size_t> next_user{0};

    for (int w = 0; w < nw; ++w) {
      workers.emplace_back([this, &next_user, nu]() {
        while (true) {
          size_t uid = next_user.fetch_add(1);
          if (uid >= nu)
            break;
          replay_user(uid);
          ++progress_.processed_users;
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
              [](const RawEvent &a, const RawEvent &b) {
                return a.sort_key < b.sort_key;
              });

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
      apply_split(evt, st, cond);
      break;
    case EventType::Merge:
      apply_merge(evt, st, cond);
      break;
    case EventType::Redemption:
      apply_redemption(evt, st, cond);
      break;
    case EventType::FPMMLPAdd:
      apply_lp_add(evt, st, cond);
      break;
    case EventType::FPMMLPRemove:
      apply_lp_remove(evt, st, cond);
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

  static void apply_split(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    int64_t implied_price = 1000000 / cond.outcome_count;
    for (int i = 0; i < cond.outcome_count; ++i) {
      st.cost[i] += evt.amount * implied_price;
      st.positions[i] += evt.amount;
    }
  }

  static void apply_merge(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    int64_t implied_price = 1000000 / cond.outcome_count;
    for (int i = 0; i < cond.outcome_count; ++i) {
      int64_t pos = st.positions[i];
      if (pos <= 0)
        continue;
      int64_t sell = std::min(evt.amount, pos);
      int64_t cost_removed = st.cost[i] * sell / pos;
      st.realized_pnl += (sell * implied_price - cost_removed) / 1000000;
      st.cost[i] -= cost_removed;
      st.positions[i] -= sell;
    }
  }

  static void apply_redemption(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    if (cond.payout_numerators.empty())
      return;

    int index_sets = evt.token_idx;
    for (int i = 0; i < cond.outcome_count && i < static_cast<int>(cond.payout_numerators.size()); ++i) {
      if (!((index_sets >> i) & 1))
        continue;
      int64_t pos = st.positions[i];
      if (pos <= 0)
        continue;
      int64_t payout_price = cond.payout_numerators[i] * 1000000;
      int64_t cost_removed = st.cost[i];
      st.realized_pnl += (pos * payout_price - cost_removed) / 1000000;
      st.cost[i] = 0;
      st.positions[i] = 0;
    }
  }

  static void apply_lp_add(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    int64_t total = evt.amount + evt.price;
    int64_t implied_price0 = total > 0 ? (evt.amount * 1000000 / total) : 500000;
    int64_t implied_price1 = total > 0 ? (evt.price * 1000000 / total) : 500000;
    st.cost[0] += evt.amount * implied_price0;
    st.positions[0] += evt.amount;
    st.cost[1] += evt.price * implied_price1;
    st.positions[1] += evt.price;
  }

  static void apply_lp_remove(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    for (int i = 0; i < 2; ++i) {
      int64_t pos = st.positions[i];
      int64_t remove = (i == 0) ? evt.amount : evt.price;
      if (pos <= 0 || remove <= 0)
        continue;
      int64_t actual = std::min(remove, pos);
      int64_t cost_removed = st.cost[i] * actual / pos;
      int64_t total = evt.amount + evt.price;
      int64_t implied_price = total > 0 ? (remove * 1000000 / total) : 500000;
      st.realized_pnl += (actual * implied_price - cost_removed) / 1000000;
      st.cost[i] -= cost_removed;
      st.positions[i] -= actual;
    }
  }

  static void apply_convert(const RawEvent &evt, ReplayState &st) {
    int64_t index_set = evt.price;
    int popcount = __builtin_popcountll(static_cast<uint64_t>(index_set));
    if (popcount > 1) {
      st.realized_pnl += (popcount - 1) * evt.amount;
    }
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

} // namespace rebuild
