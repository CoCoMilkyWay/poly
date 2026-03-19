#pragma once

#include "tracker/config.hpp"
#include "tracker/queue.hpp"
#include "tracker/state.hpp"

#include <atomic>
#include <thread>

namespace tracker {

// ============================================================================
// WsThread - RPC WebSocket 线程
// ============================================================================
// 职责:
//   - 维护 WebSocket 连接
//   - eth_subscribe newHeads / logs
//   - 解析后入 event_queue
//   - 断线自动重连

class WsThread {
public:
  WsThread(const AppConfig &cfg, AppState &state, EventQueue &queue);

  void start();
  void stop();
  void request_reconnect();

private:
  void run();
  std::vector<json> build_log_filters() const;

  const AppConfig &cfg_;
  AppState &state_;
  EventQueue &queue_;
  std::atomic<bool> running_{false};
  std::atomic<bool> reconnect_{false};
  std::thread thread_;
};

} // namespace tracker
