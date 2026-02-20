#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <vector>

#include <boost/asio.hpp>
#include <nlohmann/json.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "../infra/rpc_client.hpp"
#include "event_parser.hpp"

namespace asio = boost::asio;
using json = nlohmann::json;

class SyncCoordinator {
public:
  SyncCoordinator(const Config &config, Database &db)
      : config_(config), db_(db),
        rpc_(config.rpc_url, config.rpc_api_key),
        batch_size_(config.sync_batch_size),
        interval_seconds_(config.sync_interval_seconds) {}

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    is_syncing_ = false;
    schedule_sync(0);
  }

  bool is_syncing() const { return is_syncing_; }
  int64_t get_head_block() const { return head_block_; }

private:
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

    sync_batch(from_block, head_block_);
  }

  void sync_batch(int64_t from_block, int64_t head_block) {
    int64_t to_block = std::min(from_block + batch_size_ - 1, head_block);

    static const std::vector<std::string> ct_topics = {
        topics::TRANSFER_SINGLE, topics::TRANSFER_BATCH,
        topics::CONDITION_PREPARATION, topics::CONDITION_RESOLUTION,
        topics::POSITION_SPLIT, topics::POSITIONS_MERGE, topics::PAYOUT_REDEMPTION};
    static const std::vector<std::string> ex_topics = {
        topics::ORDER_FILLED, topics::TOKEN_REGISTERED};
    static const std::vector<std::string> nra_topics = {
        topics::MARKET_PREPARED, topics::QUESTION_PREPARED, topics::POSITIONS_CONVERTED};

    std::vector<json> results;
    try {
      results = rpc_.eth_getLogs_batch({{contracts::CONDITIONAL_TOKENS, from_block, to_block, ct_topics},
                                        {contracts::CTF_EXCHANGE, from_block, to_block, ex_topics},
                                        {contracts::NEG_RISK_CTF_EXCHANGE, from_block, to_block, ex_topics},
                                        {contracts::NEG_RISK_ADAPTER, from_block, to_block, nra_topics}});
    } catch (const std::exception &e) {
      std::cerr << "[Sync] eth_getLogs 失败: " << e.what() << ", 5s 后重试" << std::endl;
      auto timer = std::make_shared<asio::steady_timer>(*ioc_);
      timer->expires_after(std::chrono::seconds(5));
      timer->async_wait([this, timer, from_block, head_block](boost::system::error_code ec) {
        if (!ec) {
          sync_batch(from_block, head_block);
        }
      });
      return;
    }

    json logs = json::array();
    for (const auto &r : results) {
      for (const auto &log : r) {
        logs.push_back(log);
      }
    }

    ParsedEvents events = EventParser::parse_logs(logs);

    std::vector<std::tuple<std::string, std::string, std::vector<std::string>>> batches;
    batches.emplace_back("order_filled",
                         "block_number, log_index, exchange, maker, taker, token_id, side, usdc_amount, token_amount, fee",
                         std::move(events.order_filled));
    batches.emplace_back("split",
                         "block_number, log_index, stakeholder, condition_id, amount",
                         std::move(events.split));
    batches.emplace_back("merge",
                         "block_number, log_index, stakeholder, condition_id, amount",
                         std::move(events.merge));
    batches.emplace_back("redemption",
                         "block_number, log_index, redeemer, condition_id, index_sets, payout",
                         std::move(events.redemption));
    batches.emplace_back("convert",
                         "block_number, log_index, stakeholder, market_id, index_set, amount",
                         std::move(events.convert));
    batches.emplace_back("transfer",
                         "block_number, log_index, from_addr, to_addr, token_id, amount",
                         std::move(events.transfer));
    batches.emplace_back("token_map",
                         "token_id, condition_id, exchange, is_yes",
                         std::move(events.token_map));
    batches.emplace_back("condition",
                         "condition_id, oracle, question_id, payout_numerators, resolution_block",
                         std::move(events.condition));
    batches.emplace_back("neg_risk_market",
                         "market_id, oracle, fee_bips, data",
                         std::move(events.neg_risk_market));
    batches.emplace_back("neg_risk_question",
                         "question_id, market_id, question_index, data",
                         std::move(events.neg_risk_question));

    std::vector<std::string> resolution_sqls;
    for (const auto &val : events.condition_resolution) {
      // format: x'<hex>', '<payout_array>', <block_number>
      size_t first_sep = val.find(", ");
      std::string condition_id = val.substr(0, first_sep);
      size_t payout_end = val.rfind("', ");
      std::string payout = val.substr(first_sep + 2, payout_end - first_sep - 2 + 1);
      std::string block = val.substr(payout_end + 3);
      resolution_sqls.push_back("UPDATE condition SET payout_numerators = " + payout +
                                ", resolution_block = " + block +
                                " WHERE condition_id = " + condition_id);
    }

    {
      Database::WriteLock lock(db_);
      db_.atomic_multi_insert(batches, to_block, resolution_sqls);
    }

    if (to_block < head_block) {
      asio::post(*ioc_, [this, to_block, head_block]() {
        sync_batch(to_block + 1, head_block);
      });
    } else {
      std::cout << "[Sync] 本轮同步完成, " << interval_seconds_ << "s 后检查更新" << std::endl;
      is_syncing_ = false;
      schedule_sync(interval_seconds_);
    }
  }

  const Config &config_;
  Database &db_;
  RpcClient rpc_;
  asio::io_context *ioc_ = nullptr;

  int batch_size_;
  int interval_seconds_;
  std::atomic<bool> is_syncing_{false};
  std::atomic<int64_t> head_block_{0};
};
