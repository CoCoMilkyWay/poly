#pragma once

#include "tracker/json.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace tracker {

// ============================================================================
// BlockEvent - WS Thread 产生的事件
// ============================================================================

struct BlockEvent {
  uint64_t block_number = 0;
  json logs;  // array of raw logs
};

// ============================================================================
// EventQueue - mpsc 队列 (WS -> Sync)
// ============================================================================

class EventQueue {
public:
  void push(BlockEvent ev) {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(std::move(ev));
    cv_.notify_one();
  }

  std::optional<BlockEvent> try_pop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) return std::nullopt;
    BlockEvent ev = std::move(queue_.front());
    queue_.pop_front();
    return ev;
  }

  BlockEvent wait_pop() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return !queue_.empty(); });
    BlockEvent ev = std::move(queue_.front());
    queue_.pop_front();
    return ev;
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.empty();
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.clear();
  }

private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<BlockEvent> queue_;
};

} // namespace tracker
