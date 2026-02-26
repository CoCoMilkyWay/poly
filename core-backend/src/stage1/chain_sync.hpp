#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "../core/feather_writer.hpp"
#include "../infra/rpc_client.hpp"
#include "event_decode.hpp"

namespace asio = boost::asio;
using json = nlohmann::json;

namespace stage1 {

class ChainSync {
public:
  ChainSync(const Config &config, Database &db);

  void start(asio::io_context &ioc);
  void stop();

  bool is_syncing() const;
  int64_t get_head_block() const;
  double get_blocks_per_second() const;
  double get_bytes_per_block() const;

private:
  static constexpr int64_t kSyncChunkBlocks = 100000;
  static constexpr int kRetryDelayMs = 300;
  static constexpr int kRetryDelayMaxMs = 10000;
  static constexpr int kSchedulerSleepMs = 5;

  struct BasicTask {
    int64_t from_block = 0;
    int64_t to_block = 0;
    int worker_idx = 0;
    bool in_flight = false;
    bool done = false;
    std::future<RpcClient::BatchResult> future;
    std::optional<RpcClient::BatchResult> result;
    std::chrono::steady_clock::time_point retry_at = std::chrono::steady_clock::now();
    int retry_count = 0;
  };

  struct SyncChunkState {
    int sync_id = 0;
    int slot = 0;
    int64_t from_block = 0;
    int64_t to_block = 0;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::vector<BasicTask> basics;
    int done_count = 0;
  };

  static const std::vector<std::string> &ct_topics();
  static const std::vector<std::string> &ex_topics();
  static const std::vector<std::string> &nra_topics();
  static const std::vector<std::string> &fpmm_topics();

  std::vector<RpcClient::LogsQuery> build_queries(int64_t from_block, int64_t to_block);
  void schedule_sync(int delay_seconds);
  void do_sync();
  void init_done_slot(int slot, size_t nbits);
  void set_done_bit(int slot, size_t idx, uint8_t v);
  int ordered_done_in_slot(int slot);
  void render_progress_inline(const std::deque<SyncChunkState> &window);
  void clear_progress_inline();
  std::optional<SyncChunkState> build_sync_chunk(int sync_id, int slot, int64_t &cursor,
                                                 int64_t head_block, size_t &rr_worker);
  void submit_basic_task(BasicTask &task);
  void sync_loop(int64_t from_block, int64_t head_block);
  void process_batch(const RpcClient::BatchResult &r, int64_t from_block, int64_t to_block);
  static void merge_events(DecodedEvents &dst, const DecodedEvents &src);

  const Config &config_;
  Database &db_;
  FeatherWriter feather_writer_;
  RpcClient rpc_head_;
  int num_rpc_threads_;
  int64_t basic_chunk_size_;
  int sync_chunk_basic_count_;
  int super_sync_chunk_count_ = 2;
  std::vector<std::unique_ptr<RpcClient>> rpc_workers_;
  std::vector<std::vector<uint8_t>> done_list_;
  std::mutex done_list_mutex_;
  asio::io_context *ioc_ = nullptr;

  int interval_seconds_ = 30;
  std::atomic<bool> is_syncing_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<int64_t> head_block_{0};
  bool progress_line_active_ = false;
  size_t progress_line_len_ = 0;

  DecodedEvents cached_events_;
  int64_t current_partition_start_ = 0;

  struct ChunkRecord {
    int64_t to_block;
    double duration_s;
    size_t body_bytes;
    int64_t block_count;
  };
  std::deque<ChunkRecord> chunk_history_;
};

} // namespace stage1
