#pragma once

#include <cassert>
#include <duckdb.hpp>
#include <fcntl.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/file.h>
#include <unistd.h>

#include "../stage1/event_decode.hpp"

using json = nlohmann::json;

class Database {
public:
  explicit Database(const std::string &path) : db_path_(path) {
    db_ = std::make_unique<duckdb::DuckDB>(path);
    read_conn_ = std::make_unique<duckdb::Connection>(*db_);
    write_conn_ = std::make_unique<duckdb::Connection>(*db_);

    lock_path_ = path + ".lock";
    lock_fd_ = open(lock_path_.c_str(), O_CREAT | O_RDWR, 0666);
    assert(lock_fd_ >= 0 && "无法创建锁文件");
  }

  ~Database() {
    if (has_write_lock_) {
      release_write_lock();
    }
    if (lock_fd_ >= 0) {
      close(lock_fd_);
    }
  }

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  void acquire_write_lock() {
    assert(!has_write_lock_ && "已持有写锁");
    int ret = flock(lock_fd_, LOCK_EX);
    assert(ret == 0 && "获取写锁失败");
    has_write_lock_ = true;
  }

  void release_write_lock() {
    assert(has_write_lock_ && "未持有写锁");
    int ret = flock(lock_fd_, LOCK_UN);
    assert(ret == 0 && "释放写锁失败");
    has_write_lock_ = false;
  }

  bool try_write_lock() {
    if (has_write_lock_)
      return true;
    int ret = flock(lock_fd_, LOCK_EX | LOCK_NB);
    if (ret == 0) {
      has_write_lock_ = true;
      return true;
    }
    return false;
  }

  class WriteLock {
  public:
    explicit WriteLock(Database &db) : db_(db) { db_.acquire_write_lock(); }
    ~WriteLock() { db_.release_write_lock(); }
    WriteLock(const WriteLock &) = delete;
    WriteLock &operator=(const WriteLock &) = delete;

  private:
    Database &db_;
  };

  void execute(const std::string &sql) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto result = write_conn_->Query(sql);
    assert(!result->HasError() && "execute failed");
  }

  json query_json(const std::string &sql) {
    std::lock_guard<std::mutex> lock(read_mutex_);
    auto result = read_conn_->Query(sql);
    if (result->HasError()) {
      std::cerr << "[DB] query_json failed: " << result->GetError() << std::endl;
      std::cerr << "[DB] SQL: " << sql << std::endl;
    }
    assert(!result->HasError() && "query_json failed");

    json rows = json::array();
    auto &types = result->types;
    auto names = result->names;

    for (size_t row = 0; row < result->RowCount(); ++row) {
      json obj = json::object();
      for (size_t col = 0; col < result->ColumnCount(); ++col) {
        auto value = result->GetValue(col, row);
        if (value.IsNull()) {
          obj[names[col]] = nullptr;
        } else {
          switch (types[col].id()) {
          case duckdb::LogicalTypeId::BOOLEAN:
            obj[names[col]] = value.GetValue<bool>();
            break;
          case duckdb::LogicalTypeId::TINYINT:
          case duckdb::LogicalTypeId::SMALLINT:
          case duckdb::LogicalTypeId::INTEGER:
            obj[names[col]] = value.GetValue<int32_t>();
            break;
          case duckdb::LogicalTypeId::BIGINT:
            obj[names[col]] = value.GetValue<int64_t>();
            break;
          case duckdb::LogicalTypeId::FLOAT:
          case duckdb::LogicalTypeId::DOUBLE:
            obj[names[col]] = value.GetValue<double>();
            break;
          case duckdb::LogicalTypeId::BLOB: {
            auto blob = duckdb::StringValue::Get(value);
            if (!blob.empty() && blob[0] == 'x') {
              obj[names[col]] = "0" + blob;
            } else {
              obj[names[col]] = "0x" + blob;
            }
            break;
          }
          default:
            obj[names[col]] = value.ToString();
            break;
          }
        }
      }
      rows.push_back(std::move(obj));
    }
    return rows;
  }

  int64_t query_single_int(const std::string &sql) {
    std::lock_guard<std::mutex> lock(read_mutex_);
    auto result = read_conn_->Query(sql);
    if (result->HasError() || result->RowCount() == 0)
      return 0;
    auto val = result->GetValue(0, 0);
    return val.IsNull() ? 0 : val.GetValue<int64_t>();
  }

  json get_tables() {
    return query_json(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema='main' ORDER BY table_name");
  }

  int64_t get_table_count(const std::string &table) {
    return query_single_int("SELECT COUNT(*) FROM " + table);
  }

  bool table_exists(const std::string &table) {
    return query_single_int(
               "SELECT COUNT(*) FROM information_schema.tables "
               "WHERE table_schema='main' AND table_name='" +
               table + "'") > 0;
  }

  duckdb::DuckDB &get_duckdb() { return *db_; }

  std::unique_ptr<duckdb::Connection> create_connection() {
    return std::make_unique<duckdb::Connection>(*db_);
  }

  void init_schema() {
    execute(R"(
      CREATE TABLE IF NOT EXISTS sync_state (
        key TEXT PRIMARY KEY,
        value TEXT
      )
    )");

    // ConditionalTokens: 转账
    execute(R"(
      CREATE TABLE IF NOT EXISTS transfer (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index BIGINT NOT NULL,
        operator BLOB NOT NULL,
        from_addr BLOB NOT NULL,
        to_addr BLOB NOT NULL,
        token_id BLOB NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    // ConditionalTokens: 条件
    execute(R"(
      CREATE TABLE IF NOT EXISTS condition_preparation (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        condition_id BLOB PRIMARY KEY,
        oracle BLOB NOT NULL,
        question_id BLOB NOT NULL,
        outcome_slot_count INTEGER NOT NULL
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS condition_resolution (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        condition_id BLOB NOT NULL,
        oracle BLOB NOT NULL,
        question_id BLOB NOT NULL,
        outcome_slot_count INTEGER NOT NULL,
        payout_numerators TEXT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    // ConditionalTokens: 持仓操作
    execute(R"(
      CREATE TABLE IF NOT EXISTS split (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        stakeholder BLOB NOT NULL,
        collateral_token BLOB NOT NULL,
        parent_collection_id BLOB NOT NULL,
        condition_id BLOB NOT NULL,
        partition TEXT NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS merge (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        stakeholder BLOB NOT NULL,
        collateral_token BLOB NOT NULL,
        parent_collection_id BLOB NOT NULL,
        condition_id BLOB NOT NULL,
        partition TEXT NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS redemption (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        redeemer BLOB NOT NULL,
        collateral_token BLOB NOT NULL,
        parent_collection_id BLOB NOT NULL,
        condition_id BLOB NOT NULL,
        index_sets TEXT NOT NULL,
        payout BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    // FPMM: AMM池
    execute(R"(
      CREATE TABLE IF NOT EXISTS fpmm (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        creator BLOB NOT NULL,
        fpmm_addr BLOB PRIMARY KEY,
        conditional_tokens BLOB NOT NULL,
        collateral_token BLOB NOT NULL,
        condition_ids TEXT NOT NULL,
        fee BIGINT NOT NULL
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS fpmm_trade (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        fpmm_addr BLOB NOT NULL,
        trader BLOB NOT NULL,
        side INTEGER NOT NULL,
        outcome_index INTEGER NOT NULL,
        usdc_amount BIGINT NOT NULL,
        token_amount BIGINT NOT NULL,
        fee BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS fpmm_funding (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        fpmm_addr BLOB NOT NULL,
        funder BLOB NOT NULL,
        side INTEGER NOT NULL,
        amounts TEXT NOT NULL,
        collateral_from_fee_pool BIGINT NOT NULL,
        shares BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    // CTFExchange: 订单
    execute(R"(
      CREATE TABLE IF NOT EXISTS order_filled (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        exchange TEXT NOT NULL,
        order_hash BLOB NOT NULL,
        maker BLOB NOT NULL,
        taker BLOB NOT NULL,
        maker_asset_id BLOB NOT NULL,
        taker_asset_id BLOB NOT NULL,
        maker_amount BIGINT NOT NULL,
        taker_amount BIGINT NOT NULL,
        fee BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS token_map (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        exchange TEXT NOT NULL,
        token0 BLOB NOT NULL,
        token1 BLOB NOT NULL,
        condition_id BLOB NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    // NegRiskAdapter: 市场
    execute(R"(
      CREATE TABLE IF NOT EXISTS neg_risk_market (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        market_id BLOB PRIMARY KEY,
        oracle BLOB NOT NULL,
        fee_bips INTEGER NOT NULL,
        data BLOB
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS neg_risk_question (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        market_id BLOB NOT NULL,
        question_id BLOB PRIMARY KEY,
        question_index INTEGER NOT NULL,
        data BLOB
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS convert (
        block_number BIGINT NOT NULL,
        tx_hash BLOB NOT NULL,
        log_index INTEGER NOT NULL,
        stakeholder BLOB NOT NULL,
        market_id BLOB NOT NULL,
        index_set BIGINT NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    // ConditionalTokens 索引
    execute("CREATE INDEX IF NOT EXISTS idx_transfer_from ON transfer(from_addr)");
    execute("CREATE INDEX IF NOT EXISTS idx_transfer_to ON transfer(to_addr)");
    execute("CREATE INDEX IF NOT EXISTS idx_transfer_operator ON transfer(operator)");
    execute("CREATE INDEX IF NOT EXISTS idx_condition_resolution_condition_id ON condition_resolution(condition_id)");
    execute("CREATE INDEX IF NOT EXISTS idx_split_stakeholder ON split(stakeholder)");
    execute("CREATE INDEX IF NOT EXISTS idx_merge_stakeholder ON merge(stakeholder)");
    execute("CREATE INDEX IF NOT EXISTS idx_redemption_redeemer ON redemption(redeemer)");
    // FPMM 索引
    execute("CREATE INDEX IF NOT EXISTS idx_fpmm_trade_trader ON fpmm_trade(trader)");
    execute("CREATE INDEX IF NOT EXISTS idx_fpmm_trade_fpmm ON fpmm_trade(fpmm_addr)");
    // CTFExchange 索引
    execute("CREATE INDEX IF NOT EXISTS idx_order_filled_maker ON order_filled(maker)");
    execute("CREATE INDEX IF NOT EXISTS idx_order_filled_taker ON order_filled(taker)");
    // NegRiskAdapter 索引
    execute("CREATE INDEX IF NOT EXISTS idx_neg_risk_question_market ON neg_risk_question(market_id)");
    execute("CREATE INDEX IF NOT EXISTS idx_convert_stakeholder ON convert(stakeholder)");
  }

  int64_t get_last_block() {
    auto rows = query_json("SELECT value FROM sync_state WHERE key='last_block'");
    if (rows.empty() || rows[0]["value"].is_null())
      return -1;
    return std::stoll(rows[0]["value"].get<std::string>());
  }

  void set_last_block(int64_t block) {
    execute("INSERT OR REPLACE INTO sync_state (key, value) VALUES ('last_block', '" +
            std::to_string(block) + "')");
  }

  void atomic_multi_insert_appender(const stage1::DecodedEvents &events, int64_t new_last_block) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    auto r1 = write_conn_->Query("BEGIN TRANSACTION");
    assert(!r1->HasError());

    if (!events.transfer.empty()) {
      duckdb::Appender app(*write_conn_, "transfer");
      for (const auto &e : events.transfer) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.op));
        app.Append(make_blob(e.from));
        app.Append(make_blob(e.to));
        app.Append(make_blob(e.token_id));
        app.Append<int64_t>(e.amount);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.condition_preparation.empty()) {
      duckdb::Appender app(*write_conn_, "condition_preparation");
      for (const auto &e : events.condition_preparation) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.condition_id));
        app.Append(make_blob(e.oracle));
        app.Append(make_blob(e.question_id));
        app.Append<int64_t>(e.outcome_slot_count);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.condition_resolution.empty()) {
      duckdb::Appender app(*write_conn_, "condition_resolution");
      for (const auto &e : events.condition_resolution) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.condition_id));
        app.Append(make_blob(e.oracle));
        app.Append(make_blob(e.question_id));
        app.Append<int64_t>(e.outcome_slot_count);
        app.Append(duckdb::Value(e.payout_numerators));
        app.EndRow();
      }
      app.Close();
    }

    if (!events.split.empty()) {
      duckdb::Appender app(*write_conn_, "split");
      for (const auto &e : events.split) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.stakeholder));
        app.Append(make_blob(e.collateral_token));
        app.Append(make_blob(e.parent_collection_id));
        app.Append(make_blob(e.condition_id));
        app.Append(duckdb::Value(e.partition));
        app.Append<int64_t>(e.amount);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.merge.empty()) {
      duckdb::Appender app(*write_conn_, "merge");
      for (const auto &e : events.merge) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.stakeholder));
        app.Append(make_blob(e.collateral_token));
        app.Append(make_blob(e.parent_collection_id));
        app.Append(make_blob(e.condition_id));
        app.Append(duckdb::Value(e.partition));
        app.Append<int64_t>(e.amount);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.redemption.empty()) {
      duckdb::Appender app(*write_conn_, "redemption");
      for (const auto &e : events.redemption) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.redeemer));
        app.Append(make_blob(e.collateral_token));
        app.Append(make_blob(e.parent_collection_id));
        app.Append(make_blob(e.condition_id));
        app.Append(duckdb::Value(e.index_sets));
        app.Append<int64_t>(e.payout);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.fpmm.empty()) {
      duckdb::Appender app(*write_conn_, "fpmm");
      for (const auto &e : events.fpmm) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.creator));
        app.Append(make_blob(e.fpmm_addr));
        app.Append(make_blob(e.conditional_tokens));
        app.Append(make_blob(e.collateral_token));
        app.Append(duckdb::Value(e.condition_ids));
        app.Append<int64_t>(e.fee);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.fpmm_trade.empty()) {
      duckdb::Appender app(*write_conn_, "fpmm_trade");
      for (const auto &e : events.fpmm_trade) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.fpmm_addr));
        app.Append(make_blob(e.trader));
        app.Append<int64_t>(e.side);
        app.Append<int64_t>(e.outcome_index);
        app.Append<int64_t>(e.usdc_amount);
        app.Append<int64_t>(e.token_amount);
        app.Append<int64_t>(e.fee);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.fpmm_funding.empty()) {
      duckdb::Appender app(*write_conn_, "fpmm_funding");
      for (const auto &e : events.fpmm_funding) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.fpmm_addr));
        app.Append(make_blob(e.funder));
        app.Append<int64_t>(e.side);
        app.Append(duckdb::Value(e.amounts));
        app.Append<int64_t>(e.collateral_from_fee_pool);
        app.Append<int64_t>(e.shares);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.order_filled.empty()) {
      duckdb::Appender app(*write_conn_, "order_filled");
      for (const auto &e : events.order_filled) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(duckdb::Value(e.exchange));
        app.Append(make_blob(e.order_hash));
        app.Append(make_blob(e.maker));
        app.Append(make_blob(e.taker));
        app.Append(make_blob(e.maker_asset_id));
        app.Append(make_blob(e.taker_asset_id));
        app.Append<int64_t>(e.maker_amount);
        app.Append<int64_t>(e.taker_amount);
        app.Append<int64_t>(e.fee);
        app.EndRow();
      }
      app.Close();
    }

    if (!events.token_map.empty()) {
      duckdb::Appender app(*write_conn_, "token_map");
      for (const auto &e : events.token_map) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(duckdb::Value(e.exchange));
        app.Append(make_blob(e.token0));
        app.Append(make_blob(e.token1));
        app.Append(make_blob(e.condition_id));
        app.EndRow();
      }
      app.Close();
    }

    if (!events.neg_risk_market.empty()) {
      duckdb::Appender app(*write_conn_, "neg_risk_market");
      for (const auto &e : events.neg_risk_market) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.market_id));
        app.Append(make_blob(e.oracle));
        app.Append<int64_t>(e.fee_bips);
        if (e.data)
          app.Append(make_blob(*e.data));
        else
          app.Append(duckdb::Value());
        app.EndRow();
      }
      app.Close();
    }

    if (!events.neg_risk_question.empty()) {
      duckdb::Appender app(*write_conn_, "neg_risk_question");
      for (const auto &e : events.neg_risk_question) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.market_id));
        app.Append(make_blob(e.question_id));
        app.Append<int64_t>(e.question_index);
        if (e.data)
          app.Append(make_blob(*e.data));
        else
          app.Append(duckdb::Value());
        app.EndRow();
      }
      app.Close();
    }

    if (!events.convert.empty()) {
      duckdb::Appender app(*write_conn_, "convert");
      for (const auto &e : events.convert) {
        app.BeginRow();
        app.Append<int64_t>(e.block_number);
        app.Append(make_blob(e.tx_hash));
        app.Append<int64_t>(e.log_index);
        app.Append(make_blob(e.stakeholder));
        app.Append(make_blob(e.market_id));
        app.Append<int64_t>(e.index_set);
        app.Append<int64_t>(e.amount);
        app.EndRow();
      }
      app.Close();
    }

    auto r3 = write_conn_->Query(
        "INSERT OR REPLACE INTO sync_state (key, value) VALUES ('last_block', '" +
        std::to_string(new_last_block) + "')");
    assert(!r3->HasError());

    auto r4 = write_conn_->Query("COMMIT");
    assert(!r4->HasError());
  }

private:
  static duckdb::Value make_blob(const std::string &hex) {
    std::string h = hex;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X'))
      h = h.substr(2);
    std::string bytes;
    bytes.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
      unsigned char c = 0;
      for (int j = 0; j < 2; ++j) {
        char ch = h[i + j];
        c <<= 4;
        if (ch >= '0' && ch <= '9')
          c |= ch - '0';
        else if (ch >= 'a' && ch <= 'f')
          c |= ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
          c |= ch - 'A' + 10;
      }
      bytes.push_back(static_cast<char>(c));
    }
    return duckdb::Value::BLOB((duckdb::const_data_ptr_t)bytes.data(), bytes.size());
  }

  // 路径
  std::string db_path_;
  std::string lock_path_;
  // 文件锁
  int lock_fd_ = -1;
  bool has_write_lock_ = false;
  // DuckDB
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> read_conn_;
  std::unique_ptr<duckdb::Connection> write_conn_;
  std::mutex read_mutex_;
  std::mutex write_mutex_;
};
