#pragma once

#include <cassert>
#include <duckdb.hpp>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/file.h>
#include <unistd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

class Database {
public:
  explicit Database(const std::string &path) : db_path_(path) {
    auto parent = fs::path(path).parent_path();
    data_dir_ = parent.empty() ? "." : parent.string();
    duckdb::DBConfig config;
    config.SetOption("checkpoint_threshold", duckdb::Value("256MB"));
    config.SetOption("wal_autocheckpoint", duckdb::Value("256MB"));
    db_ = std::make_unique<duckdb::DuckDB>(path, &config);
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
    if (result->HasError()) {
      std::cerr << "[DB] execute failed: " << result->GetError() << std::endl;
      std::cerr << "[DB] SQL: " << sql << std::endl;
    }
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
          case duckdb::LogicalTypeId::BLOB: {
            auto blob = duckdb::StringValue::Get(value);
            std::string hex = "0x";
            hex.reserve(2 + blob.size() * 2);
            static const char hex_chars[] = "0123456789abcdef";
            for (unsigned char c : blob) {
              hex.push_back(hex_chars[c >> 4]);
              hex.push_back(hex_chars[c & 0x0f]);
            }
            obj[names[col]] = hex;
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

  duckdb::DuckDB &get_duckdb() { return *db_; }

  std::unique_ptr<duckdb::Connection> create_connection() {
    return std::make_unique<duckdb::Connection>(*db_);
  }

  void checkpoint() {
    std::lock_guard<std::mutex> lock(write_mutex_);
    auto result = write_conn_->Query("CHECKPOINT");
    assert(!result->HasError());
  }

  void init_schema() {
    execute("INSTALL nanoarrow FROM community");
    execute("LOAD nanoarrow");
    execute(R"(
      CREATE TABLE IF NOT EXISTS sync_state (
        key TEXT PRIMARY KEY,
        value TEXT
      )
    )");
    refresh_feather_views();
  }

  std::string feather_path(const std::string &table) const {
    return data_dir_ + "/stage1/" + table + ".feather";
  }

  std::string feather_table(const std::string &table) const {
    return "read_arrow('" + feather_path(table) + "')";
  }

  void refresh_feather_views() {
    static const char *feather_tables[] = {
        "transfer", "condition_preparation", "condition_resolution",
        "split", "merge", "redemption", "fpmm", "fpmm_trade", "fpmm_funding",
        "order_filled", "token_map", "neg_risk_market", "neg_risk_question", "convert"};
    for (const char *name : feather_tables) {
      std::string path = feather_path(name);
      if (fs::exists(path)) {
        execute("CREATE OR REPLACE VIEW " + std::string(name) + " AS SELECT * FROM read_arrow('" + path + "')");
      }
    }
  }

  const std::string &data_dir() const { return data_dir_; }

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

private:
  // 路径
  std::string data_dir_;
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
