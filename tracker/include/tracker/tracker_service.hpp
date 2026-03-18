#pragma once

#include "tracker/common.hpp"
#include "tracker/config.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <unordered_set>

namespace tracker {

struct StableBalances {
  BigInt usdc_e_raw = 0;
  BigInt wrapped_collateral_raw = 0;
};

struct ConditionMeta {
  std::string condition_id;
  std::string question_id;
  int outcome_slot_count = -1;
  int64_t resolution_timestamp = -1;
  std::vector<std::string> token_ids;
  std::vector<BigInt> payout_numerators;
  BigInt payout_denominator = 0;
  bool has_payout_denominator = false;
  std::string market_question;
  std::string market_description;
  std::string market_event_title;
  std::string market_slug;
  std::string market_url;
  std::vector<std::string> market_outcomes;
};

struct TokenMeta {
  std::string token_id;
  std::string condition_id;
  std::string question_id;
  int outcome_index = -1;
  int outcome_slot_count = -1;
  int64_t resolution_timestamp = -1;
  std::vector<BigInt> payout_numerators;
  BigInt payout_denominator = 0;
  bool has_payout_denominator = false;
  long double price = -1.0L;
  std::string price_source;
};

struct UserRuntimeState {
  std::string user;
  std::map<std::string, BigInt> token_amounts;
  StableBalances stable_balances;
};

struct QueryCounters {
  uint64_t rpc_http_calls = 0;
  uint64_t rpc_ws_messages = 0;
  uint64_t rpc_ws_subscriptions = 0;
  uint64_t subgraph_queries = 0;
  uint64_t gamma_queries = 0;
};

class TrackerService {
public:
  explicit TrackerService(AppConfig config);

  void bootstrap();
  void run_forever();
  void request_resync();

  json current_state_json();
  json meta_json();
  json history_json_for_user(const std::string &user);
  json health_json();

private:
  void load_seed_meta();
  void load_persisted_files();
  void restore_current_state_from_aggregate(const json &payload);
  void append_full_snapshot_locked(uint64_t block_number);
  void persist_meta_locked();
  void persist_aggregate_locked();
  void persist_snapshot_locked();
  void persist_history_locked();
  void persist_all_locked();
  json build_current_state_json_locked();
  json build_meta_json_locked();

  bool has_current_state_locked() const;
  bool watched_users_changed_locked(const std::vector<std::string> &users) const;
  void set_watched_users_locked(const std::vector<std::string> &users);

  void bootstrap_from_persisted();
  void full_resync();
  void run_live_until(std::chrono::steady_clock::time_point deadline);
  void backfill_range(uint64_t from_block, uint64_t to_block);
  void apply_log_blocks(std::map<uint64_t, std::map<std::string, json>> blocks);
  void refresh_reference_data(bool missing_only);
  void refresh_stable_balances(const std::string &block_tag);
  std::vector<json> build_log_filters(const std::vector<std::string> &users,
                                      const std::optional<uint64_t> &from_block,
                                      const std::optional<uint64_t> &to_block) const;
  bool apply_block_logs(const std::vector<json> &raw_logs, std::set<std::string> &touched_token_ids);

  uint64_t rpc_block_number();
  json rpc_call(const std::string &method, const json &params);
  json rpc_batch_call(const std::vector<json> &requests);
  json graph_query(const std::string &subgraph_id, const std::string &query, const json &variables, const std::string &label);

  AppConfig config_;
  mutable std::mutex mutex_;
  std::vector<std::string> watched_users_;
  std::unordered_set<std::string> watched_user_set_;
  std::map<std::string, UserRuntimeState> users_;
  std::map<std::string, TokenMeta> tokens_;
  std::map<std::string, ConditionMeta> conditions_;
  std::deque<json> recent_events_;
  json snapshot_root_;  // 用户持仓快照 (key: user -> block_num -> snapshot)
  json history_root_;   // 交易记录 (key: user -> block_num -> events)
  QueryCounters query_counters_;
  uint64_t last_snapshot_block_ = 0;
  uint64_t last_applied_block_ = 0;
  uint64_t head_block_ = 0;
  int64_t last_resync_started_at_ = 0;
  int64_t last_resync_finished_at_ = 0;
  std::atomic<bool> force_resync_{false};
};

} // namespace tracker
