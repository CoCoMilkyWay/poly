#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>
#include <duckdb.hpp>
#include <nlohmann/json.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "../rebuild/rebuilder_types.hpp"

namespace asio = boost::asio;
using json = nlohmann::json;

struct UserSyncStatus {
  int64_t last_block = 0;
  int64_t head_block = 0;
  bool is_syncing = false;
  double blocks_per_second = 0.0;
};

class UserSyncCoordinator {
public:
  UserSyncCoordinator(const Config &config, Database &stage1_db, Database &stage2_db)
      : config_(config), stage1_db_(stage1_db), stage2_db_(stage2_db),
        chunk_size_(config.rpc_chunk) {}

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    init_schema();
    load_mappings_from_stage2();
    schedule_sync(0);
  }

  UserSyncStatus get_status() const {
    UserSyncStatus s;
    s.last_block = last_block_.load();
    s.head_block = head_block_.load();
    s.is_syncing = is_syncing_.load();
    s.blocks_per_second = get_blocks_per_second();
    return s;
  }

  bool is_syncing() const { return is_syncing_; }
  int64_t get_last_block() const { return last_block_; }
  int64_t get_head_block() const { return head_block_; }

  double get_blocks_per_second() const {
    std::lock_guard<std::mutex> lock(history_mutex_);
    if (chunk_history_.size() < 2)
      return 0.0;
    double time_diff = chunk_history_.back().time_s - chunk_history_.front().time_s;
    if (time_diff <= 0)
      return 0.0;
    double total_blocks = 0;
    for (const auto &r : chunk_history_)
      total_blocks += r.block_count;
    return total_blocks / time_diff;
  }

  const std::vector<rebuild::ConditionInfo> &conditions() const { return conditions_; }
  const std::vector<std::string> &condition_ids() const { return cond_ids_; }

private:
  static constexpr const char *ZERO_ADDR = "0x0000000000000000000000000000000000000000";
  static constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
  static constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
  static constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";

  void init_schema() {
    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_condition (
        cond_idx INTEGER PRIMARY KEY,
        condition_id TEXT UNIQUE,
        outcome_count INTEGER DEFAULT 2,
        payout_numerators TEXT
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_token (
        token_id TEXT PRIMARY KEY,
        cond_idx INTEGER,
        is_yes INTEGER
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_fpmm (
        fpmm_addr TEXT PRIMARY KEY,
        cond_idx INTEGER
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS user_event (
        user_addr TEXT,
        sort_key BIGINT,
        cond_idx INTEGER,
        event_type INTEGER,
        token_idx INTEGER,
        amount BIGINT,
        price BIGINT,
        PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS sync_cursor (
        key TEXT PRIMARY KEY,
        value BIGINT
      )
    )");

    stage2_db_.execute("CREATE INDEX IF NOT EXISTS idx_user_event_user ON user_event(user_addr)");
  }

  void load_mappings_from_stage2() {
    conditions_.clear();
    cond_ids_.clear();
    cond_map_.clear();
    token_map_.clear();
    fpmm_map_.clear();

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

    auto cursor_rows = stage2_db_.query_json("SELECT value FROM sync_cursor WHERE key = 'last_block'");
    if (!cursor_rows.empty()) {
      last_block_ = cursor_rows[0]["value"].get<int64_t>();
    }

    std::cout << "[UserSync] Loaded " << conditions_.size() << " conditions, "
              << token_map_.size() << " tokens, "
              << fpmm_map_.size() << " FPMMs from Stage2" << std::endl;
  }

  void schedule_sync(int delay_seconds) {
    auto timer = std::make_shared<asio::steady_timer>(*ioc_);
    timer->expires_after(std::chrono::seconds(delay_seconds));
    timer->async_wait([this, timer](boost::system::error_code ec) {
      if (!ec) {
        do_sync();
      }
    });
  }

  void do_sync() {
    is_syncing_ = true;

    auto head_rows = stage1_db_.query_json("SELECT MAX(block_number) as head FROM transfer");
    if (head_rows.empty() || head_rows[0]["head"].is_null()) {
      std::cout << "[UserSync] Stage1 transfer table empty, waiting..." << std::endl;
      is_syncing_ = false;
      schedule_sync(chunk_size_ * 2);
      return;
    }
    head_block_ = head_rows[0]["head"].get<int64_t>();

    int64_t behind = head_block_ - last_block_;
    std::cout << "[UserSync] last=" << last_block_.load() << ", head=" << head_block_.load()
              << ", behind=" << behind << std::endl;

    if (behind < chunk_size_) {
      std::cout << "[UserSync] Caught up, waiting " << (chunk_size_ * 2) << "s" << std::endl;
      is_syncing_ = false;
      schedule_sync(chunk_size_ * 2);
      return;
    }

    int64_t from_block = last_block_ + 1;
    int64_t to_block = std::min(from_block + chunk_size_ - 1, head_block_.load());

    sync_chunk(from_block, to_block);
  }

  void sync_chunk(int64_t from_block, int64_t to_block) {
    auto t0 = std::chrono::steady_clock::now();

    update_mappings_for_range(from_block, to_block);
    build_semantic_index_for_range(from_block, to_block);

    std::vector<std::string> event_values;
    process_transfers_for_range(from_block, to_block, event_values);

    {
      Database::WriteLock lock(stage2_db_);
      duckdb::Connection conn(stage2_db_.get_duckdb());

      conn.Query("BEGIN TRANSACTION");

      if (!new_conditions_.empty()) {
        for (const auto &[cond_id, idx] : new_conditions_) {
          auto stmt = conn.Prepare(
              "INSERT OR REPLACE INTO rb_condition (cond_idx, condition_id, outcome_count) VALUES (?, ?, 2)");
          stmt->Execute(idx, cond_id);
        }
        new_conditions_.clear();
      }

      if (!new_tokens_.empty()) {
        for (const auto &[token_id, info] : new_tokens_) {
          auto stmt = conn.Prepare(
              "INSERT OR REPLACE INTO rb_token (token_id, cond_idx, is_yes) VALUES (?, ?, ?)");
          stmt->Execute(token_id, info.cond_idx, info.is_yes);
        }
        new_tokens_.clear();
      }

      if (!new_fpmms_.empty()) {
        for (const auto &[fpmm_addr, info] : new_fpmms_) {
          auto stmt = conn.Prepare(
              "INSERT OR REPLACE INTO rb_fpmm (fpmm_addr, cond_idx) VALUES (?, ?)");
          stmt->Execute(fpmm_addr, info.cond_idx);
        }
        new_fpmms_.clear();
      }

      if (!event_values.empty()) {
        std::string insert_sql = "INSERT OR IGNORE INTO user_event VALUES ";
        for (size_t i = 0; i < event_values.size(); ++i) {
          if (i > 0) insert_sql += ",";
          insert_sql += event_values[i];
        }
        conn.Query(insert_sql);
      }

      auto cursor_stmt = conn.Prepare(
          "INSERT OR REPLACE INTO sync_cursor (key, value) VALUES ('last_block', ?)");
      cursor_stmt->Execute(to_block);

      conn.Query("COMMIT");
    }

    last_block_ = to_block;

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double now = std::chrono::duration<double>(t1.time_since_epoch()).count();

    {
      std::lock_guard<std::mutex> lock(history_mutex_);
      chunk_history_.push_back({to_block, now, to_block - from_block + 1});
      if (chunk_history_.size() > 20)
        chunk_history_.pop_front();
    }

    std::cout << "[UserSync] Chunk " << from_block << "-" << to_block
              << " (" << event_values.size() << " events) in " << elapsed_s << "s" << std::endl;

    if (to_block < head_block_) {
      asio::post(*ioc_, [this, to_block]() {
        int64_t next_from = to_block + 1;
        int64_t next_to = std::min(next_from + chunk_size_ - 1, head_block_.load());
        sync_chunk(next_from, next_to);
      });
    } else {
      std::cout << "[UserSync] Sync complete, waiting " << (chunk_size_ * 2) << "s" << std::endl;
      is_syncing_ = false;
      schedule_sync(chunk_size_ * 2);
    }
  }

  void update_mappings_for_range(int64_t from_block, int64_t to_block) {
    duckdb::Connection conn(stage1_db_.get_duckdb());

    auto token_result = conn.Query(
        "SELECT token0, token1, condition_id FROM token_map "
        "WHERE block_number >= " + std::to_string(from_block) +
        " AND block_number <= " + std::to_string(to_block));
    assert(!token_result->HasError());

    for (size_t r = 0; r < token_result->RowCount(); ++r) {
      std::string token0 = to_lower(blob_to_hex(duckdb::StringValue::Get(token_result->GetValue(0, r))));
      std::string token1 = to_lower(blob_to_hex(duckdb::StringValue::Get(token_result->GetValue(1, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(token_result->GetValue(2, r))));

      uint32_t cond_idx = get_or_create_condition(cond_id);
      int is_yes0 = (token0 < token1) ? 1 : 0;

      if (token_map_.find(token0) == token_map_.end()) {
        token_map_[token0] = {cond_idx, static_cast<uint8_t>(is_yes0)};
        new_tokens_[token0] = {cond_idx, static_cast<uint8_t>(is_yes0)};
      }
      if (token_map_.find(token1) == token_map_.end()) {
        token_map_[token1] = {cond_idx, static_cast<uint8_t>(1 - is_yes0)};
        new_tokens_[token1] = {cond_idx, static_cast<uint8_t>(1 - is_yes0)};
      }
    }

    auto cond_result = conn.Query(
        "SELECT condition_id, payout_numerators FROM condition_resolution "
        "WHERE block_number >= " + std::to_string(from_block) +
        " AND block_number <= " + std::to_string(to_block));
    assert(!cond_result->HasError());

    for (size_t r = 0; r < cond_result->RowCount(); ++r) {
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(cond_result->GetValue(0, r))));
      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end())
        continue;

      auto &cond = conditions_[it->second];
      if (cond.payout_numerators.empty()) {
        std::string payout_str = cond_result->GetValue(1, r).ToString();
        auto payout_arr = json::parse(payout_str);
        for (const auto &v : payout_arr) {
          cond.payout_numerators.push_back(v.get<int64_t>());
        }
      }
    }

    auto fpmm_result = conn.Query(
        "SELECT fpmm_addr, condition_ids FROM fpmm "
        "WHERE block_number >= " + std::to_string(from_block) +
        " AND block_number <= " + std::to_string(to_block));
    assert(!fpmm_result->HasError());

    for (size_t r = 0; r < fpmm_result->RowCount(); ++r) {
      std::string fpmm_addr = to_lower(blob_to_hex(duckdb::StringValue::Get(fpmm_result->GetValue(0, r))));
      std::string cond_ids_str = fpmm_result->GetValue(1, r).ToString();

      auto cond_ids_arr = json::parse(cond_ids_str);
      if (cond_ids_arr.empty())
        continue;
      std::string cond_id = to_lower(cond_ids_arr[0].get<std::string>());

      if (fpmm_map_.find(fpmm_addr) == fpmm_map_.end()) {
        uint32_t cond_idx = get_or_create_condition(cond_id);
        fpmm_map_[fpmm_addr] = {cond_idx};
        new_fpmms_[fpmm_addr] = {cond_idx};
      }
    }
  }

  void build_semantic_index_for_range(int64_t from_block, int64_t to_block) {
    tx_split_.clear();
    tx_merge_.clear();
    tx_redemption_.clear();
    tx_convert_.clear();
    tx_order_.clear();
    tx_fpmm_trade_.clear();
    tx_fpmm_funding_.clear();

    std::string range_clause = " WHERE block_number >= " + std::to_string(from_block) +
                               " AND block_number <= " + std::to_string(to_block);

    auto f1 = std::async(std::launch::async, [this, &range_clause]() { index_split(range_clause); });
    auto f2 = std::async(std::launch::async, [this, &range_clause]() { index_merge(range_clause); });
    auto f3 = std::async(std::launch::async, [this, &range_clause]() { index_redemption(range_clause); });
    auto f4 = std::async(std::launch::async, [this, &range_clause]() { index_convert(range_clause); });
    auto f5 = std::async(std::launch::async, [this, &range_clause]() { index_order_filled(range_clause); });
    auto f6 = std::async(std::launch::async, [this, &range_clause]() { index_fpmm_trade(range_clause); });
    auto f7 = std::async(std::launch::async, [this, &range_clause]() { index_fpmm_funding(range_clause); });

    f1.get(); f2.get(); f3.get(); f4.get(); f5.get(); f6.get(); f7.get();
  }

  void index_split(const std::string &range_clause) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, stakeholder, condition_id, amount FROM split" + range_clause);
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      rebuild::TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_split_[key] = {amount, stakeholder, cond_id};
    }
  }

  void index_merge(const std::string &range_clause) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, stakeholder, condition_id, amount FROM merge" + range_clause);
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string cond_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t amount = result->GetValue(4, r).GetValue<int64_t>();

      rebuild::TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_merge_[key] = {amount, stakeholder, cond_id};
    }
  }

  void index_redemption(const std::string &range_clause) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, redeemer, condition_id, index_sets, payout FROM redemption" + range_clause);
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

      rebuild::TxCondKey key{block, hex_to_bytes32(tx_hash), cond_id};
      tx_redemption_[key] = {index_sets, payout, redeemer, cond_id};
    }
  }

  void index_convert(const std::string &range_clause) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, stakeholder, market_id, index_set, amount FROM convert" + range_clause);
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      std::string stakeholder = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(2, r))));
      std::string market_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      int64_t index_set = result->GetValue(4, r).GetValue<int64_t>();
      int64_t amount = result->GetValue(5, r).GetValue<int64_t>();

      rebuild::TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_convert_[key] = {market_id, index_set, amount, stakeholder};
    }
  }

  void index_order_filled(const std::string &range_clause) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, maker, taker, maker_asset_id, taker_asset_id, "
        "maker_amount, taker_amount, fee FROM order_filled" + range_clause);
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
        maker_side = 1;
        usdc = maker_amount;
        tokens = taker_amount;
      } else {
        token_id = maker_asset_id;
        maker_side = 2;
        usdc = taker_amount;
        tokens = maker_amount;
      }

      rebuild::TxTokenKey key{block, hex_to_bytes32(tx_hash), token_id};
      tx_order_[key] = {maker, taker, maker_side, usdc, tokens, fee};
    }
  }

  void index_fpmm_trade(const std::string &range_clause) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, fpmm_addr, trader, side, outcome_index, "
        "usdc_amount, token_amount FROM fpmm_trade" + range_clause);
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

      rebuild::TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_fpmm_trade_[key] = {fpmm_addr, trader, side, outcome_idx, usdc, tokens};
    }
  }

  void index_fpmm_funding(const std::string &range_clause) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, fpmm_addr, funder, side, amounts FROM fpmm_funding" + range_clause);
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

      rebuild::TxKey key{block, hex_to_bytes32(tx_hash)};
      tx_fpmm_funding_[key] = {fpmm_addr, funder, side, amount0, amount1};
    }
  }

  void process_transfers_for_range(int64_t from_block, int64_t to_block,
                                   std::vector<std::string> &event_values) {
    duckdb::Connection conn(stage1_db_.get_duckdb());
    auto result = conn.Query(
        "SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount "
        "FROM transfer WHERE block_number >= " + std::to_string(from_block) +
        " AND block_number <= " + std::to_string(to_block) +
        " ORDER BY block_number, log_index");
    assert(!result->HasError());

    for (size_t r = 0; r < result->RowCount(); ++r) {
      int64_t block = result->GetValue(0, r).GetValue<int64_t>();
      std::string tx_hash = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(1, r))));
      int64_t log_idx = result->GetValue(2, r).GetValue<int64_t>();
      std::string op = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(3, r))));
      std::string from = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(4, r))));
      std::string to = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(5, r))));
      std::string token_id = to_lower(blob_to_hex(duckdb::StringValue::Get(result->GetValue(6, r))));
      int64_t amount = result->GetValue(7, r).GetValue<int64_t>();

      classify_and_emit(block, tx_hash, log_idx, op, from, to, token_id, amount, event_values);
    }
  }

  void classify_and_emit(int64_t block, const std::string &tx_hash, int64_t log_idx,
                         const std::string &op, const std::string &from, const std::string &to,
                         const std::string &token_id, int64_t amount,
                         std::vector<std::string> &event_values) {
    int64_t sort_key = block * 1000000000LL + log_idx;
    rebuild::TxKey tx_key{block, hex_to_bytes32(tx_hash)};

    auto fpmm_it = fpmm_map_.find(op);
    if (fpmm_it != fpmm_map_.end()) {
      handle_fpmm_transfer_special(sort_key, tx_key, fpmm_it->second, from, to, amount, event_values);
      return;
    }

    auto token_it = token_map_.find(token_id);
    if (token_it == token_map_.end())
      return;

    uint32_t cond_idx = token_it->second.cond_idx;
    uint8_t token_idx = token_it->second.is_yes ? 0 : 1;
    std::string cond_id = cond_ids_[cond_idx];
    const auto &cond = conditions_[cond_idx];

    rebuild::TxCondKey tx_cond_key{block, hex_to_bytes32(tx_hash), cond_id};
    rebuild::TxTokenKey tx_token_key{block, hex_to_bytes32(tx_hash), token_id};

    if (from == ZERO_ADDR) {
      handle_mint(sort_key, tx_cond_key, tx_key, to, cond_idx, token_idx, amount, cond, event_values);
    } else if (to == ZERO_ADDR) {
      handle_burn(sort_key, tx_cond_key, tx_key, from, cond_idx, token_idx, amount, cond, event_values);
    } else if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
      handle_exchange_transfer(sort_key, tx_token_key, cond_idx, token_idx, amount, event_values);
    } else if (op == NEG_RISK_ADAPTER) {
      return;
    } else {
      emit_transfer(sort_key, from, to, cond_idx, token_idx, amount, event_values);
    }
  }

  void handle_fpmm_transfer_special(int64_t sort_key, const rebuild::TxKey &tx_key,
                                    const rebuild::FPMMInfo &fpmm_info,
                                    const std::string &from, const std::string &to, int64_t amount,
                                    std::vector<std::string> &event_values) {
    uint32_t cond_idx = fpmm_info.cond_idx;
    const auto &cond = conditions_[cond_idx];
    std::string cond_id = cond_ids_[cond_idx];

    auto trade_it = tx_fpmm_trade_.find(tx_key);
    if (trade_it != tx_fpmm_trade_.end()) {
      const auto &info = trade_it->second;
      uint8_t token_idx = (info.outcome_idx == 0) ? 0 : 1;
      int64_t price = info.tokens > 0 ? (info.usdc * 1000000 / info.tokens) : 0;
      int event_type = info.side == 1 ? rebuild::EventType::FPMMBuy : rebuild::EventType::FPMMSell;
      emit_event(info.trader, sort_key, cond_idx, event_type, token_idx, amount, price, event_values);
      return;
    }

    auto funding_it = tx_fpmm_funding_.find(tx_key);
    if (funding_it != tx_fpmm_funding_.end()) {
      const auto &info = funding_it->second;
      int64_t total = info.amount0 + info.amount1;

      if (from == ZERO_ADDR && info.side == 1) {
        uint8_t token_idx = (amount == info.amount0) ? 0 : 1;
        int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
        int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
        emit_event(info.funder, sort_key, cond_idx, rebuild::EventType::FPMMLPAdd, token_idx, amount, price, event_values);
        return;
      }

      if (to == ZERO_ADDR && info.side == 2) {
        uint8_t token_idx = (amount == info.amount0) ? 0 : 1;
        int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
        int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
        emit_event(info.funder, sort_key, cond_idx, rebuild::EventType::FPMMLPRemove, token_idx, amount, price, event_values);
        return;
      }
    }

    rebuild::TxCondKey tx_cond_key{tx_key.block, tx_key.tx_hash, cond_id};
    auto split_it = tx_split_.find(tx_cond_key);
    if (split_it != tx_split_.end() && from == ZERO_ADDR && split_it->second.stakeholder == to) {
      int64_t price = 1000000 / cond.outcome_count;
      uint8_t token_idx = 0;
      emit_event(to, sort_key, cond_idx, rebuild::EventType::Split, token_idx, amount, price, event_values);
      return;
    }

    auto merge_it = tx_merge_.find(tx_cond_key);
    if (merge_it != tx_merge_.end() && to == ZERO_ADDR && merge_it->second.stakeholder == from) {
      int64_t price = 1000000 / cond.outcome_count;
      uint8_t token_idx = 0;
      emit_event(from, sort_key, cond_idx, rebuild::EventType::Merge, token_idx, amount, price, event_values);
      return;
    }
  }

  void handle_mint(int64_t sort_key, const rebuild::TxCondKey &tx_cond_key, const rebuild::TxKey &tx_key,
                   const std::string &to, uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                   const rebuild::ConditionInfo &cond, std::vector<std::string> &event_values) {
    auto split_it = tx_split_.find(tx_cond_key);
    if (split_it != tx_split_.end() && split_it->second.stakeholder == to) {
      int64_t price = 1000000 / cond.outcome_count;
      emit_event(to, sort_key, cond_idx, rebuild::EventType::Split, token_idx, amount, price, event_values);
      return;
    }

    auto funding_it = tx_fpmm_funding_.find(tx_key);
    if (funding_it != tx_fpmm_funding_.end() && funding_it->second.side == 1) {
      const auto &info = funding_it->second;
      int64_t total = info.amount0 + info.amount1;
      int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
      int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
      emit_event(info.funder, sort_key, cond_idx, rebuild::EventType::FPMMLPAdd, token_idx, amount, price, event_values);
      return;
    }
  }

  void handle_burn(int64_t sort_key, const rebuild::TxCondKey &tx_cond_key, const rebuild::TxKey &tx_key,
                   const std::string &from, uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                   const rebuild::ConditionInfo &cond, std::vector<std::string> &event_values) {
    auto merge_it = tx_merge_.find(tx_cond_key);
    if (merge_it != tx_merge_.end() && merge_it->second.stakeholder == from) {
      int64_t price = 1000000 / cond.outcome_count;
      emit_event(from, sort_key, cond_idx, rebuild::EventType::Merge, token_idx, amount, price, event_values);
      return;
    }

    auto redeem_it = tx_redemption_.find(tx_cond_key);
    if (redeem_it != tx_redemption_.end() && redeem_it->second.redeemer == from) {
      int64_t payout_price = 0;
      if (!cond.payout_numerators.empty() && token_idx < cond.payout_numerators.size()) {
        payout_price = cond.payout_numerators[token_idx] * 1000000;
      }
      emit_event(from, sort_key, cond_idx, rebuild::EventType::Redemption, token_idx, amount, payout_price, event_values);
      return;
    }

    auto convert_it = tx_convert_.find(tx_key);
    if (convert_it != tx_convert_.end() && convert_it->second.stakeholder == from) {
      const auto &info = convert_it->second;
      int popcount = __builtin_popcountll(static_cast<uint64_t>(info.index_set));
      int64_t price = popcount > 0 ? ((popcount - 1) * 1000000 / popcount) : 0;
      emit_event(from, sort_key, cond_idx, rebuild::EventType::Convert, token_idx, amount, price, event_values);
      return;
    }

    auto funding_it = tx_fpmm_funding_.find(tx_key);
    if (funding_it != tx_fpmm_funding_.end() && funding_it->second.side == 2) {
      const auto &info = funding_it->second;
      int64_t total = info.amount0 + info.amount1;
      int64_t my_amount = (token_idx == 0) ? info.amount0 : info.amount1;
      int64_t price = total > 0 ? (my_amount * 1000000 / total) : 500000;
      emit_event(info.funder, sort_key, cond_idx, rebuild::EventType::FPMMLPRemove, token_idx, amount, price, event_values);
      return;
    }
  }

  void handle_exchange_transfer(int64_t sort_key, const rebuild::TxTokenKey &tx_token_key,
                                uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                                std::vector<std::string> &event_values) {
    auto order_it = tx_order_.find(tx_token_key);
    if (order_it == tx_order_.end())
      return;

    const auto &info = order_it->second;
    int64_t price = info.tokens > 0 ? (info.usdc * 1000000 / info.tokens) : 0;

    int maker_type = info.maker_side == 1 ? rebuild::EventType::Buy : rebuild::EventType::Sell;
    int taker_type = info.maker_side == 1 ? rebuild::EventType::Sell : rebuild::EventType::Buy;

    emit_event(info.maker, sort_key, cond_idx, maker_type, token_idx, amount, price, event_values);
    emit_event(info.taker, sort_key, cond_idx, taker_type, token_idx, amount, price, event_values);
  }

  void emit_transfer(int64_t sort_key, const std::string &from, const std::string &to,
                     uint32_t cond_idx, uint8_t token_idx, int64_t amount,
                     std::vector<std::string> &event_values) {
    emit_event(from, sort_key, cond_idx, rebuild::EventType::TransferOut, token_idx, amount, 0, event_values);
    emit_event(to, sort_key, cond_idx, rebuild::EventType::TransferIn, token_idx, amount, 0, event_values);
  }

  void emit_event(const std::string &user_addr, int64_t sort_key, uint32_t cond_idx,
                  int event_type, uint8_t token_idx, int64_t amount, int64_t price,
                  std::vector<std::string> &event_values) {
    event_values.push_back("('" + user_addr + "'," +
                           std::to_string(sort_key) + "," +
                           std::to_string(cond_idx) + "," +
                           std::to_string(event_type) + "," +
                           std::to_string(token_idx) + "," +
                           std::to_string(amount) + "," +
                           std::to_string(price) + ")");
  }

  uint32_t get_or_create_condition(const std::string &cond_id) {
    auto it = cond_map_.find(cond_id);
    if (it != cond_map_.end())
      return it->second;
    uint32_t idx = static_cast<uint32_t>(conditions_.size());
    cond_map_[cond_id] = idx;
    cond_ids_.push_back(cond_id);
    conditions_.emplace_back();
    new_conditions_[cond_id] = idx;
    return idx;
  }

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

  const Config &config_;
  Database &stage1_db_;
  Database &stage2_db_;
  asio::io_context *ioc_ = nullptr;

  int64_t chunk_size_;
  std::atomic<bool> is_syncing_{false};
  std::atomic<int64_t> last_block_{0};
  std::atomic<int64_t> head_block_{0};

  struct ChunkRecord {
    int64_t to_block;
    double time_s;
    int64_t block_count;
  };
  mutable std::mutex history_mutex_;
  std::deque<ChunkRecord> chunk_history_;

  std::vector<rebuild::ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<std::string, uint32_t> cond_map_;
  std::unordered_map<std::string, rebuild::TokenInfo> token_map_;
  std::unordered_map<std::string, rebuild::FPMMInfo> fpmm_map_;

  std::unordered_map<std::string, uint32_t> new_conditions_;
  std::unordered_map<std::string, rebuild::TokenInfo> new_tokens_;
  std::unordered_map<std::string, rebuild::FPMMInfo> new_fpmms_;

  std::unordered_map<rebuild::TxCondKey, rebuild::SplitInfo> tx_split_;
  std::unordered_map<rebuild::TxCondKey, rebuild::MergeInfo> tx_merge_;
  std::unordered_map<rebuild::TxCondKey, rebuild::RedemptionInfo> tx_redemption_;
  std::unordered_map<rebuild::TxKey, rebuild::ConvertInfo> tx_convert_;
  std::unordered_map<rebuild::TxTokenKey, rebuild::OrderInfo> tx_order_;
  std::unordered_map<rebuild::TxKey, rebuild::FPMMTradeInfo> tx_fpmm_trade_;
  std::unordered_map<rebuild::TxKey, rebuild::FPMMFundingInfo> tx_fpmm_funding_;
};
