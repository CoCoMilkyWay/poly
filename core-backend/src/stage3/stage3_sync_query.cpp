#include "misc/profiler.hpp"
#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>

namespace stage3 {
namespace {
std::string user_addr_sql(const std::string &addr_lower) {
  assert(addr_lower.size() == 42);
  return "from_hex('" + addr_lower.substr(2) + "')";
}

} // namespace

std::vector<StageSync::UserSummary> StageSync::get_users_sorted(int64_t limit) const {
  TraceN("s3/users");
  auto conn = stage3_db_.create_connection();
  int64_t safe_limit = std::max<int64_t>(1, limit);
  auto r = conn->Query(
      "SELECT lower(hex(user_addr)) AS user_hex, total_events, total_realized_pnl, total_unrealized_pnl "
      "FROM s3_user_summary "
      "ORDER BY total_events DESC "
      "LIMIT " +
      std::to_string(safe_limit));
  assert(r && !r->HasError());
  std::vector<UserSummary> out;
  out.reserve(static_cast<size_t>(r->RowCount()));
  for (idx_t i = 0; i < r->RowCount(); ++i) {
    std::string hx = r->GetValue(0, i).GetValueUnsafe<std::string>();
    out.push_back({"0x" + hx,
                   r->GetValue(1, i).GetValue<int64_t>(),
                   r->GetValue(2, i).GetValue<int64_t>(),
                   r->GetValue(3, i).GetValue<int64_t>()});
  }
  return out;
}

std::vector<StageSync::TimelineEntry> StageSync::get_user_timeline(const std::string &addr) const {
  TraceN("s3/timeline");
  std::string lower = normalize_addr(addr);
  if (lower.empty()) {
    return {};
  }
  ensure_user_query_cache(lower);
  std::lock_guard<std::mutex> lock(user_cache_mu_);
  return user_cache_.timeline;
}

std::vector<StageSync::PositionRow> StageSync::get_positions_at(const std::string &addr, int64_t sort_key) const {
  TraceN("s3/positions");
  std::string lower = normalize_addr(addr);
  if (lower.empty()) {
    return {};
  }
  ensure_user_query_cache(lower);
  std::lock_guard<std::mutex> lock(user_cache_mu_);
  if (user_cache_.snapshots.empty()) {
    return {};
  }
  auto it = std::upper_bound(
      user_cache_.snapshots.begin(),
      user_cache_.snapshots.end(),
      sort_key,
      [](int64_t sk, const UserQueryCache::Snapshot &snap) { return sk < snap.sort_key; });
  if (it == user_cache_.snapshots.begin()) {
    return {};
  }
  return std::prev(it)->positions;
}

std::vector<StageSync::PositionRow>
StageSync::build_positions_from_states(const std::unordered_map<int32_t, CondState> &states) const {
  std::vector<PositionRow> out;
  out.reserve(states.size() * 2);
  for (const auto &[cond_idx, st] : states) {
    if (cond_idx < 0) {
      continue;
    }
    uint32_t uidx = static_cast<uint32_t>(cond_idx);
    assert(uidx < conditions_.size());
    const auto &cond = conditions_[uidx];
    for (int i = 0; i < cond.outcome_count; ++i) {
      if (std::abs(st.positions[i]) <= kPosEpsilon) {
        continue;
      }
      const int64_t qty_i64 = round_i64(st.positions[i]);
      if (!StageSync::is_effective_holding_i64(qty_i64)) {
        continue;
      }
      out.push_back(PositionRow{
          static_cast<uint32_t>(cond_idx),
          static_cast<uint8_t>(i),
          static_cast<uint8_t>(cond.outcome_count),
          qty_i64,
          round_i64(st.cost[i]),
          round_i64(st.last_price[i]),
      });
    }
  }
  std::sort(out.begin(), out.end(), [](const PositionRow &a, const PositionRow &b) {
    if (a.cond_idx != b.cond_idx) {
      return a.cond_idx < b.cond_idx;
    }
    return a.token_idx < b.token_idx;
  });
  return out;
}

StageSync::UserQueryCache StageSync::build_user_query_cache(const std::string &addr_lower) const {
  UserQueryCache cache;
  cache.addr_lower = addr_lower;
  const std::string user_addr = user_addr_sql(addr_lower);
  auto fact_conn = stage3_db_.create_connection();
  auto fact = fact_conn->Query(
      "SELECT sort_key, cond_idx, event_type, token_idx, realized_cum, unrealized_pnl, token_count "
      "FROM s3_user_event_fact "
      "WHERE user_addr = " +
      user_addr + " "
                  "ORDER BY sort_key, cond_idx, event_type, token_idx");
  assert(fact && !fact->HasError());
  if (fact->RowCount() == 0) {
    return cache;
  }
  auto src_conn = stage2_db_.create_connection();
  auto ue = src_conn->Query(
      "SELECT sort_key, cond_idx, event_type, token_idx, collateral, amount, price "
      "FROM user_event "
      "WHERE user_addr = " +
      user_addr + " "
                  "ORDER BY sort_key, cond_idx, event_type, token_idx");
  assert(ue && !ue->HasError());
  assert(ue->RowCount() == fact->RowCount());

  cache.timeline.reserve(static_cast<size_t>(fact->RowCount()));

  std::unordered_map<int32_t, CondState> states;
  states.reserve(cache.timeline.size() / 2 + 1);
  bool loaded_conditions_for_query = false;
  for (idx_t i = 0; i < fact->RowCount(); ++i) {
    int64_t sk = fact->GetValue(0, i).GetValue<int64_t>();
    int32_t ci = fact->GetValue(1, i).GetValue<int32_t>();
    int32_t ty = fact->GetValue(2, i).GetValue<int32_t>();
    int32_t ti = fact->GetValue(3, i).GetValue<int32_t>();
    int64_t rpnl_cum = fact->GetValue(4, i).GetValue<int64_t>();
    int64_t upnl = fact->GetValue(5, i).GetValue<int64_t>();
    int32_t tk = fact->GetValue(6, i).GetValue<int32_t>();

    int64_t ue_sk = ue->GetValue(0, i).GetValue<int64_t>();
    int32_t ue_ci = ue->GetValue(1, i).GetValue<int32_t>();
    int32_t ue_ty = ue->GetValue(2, i).GetValue<int32_t>();
    int32_t ue_ti = ue->GetValue(3, i).GetValue<int32_t>();
    assert(sk == ue_sk && ci == ue_ci && ty == ue_ty && ti == ue_ti);
    int32_t coll = ue->GetValue(4, i).GetValue<int32_t>();
    int64_t amt = ue->GetValue(5, i).GetValue<int64_t>();
    int64_t px = ue->GetValue(6, i).GetValue<int64_t>();

    cache.timeline.push_back({
        sk,
        ci,
        ti,
        static_cast<uint8_t>(ty),
        amt,
        px,
        rpnl_cum,
        upnl,
        tk,
    });

    if (ci >= 0) {
      if (static_cast<size_t>(ci) >= conditions_.size() && !loaded_conditions_for_query) {
        const_cast<StageSync *>(this)->load_conditions();
        loaded_conditions_for_query = true;
      }
      assert(static_cast<size_t>(ci) < conditions_.size());
      InputEvent row{
          "",
          sk,
          ci,
          ty,
          ti,
          coll,
          amt,
          px,
      };
      auto &st = states[row.cond_idx];
      (void)apply_event_to_state(row, st);
      st.unrealized_pnl = compute_unrealized_pnl(st);
    }
    const bool end_of_group =
        (i + 1 == fact->RowCount()) || (fact->GetValue(0, i + 1).GetValue<int64_t>() != sk);
    if (!end_of_group) {
      continue;
    }
    UserQueryCache::Snapshot snap;
    snap.sort_key = sk;
    snap.positions = build_positions_from_states(states);
    cache.snapshots.push_back(std::move(snap));
  }
  return cache;
}

void StageSync::ensure_user_query_cache(const std::string &addr_lower) const {
  {
    std::lock_guard<std::mutex> lock(user_cache_mu_);
    if (user_cache_.addr_lower == addr_lower) {
      return;
    }
  }

  UserQueryCache built = build_user_query_cache(addr_lower);

  std::lock_guard<std::mutex> lock(user_cache_mu_);
  user_cache_ = std::move(built);
}

} // namespace stage3
