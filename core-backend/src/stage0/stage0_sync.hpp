#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"

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
    double blocks_per_second = 0.0;
    double eta_seconds = -1.0;
  };

  StageSync(const Config &config, Database &stage1_db, Database &stage0_db, int base_interval_seconds = 30);

  void start(asio::io_context &ioc);
  void stop();
  Status status() const;

private:
  static constexpr int kWorkerCount = 10;
  static constexpr int kSchedulerSleepMs = 5;
  static constexpr int kSchedulerSleepMaxMs = 40;
  static constexpr size_t kEtaWindowSize = 20;
  static constexpr int64_t kSeedScanBlockSpan = 20000;

  struct ConditionSeed {
    std::string condition_blob;
    std::string question_blob;
    std::string condition_hex_lower;
    int64_t first_seen_block = 0;
  };

  struct FetchResult {
    ConditionSeed seed;
    json market;
  };

  struct BlockTaskResult {
    int64_t block = 0;
    std::vector<FetchResult> rows;
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
    std::future<BlockTaskResult> future;
  };

  void schedule_sync(int delay_seconds);
  void do_sync();
  void refresh_status_locked(int64_t head_block, int64_t cursor, bool syncing);
  void record_commit_locked(int64_t cursor);

  void init_schema();
  void load_known_conditions();
  void ensure_cursor_floor();

  SeedScanBatch load_seed_scan_batch(int64_t start_block, int64_t head_block, size_t max_conditions) const;
  json fetch_market_by_condition(const std::string &condition_hex_lower);
  BlockTaskResult process_block_with_retry(int64_t block, const std::vector<ConditionSeed> &seeds);
  void persist_results(const std::vector<FetchResult> &rows, int64_t commit_block);
  int64_t get_cursor();
  void set_cursor_in_txn(duckdb::Connection &conn, int64_t block);

  const Config &config_;
  Database &stage1_db_;
  Database &stage0_db_;
  asio::io_context *ioc_ = nullptr;
  int base_interval_seconds_ = 30;
  std::atomic<bool> stop_requested_{false};
  std::unordered_set<std::string> known_condition_ids_;
  mutable std::mutex status_mutex_;
  Status sync_;
  std::deque<CommitRecord> commit_history_;
};

} // namespace stage0
