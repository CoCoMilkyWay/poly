#include "misc/profiler.hpp"
#include "stage3_sync.hpp"

namespace stage3 {

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

std::vector<StageSync::TimelineEntry> StageSync::get_user_timeline(const std::string &addr,
                                                                   int64_t max_sort_key) const {
  TraceN("s3/timeline");
  std::string lower = normalize_addr(addr);
  if (lower.empty()) {
    return {};
  }
  auto events = load_timeline_events(lower, max_sort_key);
  if (events.empty()) {
    return {};
  }
  return build_timeline(events);
}

std::vector<StageSync::PositionRow> StageSync::get_positions_at(const std::string &addr, int64_t sort_key) const {
  TraceN("s3/positions");
  std::string lower = normalize_addr(addr);
  if (lower.empty()) {
    return {};
  }
  auto state = build_state_until(lower, sort_key);
  std::vector<PositionRow> out;
  out.reserve(state.size() * 2);
  for (const auto &[cond_idx, st] : state) {
    if (cond_idx < 0) {
      continue;
    }
    uint32_t uidx = static_cast<uint32_t>(cond_idx);
    assert(uidx < conditions_.size());
    const auto &cond = conditions_[uidx];
    for (int i = 0; i < cond.outcome_count; ++i) {
      if (st.positions[i] <= kPosEpsilon) {
        continue;
      }
      out.push_back(PositionRow{
          static_cast<uint32_t>(cond_idx),
          static_cast<uint8_t>(i),
          round_i64(st.positions[i]),
          round_i64(st.cost[i]),
          round_i64(st.last_price[i]),
      });
    }
  }
  return out;
}

std::vector<StageSync::TimelineEvent> StageSync::load_timeline_events(const std::string &addr_lower,
                                                                      int64_t max_sort_key) const {
  auto conn = stage3_db_.create_connection();
  std::string hex40 = addr_lower.substr(2);
  auto q = conn->Query(
      "SELECT sort_key, cond_idx, event_type, token_idx, realized_cum, unrealized_pnl, token_count "
      "FROM s3_user_event_fact "
      "WHERE user_addr = from_hex('" +
      hex40 + "') "
              "AND sort_key <= " +
      std::to_string(max_sort_key) + " "
                                     "ORDER BY sort_key, cond_idx, event_type, token_idx");
  assert(q && !q->HasError());
  std::vector<TimelineEvent> out;
  out.reserve(static_cast<size_t>(q->RowCount()));
  for (idx_t i = 0; i < q->RowCount(); ++i) {
    out.push_back({
        q->GetValue(0, i).GetValue<int64_t>(),
        q->GetValue(1, i).GetValue<int32_t>(),
        q->GetValue(2, i).GetValue<int32_t>(),
        q->GetValue(3, i).GetValue<int32_t>(),
        q->GetValue(4, i).GetValue<int64_t>(),
        q->GetValue(5, i).GetValue<int64_t>(),
        q->GetValue(6, i).GetValue<int32_t>(),
    });
  }
  return out;
}

std::vector<StageSync::TimelineEntry> StageSync::build_timeline(const std::vector<TimelineEvent> &events) const {
  std::vector<TimelineEntry> timeline;
  timeline.reserve(events.size());
  for (const auto &row : events) {
    timeline.push_back({row.sort_key, static_cast<uint8_t>(row.event_type), row.realized_cum,
                        row.unrealized_pnl, row.token_count});
  }
  return timeline;
}

std::unordered_map<int32_t, StageSync::CondState> StageSync::build_state_until(const std::string &addr_lower,
                                                                               int64_t target_sort_key) const {
  auto conn = stage3_db_.create_connection();
  std::string hex40 = addr_lower.substr(2);
  std::unordered_map<int32_t, CondState> states;
  int64_t checkpoint_sort_key = -1;

  auto ck = conn->Query(
      "SELECT MAX(checkpoint_sort_key) "
      "FROM s3_user_cond_checkpoint "
      "WHERE user_addr = from_hex('" +
      hex40 + "') "
              "AND checkpoint_sort_key <= " +
      std::to_string(target_sort_key));
  assert(ck && !ck->HasError() && ck->RowCount() == 1);
  if (!ck->GetValue(0, 0).IsNull()) {
    checkpoint_sort_key = ck->GetValue(0, 0).GetValue<int64_t>();
  }

  if (checkpoint_sort_key >= 0) {
    auto base = conn->Query(
        "SELECT cond_idx, "
        "pos_0,pos_1,pos_2,pos_3,pos_4,pos_5,pos_6,pos_7, "
        "cost_0,cost_1,cost_2,cost_3,cost_4,cost_5,cost_6,cost_7, "
        "lp_0,lp_1,lp_2,lp_3,lp_4,lp_5,lp_6,lp_7, "
        "realized_pnl,event_count,last_sort_key "
        "FROM s3_user_cond_checkpoint "
        "WHERE user_addr = from_hex('" +
        hex40 + "') "
                "AND checkpoint_sort_key = " +
        std::to_string(checkpoint_sort_key));
    assert(base && !base->HasError());
    states.reserve(static_cast<size_t>(base->RowCount()));
    for (idx_t i = 0; i < base->RowCount(); ++i) {
      int32_t cond_idx = base->GetValue(0, i).GetValue<int32_t>();
      CondState st;
      load_cond_state_values(st, *base, i, 1, 9, 17, 25, 26, 27);
      st.unrealized_pnl = compute_unrealized_pnl(st);
      states.emplace(cond_idx, st);
    }
  }

  auto src_conn = stage2_db_.create_connection();
  auto delta = src_conn->Query(
      "SELECT sort_key, cond_idx, event_type, token_idx, collateral, amount, price "
      "FROM user_event "
      "WHERE user_addr = from_hex('" +
      hex40 + "') "
              "AND sort_key > " +
      std::to_string(checkpoint_sort_key) + " "
                                            "AND sort_key <= " +
      std::to_string(target_sort_key) + " "
                                        "ORDER BY sort_key, cond_idx, event_type, token_idx");
  assert(delta && !delta->HasError());
  int32_t max_cond_idx = -1;
  for (idx_t i = 0; i < delta->RowCount(); ++i) {
    int32_t cond_idx = delta->GetValue(1, i).GetValue<int32_t>();
    if (cond_idx > max_cond_idx) {
      max_cond_idx = cond_idx;
    }
  }
  if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
    const_cast<StageSync *>(this)->load_conditions();
  }
  bool has_prev_key = false;
  int64_t prev_sort_key = 0;
  int32_t prev_cond_idx = kCursorSentinel;
  int32_t prev_event_type = kCursorSentinel;
  int32_t prev_token_idx = kCursorSentinel;
  for (idx_t i = 0; i < delta->RowCount(); ++i) {
    InputEvent row{
        "",
        delta->GetValue(0, i).GetValue<int64_t>(), // sort_key
        delta->GetValue(1, i).GetValue<int32_t>(), // cond_idx
        delta->GetValue(2, i).GetValue<int32_t>(), // event_type
        delta->GetValue(3, i).GetValue<int32_t>(), // token_idx
        delta->GetValue(4, i).GetValue<int32_t>(), // collateral
        delta->GetValue(5, i).GetValue<int64_t>(), // amount
        delta->GetValue(6, i).GetValue<int64_t>(), // price
    };
    if (has_prev_key) {
      assert(
          row.sort_key > prev_sort_key ||
          (row.sort_key == prev_sort_key &&
           (row.cond_idx > prev_cond_idx ||
            (row.cond_idx == prev_cond_idx &&
             (row.event_type > prev_event_type ||
              (row.event_type == prev_event_type && row.token_idx > prev_token_idx))))));
    }
    has_prev_key = true;
    prev_sort_key = row.sort_key;
    prev_cond_idx = row.cond_idx;
    prev_event_type = row.event_type;
    prev_token_idx = row.token_idx;
    if (row.cond_idx < 0) {
      continue;
    }
    auto &st = states[row.cond_idx];
    (void)apply_event_to_state(row, st);
    st.unrealized_pnl = compute_unrealized_pnl(st);
  }
  return states;
}

} // namespace stage3
