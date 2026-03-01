#include "chain_sync.hpp"
#include "misc/profiler.hpp"
#include <algorithm>
#include <iterator>

namespace stage1 {

StageSync::StageSync(const Config &config, Database &db)
    : config_(config), db_(db),
      feather_writer_(db.data_dir()),
      rpc_head_(config.rpc_url, config.rpc_api_key, "RPC-Head", config.proxy_url, config.rpc_transport),
      num_rpc_threads_(config.stage1_rpc_threads),
      basic_chunk_size_(kStage1ChunkBlocks / config.stage1_rpc_chunk_basics),
      chunk_basic_count_(config.stage1_rpc_chunk_basics) {
  assert(num_rpc_threads_ > 0);
  assert(chunk_basic_count_ > 0);
  assert(kStage1ChunkBlocks % chunk_basic_count_ == 0);
  assert(basic_chunk_size_ > 0);
  done_list_.resize(super_sync_chunk_count_);
  rpc_workers_.reserve(num_rpc_threads_);
  for (int i = 0; i < num_rpc_threads_; ++i) {
    rpc_workers_.push_back(std::make_unique<RpcClient>(
        config.rpc_url, config.rpc_api_key, "RPC-Worker-" + std::to_string(i), config.proxy_url, config.rpc_transport));
  }
  int64_t cursor = db_.get_last_block();
  current_partition_start_ = (cursor < 0) ? FeatherWriter::partition_start(config_.initial_block)
                                          : FeatherWriter::partition_start(cursor) + FeatherWriter::PARTITION_SIZE;
}

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  is_syncing_ = false;
  stop_requested_ = false;
  schedule_sync(0);
}

void StageSync::stop() {
  stop_requested_ = true;
  rpc_head_.cancel();
  for (auto &w : rpc_workers_) {
    w->cancel();
  }
}

Status StageSync::status() const {
  int64_t last_block = db_.get_last_block();
  int64_t safe_head = head_block_.load() - kStage1ChunkBlocks;
  int64_t behind_blocks = std::max<int64_t>(0, safe_head - last_block);
  int64_t behind_chunks = (behind_blocks + kStage1ChunkBlocks - 1) / kStage1ChunkBlocks;

  double blocks_per_second = 0.0;
  if (commit_history_.size() < 2) {
    return {
        .syncing = is_syncing_,
        .last_block = last_block,
        .head_block = head_block_,
        .behind_blocks = behind_blocks,
        .behind_chunks = behind_chunks,
        .blocks_per_second = 0.0,
        .eta_seconds = (behind_blocks == 0) ? 0.0 : -1.0,
        .bytes_per_block = 0.0,
    };
  }
  const auto &first = commit_history_.front();
  const auto &last = commit_history_.back();
  double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
  if (elapsed_s <= 0.0) {
    return {
        .syncing = is_syncing_,
        .last_block = last_block,
        .head_block = head_block_,
        .behind_blocks = behind_blocks,
        .behind_chunks = behind_chunks,
        .blocks_per_second = 0.0,
        .eta_seconds = -1.0,
        .bytes_per_block = 0.0,
    };
  }
  int64_t committed_blocks = std::max<int64_t>(0, last.committed_block - first.committed_block);
  if (committed_blocks == 0) {
    return {
        .syncing = is_syncing_,
        .last_block = last_block,
        .head_block = head_block_,
        .behind_blocks = behind_blocks,
        .behind_chunks = behind_chunks,
        .blocks_per_second = 0.0,
        .eta_seconds = (behind_blocks == 0) ? 0.0 : -1.0,
        .bytes_per_block = 0.0,
    };
  }
  blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;

  double bytes_per_block = 0.0;
  if (chunk_history_.empty()) {
    bytes_per_block = 0.0;
  } else {
    size_t total_bytes = 0;
    int64_t total_blocks = 0;
    for (const auto &r : chunk_history_) {
      total_bytes += r.body_bytes;
      total_blocks += r.block_count;
    }
    if (total_blocks > 0) {
      bytes_per_block = static_cast<double>(total_bytes) / total_blocks;
    }
  }

  return {
      .syncing = is_syncing_,
      .last_block = last_block,
      .head_block = head_block_,
      .behind_blocks = behind_blocks,
      .behind_chunks = behind_chunks,
      .blocks_per_second = blocks_per_second,
      .eta_seconds = (behind_blocks == 0) ? 0.0 : static_cast<double>(behind_blocks) / blocks_per_second,
      .bytes_per_block = bytes_per_block,
  };
}

const std::vector<std::string> &StageSync::ct_topics() {
  static const std::vector<std::string> v = {
      topics::TRANSFER_SINGLE, topics::TRANSFER_BATCH, topics::CONDITION_PREPARE,
      topics::CONDITION_RESOLVE, topics::POSITION_SPLIT, topics::POSITION_MERGE, topics::POSITION_REDEEM};
  return v;
}

const std::vector<std::string> &StageSync::ex_topics() {
  static const std::vector<std::string> v = {topics::ORDER_FILL, topics::TOKEN_REGISTER};
  return v;
}

const std::vector<std::string> &StageSync::nra_topics() {
  static const std::vector<std::string> v = {topics::MARKET_PREPARE, topics::QUESTION_PREPARE, topics::POSITION_CONVERT};
  return v;
}

const std::vector<std::string> &StageSync::fpmm_topics() {
  static const std::vector<std::string> v = {
      topics::FPMM_CREATE, topics::FPMM_BUY, topics::FPMM_SELL,
      topics::FPMM_FUNDING_ADD, topics::FPMM_FUNDING_REMOVE};
  return v;
}

std::vector<RpcClient::LogsQuery> StageSync::build_queries(int64_t from_block, int64_t to_block) {
  std::vector<RpcClient::LogsQuery> queries;
  queries.reserve(5);
  queries.emplace_back(std::string(contracts::CONDITIONAL_TOKENS), from_block, to_block, &ct_topics());
  queries.emplace_back(std::string(contracts::CTF_EXCHANGE), from_block, to_block, &ex_topics());
  queries.emplace_back(std::string(contracts::NEG_RISK_CTF_EXCHANGE), from_block, to_block, &ex_topics());
  queries.emplace_back(std::string(contracts::NEG_RISK_ADAPTER), from_block, to_block, &nra_topics());
  queries.emplace_back(std::nullopt, from_block, to_block, &fpmm_topics());
  return queries;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (stop_requested_) {
    return;
  }
  auto timer = std::make_shared<asio::steady_timer>(*ioc_);
  timer->expires_after(std::chrono::seconds(delay_seconds));
  timer->async_wait([this, timer](boost::system::error_code ec) {
    if (!ec && !stop_requested_) {
      do_sync();
    }
  });
}

void StageSync::do_sync() {
  is_syncing_ = true;

  try {
    head_block_ = rpc_head_.eth_blockNumber();
  } catch (...) {
    std::cerr << "[Stage1] 获取区块高度失败, " << interval_seconds_ << "s 后重试" << std::endl;
    is_syncing_ = false;
    schedule_sync(interval_seconds_);
    return;
  }
  int64_t last_block = db_.get_last_block();
  int64_t from_block = (last_block < 0) ? config_.initial_block : last_block + 1;
  int64_t safe_head = head_block_ - kStage1ChunkBlocks;

  std::cout << "[Stage1] head=" << head_block_
            << ", safe_head=" << safe_head
            << ", last=" << last_block << std::endl;

  if (from_block > safe_head) {
    std::cout << "[Stage1] 未达到100000确认深度可同步范围, " << interval_seconds_ << "s 后检查" << std::endl;
    is_syncing_ = false;
    schedule_sync(interval_seconds_);
    return;
  }

  sync_loop(from_block, safe_head);
}

void StageSync::init_done_slot(int slot, size_t nbits) {
  std::lock_guard<std::mutex> lock(done_list_mutex_);
  done_list_[slot].assign(nbits, 0);
}

void StageSync::set_done_bit(int slot, size_t idx, uint8_t v) {
  std::lock_guard<std::mutex> lock(done_list_mutex_);
  assert(idx < done_list_[slot].size());
  done_list_[slot][idx] = v;
}

int StageSync::ordered_done_in_slot(int slot) {
  std::lock_guard<std::mutex> lock(done_list_mutex_);
  int count = 0;
  for (uint8_t b : done_list_[slot]) {
    if (b == 0) {
      break;
    }
    ++count;
  }
  return count;
}

void StageSync::render_progress_inline(const std::deque<SyncChunkState> &window) {
  assert(!window.empty());
  const auto &front = window.front();
  int ordered = ordered_done_in_slot(front.slot);
  int all_done = 0;
  int total = 0;
  for (const auto &sync : window) {
    all_done += sync.done_count;
    total += static_cast<int>(sync.basics.size());
  }
  std::string line = "[Stage1] progress (" + std::to_string(ordered) + "/" + std::to_string(all_done) +
                     "/" + std::to_string(total) + ") sync=" + std::to_string(front.sync_id) +
                     " range=" + std::to_string(front.from_block) + "-" + std::to_string(front.to_block);
  if (line.size() < progress_line_len_) {
    line += std::string(progress_line_len_ - line.size(), ' ');
  }
  std::cout << "\r" << line << std::flush;
  progress_line_len_ = line.size();
  progress_line_active_ = true;
}

void StageSync::clear_progress_inline() {
  if (!progress_line_active_) {
    return;
  }
  std::cout << "\r" << std::string(progress_line_len_, ' ') << "\r" << std::flush;
  progress_line_len_ = 0;
  progress_line_active_ = false;
}

std::optional<StageSync::SyncChunkState> StageSync::build_sync_chunk(int sync_id, int slot, int64_t &cursor, int64_t head_block, size_t &rr_worker) {
  if (cursor > head_block) {
    return std::nullopt;
  }
  SyncChunkState out;
  out.sync_id = sync_id;
  out.slot = slot;
  out.from_block = cursor;
  out.started_at = std::chrono::steady_clock::now();
  out.done_count = 0;
  out.basics.reserve(chunk_basic_count_);
  for (int i = 0; i < chunk_basic_count_ && cursor <= head_block; ++i) {
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

void StageSync::submit_basic_task(BasicTask &task) {
  TraceN("s1/basic_submit");
  task.future = rpc_workers_[task.worker_idx]->eth_getLogs_batch_async(build_queries(task.from_block, task.to_block));
  task.in_flight = true;
}

void StageSync::sync_loop(int64_t from_block, int64_t head_block) {
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
  auto has_inflight = [&]() {
    for (const auto &sync : window) {
      for (const auto &task : sync.basics) {
        if (task.in_flight) {
          return true;
        }
      }
    }
    return false;
  };
  fill_window();

  int scheduler_sleep_ms = kSchedulerSleepMs;
  while (!window.empty()) {
    bool stopping = stop_requested_.load();
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
            DecodedEvents decoded;
            {
              TraceN("s1/decode");
              decoded = EventDecoder::decode_logs(std::move(result.results));
            }
            std::vector<json>().swap(result.results);
            std::string().swap(result.raw_body);
            TraceN("s1/basic_done");
            task.done = true;
            task.retry_count = 0;
            task.response_bytes = result.response_bytes;
            task.decoded_events = std::move(decoded);
            sync.done_count += 1;
            set_done_bit(sync.slot, i, 1);
            if (!stopping && !window.empty()) {
              TraceN("s1/progress");
              render_progress_inline(window);
            }
          } else {
            assert(result.retryable && "stage1 query返回了不可重试错误, 数据可靠性无法保证");
            if (stopping) {
              task.done = true;
              sync.done_count += 1;
            } else {
              TraceN("s1/basic_retry");
              clear_progress_inline();
              task.retry_count += 1;
              int shift = std::max(0, task.retry_count - 1);
              shift = std::min(shift, 20);
              int64_t delay_ms = static_cast<int64_t>(kRetryDelayMs) << shift;
              delay_ms = std::min<int64_t>(delay_ms, kRetryDelayMaxMs);
              task.retry_at = now + std::chrono::milliseconds(delay_ms);
              std::cerr << "[Stage1] rpc失败 from=" << task.from_block
                        << " to=" << task.to_block << " err=" << result.error_msg
                        << " -> retry_in=" << delay_ms << "ms" << std::endl;
            }
          }
          progressed = true;
        } else if (!stopping && now >= task.retry_at) {
          TraceN("s1/prefetch");
          submit_basic_task(task);
          progressed = true;
        }
      }
    }

    while (!stopping && !window.empty() &&
           window.front().done_count == static_cast<int>(window.front().basics.size())) {
      clear_progress_inline();
      TraceN("s1/sync_chunk_done");
      auto finished = std::move(window.front());
      window.pop_front();
      int64_t finished_blocks = 0;
      size_t finished_bytes = 0;
      for (auto &task : finished.basics) {
        assert(task.decoded_events.has_value());
        finished_blocks += (task.to_block - task.from_block + 1);
        finished_bytes += task.response_bytes;
        process_batch(std::move(*task.decoded_events), task.to_block);
        task.decoded_events.reset();
      }
      double duration_s = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - finished.started_at)
                              .count();
      if (duration_s <= 0.0) {
        duration_s = 1e-6;
      }
      chunk_history_.push_back({finished.to_block, duration_s, finished_bytes, finished_blocks});
      if (chunk_history_.size() > 20) {
        chunk_history_.pop_front();
      }
      fill_window();
      progressed = true;
    }

    if (stopping) {
      clear_progress_inline();
      if (!has_inflight() && !db_write_in_progress_.load()) {
        break;
      }
    }

    if (!progressed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(scheduler_sleep_ms));
      scheduler_sleep_ms = std::min(scheduler_sleep_ms << 1, StageSync::kSchedulerSleepMaxMs);
    } else {
      scheduler_sleep_ms = kSchedulerSleepMs;
    }
  }

  if (stop_requested_) {
    clear_progress_inline();
    is_syncing_ = false;
    return;
  }

  clear_progress_inline();
  std::cout << "[Stage1] 本轮同步完成, " << interval_seconds_ << "s 后检查更新" << std::endl;
  is_syncing_ = false;
  schedule_sync(interval_seconds_);
}

void StageSync::process_batch(DecodedEvents &&events, int64_t to_block) {
  merge_events(cached_events_, std::move(events));

  int64_t partition_end = current_partition_start_ + FeatherWriter::PARTITION_SIZE - 1;
  if (to_block >= partition_end) {
    TraceN("s1/write");
    db_write_in_progress_ = true;
    feather_writer_.write_partition(current_partition_start_, cached_events_);
    {
      TraceN("s1/write_state");
      bool locked = db_.try_write_lock();
      assert(locked && "stage1 state写锁被占用, 可能有另一个进程持有");
      db_.set_last_block(partition_end);
      db_.release_write_lock();
    }
    record_commit_event(partition_end);
    db_write_in_progress_ = false;
    cached_events_ = DecodedEvents{};
    current_partition_start_ += FeatherWriter::PARTITION_SIZE;
    std::cout << "[Stage1] 分区 " << (current_partition_start_ - FeatherWriter::PARTITION_SIZE) << " 已落地" << std::endl;
  }
}

void StageSync::record_commit_event(int64_t committed_block) {
  commit_history_.push_back({std::chrono::steady_clock::now(), committed_block});
  if (commit_history_.size() > kEtaWindowSize) {
    commit_history_.pop_front();
  }
}

void StageSync::merge_events(DecodedEvents &dst, DecodedEvents &&src) {
  auto append = [](auto &d, auto &s) {
    if (d.empty()) {
      d = std::move(s);
      return;
    }
    d.insert(d.end(), std::make_move_iterator(s.begin()), std::make_move_iterator(s.end()));
    using VecT = std::decay_t<decltype(s)>;
    VecT().swap(s);
  };
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

} // namespace stage1
