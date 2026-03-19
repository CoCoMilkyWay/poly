#pragma once

#include "tracker/config.hpp"
#include "tracker/state.hpp"

#include <atomic>
#include <functional>
#include <thread>

namespace tracker {

// ============================================================================
// ApiThread - HTTP API 服务线程
// ============================================================================
// 职责:
//   - accept HTTP 连接
//   - 提供 /api/state, /api/meta, /api/history, /api/resync
//   - 只读 state (通过 mu 保护)
//   - 写 resync_flag 触发重新同步

class ApiThread {
public:
  using ResyncCallback = std::function<void()>;

  ApiThread(const AppConfig &cfg, AppState &state, ResyncCallback on_resync);

  void start();
  void stop();

private:
  void run();

  const AppConfig &cfg_;
  AppState &state_;
  ResyncCallback on_resync_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// ============================================================================
// JSON Builders (供 API 和 Sync 使用)
// ============================================================================

json build_state_json(AppState &state);
json build_meta_json(AppState &state);
json build_history_json(AppState &state, const std::string &user);
json build_health_json(AppState &state);

} // namespace tracker
