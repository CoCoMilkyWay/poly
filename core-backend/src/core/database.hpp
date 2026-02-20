#pragma once

#include <cassert>
#include <duckdb.hpp>
#include <fcntl.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/file.h>
#include <unistd.h>

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

  duckdb::DuckDB &get_duckdb() { return *db_; }

  void init_schema() {
    execute(R"(
      CREATE TABLE IF NOT EXISTS sync_state (
        key TEXT PRIMARY KEY,
        value TEXT
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS order_filled (
        block_number BIGINT NOT NULL,
        log_index INTEGER NOT NULL,
        exchange TEXT NOT NULL,
        maker BLOB NOT NULL,
        taker BLOB NOT NULL,
        token_id BLOB NOT NULL,
        side INTEGER NOT NULL,
        usdc_amount BIGINT NOT NULL,
        token_amount BIGINT NOT NULL,
        fee BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS split (
        block_number BIGINT NOT NULL,
        log_index INTEGER NOT NULL,
        stakeholder BLOB NOT NULL,
        condition_id BLOB NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS merge (
        block_number BIGINT NOT NULL,
        log_index INTEGER NOT NULL,
        stakeholder BLOB NOT NULL,
        condition_id BLOB NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS redemption (
        block_number BIGINT NOT NULL,
        log_index INTEGER NOT NULL,
        redeemer BLOB NOT NULL,
        condition_id BLOB NOT NULL,
        index_sets INTEGER NOT NULL,
        payout BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS convert (
        block_number BIGINT NOT NULL,
        log_index INTEGER NOT NULL,
        stakeholder BLOB NOT NULL,
        market_id BLOB NOT NULL,
        index_set BIGINT NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS transfer (
        block_number BIGINT NOT NULL,
        log_index BIGINT NOT NULL,
        from_addr BLOB NOT NULL,
        to_addr BLOB NOT NULL,
        token_id BLOB NOT NULL,
        amount BIGINT NOT NULL,
        PRIMARY KEY (block_number, log_index)
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS token_map (
        token_id BLOB PRIMARY KEY,
        condition_id BLOB NOT NULL,
        exchange TEXT NOT NULL,
        is_yes INTEGER NOT NULL
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS condition (
        condition_id BLOB PRIMARY KEY,
        oracle BLOB NOT NULL,
        question_id BLOB NOT NULL,
        payout_numerators TEXT,
        resolution_block BIGINT
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS neg_risk_market (
        market_id BLOB PRIMARY KEY,
        oracle BLOB NOT NULL,
        fee_bips INTEGER NOT NULL,
        data BLOB
      )
    )");

    execute(R"(
      CREATE TABLE IF NOT EXISTS neg_risk_question (
        question_id BLOB PRIMARY KEY,
        market_id BLOB NOT NULL,
        question_index INTEGER NOT NULL,
        data BLOB
      )
    )");

    execute("CREATE INDEX IF NOT EXISTS idx_order_filled_maker ON order_filled(maker)");
    execute("CREATE INDEX IF NOT EXISTS idx_order_filled_taker ON order_filled(taker)");
    execute("CREATE INDEX IF NOT EXISTS idx_order_filled_token ON order_filled(token_id)");
    execute("CREATE INDEX IF NOT EXISTS idx_split_stakeholder ON split(stakeholder)");
    execute("CREATE INDEX IF NOT EXISTS idx_merge_stakeholder ON merge(stakeholder)");
    execute("CREATE INDEX IF NOT EXISTS idx_redemption_redeemer ON redemption(redeemer)");
    execute("CREATE INDEX IF NOT EXISTS idx_convert_stakeholder ON convert(stakeholder)");
    execute("CREATE INDEX IF NOT EXISTS idx_transfer_from ON transfer(from_addr)");
    execute("CREATE INDEX IF NOT EXISTS idx_transfer_to ON transfer(to_addr)");
    execute("CREATE INDEX IF NOT EXISTS idx_neg_risk_question_market ON neg_risk_question(market_id)");
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

  void atomic_insert_batch(const std::string &table, const std::string &columns,
                           const std::vector<std::string> &values_list,
                           int64_t new_last_block) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    auto r1 = write_conn_->Query("BEGIN TRANSACTION");
    assert(!r1->HasError());

    if (!values_list.empty()) {
      std::string insert_sql = "INSERT OR IGNORE INTO " + table + " (" + columns + ") VALUES ";
      for (size_t i = 0; i < values_list.size(); ++i) {
        if (i > 0)
          insert_sql += ", ";
        insert_sql += "(" + values_list[i] + ")";
      }
      auto r2 = write_conn_->Query(insert_sql);
      assert(!r2->HasError());
    }

    auto r3 = write_conn_->Query(
        "INSERT OR REPLACE INTO sync_state (key, value) VALUES ('last_block', '" +
        std::to_string(new_last_block) + "')");
    assert(!r3->HasError());

    auto r4 = write_conn_->Query("COMMIT");
    assert(!r4->HasError());
  }

  void atomic_multi_insert(
      const std::vector<std::tuple<std::string, std::string, std::vector<std::string>>> &batches,
      int64_t new_last_block,
      const std::vector<std::string> &extra_sqls = {}) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    auto r1 = write_conn_->Query("BEGIN TRANSACTION");
    assert(!r1->HasError());

    for (const auto &[table, columns, values_list] : batches) {
      if (values_list.empty())
        continue;
      std::string insert_sql = "INSERT OR IGNORE INTO " + table + " (" + columns + ") VALUES ";
      for (size_t i = 0; i < values_list.size(); ++i) {
        if (i > 0)
          insert_sql += ", ";
        insert_sql += "(" + values_list[i] + ")";
      }
      auto r = write_conn_->Query(insert_sql);
      assert(!r->HasError());
    }

    for (const auto &sql : extra_sqls) {
      auto r = write_conn_->Query(sql);
      assert(!r->HasError());
    }

    auto r3 = write_conn_->Query(
        "INSERT OR REPLACE INTO sync_state (key, value) VALUES ('last_block', '" +
        std::to_string(new_last_block) + "')");
    assert(!r3->HasError());

    auto r4 = write_conn_->Query("COMMIT");
    assert(!r4->HasError());
  }

private:
  std::string db_path_;
  std::string lock_path_;
  int lock_fd_ = -1;
  bool has_write_lock_ = false;

  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> read_conn_;
  std::unique_ptr<duckdb::Connection> write_conn_;
  std::mutex read_mutex_;
  std::mutex write_mutex_;
};
