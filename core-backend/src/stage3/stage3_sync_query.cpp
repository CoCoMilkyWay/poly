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
  auto conn = stage3_db_.create_connection();
  int64_t safe_limit = std::max<int64_t>(1, limit);
  auto r = conn->Query(
      std::string(kSqlSelectUserSummaryRows) + "FROM " +
      std::string(kSqlTableUserSummaryState) + " "
                                               + std::string(kSqlOrderBySummaryEventsDesc) +
                                               "LIMIT " +
      std::to_string(safe_limit));
  assert(r && !r->HasError());
  std::vector<UserSummaryRow> out;
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

std::vector<StageSync::TimelineRow> StageSync::get_user_timeline(const std::string &addr) const {
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

StageSync::UserQueryCache StageSync::build_user_query_cache(const std::string &addr_lower) const {
  UserQueryCache cache;
  cache.addr_lower = addr_lower;
  int64_t committed_sort_key = -1;
  {
    std::lock_guard<std::mutex> lock(sync_mu_);
    committed_sort_key = sync_cursor_.sort_key;
  }
  if (committed_sort_key < 0) {
    return cache;
  }
  assert(addr_lower.size() == 42);
  const std::string user_blob = hex_to_blob(addr_lower);
  auto fact_rows = event_fact_store_->scan_by_user(user_blob);
  if (fact_rows.empty()) {
    return cache;
  }
  auto ue_rows = builder_.user_event_store().scan_by_user(user_blob);

  cache.timeline.reserve(fact_rows.size());

  std::unordered_map<uint64_t, TokenState> states;
  states.reserve(cache.timeline.size() / 2 + 1);
  bool loaded_conditions_for_query = false;
  size_t ue_i = 0;
  auto key_cmp = [](int64_t ask, int32_t aci, int32_t aty, int32_t ati,
                    int64_t bsk, int32_t bci, int32_t bty, int32_t bti) {
    if (ask != bsk)
      return (ask < bsk) ? -1 : 1;
    if (aci != bci)
      return (aci < bci) ? -1 : 1;
    if (aty != bty)
      return (aty < bty) ? -1 : 1;
    if (ati != bti)
      return (ati < bti) ? -1 : 1;
    return 0;
  };
  for (size_t i = 0; i < fact_rows.size(); ++i) {
    const auto &fact = fact_rows[i];
    if (fact.sort_key > committed_sort_key) {
      break;
    }
    int64_t sk = fact.sort_key;
    int32_t ci = fact.cond_idx;
    int32_t ty = fact.event_type;
    int32_t ti = fact.token_idx;
    int64_t rpnl_cum = fact.realized_cum;
    int64_t upnl = fact.unrealized_pnl;
    int32_t token_count_at_event = fact.token_count;

    int32_t coll = 0;
    int64_t amt = 0;
    int64_t px = 0;
    while (ue_i < ue_rows.size()) {
      const auto &ue = ue_rows[ue_i];
      int64_t ue_sk = ue.sort_key;
      int32_t ue_ci = ue.cond_idx;
      int32_t ue_ty = ue.event_type;
      int32_t ue_ti = ue.token_idx;
      int cmp = key_cmp(ue_sk, ue_ci, ue_ty, ue_ti, sk, ci, ty, ti);
      if (cmp < 0) {
        ue_i++;
        continue;
      }
      if (cmp == 0) {
        coll = ue.collateral;
        amt = ue.amount;
        px = ue.price;
        ue_i++;
      }
      break;
    }

    cache.timeline.push_back({
        sk,
        ci,
        ti,
        static_cast<uint8_t>(ty),
        amt,
        px,
        rpnl_cum,
        upnl,
        token_count_at_event,
    });

    if (ci >= 0) {
      if (static_cast<size_t>(ci) >= conditions_.size() && !loaded_conditions_for_query) {
        const_cast<StageSync *>(this)->load_conditions();
        loaded_conditions_for_query = true;
      }
      assert(static_cast<size_t>(ci) < conditions_.size());
      EventInput row{
          0u,
          sk,
          ci,
          ty,
          ti,
          coll,
          amt,
          px,
      };
      const uint64_t packed_token_key = pack_cond_token_key(row.cond_idx, row.token_idx);
      auto &st = states[packed_token_key];
      (void)apply_event_input(row, st);
    }
    const bool end_of_group =
        (i + 1 == fact_rows.size()) || (fact_rows[i + 1].sort_key != sk);
    if (!end_of_group) {
      continue;
    }
    UserQueryCache::Snapshot snap;
    snap.sort_key = sk;
    snap.positions = build_position_rows_from_states(states);
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
