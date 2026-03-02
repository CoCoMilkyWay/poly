#pragma once

#include <cassert>
#include <duckdb.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

class Database {
public:
  struct FeatherChunk {
    int64_t start_block = 0;
    int64_t end_block = 0;
  };

  explicit Database(const std::string &path);
  ~Database();

  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  void acquire_write_lock();
  void release_write_lock();
  bool try_write_lock();

  class WriteLock {
  public:
    explicit WriteLock(Database &db);
    ~WriteLock();
    WriteLock(const WriteLock &) = delete;
    WriteLock &operator=(const WriteLock &) = delete;

  private:
    Database &db_;
  };

  void execute(const std::string &sql);
  void execute_read(const std::string &sql);
  json query_json(const std::string &sql);
  int64_t query_single_int(const std::string &sql);
  json get_tables();
  int64_t get_table_count(const std::string &table);
  duckdb::DuckDB &get_duckdb();
  std::unique_ptr<duckdb::Connection> create_connection();
  void checkpoint();
  void init_schema();
  void cleanup_incomplete_partitions();
  std::string feather_dir(const std::string &table) const;
  std::string feather_table_range(const std::string &table, int64_t start_block, int64_t end_block);
  std::vector<FeatherChunk> feather_chunks(const std::string &table) const;
  const std::string &data_dir() const;
  int64_t get_last_block();
  void set_last_block(int64_t block);
  json load_counts_cache();
  void save_counts_cache(const json &cache);

private:
  std::vector<FeatherChunk> list_chunks(const std::string &table) const;
  std::string state_path() const;
  json read_state_unlocked() const;
  void write_state_unlocked(const json &state) const;

  std::string data_dir_;
  std::string db_path_;
  std::string lock_path_;
  int lock_fd_ = -1;
  bool has_write_lock_ = false;
  std::unique_ptr<duckdb::DuckDB> db_;
  std::unique_ptr<duckdb::Connection> read_conn_;
  std::unique_ptr<duckdb::Connection> write_conn_;
  std::mutex read_mutex_;
  std::mutex write_mutex_;
  mutable std::mutex state_mutex_;
};
