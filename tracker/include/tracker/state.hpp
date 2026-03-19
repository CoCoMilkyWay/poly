#pragma once

#include "tracker/codec.hpp"
#include "tracker/json.hpp"

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace tracker {

// ============================================================================
// Meta Structures
// ============================================================================

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

// ============================================================================
// User State
// ============================================================================

struct StableBalances {
  BigInt usdc_e = 0;
  BigInt wrapped = 0;
};

struct UserState {
  std::string user;
  std::map<std::string, BigInt> positions;  // token_id -> amount
  StableBalances stable;
};

// ============================================================================
// Query Counters
// ============================================================================

struct QueryCounters {
  uint64_t rpc_http = 0;
  uint64_t rpc_ws_msg = 0;
  uint64_t rpc_ws_sub = 0;
  uint64_t subgraph = 0;
  uint64_t gamma = 0;
};

// ============================================================================
// AppState - 所有可变状态
// ============================================================================

struct AppState {
  mutable std::mutex mu;

  // watched users
  std::vector<std::string> users;
  std::unordered_set<std::string> user_set;

  // per-user runtime
  std::map<std::string, UserState> user_states;

  // meta
  std::map<std::string, TokenMeta> tokens;
  std::map<std::string, ConditionMeta> conditions;

  // history (persisted)
  json snapshot_root = json::object();  // user -> block_key -> snapshot
  json history_root = json::object();   // user -> block_key -> events[]

  // recent events (in-memory ring buffer)
  std::deque<json> recent_events;

  // block tracking
  uint64_t snapshot_block = 0;
  uint64_t applied_block = 0;
  uint64_t head_block = 0;

  // timestamps
  int64_t resync_started_at = 0;
  int64_t resync_finished_at = 0;

  // counters
  QueryCounters counters;
};

// ============================================================================
// Helpers
// ============================================================================

inline void merge_condition(ConditionMeta &dst, const ConditionMeta &src) {
  if (dst.condition_id.empty()) dst.condition_id = src.condition_id;
  if (dst.question_id.empty()) dst.question_id = src.question_id;
  if (dst.outcome_slot_count < 0) dst.outcome_slot_count = src.outcome_slot_count;
  if (dst.resolution_timestamp < 0) dst.resolution_timestamp = src.resolution_timestamp;
  if (dst.token_ids.empty()) dst.token_ids = src.token_ids;
  if (dst.payout_numerators.empty()) dst.payout_numerators = src.payout_numerators;
  if (!dst.has_payout_denominator && src.has_payout_denominator) {
    dst.payout_denominator = src.payout_denominator;
    dst.has_payout_denominator = true;
  }
  if (dst.market_question.empty()) dst.market_question = src.market_question;
  if (dst.market_description.empty()) dst.market_description = src.market_description;
  if (dst.market_event_title.empty()) dst.market_event_title = src.market_event_title;
  if (dst.market_slug.empty()) dst.market_slug = src.market_slug;
  if (dst.market_url.empty()) dst.market_url = src.market_url;
  if (dst.market_outcomes.empty()) dst.market_outcomes = src.market_outcomes;
}

inline void merge_token(TokenMeta &dst, const TokenMeta &src) {
  if (dst.token_id.empty()) dst.token_id = src.token_id;
  if (dst.condition_id.empty()) dst.condition_id = src.condition_id;
  if (dst.question_id.empty()) dst.question_id = src.question_id;
  if (dst.outcome_index < 0) dst.outcome_index = src.outcome_index;
  if (dst.outcome_slot_count < 0) dst.outcome_slot_count = src.outcome_slot_count;
  if (dst.resolution_timestamp < 0) dst.resolution_timestamp = src.resolution_timestamp;
  if (dst.payout_numerators.empty()) dst.payout_numerators = src.payout_numerators;
  if (!dst.has_payout_denominator && src.has_payout_denominator) {
    dst.payout_denominator = src.payout_denominator;
    dst.has_payout_denominator = true;
  }
  if (src.price >= 0.0L) {
    dst.price = src.price;
    dst.price_source = src.price_source;
  }
}

} // namespace tracker
