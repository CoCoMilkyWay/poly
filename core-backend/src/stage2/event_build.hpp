#pragma once

#include "../core/database.hpp"
#include "types.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace stage2 {

static constexpr const char *ZERO_ADDR = "0x0000000000000000000000000000000000000000";
static constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
static constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
static constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";

struct ScanStats {
  int64_t rows = 0;
  int64_t events = 0;
};

struct BuildProgress {
  int phase = 0;
  int64_t total_conditions = 0;
  int64_t total_tokens = 0;
  int64_t total_users = 0;
  bool running = false;
  double phase1_ms = 0;
  double phase2_ms = 0;
  double phase3_ms = 0;
  ScanStats order_filled;
  ScanStats split;
  ScanStats merge;
  ScanStats redemption;
  ScanStats fpmm_trade;
  ScanStats fpmm_funding;
  ScanStats convert;
  ScanStats transfer;
};

class EventBuilder {
public:
  explicit EventBuilder(Database &stage1_db, Database &stage2_db)
      : stage1_db_(stage1_db), stage2_db_(stage2_db) {}

  explicit EventBuilder(Database &db) : stage1_db_(db), stage2_db_(db) {}

  void build_all() {
    assert(!progress_.running);
    progress_ = BuildProgress{};
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
    auto t3 = std::chrono::steady_clock::now();
    progress_.phase3_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();

    progress_.running = false;
  }

  const BuildProgress &progress() const { return progress_; }

  const std::vector<ConditionInfo> &conditions() const { return conditions_; }
  const std::vector<std::string> &condition_ids() const { return cond_ids_; }
  const std::vector<std::string> &users() const { return users_; }
  const std::vector<std::vector<RawEvent>> &user_events() const { return user_events_; }

private:
  Database &stage1_db_;
  Database &stage2_db_;
  BuildProgress progress_;

  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<std::string, uint32_t> cond_map_;
  std::unordered_map<std::string, TokenInfo> token_map_;
  std::unordered_map<std::string, FPMMInfo> fpmm_map_;

  std::unordered_map<TxCondKey, SplitInfo> tx_split_;
  std::unordered_map<TxCondKey, MergeInfo> tx_merge_;
  std::unordered_map<TxCondKey, RedemptionInfo> tx_redemption_;
  std::unordered_map<TxKey, ConvertInfo> tx_convert_;
  std::unordered_map<TxTokenKey, OrderInfo> tx_order_;
  std::unordered_map<TxKey, FPMMTradeInfo> tx_fpmm_trade_;
  std::unordered_map<TxKey, FPMMFundingInfo> tx_fpmm_funding_;

  std::vector<std::string> users_;
  std::unordered_map<std::string, uint32_t> user_map_;
  std::vector<std::vector<RawEvent>> user_events_;
  std::mutex user_mutex_;

  static std::string blob_to_hex(const std::string &blob) {
    if (blob.starts_with("0x"))
      return blob;
    static const char hex_chars[] = "0123456789abcdef";
    std::string result = "0x";
    result.reserve(2 + blob.size() * 2);
    for (unsigned char c : blob) {
      result.push_back(hex_chars[c >> 4]);
      result.push_back(hex_chars[c & 0x0f]);
    }
    return result;
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

    auto token_rows = stage1_db_.query_json("SELECT token0, token1, condition_id FROM token_map");
    for (const auto &row : token_rows) {
      std::string token0 = to_lower(row["token0"].get<std::string>());
      std::string token1 = to_lower(row["token1"].get<std::string>());
      std::string cond_id = to_lower(row["condition_id"].get<std::string>());

      uint32_t cond_idx = get_or_create_condition(cond_id);
      int is_yes0 = (token0 < token1) ? 1 : 0;
      token_map_[token0] = {cond_idx, static_cast<uint8_t>(is_yes0)};
      token_map_[token1] = {cond_idx, static_cast<uint8_t>(1 - is_yes0)};
    }

    auto cond_rows = stage1_db_.query_json("SELECT condition_id, payout_numerators FROM condition_resolution");
    for (const auto &row : cond_rows) {
      std::string cond_id = to_lower(row["condition_id"].get<std::string>());
      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end())
        continue;

      auto &cond = conditions_[it->second];
      std::string payout_str = row["payout_numerators"].get<std::string>();
      auto payout_arr = nlohmann::json::parse(payout_str);
      for (const auto &v : payout_arr) {
        cond.payout_numerators.push_back(v.get<int64_t>());
      }
    }

    auto fpmm_rows = stage1_db_.query_json("SELECT fpmm_addr, condition_ids FROM fpmm");
    for (const auto &row : fpmm_rows) {
      std::string fpmm_addr = to_lower(row["fpmm_addr"].get<std::string>());
      std::string cond_ids_str = row["condition_ids"].get<std::string>();

      auto cond_ids_arr = nlohmann::json::parse(cond_ids_str);
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
      if (!stage2_db_.table_exists("rb_condition"))
        return false;

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
          auto payout_arr = nlohmann::json::parse(row["payout_numerators"].get<std::string>());
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

      std::cout << "[EventBuilder] Loaded from Stage2: " << conditions_.size() << " conditions, "
                << token_map_.size() << " tokens, " << fpmm_map_.size() << " FPMMs" << std::endl;
      return true;
    } catch (...) {
      return false;
    }
  }

  void build_semantic_index() {
    tx_split_.clear();
    tx_merge_.clear();
    tx_redemption_.clear();
    tx_convert_.clear();
    tx_order_.clear();
    tx_fpmm_trade_.clear();
    tx_fpmm_funding_.clear();

    auto f1 = std::async(std::launch::async, [this]() { return index_split(); });
    auto f2 = std::async(std::launch::async, [this]() { return index_merge(); });
    auto f3 = std::async(std::launch::async, [this]() { return index_redemption(); });
    auto f4 = std::async(std::launch::async, [this]() { return index_convert(); });
    auto f5 = std::async(std::launch::async, [this]() { return index_order_filled(); });
    auto f6 = std::async(std::launch::async, [this]() { return index_fpmm_trade(); });
    auto f7 = std::async(std::launch::async, [this]() { return index_fpmm_funding(); });

    progress_.split = f1.get();
    progress_.merge = f2.get();
    progress_.redemption = f3.get();
    progress_.convert = f4.get();
    progress_.order_filled = f5.get();
    progress_.fpmm_trade = f6.get();
    progress_.fpmm_funding = f7.get();
  }

  ScanStats index_split() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query("SELECT block_number, tx_hash, stakeholder, condition_id, amount FROM split");
    assert(!result->HasError());

    ScanStats stats;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_split_[key] = {amount, stakeholder, cond_id};
      ++stats.rows;
    }
    return stats;
  }

  ScanStats index_merge() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query("SELECT block_number, tx_hash, stakeholder, condition_id, amount FROM merge");
    assert(!result->HasError());

    ScanStats stats;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_merge_[key] = {amount, stakeholder, cond_id};
      ++stats.rows;
    }
    return stats;
  }

  ScanStats index_redemption() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query("SELECT block_number, tx_hash, redeemer, condition_id, index_sets, payout FROM redemption");
    assert(!result->HasError());

    ScanStats stats;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string redeemer = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      std::string index_sets_str = result->GetValue(4, r).ToString();
      int64_t payout = result->GetValue(5, r).GetValue<int64_t>();

      auto index_sets_arr = nlohmann::json::parse(index_sets_str);
      int index_sets = 0;
      for (const auto &v : index_sets_arr) {
        index_sets |= static_cast<int>(v.get<int64_t>());
      }

      TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_redemption_[key] = {index_sets, payout, redeemer, cond_id};
      ++stats.rows;
    }
    return stats;
  }

  ScanStats index_convert() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query("SELECT block_number, tx_hash, stakeholder, market_id, index_set, amount FROM convert");
    assert(!result->HasError());

    ScanStats stats;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string market_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t index_set = result->GetValue(4, r).GetValue<int64_t>();
      int64_t amount = result->GetValue(5, r).GetValue<int64_t>();

      TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_convert_[key] = {market_id, index_set, amount, stakeholder};
      ++stats.rows;
    }
    return stats;
  }

  ScanStats index_order_filled() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, maker, taker, maker_asset_id, taker_asset_id, "
        "maker_amount, taker_amount, fee FROM order_filled");
    assert(!result->HasError());

    static const std::string ZERO_ASSET = "0x0000000000000000000000000000000000000000000000000000000000000000";

    ScanStats stats;
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
        maker_side = 1;
        usdc = maker_amount;
        tokens = taker_amount;
      } else {
        token_id = maker_asset_id;
        maker_side = 2;
        usdc = taker_amount;
        tokens = maker_amount;
      }

      TxTokenKey key{block, hex_to_bytes32(tx_hash), token_id};
      tx_order_[key] = {maker, taker, maker_side, usdc, tokens, fee};
      ++stats.rows;
    }
    return stats;
  }

  ScanStats index_fpmm_trade() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, fpmm_addr, trader, side, outcome_index, "
        "usdc_amount, token_amount FROM fpmm_trade");
    assert(!result->HasError());

    ScanStats stats;
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
      ++stats.rows;
    }
    return stats;
  }

  ScanStats index_fpmm_funding() {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query("SELECT block_number, tx_hash, fpmm_addr, funder, side, amounts FROM fpmm_funding");
    assert(!result->HasError());

    ScanStats stats;
    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string fpmm_addr = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string funder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int side = result->GetValue(4, r).GetValue<int32_t>();
      std::string amounts_str = result->GetValue(5, r).ToString();

      auto amounts_arr = nlohmann::json::parse(amounts_str);
      int64_t amount0 = amounts_arr.size() > 0 ? amounts_arr[0].get<int64_t>() : 0;
      int64_t amount1 = amounts_arr.size() > 1 ? amounts_arr[1].get<int64_t>() : 0;

      TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_fpmm_funding_[key] = {fpmm_addr, funder, side, amount0, amount1};
      ++stats.rows;
    }
    return stats;
  }

  void process_transfers() {
    users_.clear();
    user_map_.clear();
    user_events_.clear();

    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount "
        "FROM transfer ORDER BY block_number, log_index");
    assert(!result->HasError());

    ScanStats stats;
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
      ++stats.rows;
      stats.events += n;
    }

    progress_.transfer = stats;
    progress_.total_users = static_cast<int64_t>(users_.size());
  }

  int classify_and_emit(int64_t block, const std::string &tx_hash, int64_t log_idx,
                        const std::string &op, const std::string &from, const std::string &to,
                        const std::string &token_id, int64_t amount) {
    int64_t sort_key = block * 1000000000LL + log_idx;
    TxKey tx_key{block, hex_to_bytes32(tx_hash)};

    auto fpmm_it = fpmm_map_.find(op);
    if (fpmm_it != fpmm_map_.end()) {
      return handle_fpmm_transfer(sort_key, tx_key, fpmm_it->second, from, to, amount);
    }

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

  int handle_fpmm_transfer(int64_t sort_key, const TxKey &tx_key, const FPMMInfo &fpmm_info,
                           const std::string &from, const std::string &to, int64_t amount) {
    uint32_t cond_idx = fpmm_info.cond_idx;

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

    auto funding_it = tx_fpmm_funding_.find(tx_key);
    if (funding_it != tx_fpmm_funding_.end()) {
      const auto &info = funding_it->second;
      int64_t total = info.amount0 + info.amount1;

      if (from == ZERO_ADDR && info.side == 1) {
        uint8_t token_idx = (amount == info.amount0) ? 0 : 1;
        int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
        int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
        uint32_t uid = intern_user(info.funder);
        RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, 0, amount, price};
        push_event(uid, evt);
        return 1;
      }

      if (to == ZERO_ADDR && info.side == 2) {
        uint8_t token_idx = (amount == info.amount0) ? 0 : 1;
        int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
        int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
        uint32_t uid = intern_user(info.funder);
        RawEvent evt{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, 0, amount, price};
        push_event(uid, evt);
        return 1;
      }
    }

    return 0;
  }

  int handle_mint(int64_t sort_key, const TxCondKey &tx_cond_key, const TxKey &tx_key,
                  const std::string &to, uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                  const ConditionInfo &cond) {
    auto split_it = tx_split_.find(tx_cond_key);
    if (split_it != tx_split_.end() && split_it->second.stakeholder == to) {
      int64_t price = 1000000 / cond.outcome_count;
      uint32_t uid = intern_user(to);
      RawEvent evt{sort_key, cond_idx, EventType::Split, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

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

    return 0;
  }

  int handle_burn(int64_t sort_key, const TxCondKey &tx_cond_key, const TxKey &tx_key,
                  const std::string &from, uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                  const ConditionInfo &cond) {
    auto merge_it = tx_merge_.find(tx_cond_key);
    if (merge_it != tx_merge_.end() && merge_it->second.stakeholder == from) {
      int64_t price = 1000000 / cond.outcome_count;
      uint32_t uid = intern_user(from);
      RawEvent evt{sort_key, cond_idx, EventType::Merge, token_idx, 0, amount, price};
      push_event(uid, evt);
      return 1;
    }

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
};

} // namespace stage2
