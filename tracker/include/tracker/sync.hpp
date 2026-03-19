#pragma once

#include "tracker/config.hpp"
#include "tracker/queue.hpp"
#include "tracker/state.hpp"
#include "tracker/ws.hpp"

#include <atomic>
#include <set>

namespace tracker {

// ============================================================================
// SyncThread - 核心同步线程 (状态持有者)
// ============================================================================
// 职责:
//   - 拥有全部状态 (AppState)
//   - full_resync()
//   - drain 队列, apply_logs()
//   - 持久化 persist_all()
//   - 发布 state (通过 AppState.mu 保护)

class SyncThread {
public:
  SyncThread(const AppConfig &cfg, AppState &state, EventQueue &queue,
             WsThread &ws);

  void run(); // blocking, call from main
  void request_resync();

private:
  // full resync stages
  void full_resync();
  void fetch_positions();
  void fetch_balances(const std::string &block_tag);
  void fetch_market_data(bool missing_only);
  void fetch_conditions(const std::vector<std::string> &ids);
  void fetch_gamma(const std::vector<std::string> &ids);
  void backfill_range(uint64_t from, uint64_t to);

  // live processing
  void drain_queue();
  bool apply_logs(const std::vector<json> &logs,
                  std::set<std::string> &touched);

  // persistence
  void load_files();
  void load_seed();
  void persist_all();
  void append_snapshot(uint64_t block);

  // rpc helpers
  uint64_t rpc_block_number();
  json rpc_call(const std::string &method, const json &params);
  json rpc_batch(const std::vector<json> &reqs);

  // filter building
  std::vector<json> build_log_filters(uint64_t from, uint64_t to) const;

  const AppConfig &cfg_;
  AppState &state_;
  EventQueue &queue_;
  WsThread &ws_;
  std::atomic<bool> resync_flag_{false};
};

} // namespace tracker
