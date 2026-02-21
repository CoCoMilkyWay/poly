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

// 合约地址常量
static constexpr const char *ZERO_ADDR = "0x0000000000000000000000000000000000000000";
static constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
static constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
static constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";
static constexpr const char *USDC_E = "0x2791bca1f2de4661ed88a30c99a7a9449aa84174";

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
  int64_t transfer_rows = 0;
  int64_t transfer_events = 0;
};

class Engine {
public:
  explicit Engine(Database &stage1_db, Database &stage2_db)
      : db_(stage1_db), stage2_db_(stage2_db) {}

  explicit Engine(Database &db) : db_(db), stage2_db_(db) {}

  void rebuild_all() {
    assert(!progress_.running);
    progress_ = RebuildProgress{};
    progress_.running = true;
    progress_.phase = 1;

    auto t0 = std::chrono::steady_clock::now();
    load_metadata();
    auto t1 = std::chrono::steady_clock::now();
    progress_.phase1_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    progress_.phase = 2;
    build_semantic_index();
    auto t2 = std::chrono::steady_clock::now();
    progress_.phase2_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();

    progress_.phase = 3;
    process_transfers();
    replay_all();
    auto t3 = std::chrono::steady_clock::now();
    progress_.phase3_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

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

  const UserState *find_user(const std::string &addr) const {
    return get_user_state(addr);
  }

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

  std::vector<PositionAtTime> get_positions_at(const std::string &addr, int64_t sort_key) const {
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
  Database &db_;
  Database &stage2_db_;
  RebuildProgress progress_;

  // Phase 1: 元数据映射
  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<std::string, uint32_t> cond_map_;
  std::unordered_map<std::string, TokenInfo> token_map_;
  std::unordered_map<std::string, FPMMInfo> fpmm_map_;

  // Phase 2: 语义索引 (仅内存, chunk 内有效)
  std::unordered_map<TxCondKey, SplitInfo> tx_split_;
  std::unordered_map<TxCondKey, MergeInfo> tx_merge_;
  std::unordered_map<TxCondKey, RedemptionInfo> tx_redemption_;
  std::unordered_map<TxKey, ConvertInfo> tx_convert_;
  std::unordered_map<TxTokenKey, OrderInfo> tx_order_;
  std::unordered_map<TxKey, FPMMTradeInfo> tx_fpmm_trade_;
  std::unordered_map<TxKey, FPMMFundingInfo> tx_fpmm_funding_;

  // Phase 3: 用户事件
  std::vector<std::string> users_;
  std::unordered_map<std::string, uint32_t> user_map_;
  std::vector<std::vector<RawEvent>> user_events_;
  std::vector<UserState> user_states_;

  std::mutex user_mutex_;

  // ============================================================================
  // 工具函数
  // ============================================================================

  static std::string blob_to_hex(const std::string &blob) {
    if (blob.starts_with("0x"))
      return blob;
    return "0x" + blob;
  }

  static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  }

  static std::array<uint8_t, 32> hex_to_bytes32(const std::string &hex) {
    std::array<uint8_t, 32> result{};
    std::string h = hex;
    if (h.starts_with("0x"))
      h = h.substr(2);
    for (size_t i = 0; i < 32 && i * 2 < h.size(); ++i) {
      result[i] = static_cast<uint8_t>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
    }
    return result;
  }

  uint32_t intern_user(const std::string &addr) {
    std::string lower = to_lower(addr);
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

  // ============================================================================
  // Phase 1: 加载元数据
  // ============================================================================

  void load_metadata() {
    conditions_.clear();
    cond_ids_.clear();
    cond_map_.clear();
    token_map_.clear();
    fpmm_map_.clear();

    if (try_load_from_stage2()) {
      progress_.total_conditions = static_cast<int64_t>(conditions_.size());
      progress_.total_tokens = static_cast<int64_t>(token_map_.size());
      return;
    }

    // 从 token_map 表加载 (订单簿时代)
    auto token_rows = db_.query_json("SELECT token0, token1, condition_id FROM token_map");
    for (const auto &row : token_rows) {
      std::string token0 = to_lower(row["token0"].get<std::string>());
      std::string token1 = to_lower(row["token1"].get<std::string>());
      std::string cond_id = to_lower(row["condition_id"].get<std::string>());

      uint32_t cond_idx = get_or_create_condition(cond_id);
      int is_yes0 = (token0 < token1) ? 1 : 0;
      token_map_[token0] = {cond_idx, static_cast<uint8_t>(is_yes0)};
      token_map_[token1] = {cond_idx, static_cast<uint8_t>(1 - is_yes0)};
    }

    // 从 condition_resolution 加载结算信息
    auto cond_rows = db_.query_json("SELECT condition_id, payout_numerators FROM condition_resolution");
    for (const auto &row : cond_rows) {
      std::string cond_id = to_lower(row["condition_id"].get<std::string>());
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

    // 从 fpmm 表加载 (AMM 时代)
    auto fpmm_rows = db_.query_json("SELECT fpmm_addr, condition_ids FROM fpmm");
    for (const auto &row : fpmm_rows) {
      std::string fpmm_addr = to_lower(row["fpmm_addr"].get<std::string>());
      std::string cond_ids_str = row["condition_ids"].get<std::string>();

      auto cond_ids_arr = json::parse(cond_ids_str);
      if (cond_ids_arr.empty())
        continue;
      std::string cond_id = to_lower(cond_ids_arr[0].get<std::string>());

      uint32_t cond_idx = get_or_create_condition(cond_id);
      fpmm_map_[fpmm_addr] = {cond_idx};
    }

    progress_.total_conditions = static_cast<int64_t>(conditions_.size());
    progress_.total_tokens = static_cast<int64_t>(token_map_.size());
  }

  bool try_load_from_stage2() {
    try {
      auto count_rows = stage2_db_.query_json("SELECT COUNT(*) as cnt FROM rb_condition");
      if (count_rows.empty() || count_rows[0]["cnt"].get<int64_t>() == 0)
        return false;

      auto cond_rows = stage2_db_.query_json(
          "SELECT cond_idx, condition_id, outcome_count, payout_numerators FROM rb_condition ORDER BY cond_idx");
      for (const auto &row : cond_rows) {
        uint32_t idx = row["cond_idx"].get<uint32_t>();
        std::string cond_id = row["condition_id"].get<std::string>();
        int outcome_count = row["outcome_count"].get<int>();

        while (conditions_.size() <= idx) {
          conditions_.emplace_back();
          cond_ids_.push_back("");
        }

        conditions_[idx].outcome_count = static_cast<uint8_t>(outcome_count);
        cond_ids_[idx] = cond_id;
        cond_map_[cond_id] = idx;

        if (!row["payout_numerators"].is_null()) {
          auto payout_arr = json::parse(row["payout_numerators"].get<std::string>());
          for (const auto &v : payout_arr) {
            conditions_[idx].payout_numerators.push_back(v.get<int64_t>());
          }
        }
      }

      auto token_rows = stage2_db_.query_json("SELECT token_id, cond_idx, is_yes FROM rb_token");
      for (const auto &row : token_rows) {
        std::string token_id = row["token_id"].get<std::string>();
        uint32_t cond_idx = row["cond_idx"].get<uint32_t>();
        int is_yes = row["is_yes"].get<int>();
        token_map_[token_id] = {cond_idx, static_cast<uint8_t>(is_yes)};
      }

      auto fpmm_rows = stage2_db_.query_json("SELECT fpmm_addr, cond_idx FROM rb_fpmm");
      for (const auto &row : fpmm_rows) {
        std::string fpmm_addr = row["fpmm_addr"].get<std::string>();
        uint32_t cond_idx = row["cond_idx"].get<uint32_t>();
        fpmm_map_[fpmm_addr] = {cond_idx};
      }

      std::cout << "[Engine] Loaded from Stage2: " << conditions_.size() << " conditions, "
                << token_map_.size() << " tokens, " << fpmm_map_.size() << " FPMMs" << std::endl;
      return true;
    } catch (...) {
      return false;
    }
  }

  uint32_t get_or_create_condition(const std::string &cond_id) {
    auto it = cond_map_.find(cond_id);
    if (it != cond_map_.end())
      return it->second;
    uint32_t idx = static_cast<uint32_t>(conditions_.size());
    cond_map_[cond_id] = idx;
    cond_ids_.push_back(cond_id);
    conditions_.emplace_back();
    return idx;
  }

  // ============================================================================
  // Phase 2: 构建语义索引
  // ============================================================================

  void build_semantic_index() {
    tx_split_.clear();
    tx_merge_.clear();
    tx_redemption_.clear();
    tx_convert_.clear();
    tx_order_.clear();
    tx_fpmm_trade_.clear();
    tx_fpmm_funding_.clear();

    auto f1 = std::async(std::launch::async, [this]() { index_split(); });
    auto f2 = std::async(std::launch::async, [this]() { index_merge(); });
    auto f3 = std::async(std::launch::async, [this]() { index_redemption(); });
    auto f4 = std::async(std::launch::async, [this]() { index_convert(); });
    auto f5 = std::async(std::launch::async, [this]() { index_order_filled(); });
    auto f6 = std::async(std::launch::async, [this]() { index_fpmm_trade(); });
    auto f7 = std::async(std::launch::async, [this]() { index_fpmm_funding(); });

    f1.get();
    f2.get();
    f3.get();
    f4.get();
    f5.get();
    f6.get();
    f7.get();
  }

  void index_split() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, stakeholder, condition_id, amount FROM split");
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_split_[key] = {amount, stakeholder, cond_id};
    }
  }

  void index_merge() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, stakeholder, condition_id, amount FROM merge");
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_merge_[key] = {amount, stakeholder, cond_id};
    }
  }

  void index_redemption() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, redeemer, condition_id, index_sets, payout FROM redemption");
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string redeemer = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      std::string index_sets_str = result->GetValue(4, r).ToString();
      int64_t payout = result->GetValue(5, r).GetValue<int64_t>();

      auto index_sets_arr = json::parse(index_sets_str);
      int index_sets = 0;
      for (const auto &v : index_sets_arr) {
        index_sets |= static_cast<int>(v.get<int64_t>());
      }

      TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_redemption_[key] = {index_sets, payout, redeemer, cond_id};
    }
  }

  void index_convert() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, stakeholder, market_id, index_set, amount FROM convert");
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string market_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t index_set = result->GetValue(4, r).GetValue<int64_t>();
      int64_t amount = result->GetValue(5, r).GetValue<int64_t>();

      TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_convert_[key] = {market_id, index_set, amount, stakeholder};
    }
  }

  void index_order_filled() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, maker, taker, maker_asset_id, taker_asset_id, "
        "maker_amount, taker_amount, fee FROM order_filled");
    assert(!result->HasError());

    static const std::string ZERO_ASSET = "0x0000000000000000000000000000000000000000000000000000000000000000";

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string maker = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string taker = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      std::string maker_asset_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(4, r))));
      std::string taker_asset_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(5, r))));
      int64_t maker_amount = result->GetValue(6, r).GetValue<int64_t>();
      int64_t taker_amount = result->GetValue(7, r).GetValue<int64_t>();
      int64_t fee = result->GetValue(8, r).GetValue<int64_t>();

      std::string token_id;
      int maker_side;
      int64_t usdc, tokens;
      if (maker_asset_id == ZERO_ASSET) {
        token_id = taker_asset_id;
        maker_side = 1; // maker买
        usdc = maker_amount;
        tokens = taker_amount;
      } else {
        token_id = maker_asset_id;
        maker_side = 2; // maker卖
        usdc = taker_amount;
        tokens = maker_amount;
      }

      TxTokenKey key{block, hex_to_bytes32(tx_hash), token_id};
      tx_order_[key] = {maker, taker, maker_side, usdc, tokens, fee};
    }
  }

  void index_fpmm_trade() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, fpmm_addr, trader, side, outcome_index, "
        "usdc_amount, token_amount FROM fpmm_trade");
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string fpmm_addr = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string trader = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int side = result->GetValue(4, r).GetValue<int32_t>();
      int outcome_idx = result->GetValue(5, r).GetValue<int32_t>();
      int64_t usdc = result->GetValue(6, r).GetValue<int64_t>();
      int64_t tokens = result->GetValue(7, r).GetValue<int64_t>();

      TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_fpmm_trade_[key] = {fpmm_addr, trader, side, outcome_idx, usdc, tokens};
    }
  }

  void index_fpmm_funding() {
    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, fpmm_addr, funder, side, amounts FROM fpmm_funding");
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string fpmm_addr = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string funder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int side = result->GetValue(4, r).GetValue<int32_t>();
      std::string amounts_str = result->GetValue(5, r).ToString();

      auto amounts_arr = json::parse(amounts_str);
      int64_t amount0 = amounts_arr.size() > 0 ? amounts_arr[0].get<int64_t>() : 0;
      int64_t amount1 = amounts_arr.size() > 1 ? amounts_arr[1].get<int64_t>() : 0;

      TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_fpmm_funding_[key] = {fpmm_addr, funder, side, amount0, amount1};
    }
  }

  // ============================================================================
  // Phase 3: Transfer 分类与处理
  // ============================================================================

  void process_transfers() {
    users_.clear();
    user_map_.clear();
    user_events_.clear();

    duckdb::Connection conn(db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount "
        "FROM transfer ORDER BY block_number, log_index");
    assert(!result->HasError());

    int64_t rows = 0, events = 0;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      int64_t log_idx = result->GetValue(2, r).GetValue<int64_t>();
      std::string op = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      std::string from = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(4, r))));
      std::string to = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(5, r))));
      std::string token_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(6, r))));
      int64_t amount = result->GetValue(7, r).GetValue<int64_t>();

      int n = classify_and_emit(block, tx_hash, log_idx, op, from, to, token_id, amount);
      ++rows;
      events += n;
    }

    progress_.transfer_rows = rows;
    progress_.transfer_events = events;
    progress_.total_events = events;
    progress_.total_users = static_cast<int64_t>(users_.size());
  }

  int classify_and_emit(int64_t block, const std::string &tx_hash, int64_t log_idx,
                        const std::string &op, const std::string &from, const std::string &to,
                        const std::string &token_id, int64_t amount) {
    int64_t sort_key = block * 1000000000LL + log_idx;
    TxKey tx_key{block, hex_to_bytes32(tx_hash)};

    // FPMM 特殊处理: 可能没有 TokenRegistered, 需要从 fpmm_map_ 和 trade info 获取
    auto fpmm_it = fpmm_map_.find(op);
    if (fpmm_it != fpmm_map_.end()) {
      return handle_fpmm_transfer_special(sort_key, tx_key, fpmm_it->second, from, to, amount);
    }

    // 标准处理: 从 token_map_ 查找
    auto token_it = token_map_.find(token_id);
    if (token_it == token_map_.end())
      return 0;

    uint32_t cond_idx = token_it->second.cond_idx;
    uint8_t token_idx = token_it->second.is_yes ? 0 : 1;
    std::string cond_id = cond_ids_[cond_idx];
    const auto &cond = conditions_[cond_idx];

    TxCondKey tx_cond_key{block, hex_to_bytes32(tx_hash), cond_id};
    TxTokenKey tx_token_key{block, hex_to_bytes32(tx_hash), token_id};

    if (from == ZERO_ADDR) {
      return handle_mint(sort_key, tx_cond_key, tx_key, to, cond_idx, token_idx, amount, cond);
    } else if (to == ZERO_ADDR) {
      return handle_burn(sort_key, tx_cond_key, tx_key, from, cond_idx, token_idx, amount, cond);
    } else if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
      return handle_exchange_transfer(sort_key, tx_token_key, cond_idx, token_idx, amount);
    } else if (op == NEG_RISK_ADAPTER) {
      return 0;
    } else {
      return emit_transfer(sort_key, from, to, cond_idx, token_idx, amount);
    }
  }

  int handle_fpmm_transfer_special(int64_t sort_key, const TxKey &tx_key, const FPMMInfo &fpmm_info,
                                   const std::string &from, const std::string &to, int64_t amount) {
    uint32_t cond_idx = fpmm_info.cond_idx;
    const auto &cond = conditions_[cond_idx];
    std::string cond_id = cond_ids_[cond_idx];

    // FPMM trade?
    auto trade_it = tx_fpmm_trade_.find(tx_key);
    if (trade_it != tx_fpmm_trade_.end()) {
      const auto &info = trade_it->second;
      uint8_t token_idx = (info.outcome_idx == 0) ? 0 : 1;
      int64_t price = info.tokens > 0 ? (info.usdc * 1000000 / info.tokens) : 0;
      uint32_t uid = intern_user(info.trader);
      RawEvent evt{sort_key, cond_idx,
                   static_cast<uint8_t>(info.side == 1 ? EventType::FPMMBuy : EventType::FPMMSell),
                   token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // FPMM funding?
    auto funding_it = tx_fpmm_funding_.find(tx_key);
    if (funding_it != tx_fpmm_funding_.end()) {
      const auto &info = funding_it->second;
      int64_t total = info.amount0 + info.amount1;

      if (from == ZERO_ADDR && info.side == 1) {
        // LP Add mint
        uint8_t token_idx = (amount == info.amount0) ? 0 : 1;
        int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
        int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
        uint32_t uid = intern_user(info.funder);
        RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, 0, amount, price};
        push_event(uid, evt);
        return 1;
      }

      if (to == ZERO_ADDR && info.side == 2) {
        // LP Remove burn
        uint8_t token_idx = (amount == info.amount0) ? 0 : 1;
        int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
        int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
        uint32_t uid = intern_user(info.funder);
        RawEvent evt{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, 0, amount, price};
        push_event(uid, evt);
        return 1;
      }
    }

    // Split for FPMM
    TxCondKey tx_cond_key{tx_key.block, tx_key.tx_hash, cond_id};
    auto split_it = tx_split_.find(tx_cond_key);
    if (split_it != tx_split_.end() && from == ZERO_ADDR && split_it->second.stakeholder == to) {
      int64_t price = 1000000 / cond.outcome_count;
      uint32_t uid = intern_user(to);
      uint8_t token_idx = 0; // 简化: 假设第一个是 YES
      RawEvent evt{sort_key, cond_idx, EventType::Split, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // Merge for FPMM
    auto merge_it = tx_merge_.find(tx_cond_key);
    if (merge_it != tx_merge_.end() && to == ZERO_ADDR && merge_it->second.stakeholder == from) {
      int64_t price = 1000000 / cond.outcome_count;
      uint32_t uid = intern_user(from);
      uint8_t token_idx = 0;
      RawEvent evt{sort_key, cond_idx, EventType::Merge, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    return 0;
  }

  int handle_mint(int64_t sort_key, const TxCondKey &tx_cond_key, const TxKey &tx_key,
                  const std::string &to, uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                  const ConditionInfo &cond) {
    // Split?
    auto split_it = tx_split_.find(tx_cond_key);
    if (split_it != tx_split_.end() && split_it->second.stakeholder == to) {
      int64_t price = 1000000 / cond.outcome_count;
      uint32_t uid = intern_user(to);
      RawEvent evt{sort_key, cond_idx, EventType::Split, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // FPMMLPAdd?
    auto funding_it = tx_fpmm_funding_.find(tx_key);
    if (funding_it != tx_fpmm_funding_.end() && funding_it->second.side == 1) {
      const auto &info = funding_it->second;
      int64_t total = info.amount0 + info.amount1;
      int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
      int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
      uint32_t uid = intern_user(info.funder);
      RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // FPMM/NegRisk 内部 mint, 跳过
    return 0;
  }

  int handle_burn(int64_t sort_key, const TxCondKey &tx_cond_key, const TxKey &tx_key,
                  const std::string &from, uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                  const ConditionInfo &cond) {
    // Merge?
    auto merge_it = tx_merge_.find(tx_cond_key);
    if (merge_it != tx_merge_.end() && merge_it->second.stakeholder == from) {
      int64_t price = 1000000 / cond.outcome_count;
      uint32_t uid = intern_user(from);
      RawEvent evt{sort_key, cond_idx, EventType::Merge, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // Redemption?
    auto redeem_it = tx_redemption_.find(tx_cond_key);
    if (redeem_it != tx_redemption_.end() && redeem_it->second.redeemer == from) {
      int64_t payout_price = 0;
      if (!cond.payout_numerators.empty() && token_idx < cond.payout_numerators.size()) {
        payout_price = cond.payout_numerators[token_idx] * 1000000;
      }
      uint32_t uid = intern_user(from);
      RawEvent evt{sort_key, cond_idx, EventType::Redemption, token_idx, 0, amount, payout_price};
      push_event(uid, evt);
      return 1;
    }

    // Convert?
    auto convert_it = tx_convert_.find(tx_key);
    if (convert_it != tx_convert_.end() && convert_it->second.stakeholder == from) {
      const auto &info = convert_it->second;
      int popcount = __builtin_popcountll(static_cast<uint64_t>(info.index_set));
      int64_t price = popcount > 0 ? ((popcount - 1) * 1000000 / popcount) : 0;
      uint32_t uid = intern_user(from);
      RawEvent evt{sort_key, cond_idx, EventType::Convert, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // FPMMLPRemove?
    auto funding_it = tx_fpmm_funding_.find(tx_key);
    if (funding_it != tx_fpmm_funding_.end() && funding_it->second.side == 2) {
      const auto &info = funding_it->second;
      int64_t total = info.amount0 + info.amount1;
      int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
      int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
      uint32_t uid = intern_user(info.funder);
      RawEvent evt{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // FPMM/NegRisk 内部 burn, 跳过
    return 0;
  }

  int handle_exchange_transfer(int64_t sort_key, const TxTokenKey &tx_token_key,
                               uint32_t cond_idx, uint8_t token_idx, int64_t amount) {
    auto order_it = tx_order_.find(tx_token_key);
    if (order_it == tx_order_.end())
      return 0;

    const auto &info = order_it->second;
    int64_t price = info.tokens > 0 ? (info.usdc * 1000000 / info.tokens) : 0;

    uint32_t maker_uid = intern_user(info.maker);
    uint32_t taker_uid = intern_user(info.taker);

    RawEvent maker_evt{sort_key, cond_idx, 0, token_idx, 0, amount, price};
    RawEvent taker_evt{sort_key, cond_idx, 0, token_idx, 0, amount, price};

    if (info.maker_side == 1) {
      maker_evt.type = EventType::Buy;
      taker_evt.type = EventType::Sell;
    } else {
      maker_evt.type = EventType::Sell;
      taker_evt.type = EventType::Buy;
    }

    push_event(maker_uid, maker_evt);
    push_event(taker_uid, taker_evt);
    return 2;
  }

  int handle_fpmm_transfer(int64_t sort_key, const TxKey &tx_key, const std::string &op,
                           const std::string &from, const std::string &to,
                           uint32_t cond_idx, uint8_t token_idx, int64_t amount) {
    auto trade_it = tx_fpmm_trade_.find(tx_key);
    if (trade_it != tx_fpmm_trade_.end()) {
      const auto &info = trade_it->second;
      int64_t price = info.tokens > 0 ? (info.usdc * 1000000 / info.tokens) : 0;
      uint32_t uid = intern_user(info.trader);
      RawEvent evt{sort_key, cond_idx,
                   static_cast<uint8_t>(info.side == 1 ? EventType::FPMMBuy : EventType::FPMMSell),
                   token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

    // FPMM 返还多余 token 给 LP
    if (from == op) {
      uint32_t uid = intern_user(to);
      RawEvent evt{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0};
      push_event(uid, evt);
      return 1;
    }

    // FPMM 内部 transfer, 跳过
    return 0;
  }

  int emit_transfer(int64_t sort_key, const std::string &from, const std::string &to,
                    uint32_t cond_idx, uint8_t token_idx, int64_t amount) {
    uint32_t from_uid = intern_user(from);
    uint32_t to_uid = intern_user(to);

    RawEvent out_evt{sort_key, cond_idx, EventType::TransferOut, token_idx, 0, amount, 0};
    RawEvent in_evt{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0};

    push_event(from_uid, out_evt);
    push_event(to_uid, in_evt);
    return 2;
  }

  // ============================================================================
  // Replay
  // ============================================================================

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

} // namespace rebuild
