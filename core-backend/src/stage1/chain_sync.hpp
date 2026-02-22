#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
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
        rpc_(config.rpc_url, config.rpc_api_key),
        batch_size_(config.rpc_chunk),
        current_batch_size_(config.rpc_chunk),
        interval_seconds_(config.sync_interval_seconds) {}

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
    if (chunk_history_.size() < 2)
      return 0.0;
    double time_diff = chunk_history_.back().time_s - chunk_history_.front().time_s;
    if (time_diff <= 0)
      return 0.0;
    double total_blocks = 0;
    for (const auto &r : chunk_history_)
      total_blocks += r.block_count;
    return total_blocks / time_diff;
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
  struct PendingFetch {
    std::future<RpcClient::BatchResult> future;
    int64_t from_block = 0;
    int64_t to_block = 0;
  };

  PendingFetch prefetch_async(int64_t from, int64_t to) {
    return {rpc_.eth_getLogs_batch_async(build_queries(from, to)), from, to};
  }

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
    TraceN("s1/sync");
    is_syncing_ = true;

    try {
      head_block_ = rpc_.eth_blockNumber();
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

  void sync_loop(int64_t from_block, int64_t head_block) {
    std::optional<PendingFetch> pending;

    int64_t to_block = std::min(from_block + current_batch_size_ - 1, head_block);
    {
      TraceN("s1/prefetch");
      pending = prefetch_async(from_block, to_block);
    }

    while (!stop_requested_ && pending) {
      TraceN("s1/batch");

      int64_t cur_from = pending->from_block;
      int64_t cur_to = pending->to_block;
      RpcClient::BatchResult rpc_result;
      {
        TraceN("s1/wait_rpc");
        rpc_result = pending->future.get();
      }
      pending.reset();

      if (!rpc_result.success) {
        int64_t reduced = std::max(current_batch_size_ / 2, (int64_t)1);
        std::cerr << "[Sync] eth_getLogs 失败: " << rpc_result.error_msg
                  << ", chunk " << current_batch_size_ << " -> " << reduced << ", 5s 后重试" << std::endl;
        current_batch_size_ = reduced;

        auto timer = std::make_shared<asio::steady_timer>(*ioc_);
        timer->expires_after(std::chrono::seconds(5));
        timer->async_wait([this, timer, cur_from, head_block](boost::system::error_code ec) {
          if (!ec && !stop_requested_) {
            sync_loop(cur_from, head_block);
          }
        });
        return;
      }

      current_batch_size_ = batch_size_;

      int64_t next_from = cur_to + 1;
      if (next_from <= head_block && !stop_requested_) {
        TraceN("s1/prefetch");
        int64_t next_to = std::min(next_from + current_batch_size_ - 1, head_block);
        pending = prefetch_async(next_from, next_to);
      }

      process_batch(rpc_result, cur_from, cur_to);
    }

    if (stop_requested_) {
      is_syncing_ = false;
      return;
    }

    std::cout << "[Sync] 本轮同步完成, " << interval_seconds_ << "s 后检查更新" << std::endl;
    is_syncing_ = false;
    schedule_sync(interval_seconds_);
  }

  void process_batch(const RpcClient::BatchResult &r, int64_t from_block, int64_t to_block) {
    json logs = json::array();
    {
      TraceN("s1/merge");
      for (const auto &result : r.results) {
        for (const auto &log : result) {
          logs.push_back(log);
        }
      }
    }
    DecodedEvents events;
    {
      TraceN("s1/decode");
      events = EventDecoder::decode_logs(logs);
    }

    {
      TraceN("s1/write");
      Database::WriteLock lock(db_);
      db_.atomic_multi_insert_appender(events, to_block);
    }

    {
      TraceN("s1/fetch");
      double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
      chunk_history_.push_back({to_block, now, r.response_bytes, to_block - from_block + 1});
      if (chunk_history_.size() > 20)
        chunk_history_.pop_front();
    }
  }

  const Config &config_;
  Database &db_;
  RpcClient rpc_;
  asio::io_context *ioc_ = nullptr;

  int64_t batch_size_;
  int64_t current_batch_size_;
  int interval_seconds_;
  std::atomic<bool> is_syncing_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<int64_t> head_block_{0};
  struct ChunkRecord {
    int64_t to_block;
    double time_s;
    size_t body_bytes;
    int64_t block_count;
  };
  std::deque<ChunkRecord> chunk_history_;
};

} // namespace stage1
