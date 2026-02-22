#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
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
    schedule_sync(0);
  }

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
    auto timer = std::make_shared<asio::steady_timer>(*ioc_);
    timer->expires_after(std::chrono::seconds(delay_seconds));
    timer->async_wait([this, timer](boost::system::error_code ec) {
      if (!ec) {
        do_sync();
      }
    });
  }

  void do_sync() {
    TraceN("Stage1::do_sync");
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
    while (from_block <= head_block) {
      int64_t to_block = std::min(from_block + current_batch_size_ - 1, head_block);

      if (!sync_one_batch(from_block, to_block, head_block)) {
        return;
      }

      from_block = to_block + 1;
    }

    std::cout << "[Sync] 本轮同步完成, " << interval_seconds_ << "s 后检查更新" << std::endl;
    is_syncing_ = false;
    schedule_sync(interval_seconds_);
  }

  bool sync_one_batch(int64_t from_block, int64_t to_block, int64_t head_block) {
    TraceN("Stage1::sync_batch");

    std::vector<json> results;
    try {
      results = rpc_.eth_getLogs_batch(build_queries(from_block, to_block));
    } catch (const std::exception &e) {
      int64_t reduced = std::max(current_batch_size_ / 2, (int64_t)1);
      std::cerr << "[Sync] eth_getLogs 失败: " << e.what()
                << ", chunk " << current_batch_size_ << " -> " << reduced << ", 5s 后重试" << std::endl;
      current_batch_size_ = reduced;

      auto timer = std::make_shared<asio::steady_timer>(*ioc_);
      timer->expires_after(std::chrono::seconds(5));
      timer->async_wait([this, timer, from_block, head_block](boost::system::error_code ec) {
        if (!ec) {
          sync_loop(from_block, head_block);
        }
      });
      return false;
    }

    current_batch_size_ = batch_size_;
    size_t response_bytes = rpc_.get_last_response_size();

    json logs = json::array();
    for (const auto &r : results) {
      for (const auto &log : r) {
        logs.push_back(log);
      }
    }

    DecodedEvents events;
    {
      TraceN("Stage1::decode_logs");
      events = EventDecoder::decode_logs(logs);
    }

    std::vector<std::tuple<std::string, std::string, std::vector<std::string>>> batches;
    batches.emplace_back("transfer",
                         "block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount",
                         std::move(events.transfer));
    batches.emplace_back("condition_preparation",
                         "block_number, tx_hash, log_index, condition_id, oracle, question_id, outcome_slot_count",
                         std::move(events.condition_preparation));
    batches.emplace_back("condition_resolution",
                         "block_number, tx_hash, log_index, condition_id, oracle, question_id, outcome_slot_count, payout_numerators",
                         std::move(events.condition_resolution));
    batches.emplace_back("split",
                         "block_number, tx_hash, log_index, stakeholder, collateral_token, parent_collection_id, condition_id, partition, amount",
                         std::move(events.split));
    batches.emplace_back("merge",
                         "block_number, tx_hash, log_index, stakeholder, collateral_token, parent_collection_id, condition_id, partition, amount",
                         std::move(events.merge));
    batches.emplace_back("redemption",
                         "block_number, tx_hash, log_index, redeemer, collateral_token, parent_collection_id, condition_id, index_sets, payout",
                         std::move(events.redemption));
    batches.emplace_back("fpmm",
                         "block_number, tx_hash, log_index, creator, fpmm_addr, conditional_tokens, collateral_token, condition_ids, fee",
                         std::move(events.fpmm));
    batches.emplace_back("fpmm_trade",
                         "block_number, tx_hash, log_index, fpmm_addr, trader, side, outcome_index, usdc_amount, token_amount, fee",
                         std::move(events.fpmm_trade));
    batches.emplace_back("fpmm_funding",
                         "block_number, tx_hash, log_index, fpmm_addr, funder, side, amounts, collateral_from_fee_pool, shares",
                         std::move(events.fpmm_funding));
    batches.emplace_back("order_filled",
                         "block_number, tx_hash, log_index, exchange, order_hash, maker, taker, maker_asset_id, taker_asset_id, maker_amount, taker_amount, fee",
                         std::move(events.order_filled));
    batches.emplace_back("token_map",
                         "block_number, tx_hash, log_index, exchange, token0, token1, condition_id",
                         std::move(events.token_map));
    batches.emplace_back("neg_risk_market",
                         "block_number, tx_hash, log_index, market_id, oracle, fee_bips, data",
                         std::move(events.neg_risk_market));
    batches.emplace_back("neg_risk_question",
                         "block_number, tx_hash, log_index, market_id, question_id, question_index, data",
                         std::move(events.neg_risk_question));
    batches.emplace_back("convert",
                         "block_number, tx_hash, log_index, stakeholder, market_id, index_set, amount",
                         std::move(events.convert));

    {
      TraceN("Stage1::db_write");
      Database::WriteLock lock(db_);
      db_.atomic_multi_insert(batches, to_block);
    }

    double now = std::chrono::duration<double>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
    chunk_history_.push_back({to_block, now, response_bytes, to_block - from_block + 1});
    if (chunk_history_.size() > 20)
      chunk_history_.pop_front();

    return true;
  }

  const Config &config_;
  Database &db_;
  RpcClient rpc_;
  asio::io_context *ioc_ = nullptr;

  int64_t batch_size_;
  int64_t current_batch_size_;
  int interval_seconds_;
  std::atomic<bool> is_syncing_{false};
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
