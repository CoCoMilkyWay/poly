#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>

namespace stage3 {
namespace {
constexpr const char *kSqlSelectUserSummaryRows =
    "SELECT lower(hex(user_addr)) AS user_hex, total_events, total_realized_pnl, total_unrealized_pnl ";
constexpr const char *kSqlOrderBySummaryEventsDesc = " ORDER BY total_events DESC ";

} // namespace

std::vector<StageSync::UserSummaryRow> StageSync::get_users_sorted(int64_t limit) const {
  TraceN("s3/users");
  auto connection = stage3_db_.create_connection();
  const int64_t query_limit = std::max<int64_t>(1, limit);
  auto query_result = connection->Query(
      std::string(kSqlSelectUserSummaryRows) + "FROM " +
      std::string(kSqlTableUserSummaryState) + " "
                                               + std::string(kSqlOrderBySummaryEventsDesc) +
                                               "LIMIT " +
      std::to_string(query_limit));
  assert(query_result && !query_result->HasError());
  std::vector<UserSummaryRow> rows;
  rows.reserve(static_cast<size_t>(query_result->RowCount()));
  for (idx_t i = 0; i < query_result->RowCount(); ++i) {
    std::string user_hex = query_result->GetValue(0, i).GetValueUnsafe<std::string>();
    rows.push_back({"0x" + user_hex,
                    query_result->GetValue(1, i).GetValue<int64_t>(),
                    query_result->GetValue(2, i).GetValue<int64_t>(),
                    query_result->GetValue(3, i).GetValue<int64_t>()});
  }
  return rows;
}

std::vector<StageSync::TimelineRow> StageSync::get_user_timeline(const std::string &addr) const {
  TraceN("s3/timeline");
  std::string addr_lower = normalize_addr(addr);
  if (addr_lower.empty()) {
    return {};
  }
  ensure_user_query_cache_state(addr_lower);
  std::lock_guard<std::mutex> lock(user_query_cache_mu_);
  return user_query_cache_state_.timeline;
}

std::vector<StageSync::PositionRow> StageSync::get_positions_at(const std::string &addr, int64_t sort_key) const {
  TraceN("s3/positions");
  std::string addr_lower = normalize_addr(addr);
  if (addr_lower.empty()) {
    return {};
  }
  ensure_user_query_cache_state(addr_lower);
  std::lock_guard<std::mutex> lock(user_query_cache_mu_);
  if (user_query_cache_state_.snapshots.empty()) {
    return {};
  }
  auto snapshot_it = std::upper_bound(
      user_query_cache_state_.snapshots.begin(),
      user_query_cache_state_.snapshots.end(),
      sort_key,
      [](int64_t sort_key, const UserQueryCacheState::PositionSnapshot &snapshot) { return sort_key < snapshot.sort_key; });
  if (snapshot_it == user_query_cache_state_.snapshots.begin()) {
    return {};
  }
  return std::prev(snapshot_it)->positions;
}

std::vector<StageSync::PositionRow>
StageSync::build_position_rows_from_states(const std::unordered_map<uint64_t, StageSync::TokenState> &states) const {
  std::vector<PositionRow> out;
  out.reserve(states.size());
  for (const auto &[packed_key, st] : states) {
    const int32_t cond_idx = static_cast<int32_t>(packed_key >> 32);
    const int32_t token_idx = static_cast<int32_t>(packed_key & 0xffffffffU);
    if (cond_idx < 0) {
      continue;
    }
    uint32_t uidx = static_cast<uint32_t>(cond_idx);
    assert(uidx < conditions_.size());
    const auto &cond = conditions_[uidx];
    if (std::abs(st.pos) <= kPosEpsilon) {
      continue;
    }
    const int64_t qty_i64 = round_i64(st.pos);
    if (!StageSync::is_effective_holding_i64(qty_i64)) {
      continue;
    }
    out.push_back(PositionRow{
        static_cast<uint32_t>(cond_idx),
        static_cast<uint8_t>(token_idx),
        static_cast<uint8_t>(cond.outcome_count),
        qty_i64,
        round_i64(st.cost),
        round_i64(st.lp),
        round_i64(st.entry_block),
    });
  }
  std::sort(out.begin(), out.end(), [](const PositionRow &a, const PositionRow &b) {
    if (a.cond_idx != b.cond_idx) {
      return a.cond_idx < b.cond_idx;
    }
    return a.token_idx < b.token_idx;
  });
  return out;
}

StageSync::UserQueryCacheState StageSync::build_user_query_cache_state(const std::string &addr_lower) const {
  UserQueryCacheState cache_state;
  cache_state.addr_lower = addr_lower;
  int64_t committed_sort_key = -1;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    committed_sort_key = sync_cursor_.sort_key;
  }
  if (committed_sort_key < 0) {
    return cache_state;
  }
  assert(addr_lower.size() == 42);
  const std::string user_blob = hex_to_blob(addr_lower);
  auto fact_rows = event_fact_store_->scan_by_user(user_blob);
  if (fact_rows.empty()) {
    return cache_state;
  }
  auto event_rows = builder_.user_event_store().scan_by_user(user_blob);

  cache_state.timeline.reserve(fact_rows.size());

  std::unordered_map<uint64_t, TokenState> token_states;
  token_states.reserve(cache_state.timeline.size() / 2 + 1);
  bool loaded_conditions_for_query = false;
  size_t event_idx = 0;
  auto compare_event_key = [](int64_t a_sort_key, int32_t a_cond_idx, int32_t a_event_type, int32_t a_token_idx,
                              int64_t b_sort_key, int32_t b_cond_idx, int32_t b_event_type, int32_t b_token_idx) {
    if (a_sort_key != b_sort_key)
      return (a_sort_key < b_sort_key) ? -1 : 1;
    if (a_cond_idx != b_cond_idx)
      return (a_cond_idx < b_cond_idx) ? -1 : 1;
    if (a_event_type != b_event_type)
      return (a_event_type < b_event_type) ? -1 : 1;
    if (a_token_idx != b_token_idx)
      return (a_token_idx < b_token_idx) ? -1 : 1;
    return 0;
  };
  for (size_t i = 0; i < fact_rows.size(); ++i) {
    const auto &fact = fact_rows[i];
    if (fact.sort_key > committed_sort_key) {
      break;
    }
    const int64_t fact_sort_key = fact.sort_key;
    const int32_t fact_cond_idx = fact.cond_idx;
    const int32_t fact_event_type = fact.event_type;
    const int32_t fact_token_idx = fact.token_idx;
    const int64_t fact_realized_cum = fact.realized_cum;
    const int64_t fact_unrealized_pnl = fact.unrealized_pnl;
    const int32_t fact_token_count = fact.token_count;

    int32_t event_collateral = 0;
    int64_t event_amount = 0;
    int64_t event_price = 0;
    while (event_idx < event_rows.size()) {
      const auto &event = event_rows[event_idx];
      int cmp = compare_event_key(event.sort_key,
                                  event.cond_idx,
                                  event.event_type,
                                  event.token_idx,
                                  fact_sort_key,
                                  fact_cond_idx,
                                  fact_event_type,
                                  fact_token_idx);
      if (cmp < 0) {
        event_idx++;
        continue;
      }
      if (cmp == 0) {
        event_collateral = event.collateral;
        event_amount = event.amount;
        event_price = event.price;
        event_idx++;
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
      if (static_cast<size_t>(fact_cond_idx) >= conditions_.size() && !loaded_conditions_for_query) {
        const_cast<StageSync *>(this)->load_conditions();
        loaded_conditions_for_query = true;
      }
      assert(static_cast<size_t>(fact_cond_idx) < conditions_.size());
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
      auto &st = token_states[packed_token_key];
      (void)apply_event_input(row, st);
    }
    const bool end_of_group =
        (i + 1 == fact_rows.size()) || (fact_rows[i + 1].sort_key != fact_sort_key);
    if (!end_of_group) {
      continue;
    }
    UserQueryCacheState::PositionSnapshot snap;
    snap.sort_key = fact_sort_key;
    snap.positions = build_position_rows_from_states(token_states);
    cache_state.snapshots.push_back(std::move(snap));
  }
  return cache_state;
}

void StageSync::ensure_user_query_cache_state(const std::string &addr_lower) const {
  {
    std::lock_guard<std::mutex> lock(user_query_cache_mu_);
    if (user_query_cache_state_.addr_lower == addr_lower) {
      return;
    }
  }

  UserQueryCacheState built_cache_state = build_user_query_cache_state(addr_lower);

  std::lock_guard<std::mutex> lock(user_query_cache_mu_);
  user_query_cache_state_ = std::move(built_cache_state);
}

} // namespace stage3
