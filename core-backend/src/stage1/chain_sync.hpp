#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

struct Status {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
  double bytes_per_block = 0.0;
};

class StageSync {
public:
  StageSync(const Config &config, Database &db);
  ~StageSync();

  void start(asio::io_context &ioc);
  void stop();

  Status status() const;

private:
  static constexpr int64_t kFinalityDepthBlocks = 100000;  // 远离区块链顶端(数据完整性)
  static constexpr int64_t kChunkTransferTarget = 5000000; // 控制单个落盘文件大小
  static constexpr int64_t kCommitBlockGranularity = 100;
  static constexpr int kRetryDelayMs = 300;
  static constexpr int kRetryDelayMaxMs = 1000;
  static constexpr int kSchedulerSleepMs = 5;
  static constexpr int kSchedulerSleepMaxMs = 40;

  struct InFlightTask {
    int64_t from_block = 0;
    int64_t to_block = 0;
    int worker_idx = 0;
    bool query_in_flight = false;
    bool decode_in_flight = false;
    bool done = false;
    std::future<RpcClient::BatchResult> query_future;
    std::future<DecodedEvents> decode_future;
    size_t response_bytes = 0;
    std::chrono::steady_clock::time_point retry_at = std::chrono::steady_clock::now();
    int retry_count = 0;
  };

  struct BufferedBatch {
    int64_t from_block = 0;
    int64_t to_block = 0;
    size_t response_bytes = 0;
    int64_t transfer_rows = 0;
    DecodedEvents events;
  };

  static const std::vector<std::string> &ct_topics();
  static const std::vector<std::string> &ex_topics();
  static const std::vector<std::string> &nra_topics();
  static const std::vector<std::string> &fpmm_topics();

  std::vector<RpcClient::LogsQuery> build_queries(int64_t from_block, int64_t to_block);
  void schedule_sync(int delay_seconds);
  void do_sync();
  void sync_loop(int64_t safe_head);
  void render_progress_inline(int64_t safe_head, size_t inflight_count) const;
  void clear_progress_inline();
  void submit_rpc_task(InFlightTask &task);
  void promote_ready_batches();
  bool find_committable_prefix(size_t &consume_batches, int64_t &commit_end_block) const;
  bool try_commit_ready_chunks();
  std::future<DecodedEvents> submit_decode_task(std::shared_ptr<std::vector<json>> shared_raw_logs);
  void start_decode_pool();
  void stop_decode_pool();
  static int64_t transfer_row_count(const DecodedEvents &events);

  const Config &config_;
  Database &db_;
  FeatherWriter feather_writer_;
  RpcClient rpc_head_;
  int num_rpc_threads_;
  int64_t rpc_block_span_ = 0;
  int64_t buffer_low_water_transfers_ = kChunkTransferTarget;
  int64_t buffer_high_water_transfers_ = kChunkTransferTarget;
  bool rpc_paused_ = false;
  std::vector<std::unique_ptr<RpcClient>> rpc_workers_;
  int num_decode_threads_ = 0;
  struct DecodeTask {
    std::shared_ptr<std::vector<json>> shared_raw_logs;
    std::shared_ptr<std::promise<DecodedEvents>> promise;
  };
  std::vector<std::thread> decode_workers_;
  std::deque<DecodeTask> decode_queue_;
  std::mutex decode_mutex_;
  std::condition_variable decode_cv_;
  bool decode_running_ = false;
  asio::io_context *ioc_ = nullptr;

  int interval_seconds_ = 30;
  std::atomic<bool> is_syncing_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> db_write_in_progress_{false};
  std::atomic<int64_t> head_block_{0};
  mutable bool progress_line_active_ = false;
  mutable size_t progress_line_len_ = 0;

  int64_t current_chunk_start_block_ = 0;
  int64_t next_query_block_ = 0;
  int64_t next_buffer_block_ = 0;
  size_t rr_worker_ = 0;
  int64_t buffered_transfer_rows_ = 0;
  int64_t ready_transfer_rows_ = 0;
  std::deque<BufferedBatch> buffered_batches_;
  std::map<int64_t, BufferedBatch> ready_batches_;

  struct ChunkRecord {
    int64_t to_block;
    size_t body_bytes;
    int64_t block_count;
  };
  struct CommitRecord {
    std::chrono::steady_clock::time_point committed_at;
    int64_t committed_block = 0;
  };
  static constexpr size_t kEtaWindowSize = 20;
  void record_commit_event(int64_t committed_block);
  std::deque<ChunkRecord> chunk_history_;
  std::deque<CommitRecord> commit_history_;
};

} // namespace stage1
