#pragma once

#include "tracker/json.hpp"

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace tracker {

// ============================================================================
// QueueEvent - WS Thread 产生的事件
// ============================================================================

enum class QueueEventKind : uint8_t {
  Head = 0,
  Logs = 1,
  Resync = 2,
};

struct QueueEvent {
  uint64_t session_id = 0;
  QueueEventKind kind = QueueEventKind::Head;
  uint64_t block_number = 0;
  json logs;  // array of raw logs
};

// ============================================================================
// EventQueue - mpsc 队列 (WS -> Sync)
// ============================================================================

class EventQueue {
public:
  void push(QueueEvent ev) {
    std::lock_guard<std::mutex> lock(mu_);
    queue_.push_back(std::move(ev));
    cv_.notify_one();
  }

  std::optional<QueueEvent> try_pop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (queue_.empty()) return std::nullopt;
    QueueEvent ev = std::move(queue_.front());
    queue_.pop_front();
    return ev;
  }

  QueueEvent wait_pop() {
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this] { return !queue_.empty(); });
    QueueEvent ev = std::move(queue_.front());
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
  std::deque<QueueEvent> queue_;
};

} // namespace tracker
