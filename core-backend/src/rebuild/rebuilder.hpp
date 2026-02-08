#pragma once

// ============================================================================
// PnL Rebuild Engine — 三阶段全量重建
//
// Phase 1: load_metadata()    — 扫描 condition 表, 构建 token→condition 映射
// Phase 2: collect_events()   — 4次全表扫描, 事件写入 per-user 桶
// Phase 3: replay_all()       — 并行回放, 生成 Snapshot 链, 释放 RawEvent
// ============================================================================

#include "rebuilder_types.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cstring>
#include <duckdb.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

// ============================================================================
// Tuning parameters
// ============================================================================
#define REBUILD_P3_WORKERS 16         // Phase 3 parallel replay worker count
#define REBUILD_USER_RESERVE 1200000  // Pre-allocate user map/vector capacity
#define REBUILD_COND_RESERVE 500000   // Pre-allocate condition map capacity
#define REBUILD_TOKEN_RESERVE 1000000 // Pre-allocate token map capacity

namespace rebuild {

// Transparent string hash — enables string_view lookup on string-keyed maps
struct StringHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const noexcept {
    return std::hash<std::string_view>{}(sv);
  }
  size_t operator()(const std::string &s) const noexcept {
    return std::hash<std::string_view>{}(std::string_view(s));
  }
};
struct StringEqual {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept {
    return a == b;
  }
};

template <typename V>
using StrMap = std::unordered_map<std::string, V, StringHash, StringEqual>;

// Parse int64 from DuckDB string_t without heap allocation
static int64_t parse_i64(const duckdb::string_t &s) {
  int64_t val = 0;
  auto [ptr, ec] = std::from_chars(s.GetData(), s.GetData() + s.GetSize(), val);
  assert(ec == std::errc{});
  return val;
}

// Per-scan thread-local collection (merged after all scans complete)
struct ScanResult {
  StrMap<std::vector<RawEvent>> user_events;
  int64_t rows = 0;
  int64_t events = 0;
};

class Engine {
public:
  explicit Engine(duckdb::DuckDB &db) : db_(db) {}

  // ==========================================================================
  // 入口: 全量重建
  // ==========================================================================
  void rebuild_all() {
    bool expected = false;
    assert(running_.compare_exchange_strong(expected, true) && "rebuild already running");

    eof_rows_ = 0; eof_events_ = 0; split_rows_ = 0; split_events_ = 0;
    merge_rows_ = 0; merge_events_ = 0; redemption_rows_ = 0; redemption_events_ = 0;
    eof_done_ = false; split_done_ = false; merge_done_ = false; redemption_done_ = false;
    phase1_ms_ = phase2_ms_ = phase3_ms_ = 0;

    phase_ = 1;
    auto t0 = clock::now();
    load_metadata();
    phase1_ms_ = ms_since(t0);

    auto t1 = clock::now();
    collect_events();
    phase2_ms_ = ms_since(t1);

    phase_ = 6;
    auto t2 = clock::now();
    replay_all();
    phase3_ms_ = ms_since(t2);

    phase_ = 7;
    running_ = false;

    std::cout << "[rebuild] done: "
              << users_.size() << " users, "
              << total_events_ << " events | "
              << "p1=" << (int)phase1_ms_ << "ms "
              << "p2=" << (int)phase2_ms_ << "ms "
              << "p3=" << (int)phase3_ms_ << "ms "
              << "total=" << (int)(phase1_ms_ + phase2_ms_ + phase3_ms_) << "ms" << std::endl;
  }

  // ==========================================================================
  // Persistence — binary dump/load of full engine state
  // ==========================================================================
  static constexpr uint32_t PERSIST_MAGIC = 0x524C4E50; // "PNLR"
  static constexpr uint32_t PERSIST_VERSION = 1;

  static bool has_persist(const std::string &dir) {
    return std::filesystem::exists(dir + "/rebuild.bin");
  }

  void save_persist(const std::string &dir) const {
    std::filesystem::create_directories(dir);
    std::string path = dir + "/rebuild.bin";
    std::ofstream f(path, std::ios::binary);
    assert(f.is_open());

    auto w = [&](const void *data, size_t n) { f.write((const char *)data, n); };
    auto w32 = [&](uint32_t v) { w(&v, 4); };
    auto w64 = [&](int64_t v) { w(&v, 8); };
    auto wstr = [&](const std::string &s) { w32((uint32_t)s.size()); w(s.data(), s.size()); };

    // Header
    w32(PERSIST_MAGIC);
    w32(PERSIST_VERSION);
    w32((uint32_t)conditions_.size());
    w32((uint32_t)token_map_.size());
    w32((uint32_t)users_.size());
    w64(total_events_);

    // Conditions
    for (size_t i = 0; i < conditions_.size(); ++i) {
      wstr(cond_ids_[i]);
      const auto &c = conditions_[i];
      uint8_t oc = c.outcome_count;
      w(&oc, 1);
      w64(c.payout_denominator);
      w32((uint32_t)c.payout_numerators.size());
      for (auto pn : c.payout_numerators)
        w64(pn);
    }

    // Token map
    for (const auto &[token_id, pair] : token_map_) {
      wstr(token_id);
      w32(pair.first);
      uint8_t ti = pair.second;
      w(&ti, 1);
    }

    // Users + states
    for (size_t i = 0; i < users_.size(); ++i) {
      wstr(users_[i]);
      const auto &us = user_states_[i];
      w32((uint32_t)us.conditions.size());
      for (const auto &ch : us.conditions) {
        w32(ch.cond_idx);
        w32((uint32_t)ch.snapshots.size());
        if (!ch.snapshots.empty())
          w(ch.snapshots.data(), ch.snapshots.size() * sizeof(Snapshot));
      }
    }

    f.close();
    auto fsize = std::filesystem::file_size(path);
    std::cout << "[rebuild] persisted to " << path
              << " (" << (fsize / 1048576) << " MB)" << std::endl;
  }

  void load_persist(const std::string &dir) {
    bool expected = false;
    assert(running_.compare_exchange_strong(expected, true) && "rebuild already running");

    std::string path = dir + "/rebuild.bin";
    std::ifstream f(path, std::ios::binary);
    assert(f.is_open());

    auto fsize = std::filesystem::file_size(path);

    auto r = [&](void *data, size_t n) { f.read((char *)data, n); assert(f.good()); };
    auto r32 = [&]() -> uint32_t { uint32_t v; r(&v, 4); return v; };
    auto r64 = [&]() -> int64_t { int64_t v; r(&v, 8); return v; };
    auto rstr = [&]() -> std::string { uint32_t n = r32(); std::string s(n, '\0'); r(s.data(), n); return s; };

    phase_ = 1;
    phase1_ms_ = phase2_ms_ = phase3_ms_ = 0;

    // Header
    uint32_t magic = r32();
    assert(magic == PERSIST_MAGIC && "bad persist magic");
    uint32_t version = r32();
    assert(version == PERSIST_VERSION && "bad persist version");
    uint32_t n_conds = r32();
    uint32_t n_tokens = r32();
    uint32_t n_users = r32();
    total_events_ = r64();

    // Conditions
    conditions_.clear();
    cond_ids_.clear();
    cond_map_.clear();
    conditions_.reserve(n_conds);
    cond_ids_.reserve(n_conds);
    cond_map_.reserve(n_conds);

    for (uint32_t i = 0; i < n_conds; ++i) {
      std::string id = rstr();
      ConditionInfo info;
      uint8_t oc;
      r(&oc, 1);
      info.outcome_count = oc;
      info.payout_denominator = r64();
      uint32_t n_pn = r32();
      info.payout_numerators.resize(n_pn);
      for (uint32_t j = 0; j < n_pn; ++j)
        info.payout_numerators[j] = r64();

      cond_map_[id] = i;
      conditions_.push_back(std::move(info));
      cond_ids_.push_back(std::move(id));
    }

    // Token map
    token_map_.clear();
    token_map_.reserve(n_tokens);
    for (uint32_t i = 0; i < n_tokens; ++i) {
      std::string token_id = rstr();
      uint32_t ci = r32();
      uint8_t ti;
      r(&ti, 1);
      token_map_[std::move(token_id)] = {ci, ti};
    }

    phase_ = 6;

    // Users + states
    users_.clear();
    user_map_.clear();
    user_states_.clear();
    users_.reserve(n_users);
    user_map_.reserve(n_users);
    user_states_.resize(n_users);
    processed_users_ = 0;

    for (uint32_t i = 0; i < n_users; ++i) {
      std::string uid = rstr();
      user_map_[uid] = i;
      users_.push_back(std::move(uid));

      uint32_t n_ch = r32();
      auto &us = user_states_[i];
      us.conditions.resize(n_ch);
      for (uint32_t j = 0; j < n_ch; ++j) {
        us.conditions[j].cond_idx = r32();
        uint32_t n_snaps = r32();
        us.conditions[j].snapshots.resize(n_snaps);
        if (n_snaps > 0)
          r(us.conditions[j].snapshots.data(), n_snaps * sizeof(Snapshot));
      }
      processed_users_.fetch_add(1, std::memory_order_relaxed);
    }

    f.close();
    phase_ = 7;
    running_ = false;

    std::cout << "[rebuild] loaded from " << path
              << " (" << (fsize / 1048576) << " MB): "
              << users_.size() << " users, "
              << total_events_ << " events" << std::endl;
  }

  // ==========================================================================
  // Accessors
  // ==========================================================================
  const std::vector<std::string> &users() const { return users_; }
  const std::vector<UserState> &user_states() const { return user_states_; }
  const std::vector<ConditionInfo> &conditions() const { return conditions_; }
  const std::vector<std::string> &condition_ids() const { return cond_ids_; }

  const UserState *find_user(const std::string &user_id) const {
    auto it = user_map_.find(user_id);
    if (it == user_map_.end())
      return nullptr;
    return &user_states_[it->second];
  }

  RebuildProgress get_progress() const {
    RebuildProgress p;
    p.phase = phase_.load(std::memory_order_relaxed);
    p.total_conditions = (int64_t)conditions_.size();
    p.total_tokens = (int64_t)token_map_.size();
    p.total_events = total_events_;
    p.total_users = (int64_t)users_.size();
    p.processed_users = processed_users_.load(std::memory_order_relaxed);
    p.running = running_.load(std::memory_order_relaxed);
    p.phase1_ms = phase1_ms_;
    p.phase2_ms = phase2_ms_;
    p.phase3_ms = phase3_ms_;
    p.eof_rows = eof_rows_.load(std::memory_order_relaxed);
    p.eof_events = eof_events_.load(std::memory_order_relaxed);
    p.split_rows = split_rows_.load(std::memory_order_relaxed);
    p.split_events = split_events_.load(std::memory_order_relaxed);
    p.merge_rows = merge_rows_.load(std::memory_order_relaxed);
    p.merge_events = merge_events_.load(std::memory_order_relaxed);
    p.redemption_rows = redemption_rows_.load(std::memory_order_relaxed);
    p.redemption_events = redemption_events_.load(std::memory_order_relaxed);
    p.eof_done = eof_done_.load(std::memory_order_relaxed);
    p.split_done = split_done_.load(std::memory_order_relaxed);
    p.merge_done = merge_done_.load(std::memory_order_relaxed);
    p.redemption_done = redemption_done_.load(std::memory_order_relaxed);
    return p;
  }

private:
  using clock = std::chrono::high_resolution_clock;
  static double ms_since(clock::time_point t) {
    return std::chrono::duration<double, std::milli>(clock::now() - t).count();
  }

  // ==========================================================================
  // Phase 1: Metadata — condition 表 → cond_map_ + token_map_
  // ==========================================================================
  void load_metadata() {
    conditions_.clear();
    cond_ids_.clear();
    cond_map_.clear();
    token_map_.clear();
    cond_map_.reserve(REBUILD_COND_RESERVE);
    token_map_.reserve(REBUILD_TOKEN_RESERVE);

    auto conn = std::make_unique<duckdb::Connection>(db_);
    auto result = conn->Query(
        "SELECT id, outcomeSlotCount, positionIds, payoutNumerators, payoutDenominator "
        "FROM condition");
    assert(!result->HasError());

    duckdb::unique_ptr<duckdb::DataChunk> chunk;
    while ((chunk = result->Fetch()) != nullptr && chunk->size() > 0) {
      auto count = chunk->size();
      auto id_col = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[0]);
      auto oc_col = duckdb::FlatVector::GetData<int32_t>(chunk->data[1]);
      auto &pos_valid = duckdb::FlatVector::Validity(chunk->data[2]);
      auto pos_col = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[2]);
      auto &pn_valid = duckdb::FlatVector::Validity(chunk->data[3]);
      auto pn_col = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[3]);
      auto &pd_valid = duckdb::FlatVector::Validity(chunk->data[4]);
      auto pd_col = duckdb::FlatVector::GetData<int64_t>(chunk->data[4]);

      for (duckdb::idx_t i = 0; i < count; ++i) {
        std::string cond_id(id_col[i].GetData(), id_col[i].GetSize());
        int32_t outcome_count = oc_col[i];
        assert(outcome_count > 0 && outcome_count <= MAX_OUTCOMES);

        uint32_t idx = (uint32_t)conditions_.size();
        ConditionInfo info;
        info.outcome_count = (uint8_t)outcome_count;

        // positionIds: JSON array → token_map_ entries
        if (pos_valid.RowIsValid(i)) {
          std::string s(pos_col[i].GetData(), pos_col[i].GetSize());
          if (!s.empty()) {
            auto arr = json::parse(s);
            for (uint8_t j = 0; j < (uint8_t)arr.size(); ++j)
              token_map_[arr[j].get<std::string>()] = {idx, j};
          }
        }

        // payoutNumerators: JSON array of ints/strings
        if (pn_valid.RowIsValid(i)) {
          std::string s(pn_col[i].GetData(), pn_col[i].GetSize());
          if (!s.empty() && s != "NULL") {
            auto arr = json::parse(s);
            for (const auto &n : arr)
              info.payout_numerators.push_back(
                  n.is_string() ? std::stoll(n.get<std::string>()) : n.get<int64_t>());
          }
        }

        // payoutDenominator
        if (pd_valid.RowIsValid(i))
          info.payout_denominator = pd_col[i];

        conditions_.push_back(std::move(info));
        cond_ids_.push_back(std::move(cond_id));
        cond_map_[cond_ids_.back()] = idx;
      }
    }

    std::cout << "[rebuild] p1: " << conditions_.size() << " conditions, "
              << token_map_.size() << " tokens" << std::endl;
  }

  // ==========================================================================
  // Phase 2: Event collection — 4 table scans → per-user RawEvent vectors
  // ==========================================================================
  uint32_t intern_user(const std::string &id) {
    auto [it, ok] = user_map_.emplace(id, (uint32_t)users_.size());
    if (ok) {
      users_.push_back(id);
      user_events_.emplace_back();
    }
    return it->second;
  }

  static void push_user_event(StrMap<std::vector<RawEvent>> &m,
                              const duckdb::string_t &u, const RawEvent &evt) {
    std::string_view sv(u.GetData(), u.GetSize());
    auto it = m.find(sv);
    if (it == m.end())
      it = m.emplace(std::string(sv), std::vector<RawEvent>{}).first;
    it->second.push_back(evt);
  }

  void collect_events() {
    users_.clear();
    user_map_.clear();
    user_events_.clear();
    total_events_ = 0;
    user_map_.reserve(REBUILD_USER_RESERVE);
    users_.reserve(REBUILD_USER_RESERVE);
    user_events_.reserve(REBUILD_USER_RESERVE);

    phase_ = 2;

    // 4 parallel scans with independent connections
    auto conn1 = std::make_unique<duckdb::Connection>(db_);
    auto conn2 = std::make_unique<duckdb::Connection>(db_);
    auto conn3 = std::make_unique<duckdb::Connection>(db_);
    auto conn4 = std::make_unique<duckdb::Connection>(db_);

    auto f_eof = std::async(std::launch::async, [&]() { return scan_eof_chunked(*conn1); });
    auto f_split = std::async(std::launch::async, [&]() { return scan_split_chunked(*conn2); });
    auto f_merge = std::async(std::launch::async, [&]() { return scan_merge_chunked(*conn3); });
    auto f_redemption = std::async(std::launch::async, [&]() { return scan_redemption_chunked(*conn4); });

    auto sr_eof = f_eof.get();
    auto sr_split = f_split.get();
    auto sr_merge = f_merge.get();
    auto sr_redemption = f_redemption.get();

    // Merge thread-local results into per-user event vectors
    auto merge_fn = [&](ScanResult &sr) {
      for (auto &[uid, evts] : sr.user_events) {
        auto ui = intern_user(uid);
        auto &dest = user_events_[ui];
        if (dest.empty())
          dest = std::move(evts);
        else
          dest.insert(dest.end(), evts.begin(), evts.end());
      }
      sr.user_events.clear();
    };
    merge_fn(sr_eof);
    merge_fn(sr_split);
    merge_fn(sr_merge);
    merge_fn(sr_redemption);

    total_events_ = eof_events_ + split_events_ + merge_events_ + redemption_events_;

    std::cout << "[rebuild]   eof: " << eof_rows_ << " rows → " << eof_events_ << " events" << std::endl;
    std::cout << "[rebuild]   split: " << split_rows_ << " rows → " << split_events_ << " events" << std::endl;
    std::cout << "[rebuild]   merge: " << merge_rows_ << " rows → " << merge_events_ << " events" << std::endl;
    std::cout << "[rebuild]   redemption: " << redemption_rows_ << " rows → " << redemption_events_ << " events" << std::endl;
    std::cout << "[rebuild] p2: " << total_events_ << " events → "
              << users_.size() << " users" << std::endl;
  }

  // --- enriched_order_filled: chunk API, returns thread-local ScanResult
  ScanResult scan_eof_chunked(duckdb::Connection &conn) {
    ScanResult sr;
    auto r = conn.Query(
        "SELECT timestamp, maker, taker, market, side, size, price "
        "FROM enriched_order_filled ORDER BY timestamp");
    assert(!r->HasError());

    duckdb::unique_ptr<duckdb::DataChunk> chunk;
    while ((chunk = r->Fetch()) != nullptr && chunk->size() > 0) {
      auto count = chunk->size();
      auto ts = duckdb::FlatVector::GetData<int64_t>(chunk->data[0]);
      auto maker = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[1]);
      auto taker = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[2]);
      auto market = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[3]);
      auto side = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[4]);
      auto size = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[5]);
      auto price = duckdb::FlatVector::GetData<double>(chunk->data[6]);

      sr.rows += count;
      for (duckdb::idx_t i = 0; i < count; ++i) {
        std::string_view market_sv(market[i].GetData(), market[i].GetSize());
        auto tok = token_map_.find(market_sv);
        if (tok == token_map_.end())
          continue;

        uint32_t ci = tok->second.first;
        uint8_t ti = tok->second.second;
        bool is_buy = (side[i].GetData()[0] == 'B');
        int64_t sz = parse_i64(size[i]);
        int64_t pr = (int64_t)(price[i] * 1000000);

        // side = taker's direction: BUY → taker buys, maker sells; SELL → taker sells, maker buys
        push_user_event(sr.user_events, taker[i],
                        RawEvent{ts[i], ci, (uint8_t)(is_buy ? Buy : Sell), ti, 0, sz, pr});
        push_user_event(sr.user_events, maker[i],
                        RawEvent{ts[i], ci, (uint8_t)(is_buy ? Sell : Buy), ti, 0, sz, pr});
        sr.events += 2;
      }
      eof_rows_.store(sr.rows, std::memory_order_relaxed);
    }
    eof_events_.store(sr.events, std::memory_order_relaxed);
    eof_done_.store(true, std::memory_order_relaxed);
    return sr;
  }

  // --- split: chunk API, returns thread-local ScanResult
  ScanResult scan_split_chunked(duckdb::Connection &conn) {
    ScanResult sr;
    auto r = conn.Query(
        "SELECT timestamp, stakeholder, condition, amount "
        "FROM split ORDER BY timestamp");
    assert(!r->HasError());

    duckdb::unique_ptr<duckdb::DataChunk> chunk;
    while ((chunk = r->Fetch()) != nullptr && chunk->size() > 0) {
      auto count = chunk->size();
      auto ts = duckdb::FlatVector::GetData<int64_t>(chunk->data[0]);
      auto user = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[1]);
      auto cond = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[2]);
      auto amt = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[3]);

      sr.rows += count;
      for (duckdb::idx_t i = 0; i < count; ++i) {
        std::string_view cond_sv(cond[i].GetData(), cond[i].GetSize());
        auto ci = cond_map_.find(cond_sv);
        if (ci == cond_map_.end())
          continue;

        push_user_event(sr.user_events, user[i],
                        RawEvent{ts[i], ci->second, (uint8_t)Split, 0xFF, 0, parse_i64(amt[i]), 0});
        ++sr.events;
      }
      split_rows_.store(sr.rows, std::memory_order_relaxed);
    }
    split_events_.store(sr.events, std::memory_order_relaxed);
    split_done_.store(true, std::memory_order_relaxed);
    return sr;
  }

  // --- merge: chunk API, returns thread-local ScanResult
  ScanResult scan_merge_chunked(duckdb::Connection &conn) {
    ScanResult sr;
    auto r = conn.Query(
        "SELECT timestamp, stakeholder, condition, amount "
        "FROM merge ORDER BY timestamp");
    assert(!r->HasError());

    duckdb::unique_ptr<duckdb::DataChunk> chunk;
    while ((chunk = r->Fetch()) != nullptr && chunk->size() > 0) {
      auto count = chunk->size();
      auto ts = duckdb::FlatVector::GetData<int64_t>(chunk->data[0]);
      auto user = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[1]);
      auto cond = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[2]);
      auto amt = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[3]);

      sr.rows += count;
      for (duckdb::idx_t i = 0; i < count; ++i) {
        std::string_view cond_sv(cond[i].GetData(), cond[i].GetSize());
        auto ci = cond_map_.find(cond_sv);
        if (ci == cond_map_.end())
          continue;

        push_user_event(sr.user_events, user[i],
                        RawEvent{ts[i], ci->second, (uint8_t)Merge, 0xFF, 0, parse_i64(amt[i]), 0});
        ++sr.events;
      }
      merge_rows_.store(sr.rows, std::memory_order_relaxed);
    }
    merge_events_.store(sr.events, std::memory_order_relaxed);
    merge_done_.store(true, std::memory_order_relaxed);
    return sr;
  }

  // --- redemption: chunk API, returns thread-local ScanResult
  ScanResult scan_redemption_chunked(duckdb::Connection &conn) {
    ScanResult sr;
    auto r = conn.Query(
        "SELECT timestamp, redeemer, condition, payout "
        "FROM redemption ORDER BY timestamp");
    assert(!r->HasError());

    duckdb::unique_ptr<duckdb::DataChunk> chunk;
    while ((chunk = r->Fetch()) != nullptr && chunk->size() > 0) {
      auto count = chunk->size();
      auto ts = duckdb::FlatVector::GetData<int64_t>(chunk->data[0]);
      auto user = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[1]);
      auto cond = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[2]);
      auto pay = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[3]);

      sr.rows += count;
      for (duckdb::idx_t i = 0; i < count; ++i) {
        std::string_view cond_sv(cond[i].GetData(), cond[i].GetSize());
        auto ci = cond_map_.find(cond_sv);
        if (ci == cond_map_.end())
          continue;

        push_user_event(sr.user_events, user[i],
                        RawEvent{ts[i], ci->second, (uint8_t)Redemption, 0xFF, 0, parse_i64(pay[i]), 0});
        ++sr.events;
      }
      redemption_rows_.store(sr.rows, std::memory_order_relaxed);
    }
    redemption_events_.store(sr.events, std::memory_order_relaxed);
    redemption_done_.store(true, std::memory_order_relaxed);
    return sr;
  }

  // ==========================================================================
  // Phase 3: Parallel replay — sort per-user, build Snapshots, free RawEvents
  // ==========================================================================
  void replay_all() {
    size_t nu = users_.size();
    user_states_.resize(nu);
    processed_users_ = 0;

    int nw = std::min((unsigned)REBUILD_P3_WORKERS, std::max(1u, std::thread::hardware_concurrency()));
    size_t chunk = (nu + nw - 1) / nw;

    std::vector<std::future<void>> futs;
    for (int w = 0; w < nw; ++w) {
      size_t s = w * chunk, e = std::min(s + chunk, nu);
      if (s >= nu)
        break;
      futs.push_back(std::async(std::launch::async, [this, s, e]() {
        for (size_t u = s; u < e; ++u) {
          replay_user(u);
          processed_users_.fetch_add(1, std::memory_order_relaxed);
        }
      }));
    }
    for (auto &f : futs)
      f.get();

    // Free all raw events
    user_events_.clear();
    user_events_.shrink_to_fit();

    std::cout << "[rebuild] p3: " << nu << " users, " << nw << " workers" << std::endl;
  }

  void replay_user(size_t uid) {
    auto &events = user_events_[uid];

    // Sort by timestamp
    std::sort(events.begin(), events.end(),
              [](const RawEvent &a, const RawEvent &b) {
                return a.timestamp < b.timestamp;
              });

    // Per-condition replay state and snapshots
    std::unordered_map<uint32_t, ReplayState> states;
    std::unordered_map<uint32_t, std::vector<Snapshot>> snaps;

    for (const auto &evt : events) {
      auto &st = states[evt.cond_idx];
      const auto &cond = conditions_[evt.cond_idx];

      apply_event(evt, st, cond);

      // Record snapshot
      Snapshot snap;
      snap.timestamp = evt.timestamp;
      snap.delta = evt.amount;
      snap.price = evt.price;
      std::memcpy(snap.positions, st.positions, sizeof(st.positions));

      // cost_basis = sum(cost[i]) / 1e6 → raw USDC
      int64_t total_cost = 0;
      for (int k = 0; k < cond.outcome_count; ++k)
        total_cost += st.cost[k];
      snap.cost_basis = total_cost / 1000000;

      snap.realized_pnl = st.realized_pnl;
      snap.event_type = evt.type;
      snap.token_idx = evt.token_idx;
      snap.outcome_count = cond.outcome_count;
      std::memset(snap._pad, 0, sizeof(snap._pad));

      snaps[evt.cond_idx].push_back(snap);
    }

    // Build UserState
    auto &us = user_states_[uid];
    us.conditions.clear();
    us.conditions.reserve(snaps.size());
    for (auto &[ci, sv] : snaps) {
      us.conditions.push_back(UserConditionHistory{ci, std::move(sv)});
    }

    // Free raw events for this user
    events.clear();
    events.shrink_to_fit();
  }

  // ==========================================================================
  // Event application logic
  // ==========================================================================
  static void apply_event(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    switch ((EventType)evt.type) {
    case Buy:
      apply_buy(evt, st);
      break;
    case Sell:
      apply_sell(evt, st);
      break;
    case Split:
      apply_split(evt, st, cond);
      break;
    case Merge:
      apply_merge(evt, st, cond);
      break;
    case Redemption:
      apply_redemption(evt, st, cond);
      break;
    }
  }

  // Buy token[i]: position += amount, cost += amount * price
  static void apply_buy(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    assert(i < MAX_OUTCOMES);
    st.cost[i] += evt.amount * evt.price;
    st.positions[i] += evt.amount;
  }

  // Sell token[i]: realize PnL, reduce position
  static void apply_sell(const RawEvent &evt, ReplayState &st) {
    int i = evt.token_idx;
    assert(i < MAX_OUTCOMES);

    int64_t pos = st.positions[i];
    if (pos <= 0) return; // nothing to sell
    int64_t cost_removed = st.cost[i] * evt.amount / pos;
    st.realized_pnl += (evt.amount * evt.price - cost_removed) / 1000000;
    st.cost[i] -= cost_removed;
    st.positions[i] -= evt.amount;
  }

  // Split: pay amount USDC → get amount of each outcome token
  // Implied price per token = 1e6 / outcome_count
  static void apply_split(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    int64_t implied_price = 1000000 / cond.outcome_count;
    for (int i = 0; i < cond.outcome_count; ++i) {
      st.cost[i] += evt.amount * implied_price;
      st.positions[i] += evt.amount;
    }
  }

  // Merge: destroy amount of each token → receive amount USDC
  // Implied sell price per token = 1e6 / outcome_count
  static void apply_merge(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    int64_t implied_price = 1000000 / cond.outcome_count;
    for (int i = 0; i < cond.outcome_count; ++i) {
      int64_t pos = st.positions[i];
      if (pos <= 0) continue;
      int64_t cost_removed = st.cost[i] * evt.amount / pos;
      st.realized_pnl += (evt.amount * implied_price - cost_removed) / 1000000;
      st.cost[i] -= cost_removed;
      st.positions[i] -= evt.amount;
    }
  }

  // Redemption: clear all positions at payout price
  static void apply_redemption(const RawEvent &evt, ReplayState &st, const ConditionInfo &cond) {
    if (cond.payout_denominator == 0)
      return; // 未结算

    for (int i = 0; i < cond.outcome_count && i < (int)cond.payout_numerators.size(); ++i) {
      int64_t pos = st.positions[i];
      if (pos <= 0)
        continue;
      int64_t payout_price = cond.payout_numerators[i] * 1000000 / cond.payout_denominator;
      int64_t cost_removed = st.cost[i];
      st.realized_pnl += (pos * payout_price - cost_removed) / 1000000;
      st.cost[i] = 0;
      st.positions[i] = 0;
    }
  }

  // ==========================================================================
  // Data members
  // ==========================================================================
  duckdb::DuckDB &db_;

  // Phase 1
  std::vector<ConditionInfo> conditions_;          // cond_idx → info
  std::vector<std::string> cond_ids_;              // cond_idx → id
  StrMap<uint32_t> cond_map_;                      // id → cond_idx
  StrMap<std::pair<uint32_t, uint8_t>> token_map_; // token_id → (cond_idx, tok_idx)

  // Phase 2 (freed after Phase 3)
  std::vector<std::string> users_;                 // user_idx → id
  StrMap<uint32_t> user_map_;                      // id → user_idx
  std::vector<std::vector<RawEvent>> user_events_; // user_idx → events

  // Phase 3
  std::vector<UserState> user_states_; // user_idx → state

  // Stats
  int64_t total_events_ = 0;
  std::atomic<int64_t> processed_users_{0};
  std::atomic<bool> running_{false};
  std::atomic<int> phase_{0};
  double phase1_ms_ = 0, phase2_ms_ = 0, phase3_ms_ = 0;
  std::atomic<int64_t> eof_rows_{0}, eof_events_{0};
  std::atomic<int64_t> split_rows_{0}, split_events_{0};
  std::atomic<int64_t> merge_rows_{0}, merge_events_{0};
  std::atomic<int64_t> redemption_rows_{0}, redemption_events_{0};
  std::atomic<bool> eof_done_{false}, split_done_{false}, merge_done_{false}, redemption_done_{false};
};

} // namespace rebuild
