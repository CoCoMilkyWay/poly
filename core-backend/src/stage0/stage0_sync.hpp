#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "stage0_tag.hpp"

namespace asio = boost::asio;
using json = nlohmann::json;

namespace stage0 {

class StageSync {
public:
  struct Status {
    bool syncing = false;
    int64_t last_block = 0;
    int64_t head_block = 0;
    int64_t behind_blocks = 0;
    int64_t condition_count = 0;
    int64_t ctf_condition_count = 0;
    int64_t negrisk_condition_count = 0;
    int64_t nonpoly_condition_count = 0;
    double blocks_per_second = 0.0;
    double eta_seconds = -1.0;
    // Tag status
    int64_t tag_last_block = 0;
    int64_t tagged_count = 0;
    int64_t untagged_count = 0;
  };

  StageSync(const Config &config, Database &stage1_db, Database &stage0_db, int base_interval_seconds);

  void start(asio::io_context &ioc);
  void stop();
  Status status() const;
  void reset_tag_progress();

private:
  static constexpr int kWorkerCount = 50;
  static constexpr int kSchedulerSleepMs = 5;
  static constexpr int kSchedulerSleepMaxMs = 40;
  static constexpr size_t kEtaWindowSize = 20;

  struct ConditionSeed {
    std::string condition_blob;
    std::string condition_hex_lower;
    int64_t first_seen_block = 0;
  };

  struct FetchResult {
    ConditionSeed seed;
    json market;
  };

  struct BlockTaskResult {
    int64_t block = 0;
    bool has_seeds = false;
    std::vector<FetchResult> rows;
    std::vector<ConditionSeed> empty_seeds;
  };

  struct SeedScanBatch {
    int64_t scanned_to_block = 0;
    std::map<int64_t, std::vector<ConditionSeed>> seeds_by_block;
  };

  struct CommitRecord {
    std::chrono::steady_clock::time_point committed_at;
    int64_t cursor = 0;
  };

  struct InFlightTask {
    int worker_slot = -1;
    int64_t block = -1;
    std::chrono::steady_clock::time_point started_at;
    std::vector<ConditionSeed> seeds;
    size_t next_seed_index = 0;
    size_t failed_seeds = 0;
    size_t empty_seeds = 0;
    std::vector<FetchResult> rows;
    std::vector<ConditionSeed> empty_rows;
    std::vector<std::string> debug_logs;
    bool done = false;
  };

  void schedule_sync(int delay_seconds);
  void do_sync();
  void refresh_status_locked(int64_t head_block, int64_t cursor, bool syncing);
  void record_commit_locked(int64_t cursor);

  void init_schema();
  void load_known_conditions();
  void ensure_cursor_floor();

  SeedScanBatch load_seed_scan_batch(int64_t start_block, int64_t head_block, size_t max_conditions) const;
  void persist_results_in_txn(duckdb::Appender &ap, const std::vector<FetchResult> &rows);
  int64_t get_scan_cursor();
  void set_scan_cursor_in_txn(duckdb::Connection &conn, int64_t block);

  // Tag methods
  void init_tagger();
  int64_t do_tag_sync();
  int64_t get_tag_cursor();
  void set_tag_cursor_in_txn(duckdb::Connection &conn, int64_t cursor);
  void load_tag_counts();

  const Config &config_;
  Database &stage1_db_;
  Database &stage0_db_;
  asio::io_context *ioc_ = nullptr;
  int base_interval_seconds_ = 0;
  std::atomic<bool> stop_requested_{false};
  std::unordered_set<std::string> known_condition_ids_;
  int64_t known_ctf_condition_count_ = 0;
  int64_t known_negrisk_condition_count_ = 0;
  int64_t known_nonpoly_condition_count_ = 0;
  bool runtime_scan_cursor_inited_ = false;
  int64_t runtime_scan_cursor_ = -1;
  mutable std::mutex status_mutex_;
  Status sync_;
  std::deque<CommitRecord> commit_history_;

  // Tagger
  std::unique_ptr<Tagger> tagger_;
  int64_t tag_last_block_ = 0;
  int64_t tagged_count_ = 0;
  int64_t untagged_count_ = 0;
};

} // namespace stage0
