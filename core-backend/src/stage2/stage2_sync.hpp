#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <optional>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "../core/database.hpp"
#include "stage2_builder.hpp"

namespace asio = boost::asio;
using json = nlohmann::json;

namespace stage2 {

class StageSync {
public:
  struct Status {
    bool syncing = false;
    int64_t last_block = 0;
    int64_t head_block = 0;
    int64_t behind_blocks = 0;
    int64_t behind_chunks = 0;
    double blocks_per_second = 0.0;
    double eta_seconds = -1.0;
  };

  StageSync(Database &stage1_db, Database &stage2_db, int base_interval);
  void flush_restore_cache_snapshot();

  void start(asio::io_context &ioc);

  void stop();

  const Status &status() const;
  json memory_breakdown() const;

  EventBuilder &builder();

private:
  static constexpr size_t kEtaWindowSize = 20;
  struct CommitRecord {
    std::chrono::steady_clock::time_point committed_at;
    int64_t cursor = 0;
  };
  struct ChunkBoundary {
    int64_t start = 0;
    int64_t end = 0;
  };

  void schedule_sync(int delay_seconds);

  void do_sync();
  static std::optional<ChunkBoundary> next_transfer_chunk(const std::vector<Database::FeatherChunk> &chunks,
                                                          int64_t cursor, int64_t head_block);
  static int64_t pending_transfer_chunks(const std::vector<Database::FeatherChunk> &chunks,
                                         int64_t cursor, int64_t head_block);

  Database &stage1_db_;
  Database &stage2_db_;
  EventBuilder builder_;
  asio::io_context *ioc_ = nullptr;
  int base_interval_;
  Status sync_;
  std::atomic<bool> stop_requested_{false};
  std::deque<CommitRecord> commit_history_;
};

} // namespace stage2
