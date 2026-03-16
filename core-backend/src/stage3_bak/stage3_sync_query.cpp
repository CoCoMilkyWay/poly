#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace stage3 {
namespace {
constexpr const char *kSqlSelectUserSummaryRows =
    "SELECT lower(hex(user_addr)) AS user_hex, total_events, total_realized_pnl, total_unrealized_pnl ";
constexpr const char *kSqlOrderBySummaryEventsDesc = " ORDER BY total_events DESC ";

struct EventOrderKey {
  int64_t sort_key = 0;
  int32_t cond_idx = 0;
  int32_t event_type = 0;
  int32_t token_idx = 0;
};

int event_order_key_compare(const EventOrderKey &a, const EventOrderKey &b) {
  if (a.sort_key != b.sort_key) {
    return (a.sort_key < b.sort_key) ? -1 : 1;
  }
  if (a.cond_idx != b.cond_idx) {
    return (a.cond_idx < b.cond_idx) ? -1 : 1;
  }
  if (a.event_type != b.event_type) {
    return (a.event_type < b.event_type) ? -1 : 1;
  }
  if (a.token_idx != b.token_idx) {
    return (a.token_idx < b.token_idx) ? -1 : 1;
  }
  return 0;
}

} // namespace

std::vector<StageSync::UserSummaryRow> StageSync::get_users_sorted(int64_t limit) const {
  TraceN("s3/users");
  // L0: query execution
  auto query_connection = stage3_db_.create_connection();
  const int64_t query_limit = std::max<int64_t>(1, limit);
  auto summary_query_result = query_connection->Query(
      std::string(kSqlSelectUserSummaryRows) + "FROM " +
      std::string(kSqlTableUserSummaryState) + " " + std::string(kSqlOrderBySummaryEventsDesc) +
      "LIMIT " +
      std::to_string(query_limit));
  assert(summary_query_result && !summary_query_result->HasError());
  // L1: row materialization
  std::vector<UserSummaryRow> summary_rows;
  summary_rows.reserve(static_cast<size_t>(summary_query_result->RowCount()));
  for (idx_t i = 0; i < summary_query_result->RowCount(); ++i) {
    std::string user_hex = summary_query_result->GetValue(0, i).GetValueUnsafe<std::string>();
    summary_rows.push_back({"0x" + user_hex,
                            summary_query_result->GetValue(1, i).GetValue<int64_t>(),
                            summary_query_result->GetValue(2, i).GetValue<int64_t>(),
                            summary_query_result->GetValue(3, i).GetValue<int64_t>()});
  }
  return summary_rows;
}

std::vector<StageSync::TimelineRow> StageSync::get_user_timeline(const std::string &addr) const {
  TraceN("s3/timeline");
  // L0: addr normalize + cache ensure
  std::string user_addr_lower = normalize_addr(addr);
  if (user_addr_lower.empty()) {
    return {};
  }
  ensure_user_query_cache_state(user_addr_lower);
  std::lock_guard<std::mutex> lock(user_query_cache_mu_);
  return user_query_cache_state_.timeline;
}

std::vector<StageSync::PositionRow> StageSync::get_positions_at(const std::string &addr, int64_t sort_key) const {
  TraceN("s3/positions");
  // L0: addr normalize + cache ensure
  std::string user_addr_lower = normalize_addr(addr);
  if (user_addr_lower.empty()) {
    return {};
  }
  ensure_user_query_cache_state(user_addr_lower);
  std::lock_guard<std::mutex> lock(user_query_cache_mu_);
  if (user_query_cache_state_.snapshots.empty()) {
    return {};
  }
  // L1: locate snapshot by sort_key
  auto snapshot_upper_it = std::upper_bound(
      user_query_cache_state_.snapshots.begin(),
      user_query_cache_state_.snapshots.end(),
      sort_key,
      [](int64_t sort_key, const UserQueryCacheState::PositionSnapshot &snapshot) { return sort_key < snapshot.sort_key; });
  if (snapshot_upper_it == user_query_cache_state_.snapshots.begin()) {
    return {};
  }
  return std::prev(snapshot_upper_it)->positions;
}

StageSync::UserQueryCacheState StageSync::build_user_query_cache_state(const std::string &user_addr_lower) const {
  UserQueryCacheState cache_state;
  cache_state.addr_lower = user_addr_lower;

  // L0: committed cursor
  int64_t committed_sort_key = -1;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    committed_sort_key = sync_cursor_.sort_key;
  }
  if (committed_sort_key < 0) {
    return cache_state;
  }
  assert(user_addr_lower.size() == 42);
  const std::string user_blob = hex_to_blob(user_addr_lower);

  // L1: input records
  auto fact_records = event_fact_store_->scan_by_user(user_blob);
  if (fact_records.empty()) {
    return cache_state;
  }
  auto event_records = builder_.user_event_store().scan_by_user(user_blob);

  cache_state.timeline.reserve(fact_records.size());

  // L2: runtime state
  std::unordered_map<uint64_t, TokenState> token_state_by_token_key;
  token_state_by_token_key.reserve(fact_records.size() / 2 + 1);
  std::map<uint64_t, PositionRow> active_position_by_token_key;
  bool did_load_conditions = false;
  size_t event_record_idx = 0;

  // L3: local writers
  auto append_position_snapshot = [&](int64_t snapshot_sort_key) {
    UserQueryCacheState::PositionSnapshot snap;
    snap.sort_key = snapshot_sort_key;
    snap.positions.reserve(active_position_by_token_key.size());
    for (const auto &[_, pos] : active_position_by_token_key) {
      snap.positions.push_back(pos);
    }
    cache_state.snapshots.push_back(std::move(snap));
  };

  // L4: stream merge (fact_records + event_records)
  for (size_t fact_record_idx = 0; fact_record_idx < fact_records.size(); ++fact_record_idx) {
    const auto &fact_record = fact_records[fact_record_idx];
    if (fact_record.sort_key > committed_sort_key) {
      break;
    }
    const int64_t fact_sort_key = fact_record.sort_key;
    const int32_t fact_cond_idx = fact_record.cond_idx;
    const int32_t fact_event_type = fact_record.event_type;
    const int32_t fact_token_idx = fact_record.token_idx;
    const int64_t fact_realized_cum = fact_record.realized_cum;
    const int64_t fact_unrealized_pnl = fact_record.unrealized_pnl;
    const int32_t fact_token_count = fact_record.token_count;

    int32_t event_collateral = 0;
    int64_t event_amount = 0;
    int64_t event_price = 0;
    const EventOrderKey fact_key{fact_sort_key, fact_cond_idx, fact_event_type, fact_token_idx};
    while (event_record_idx < event_records.size()) {
      const auto &event_record = event_records[event_record_idx];
      const EventOrderKey event_key{
          event_record.sort_key,
          event_record.cond_idx,
          event_record.event_type,
          event_record.token_idx,
      };
      const int order_cmp = event_order_key_compare(event_key, fact_key);
      if (order_cmp < 0) {
        event_record_idx++;
        continue;
      }
      if (order_cmp == 0) {
        event_collateral = event_record.collateral;
        event_amount = event_record.amount;
        event_price = event_record.price;
        event_record_idx++;
      }
      break;
    }

    cache_state.timeline.push_back({
        fact_sort_key,
        fact_cond_idx,
        fact_token_idx,
        static_cast<uint8_t>(fact_event_type),
        event_amount,
        event_price,
        fact_realized_cum,
        fact_unrealized_pnl,
        fact_token_count,
    });

    if (fact_cond_idx >= 0) {
      if (static_cast<size_t>(fact_cond_idx) >= conditions_.size() && !did_load_conditions) {
        const_cast<StageSync *>(this)->load_conditions();
        did_load_conditions = true;
      }
      assert(static_cast<size_t>(fact_cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(fact_cond_idx)];
      EventInput row{
          0u,
          fact_sort_key,
          fact_cond_idx,
          fact_event_type,
          fact_token_idx,
          event_collateral,
          event_amount,
          event_price,
      };
      const uint64_t packed_token_key = pack_cond_token_key(row.cond_idx, row.token_idx);
      auto &st = token_state_by_token_key[packed_token_key];
      (void)apply_event_input(row, st);
      const int64_t qty_i64 = feature_comp::round_i64(st.pos);
      if (std::abs(st.pos) <= kPosEpsilon || !StageSync::is_effective_holding_i64(qty_i64)) {
        active_position_by_token_key.erase(packed_token_key);
      } else {
        active_position_by_token_key[packed_token_key] = PositionRow{
            static_cast<uint32_t>(fact_cond_idx),
            static_cast<uint8_t>(fact_token_idx),
            static_cast<uint8_t>(cond.outcome_count),
            qty_i64,
            feature_comp::round_i64(st.cost),
            feature_comp::round_i64(st.lp),
            feature_comp::round_i64(st.entry_block),
        };
      }
    }
    const bool end_of_group =
        (fact_record_idx + 1 == fact_records.size()) ||
        (fact_records[fact_record_idx + 1].sort_key != fact_sort_key);
    if (!end_of_group) {
      continue;
    }
    append_position_snapshot(fact_sort_key);
  }
  return cache_state;
}

void StageSync::ensure_user_query_cache_state(const std::string &user_addr_lower) const {
  {
    std::lock_guard<std::mutex> lock(user_query_cache_mu_);
    if (user_query_cache_state_.addr_lower == user_addr_lower) {
      return;
    }
  }

  UserQueryCacheState next_cache_state = build_user_query_cache_state(user_addr_lower);

  std::lock_guard<std::mutex> lock(user_query_cache_mu_);
  user_query_cache_state_ = std::move(next_cache_state);
}

} // namespace stage3
