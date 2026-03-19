#pragma once

#include "tracker/config.hpp"
#include "tracker/queue.hpp"
#include "tracker/state.hpp"
#include "tracker/ws.hpp"

#include <atomic>
#include <deque>

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
  SyncThread(const AppConfig &cfg, AppState &shared, EventQueue &queue,
             WsThread &ws);

  void run(); // blocking, call from main
  void request_resync();

private:
  void full_resync();
  void drain_queue();
  void handle_queue_event(QueueEvent ev);
  void handle_overlap_queue(uint64_t session_id, uint64_t overlap_block);
  void load_files();
  void load_seed();
  void publish_all();
  void persist_snapshot();
  void persist_meta();
  void persist_history();
  void persist_state();
  void fetch_user_snapshots();
  void fetch_snapshot_balances();
  void append_snapshot_roots();
  std::vector<std::string> collect_active_token_ids() const;
  void fetch_gamma_by_token_ids(const std::vector<std::string> &token_ids);
  void fetch_gamma_by_condition_ids(const std::vector<std::string> &condition_ids);
  void fetch_gamma_market_questions(const std::string &market_id);
  void ensure_token_meta(const std::string &token_id);
  void ensure_condition_meta(const std::string &condition_id,
                             Collateral hint_collateral);
  void ensure_market_questions(const std::string &market_id);
  void backfill_range(uint64_t from_block, uint64_t to_block);
  void apply_block_logs(const std::vector<json> &logs);
  void apply_condition_resolution(const json &log);
  void apply_order_fill(const json &log);
  void apply_split(const json &log);
  void apply_merge(const json &log);
  void apply_redeem(const json &log);
  void apply_convert(const json &log, const std::vector<json> &tx_logs);
  bool user_visible_at(const std::string &user, uint64_t block_number) const;
  uint64_t rpc_block_number();
  json rpc_call(const std::string &method, const json &params);
  json rpc_batch(const std::vector<json> &reqs);

  const AppConfig &cfg_;
  AppState &shared_;
  EventQueue &queue_;
  WsThread &ws_;
  RuntimeState rt_;
  std::deque<QueueEvent> deferred_;
  std::atomic<bool> resync_flag_{false};
  uint64_t current_session_id_ = 0;
};

} // namespace tracker
