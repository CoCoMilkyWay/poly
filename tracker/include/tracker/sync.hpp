#pragma once

#include "tracker/config.hpp"
#include "tracker/queue.hpp"
#include "tracker/state.hpp"
#include "tracker/ws.hpp"

#include <atomic>
#include <deque>

namespace tracker {

// TransferSingle/Batch 解析后的 token 转移 (Transfer 驱动设计)
struct TransferLeg {
  int64_t log_index = 0;
  std::string from;
  std::string to;
  std::string token_id;
  BigInt amount = 0;
};

// 待提交事件 (apply_* 产出 → commit_events 消费)
struct PendingEmit {
  std::string user;
  std::string token_id;
  std::string condition_id;
  uint8_t token_idx = 0xFF;
  uint8_t collateral = 0;
  EventType type = EventType::OrderBuy;
  int64_t amount = 0;
  int64_t price = 0;
};

// ============================================================================
// SyncThread - 核心同步线程 (状态持有者)
// ============================================================================
// 职责:
//   - 拥有全部状态 (AppState)
//   - full_resync()
//   - drain 队列, apply_logs()
//   - 持久化 snapshot/history/meta (aggregate/user_views/token_holders 纯内存)
//   - 原子发布 state/meta/snapshot/history

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
  void clear_derived_state();
  void rebuild_derived_state();
  void refresh_users(const std::unordered_set<std::string> &users);
  ConditionMeta &prepare_condition(const std::string &condition_id, Collateral hint_collateral);
  void remove_user_aggregate(const std::string &user);
  void add_user_aggregate(const std::string &user);
  void rebuild_user_view(const std::string &user);
  std::unordered_set<std::string>
  collect_condition_users(const std::string &condition_id) const;
  void fetch_user_snapshots();
  void fetch_snapshot_balances();
  void append_snapshot_roots();
  std::vector<std::string> collect_active_token_ids() const;
  void fetch_gamma_by_token_ids(const std::vector<std::string> &token_ids);
  void refresh_prices(const std::vector<std::string> &token_ids);
  void fetch_gamma_by_condition_ids(const std::vector<std::string> &condition_ids);
  void fetch_gamma_market_questions(const std::string &market_id);
  bool ensure_token_meta(const std::string &token_id);
  bool ensure_condition_meta(const std::string &condition_id, Collateral hint_collateral);
  void ensure_market_questions(const std::string &market_id);
  void backfill_range(uint64_t from_block, uint64_t to_block);
  void apply_block_logs(const std::vector<json> &logs, const std::string &source);
  // apply_* 系列: 处理各类事件, 更新 dirty_users_/dirty_conds_
  // matched_indices: 追踪已消费的 TransferLeg (防止重复匹配)
  void apply_condition_resolution(const json &log);
  void apply_order_fill(const json &log, const std::vector<TransferLeg> &transfers, std::unordered_set<int64_t> &matched_indices);
  void apply_split_or_merge(const json &log, bool is_split, const std::vector<TransferLeg> &transfers, std::unordered_set<int64_t> &matched_indices);
  void apply_redeem(const json &log, const std::vector<TransferLeg> &transfers, std::unordered_set<int64_t> &matched_indices);
  void apply_convert(const json &log, const std::vector<TransferLeg> &transfers, std::unordered_set<int64_t> &matched_indices);
  // commit_events: 提交 PendingEmit 列表, 更新仓位/历史/dirty_users_
  void commit_events(const json &log, const std::vector<PendingEmit> &events);
  bool user_visible_at(const std::string &user, uint64_t block_number) const;
  BigInt query_balance_at_block(const std::string &user, const std::string &token_id, uint64_t block_number);
  uint64_t rpc_block_number();
  json rpc_call(const std::string &method, const json &params);
  json rpc_batch(const std::vector<json> &reqs);

  const AppConfig &cfg_;
  AppState &shared_;
  EventQueue &queue_;
  WsThread &ws_;
  RuntimeState rt_;
  std::deque<QueueEvent> deferred_;
  std::vector<std::string> stale_users_; // 需要从 snapshot API 抓取 snapshot 的用户
  std::atomic<bool> resync_flag_{false};
  uint64_t current_session_id_ = 0;
  // apply_block_logs 期间使用的临时状态
  std::unordered_set<std::string> dirty_users_;
  std::unordered_set<std::string> dirty_conds_;
};

} // namespace tracker
