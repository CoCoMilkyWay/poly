#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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
#include "misc/profiler.hpp"

namespace asio = boost::asio;
using json = nlohmann::json;

namespace stage1 {

class ChainSync {
public:
  ChainSync(const Config &config, Database &db)
      : config_(config), db_(db),
        feather_writer_(db.data_dir()),
        rpc_head_(config.rpc_url, config.rpc_api_key, "RPC-Head"),
        num_rpc_threads_(config.stage1_rpc_query_threads),
        sync_chunk_basic_count_(config.stage1_rpc_sync_chunk_basics),
        basic_chunk_size_(kSyncChunkBlocks / config.stage1_rpc_sync_chunk_basics) {
    assert(num_rpc_threads_ > 0);
    assert(sync_chunk_basic_count_ > 0);
    assert(kSyncChunkBlocks % sync_chunk_basic_count_ == 0);
    assert(basic_chunk_size_ > 0);
    done_list_.resize(super_sync_chunk_count_);
    rpc_workers_.reserve(num_rpc_threads_);
    for (int i = 0; i < num_rpc_threads_; ++i) {
      rpc_workers_.push_back(std::make_unique<RpcClient>(
          config.rpc_url, config.rpc_api_key, "RPC-Worker-" + std::to_string(i)));
    }
    int64_t cursor = db_.get_last_block();
    current_partition_start_ = (cursor < 0) ? FeatherWriter::partition_start(config_.initial_block)
                                            : FeatherWriter::partition_start(cursor) + FeatherWriter::PARTITION_SIZE;
  }

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    is_syncing_ = false;
    stop_requested_ = false;
    schedule_sync(0);
  }

  void stop() { stop_requested_ = true; }

  bool is_syncing() const { return is_syncing_; }
  int64_t get_head_block() const { return head_block_; }

  double get_blocks_per_second() const {
    if (chunk_history_.empty())
      return 0.0;
    size_t n = std::min<size_t>(20, chunk_history_.size());
    double total_blocks = 0.0;
    double total_duration = 0.0;
    for (size_t i = chunk_history_.size() - n; i < chunk_history_.size(); ++i) {
      const auto &r = chunk_history_[i];
      total_blocks += static_cast<double>(r.block_count);
      total_duration += r.duration_s;
    }
    if (total_duration <= 0.0)
      return 0.0;
    return total_blocks / total_duration;
  }

  double get_bytes_per_block() const {
    if (chunk_history_.empty())
      return 0.0;
    size_t total_bytes = 0;
    int64_t total_blocks = 0;
    for (const auto &r : chunk_history_) {
      total_bytes += r.body_bytes;
      total_blocks += r.block_count;
    }
    if (total_blocks == 0)
      return 0.0;
    return static_cast<double>(total_bytes) / total_blocks;
  }

private:
  static constexpr int64_t kSyncChunkBlocks = 100000;
  static constexpr int kRetryDelayMs = 300;
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
  };

  struct SyncChunkState {
    int sync_id = 0;
    int slot = 0; // ping-pong: 0/1
    int64_t from_block = 0;
    int64_t to_block = 0;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::vector<BasicTask> basics;
    int done_count = 0;
  };

  static const std::vector<std::string> &ct_topics() {
    static const std::vector<std::string> v = {
        topics::TRANSFER_SINGLE, topics::TRANSFER_BATCH, topics::CONDITION_PREPARE,
        topics::CONDITION_RESOLVE, topics::POSITION_SPLIT, topics::POSITION_MERGE, topics::POSITION_REDEEM};
    return v;
  }
  static const std::vector<std::string> &ex_topics() {
    static const std::vector<std::string> v = {topics::ORDER_FILL, topics::TOKEN_REGISTER};
    return v;
  }
  static const std::vector<std::string> &nra_topics() {
    static const std::vector<std::string> v = {topics::MARKET_PREPARE, topics::QUESTION_PREPARE, topics::POSITION_CONVERT};
    return v;
  }
  static const std::vector<std::string> &fpmm_topics() {
    static const std::vector<std::string> v = {
        topics::FPMM_CREATE, topics::FPMM_BUY, topics::FPMM_SELL,
        topics::FPMM_FUNDING_ADD, topics::FPMM_FUNDING_REMOVE};
    return v;
  }

  std::vector<RpcClient::LogsQuery> build_queries(int64_t from_block, int64_t to_block) {
    return {{std::string(contracts::CONDITIONAL_TOKENS), from_block, to_block, ct_topics()},
            {std::string(contracts::CTF_EXCHANGE), from_block, to_block, ex_topics()},
            {std::string(contracts::NEG_RISK_CTF_EXCHANGE), from_block, to_block, ex_topics()},
            {std::string(contracts::NEG_RISK_ADAPTER), from_block, to_block, nra_topics()},
            {std::nullopt, from_block, to_block, fpmm_topics()}};
  }

  void schedule_sync(int delay_seconds) {
    if (stop_requested_)
      return;
    auto timer = std::make_shared<asio::steady_timer>(*ioc_);
    timer->expires_after(std::chrono::seconds(delay_seconds));
    timer->async_wait([this, timer](boost::system::error_code ec) {
      if (!ec && !stop_requested_) {
        do_sync();
      }
    });
  }

  void do_sync() {
    is_syncing_ = true;

    try {
      head_block_ = rpc_head_.eth_blockNumber();
    } catch (...) {
      std::cerr << "[Sync] 获取区块高度失败, " << interval_seconds_ << "s 后重试" << std::endl;
      is_syncing_ = false;
      schedule_sync(interval_seconds_);
      return;
    }
    int64_t last_block = db_.get_last_block();
    int64_t from_block = (last_block < 0) ? config_.initial_block : last_block + 1;

    std::cout << "[Sync] head=" << head_block_ << ", last=" << last_block << std::endl;

    if (from_block > head_block_) {
      std::cout << "[Sync] 已同步到最新, " << interval_seconds_ << "s 后检查" << std::endl;
      is_syncing_ = false;
      schedule_sync(interval_seconds_);
      return;
    }

    sync_loop(from_block, head_block_);
  }

  void init_done_slot(int slot, size_t nbits) {
    std::lock_guard<std::mutex> lock(done_list_mutex_);
    done_list_[slot].assign(nbits, 0);
  }

  void set_done_bit(int slot, size_t idx, uint8_t v) {
    std::lock_guard<std::mutex> lock(done_list_mutex_);
    assert(idx < done_list_[slot].size());
    done_list_[slot][idx] = v;
  }

  int ordered_done_in_slot(int slot) {
    std::lock_guard<std::mutex> lock(done_list_mutex_);
    int count = 0;
    for (uint8_t b : done_list_[slot]) {
      if (b == 0)
        break;
      ++count;
    }
    return count;
  }

  int all_done_in_slot(int slot) {
    std::lock_guard<std::mutex> lock(done_list_mutex_);
    int count = 0;
    for (uint8_t b : done_list_[slot]) {
      if (b != 0)
        ++count;
    }
    return count;
  }

  int total_in_slot(int slot) {
    std::lock_guard<std::mutex> lock(done_list_mutex_);
    return static_cast<int>(done_list_[slot].size());
  }

  void render_progress_inline(const SyncChunkState &front) {
    int ordered = ordered_done_in_slot(front.slot);
    int all_done = all_done_in_slot(front.slot);
    int total = total_in_slot(front.slot);
    std::string line = "[Sync] progress (" + std::to_string(ordered) + "/" + std::to_string(all_done) +
                       "/" + std::to_string(total) + ") sync=" + std::to_string(front.sync_id) +
                       " range=" + std::to_string(front.from_block) + "-" + std::to_string(front.to_block);
    if (line.size() < progress_line_len_) {
      line += std::string(progress_line_len_ - line.size(), ' ');
    }
    std::cout << "\r" << line << std::flush;
    progress_line_len_ = line.size();
    progress_line_active_ = true;
  }

  void clear_progress_inline() {
    if (!progress_line_active_) {
      return;
    }
    std::cout << "\r" << std::string(progress_line_len_, ' ') << "\r" << std::flush;
    progress_line_len_ = 0;
    progress_line_active_ = false;
  }

  std::optional<SyncChunkState> build_sync_chunk(int sync_id, int slot, int64_t &cursor, int64_t head_block, size_t &rr_worker) {
    if (cursor > head_block) {
      return std::nullopt;
    }
    SyncChunkState out;
    out.sync_id = sync_id;
    out.slot = slot;
    out.from_block = cursor;
    out.started_at = std::chrono::steady_clock::now();
    out.done_count = 0;
    for (int i = 0; i < sync_chunk_basic_count_ && cursor <= head_block; ++i) {
      int64_t to_block = std::min(cursor + basic_chunk_size_ - 1, head_block);
      out.basics.push_back(BasicTask{
          .from_block = cursor,
          .to_block = to_block,
          .worker_idx = static_cast<int>(rr_worker % rpc_workers_.size()),
      });
      rr_worker += 1;
      cursor = to_block + 1;
      out.to_block = to_block;
    }
    init_done_slot(slot, out.basics.size());
    return out;
  }

  void submit_basic_task(BasicTask &task) {
    TraceN("s1/basic_submit");
    task.future = rpc_workers_[task.worker_idx]->eth_getLogs_batch_async(build_queries(task.from_block, task.to_block));
    task.in_flight = true;
  }

  void sync_loop(int64_t from_block, int64_t head_block) {
    std::deque<SyncChunkState> window;
    int sync_id = 0;
    int64_t cursor = from_block;
    size_t rr_worker = 0;

    auto fill_window = [&]() {
      while (window.size() < static_cast<size_t>(super_sync_chunk_count_) && cursor <= head_block) {
        int slot = sync_id % super_sync_chunk_count_;
        auto chunk = build_sync_chunk(sync_id + 1, slot, cursor, head_block, rr_worker);
        assert(chunk.has_value());
        for (auto &task : chunk->basics) {
          TraceN("s1/prefetch");
          submit_basic_task(task);
        }
        window.push_back(std::move(*chunk));
        sync_id += 1;
      }
    };
    fill_window();

    auto last_progress_print = std::chrono::steady_clock::now();
    while (!stop_requested_ && !window.empty()) {
      bool progressed = false;
      auto now = std::chrono::steady_clock::now();

      for (auto &sync : window) {
        for (size_t i = 0; i < sync.basics.size(); ++i) {
          auto &task = sync.basics[i];
          if (task.done) {
            continue;
          }
          if (task.in_flight) {
            if (task.future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
              continue;
            }
            RpcClient::BatchResult result;
            try {
              result = task.future.get();
            } catch (const std::exception &e) {
              result.success = false;
              result.error_msg = std::string("future.get exception: ") + e.what();
            } catch (...) {
              result.success = false;
              result.error_msg = "future.get unknown exception";
            }
            task.in_flight = false;
            if (result.success) {
              TraceN("s1/basic_done");
              task.done = true;
              task.result = std::move(result);
              sync.done_count += 1;
              set_done_bit(sync.slot, i, 1);
            } else {
              TraceN("s1/basic_retry");
              clear_progress_inline();
              task.retry_at = now + std::chrono::milliseconds(kRetryDelayMs);
              std::cerr << "[Sync] rpc失败 from=" << task.from_block
                        << " to=" << task.to_block << " err=" << result.error_msg
                        << " -> retry" << std::endl;
            }
            progressed = true;
          } else if (now >= task.retry_at) {
            TraceN("s1/prefetch");
            submit_basic_task(task);
            progressed = true;
          }
        }
      }

      if (!window.empty()) {
        auto print_now = std::chrono::steady_clock::now();
        if (print_now - last_progress_print >= std::chrono::milliseconds(500)) {
          TraceN("s1/progress");
          const auto &front = window.front();
          render_progress_inline(front);
          last_progress_print = print_now;
        }
      }

      while (!window.empty() && window.front().done_count == static_cast<int>(window.front().basics.size())) {
        clear_progress_inline();
        TraceN("s1/sync_chunk_done");
        auto finished = std::move(window.front());
        window.pop_front();
        int64_t finished_blocks = 0;
        size_t finished_bytes = 0;
        for (auto &task : finished.basics) {
          assert(task.result.has_value());
          finished_blocks += (task.to_block - task.from_block + 1);
          finished_bytes += task.result->response_bytes;
          process_batch(*task.result, task.from_block, task.to_block);
        }
        double duration_s = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - finished.started_at)
                                .count();
        if (duration_s <= 0.0)
          duration_s = 1e-6;
        chunk_history_.push_back({finished.to_block, duration_s, finished_bytes, finished_blocks});
        if (chunk_history_.size() > 20)
          chunk_history_.pop_front();
        fill_window();
        progressed = true;
      }

      if (!progressed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kSchedulerSleepMs));
      }
    }

    if (stop_requested_) {
      clear_progress_inline();
      is_syncing_ = false;
      return;
    }

    clear_progress_inline();
    std::cout << "[Sync] 本轮同步完成, " << interval_seconds_ << "s 后检查更新" << std::endl;
    is_syncing_ = false;
    schedule_sync(interval_seconds_);
  }

  void process_batch(const RpcClient::BatchResult &r, int64_t from_block, int64_t to_block) {
    DecodedEvents events;
    {
      TraceN("s1/decode");
      events = EventDecoder::decode_logs(r.results);
    }

    merge_events(cached_events_, events);

    int64_t partition_end = current_partition_start_ + FeatherWriter::PARTITION_SIZE - 1;
    if (to_block >= partition_end) {
      TraceN("s1/write");
      feather_writer_.write_partition(current_partition_start_, cached_events_);
      {
        Database::WriteLock lock(db_);
        db_.set_last_block(partition_end);
      }
      cached_events_ = DecodedEvents{};
      current_partition_start_ += FeatherWriter::PARTITION_SIZE;
      std::cout << "[Sync] 分区 " << (current_partition_start_ - FeatherWriter::PARTITION_SIZE) << " 已落地" << std::endl;
    }
  }

  static void merge_events(DecodedEvents &dst, const DecodedEvents &src) {
    auto append = [](auto &d, const auto &s) { d.insert(d.end(), s.begin(), s.end()); };
    append(dst.transfer, src.transfer);
    append(dst.condition_preparation, src.condition_preparation);
    append(dst.condition_resolution, src.condition_resolution);
    append(dst.split, src.split);
    append(dst.merge, src.merge);
    append(dst.redemption, src.redemption);
    append(dst.fpmm, src.fpmm);
    append(dst.fpmm_trade, src.fpmm_trade);
    append(dst.fpmm_funding, src.fpmm_funding);
    append(dst.order_filled, src.order_filled);
    append(dst.token_map, src.token_map);
    append(dst.neg_risk_market, src.neg_risk_market);
    append(dst.neg_risk_question, src.neg_risk_question);
    append(dst.convert, src.convert);
  }

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

  int interval_seconds_ = 30; // 30s 冷静期
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
