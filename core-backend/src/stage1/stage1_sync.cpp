#include "stage1_sync.hpp"
#include "misc/profiler.hpp"

#include <algorithm>
#include <iomanip>

namespace stage1 {

StageSync::StageSync(const Config &config, Database &db, int interval_seconds)
    : config_(config), db_(db),
      feather_writer_(db.data_dir()),
      rpc_head_(config.rpc_url, config.rpc_api_key, "RPC-Head", config.proxy_url, config.rpc_transport),
      num_rpc_threads_(config.stage1_rpc_threads),
      num_decode_threads_(config.stage1_rpc_threads),
      rpc_block_span_(config.stage1_rpc_block_span),
      interval_seconds_(interval_seconds),
      buffer_high_water_transfers_(
          static_cast<int64_t>(static_cast<double>(kChunkTransferTarget) * config.stage1_rpc_buffer_multiplier)) {
  assert(num_rpc_threads_ > 0);
  assert(num_decode_threads_ > 0);
  assert(rpc_block_span_ > 0);
  assert(config.stage1_rpc_buffer_multiplier >= 1.0);
  assert(interval_seconds_ > 0);
  assert(config_.initial_block % kCommitBlockGranularity == 0);
  assert(rpc_block_span_ % kCommitBlockGranularity == 0);
  if (buffer_high_water_transfers_ < kChunkTransferTarget) {
    buffer_high_water_transfers_ = kChunkTransferTarget;
  }
  buffer_low_water_transfers_ = std::max<int64_t>(kChunkTransferTarget, buffer_high_water_transfers_ - kChunkTransferTarget);
  assert(buffer_low_water_transfers_ <= buffer_high_water_transfers_);

  rpc_workers_.reserve(static_cast<size_t>(num_rpc_threads_));
  for (int i = 0; i < num_rpc_threads_; ++i) {
    rpc_workers_.push_back(std::make_unique<RpcClient>(
        config.rpc_url, config.rpc_api_key,
        "RPC-Worker-" + std::to_string(i),
        config.proxy_url, config.rpc_transport));
  }

  int64_t committed = db_.get_last_block();
  current_chunk_start_block_ = (committed < 0) ? config_.initial_block : committed + 1;
  assert(current_chunk_start_block_ % kCommitBlockGranularity == 0);
  next_query_block_ = current_chunk_start_block_;
  next_buffer_block_ = current_chunk_start_block_;
  rpc_paused_ = false;
  ready_transfer_rows_ = 0;

  start_decode_pool();
}

StageSync::~StageSync() {
  stop_decode_pool();
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

void StageSync::start_decode_pool() {
  assert(!decode_running_);
  decode_running_ = true;
  decode_workers_.reserve(static_cast<size_t>(num_decode_threads_));
  for (int i = 0; i < num_decode_threads_; ++i) {
    decode_workers_.emplace_back([this]() {
      while (true) {
        DecodeTask task;
        {
          std::unique_lock<std::mutex> lock(decode_mutex_);
          decode_cv_.wait(lock, [this] { return !decode_running_ || !decode_queue_.empty(); });
          if (!decode_running_ && decode_queue_.empty()) {
            break;
          }
          task = std::move(decode_queue_.front());
          decode_queue_.pop_front();
        }
        assert(task.shared_raw_logs != nullptr);
        assert(task.promise != nullptr);
        DecodedEvents decoded = EventDecoder::decode_logs(std::move(*task.shared_raw_logs));
        std::vector<json>().swap(*task.shared_raw_logs);
        task.shared_raw_logs.reset();
        task.promise->set_value(std::move(decoded));
      }
    });
  }
}

void StageSync::stop_decode_pool() {
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    decode_running_ = false;
  }
  decode_cv_.notify_all();
  for (auto &t : decode_workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
  decode_workers_.clear();
  assert(decode_queue_.empty());
}

std::future<DecodedEvents> StageSync::submit_decode_task(std::shared_ptr<std::vector<json>> shared_raw_logs) {
  assert(shared_raw_logs != nullptr);
  auto promise = std::make_shared<std::promise<DecodedEvents>>();
  auto future = promise->get_future();
  {
    std::lock_guard<std::mutex> lock(decode_mutex_);
    assert(decode_running_);
    decode_queue_.push_back(DecodeTask{
        .shared_raw_logs = std::move(shared_raw_logs),
        .promise = promise,
    });
  }
  decode_cv_.notify_one();
  return future;
}

Status StageSync::status() const {
  int64_t last_block = db_.get_last_block();
  int64_t safe_head = head_block_.load() - kFinalityDepthBlocks;
  int64_t behind_blocks = std::max<int64_t>(0, safe_head - last_block);
  int64_t behind_chunks = (behind_blocks + rpc_block_span_ - 1) / rpc_block_span_;

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
  if (!chunk_history_.empty()) {
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
  Trace;
  is_syncing_ = true;

  head_block_ = rpc_head_.eth_blockNumber();
  int64_t safe_head = head_block_ - kFinalityDepthBlocks;
  int64_t last_block = db_.get_last_block();

  int64_t expected_chunk_start = (last_block < 0) ? config_.initial_block : last_block + 1;
  int64_t trace_chunk_end = std::max<int64_t>(expected_chunk_start - 1, safe_head);
  std::string sync_trace_name =
      "s1/sync " + std::to_string(expected_chunk_start) + "-" + std::to_string(trace_chunk_end);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());
  assert(expected_chunk_start % kCommitBlockGranularity == 0);
  if (buffered_batches_.empty() && ready_batches_.empty()) {
    current_chunk_start_block_ = expected_chunk_start;
    next_query_block_ = expected_chunk_start;
    next_buffer_block_ = expected_chunk_start;
  } else {
    assert(current_chunk_start_block_ == expected_chunk_start);
  }
  if (buffered_batches_.empty()) {
    assert(buffered_transfer_rows_ == 0);
  }
  if (ready_batches_.empty()) {
    assert(ready_transfer_rows_ == 0);
  }

  std::cout << "[Stage1] head=" << head_block_
            << ", safe_head=" << safe_head
            << ", committed_last=" << last_block
            << ", next_query=" << next_query_block_
            << ", buffered_transfer=" << buffered_transfer_rows_ << std::endl;

  sync_loop(safe_head);

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

void StageSync::render_progress_inline(int64_t safe_head, size_t inflight_count) const {
  (void)safe_head;
  (void)inflight_count;
  auto to_w = [](int64_t v) -> int64_t { return (v + 5000) / 10000; };
  int64_t ordered_to = next_buffer_block_ - 1;
  int64_t ordered_transfer_rows = buffered_transfer_rows_;
  int64_t pending_transfer_rows = buffered_transfer_rows_ + ready_transfer_rows_;
  std::string line =
      "[Stage1] " + std::to_string(current_chunk_start_block_) + "-" + std::to_string(ordered_to) +
      " (" + std::to_string(to_w(ordered_transfer_rows)) + "W/" +
      std::to_string(to_w(pending_transfer_rows)) + "W/" +
      std::to_string(to_w(kChunkTransferTarget)) + "W/" +
      std::to_string(to_w(buffer_high_water_transfers_)) + "W)";
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

void StageSync::submit_rpc_task(InFlightTask &task) {
  TraceN("s1/rpc_submit");
  task.query_future = rpc_workers_[task.worker_idx]->eth_getLogs_batch_async(build_queries(task.from_block, task.to_block));
  task.query_in_flight = true;
  task.decode_in_flight = false;
  task.done = false;
}

void StageSync::promote_ready_batches() {
  while (true) {
    auto it = ready_batches_.find(next_buffer_block_);
    if (it == ready_batches_.end()) {
      break;
    }
    if (!buffered_batches_.empty()) {
      assert(buffered_batches_.back().to_block + 1 == it->second.from_block);
    }
    assert(ready_transfer_rows_ >= it->second.transfer_rows);
    ready_transfer_rows_ -= it->second.transfer_rows;
    buffered_transfer_rows_ += it->second.transfer_rows;
    next_buffer_block_ = it->second.to_block + 1;
    buffered_batches_.push_back(std::move(it->second));
    ready_batches_.erase(it);
  }
}

bool StageSync::find_committable_prefix(size_t &consume_batches, int64_t &commit_end_block) const {
  consume_batches = 0;
  commit_end_block = -1;
  if (buffered_transfer_rows_ < kChunkTransferTarget) {
    return false;
  }
  int64_t cumulative_transfers = 0;
  for (size_t i = 0; i < buffered_batches_.size(); ++i) {
    cumulative_transfers += buffered_batches_[i].transfer_rows;
    if (cumulative_transfers >= kChunkTransferTarget &&
        ((buffered_batches_[i].to_block + 1) % kCommitBlockGranularity == 0)) {
      consume_batches = i + 1;
      commit_end_block = buffered_batches_[i].to_block;
      return true;
    }
  }
  return false;
}

bool StageSync::try_commit_ready_chunks() {
  bool committed_any = false;
  size_t consume_batches = 0;
  int64_t commit_end_block = -1;
  while (find_committable_prefix(consume_batches, commit_end_block)) {

    assert(!buffered_batches_.empty());
    assert(buffered_batches_.front().from_block == current_chunk_start_block_);

    const int64_t landed_chunk_start = current_chunk_start_block_;
    int64_t landed_transfer_rows = 0;
    size_t landed_response_bytes = 0;

    TraceN("s1/write_chunk");
    db_write_in_progress_ = true;
    feather_writer_.begin_partition(landed_chunk_start, commit_end_block);
    for (size_t i = 0; i < consume_batches; ++i) {
      auto &batch = buffered_batches_[i];
      landed_transfer_rows += batch.transfer_rows;
      landed_response_bytes += batch.response_bytes;
      feather_writer_.append_partition_batch(batch.events);
    }
    feather_writer_.finalize_partition();
    {
      TraceN("s1/write_state");
      bool locked = db_.try_write_lock();
      assert(locked && "stage1 state写锁被占用, 可能有另一个进程持有");
      db_.set_last_block(commit_end_block);
      db_.release_write_lock();
    }
    db_write_in_progress_ = false;

    record_commit_event(commit_end_block);
    chunk_history_.push_back({
        .to_block = commit_end_block,
        .body_bytes = landed_response_bytes,
        .block_count = commit_end_block - landed_chunk_start + 1,
    });
    if (chunk_history_.size() > 20) {
      chunk_history_.pop_front();
    }

    for (size_t i = 0; i < consume_batches; ++i) {
      assert(buffered_transfer_rows_ >= buffered_batches_.front().transfer_rows);
      buffered_transfer_rows_ -= buffered_batches_.front().transfer_rows;
      buffered_batches_.pop_front();
    }

    current_chunk_start_block_ = commit_end_block + 1;
    assert(current_chunk_start_block_ % kCommitBlockGranularity == 0);
    if (!buffered_batches_.empty()) {
      assert(buffered_batches_.front().from_block == current_chunk_start_block_);
    }

    const auto landed_bytes = feather_writer_.partition_total_size_bytes(landed_chunk_start, commit_end_block);
    const double landed_mb = static_cast<double>(landed_bytes) / (1024.0 * 1024.0);
    std::cout << "\n[Stage1] chunk " << landed_chunk_start << "-" << commit_end_block
              << " 已落地, transfer_rows=" << landed_transfer_rows
              << ", rpc_bytes=" << landed_response_bytes
              << ", files=" << std::fixed << std::setprecision(2) << landed_mb << "MB"
              << std::defaultfloat << std::endl;

    committed_any = true;
  }
  return committed_any;
}

void StageSync::sync_loop(int64_t safe_head) {
  std::vector<InFlightTask> inflight;
  inflight.reserve(static_cast<size_t>(num_rpc_threads_));
  const size_t max_inflight = static_cast<size_t>(num_rpc_threads_);
  assert(max_inflight > 0);
  const int64_t chain_head = head_block_.load();
  int64_t query_head = safe_head;
  if (query_head >= 0) {
    int64_t aligned = ((query_head + 1 + kCommitBlockGranularity - 1) / kCommitBlockGranularity) *
                          kCommitBlockGranularity -
                      1;
    query_head = std::min<int64_t>(aligned, chain_head);
  }

  auto pending_transfer_rows = [this]() {
    return buffered_transfer_rows_ + ready_transfer_rows_;
  };

  int scheduler_sleep_ms = kSchedulerSleepMs;
  while (true) {
    bool stopping = stop_requested_.load();
    bool progressed = false;
    auto now = std::chrono::steady_clock::now();

    if (rpc_paused_) {
      if (pending_transfer_rows() <= buffer_low_water_transfers_) {
        rpc_paused_ = false;
      }
    }

    while (!stopping && next_query_block_ <= query_head && inflight.size() < max_inflight) {
      if (rpc_paused_) {
        break;
      }
      if (pending_transfer_rows() >= buffer_high_water_transfers_) {
        rpc_paused_ = true;
        break;
      }
      InFlightTask task;
      task.from_block = next_query_block_;
      task.to_block = std::min(task.from_block + rpc_block_span_ - 1, query_head);
      task.worker_idx = static_cast<int>(rr_worker_ % rpc_workers_.size());
      rr_worker_ += 1;
      submit_rpc_task(task);
      inflight.push_back(std::move(task));
      next_query_block_ = inflight.back().to_block + 1;
      progressed = true;
    }

    for (auto &task : inflight) {
      if (task.done) {
        continue;
      }
      if (task.query_in_flight) {
        if (task.query_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
          continue;
        }

        RpcClient::BatchResult result = task.query_future.get();
        task.query_in_flight = false;

        if (result.success) {
          task.response_bytes = result.response_bytes;
          auto shared_raw_logs = std::make_shared<std::vector<json>>(std::move(result.results));
          std::string().swap(result.raw_body);
          task.decode_future = submit_decode_task(std::move(shared_raw_logs));
          task.decode_in_flight = true;
        } else {
          assert(result.retryable && "stage1 query返回了不可重试错误, 数据可靠性无法保证");
          if (stopping) {
            task.done = true;
          } else {
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
        continue;
      }

      if (task.decode_in_flight) {
        if (task.decode_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
          continue;
        }

        task.decode_in_flight = false;
        DecodedEvents decoded = task.decode_future.get();

        BufferedBatch batch;
        batch.from_block = task.from_block;
        batch.to_block = task.to_block;
        batch.response_bytes = task.response_bytes;
        batch.transfer_rows = transfer_row_count(decoded);
        batch.events = std::move(decoded);

        auto [it, inserted] = ready_batches_.emplace(batch.from_block, std::move(batch));
        assert(inserted && "stage1 ready batch重复, block range异常");
        ready_transfer_rows_ += it->second.transfer_rows;

        task.done = true;
        task.retry_count = 0;
        progressed = true;
        continue;
      }

      if (!stopping && now >= task.retry_at) {
        submit_rpc_task(task);
        progressed = true;
      }
    }

    size_t before_inflight = inflight.size();
    inflight.erase(
        std::remove_if(inflight.begin(), inflight.end(),
                       [](const InFlightTask &task) { return task.done; }),
        inflight.end());
    if (inflight.size() != before_inflight) {
      progressed = true;
    }

    size_t before_buffered = buffered_batches_.size();
    promote_ready_batches();
    if (buffered_batches_.size() != before_buffered) {
      progressed = true;
    }

    if (!rpc_paused_ && pending_transfer_rows() >= buffer_high_water_transfers_) {
      rpc_paused_ = true;
      progressed = true;
    }

    if (try_commit_ready_chunks()) {
      progressed = true;
    }

    render_progress_inline(safe_head, inflight.size());

    if (stopping) {
      if (inflight.empty() && !db_write_in_progress_.load()) {
        break;
      }
    }

    if (next_query_block_ > query_head && inflight.empty()) {
      break;
    }

    if (!progressed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(scheduler_sleep_ms));
      scheduler_sleep_ms = std::min(scheduler_sleep_ms << 1, StageSync::kSchedulerSleepMaxMs);
    } else {
      scheduler_sleep_ms = kSchedulerSleepMs;
    }
  }
}

void StageSync::record_commit_event(int64_t committed_block) {
  commit_history_.push_back({std::chrono::steady_clock::now(), committed_block});
  if (commit_history_.size() > kEtaWindowSize) {
    commit_history_.pop_front();
  }
}

int64_t StageSync::transfer_row_count(const DecodedEvents &events) {
  return static_cast<int64_t>(events.transfer.size());
}

} // namespace stage1
