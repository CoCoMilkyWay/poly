#pragma once

#include <atomic>

#include <boost/asio.hpp>

#include "../core/database.hpp"
#include "../core/feather_writer.hpp"
#include "stage2_builder.hpp"

namespace asio = boost::asio;

namespace stage2 {

struct SyncProgress {
  bool syncing = false;
  int64_t stage1_last_block = 0;
  int64_t stage2_cursor = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  int64_t chunks_per_rebuild = 0;
  int phase = 0;
};

class EventSync {
public:
  EventSync(Database &stage1_db, Database &stage2_db, int base_interval = 30);

  void start(asio::io_context &ioc);

  void stop();

  const SyncProgress &progress() const;
  const BuildProgress &build_progress() const;

  EventBuilder &builder();

private:
  static constexpr int kStage2ChunkBlocks = FeatherWriter::PARTITION_SIZE;

  void schedule_sync(int delay_seconds);

  void do_sync();

  Database &stage1_db_;
  Database &stage2_db_;
  EventBuilder builder_;
  asio::io_context *ioc_ = nullptr;
  int chunk_size_;
  int base_interval_;
  SyncProgress progress_;
  std::atomic<bool> stop_requested_{false};
};

} // namespace stage2
