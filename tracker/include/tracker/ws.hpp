#pragma once

#include "tracker/config.hpp"
#include "tracker/queue.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace tracker {

// ============================================================================
// WsThread - RPC WebSocket 线程
// ============================================================================
// 职责:
//   - 维护 WebSocket 连接
//   - eth_subscribe newHeads / logs
//   - 解析后入 event_queue
//   - 断线自动重连

struct WsCounters {
  uint64_t msg = 0;
  uint64_t sub = 0;
};

struct WsSessionInfo {
  uint64_t session_id = 0;
  uint64_t start_block = 0;
};

class WsThread {
public:
  WsThread(const AppConfig &cfg, EventQueue &queue);

  void start();
  void stop();
  WsSessionInfo start_session(const std::vector<std::string> &users);
  [[nodiscard]] WsCounters counters() const;

private:
  void run();

  const AppConfig &cfg_;
  EventQueue &queue_;
  std::atomic<bool> running_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  uint64_t desired_session_id_ = 0;
  std::vector<std::string> desired_users_;
  uint64_t ready_session_id_ = 0;
  uint64_t ready_start_block_ = 0;
  std::atomic<uint64_t> ws_msg_count_{0};
  std::atomic<uint64_t> ws_sub_count_{0};
};

} // namespace tracker
