#include "stage3_sync.hpp"
namespace stage3 {

std::string StageSync::normalize_addr(const std::string &addr) {
  std::string lower = to_lower(addr);
  if (lower.rfind("0x", 0) != 0 || lower.size() != 42) {
    return {};
  }
  return lower;
}

int64_t StageSync::mul_div_1e6(int64_t amount, int64_t price) {
  assert(price >= 0);
  __int128 v = static_cast<__int128>(amount) * static_cast<__int128>(price);
  v /= 1000000;
  assert(v >= std::numeric_limits<int64_t>::min() && v <= std::numeric_limits<int64_t>::max());
  return static_cast<int64_t>(v);
}

int64_t StageSync::add_i64_checked(int64_t a, int64_t b) {
  __int128 v = static_cast<__int128>(a) + static_cast<__int128>(b);
  assert(v >= std::numeric_limits<int64_t>::min() && v <= std::numeric_limits<int64_t>::max());
  return static_cast<int64_t>(v);
}

int64_t StageSync::sub_i64_checked(int64_t a, int64_t b) {
  __int128 v = static_cast<__int128>(a) - static_cast<__int128>(b);
  assert(v >= std::numeric_limits<int64_t>::min() && v <= std::numeric_limits<int64_t>::max());
  return static_cast<int64_t>(v);
}

int64_t StageSync::convert_payout_amount(const ConditionInfo &cond, int64_t qty) const {
  int64_t popcount = 0;
  for (int64_t p : cond.payout_numerators) {
    if (p > 0) {
      ++popcount;
    }
  }
  assert(popcount > 0);
  __int128 proceeds = static_cast<__int128>(qty) * static_cast<__int128>(popcount - 1);
  proceeds /= popcount;
  assert(proceeds >= std::numeric_limits<int64_t>::min() && proceeds <= std::numeric_limits<int64_t>::max());
  return static_cast<int64_t>(proceeds);
}

bool StageSync::is_trade_event(EventType ty) {
  return ty == EventType::OrderBuy || ty == EventType::OrderSell ||
         ty == EventType::FPMMBuy || ty == EventType::FPMMSell;
}

bool StageSync::is_usd_collateral(int32_t collateral) {
  return collateral == static_cast<int32_t>(Collateral::USDC) ||
         collateral == static_cast<int32_t>(Collateral::USDCe) ||
         collateral == static_cast<int32_t>(Collateral::USDT) ||
         collateral == static_cast<int32_t>(Collateral::WrappedUSDCe);
}

int64_t StageSync::compute_unrealized_pnl(const CondState &st) {
  __int128 sum = 0;
  for (int j = 0; j < MAX_OUTCOMES; ++j) {
    if (st.positions[j] <= 0 || st.last_price[j] <= 0) {
      continue;
    }
    __int128 mtm = static_cast<__int128>(st.positions[j]) * static_cast<__int128>(st.last_price[j]);
    mtm /= 1000000;
    sum += mtm - static_cast<__int128>(st.cost[j]);
  }
  assert(sum >= std::numeric_limits<int64_t>::min() && sum <= std::numeric_limits<int64_t>::max());
  return static_cast<int64_t>(sum);
}

int64_t StageSync::apply_event_to_state(const InputEvent &row, CondState &st) const {
  assert(row.cond_idx >= 0);
  assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
  const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
  assert(cond.outcome_count > 0 && cond.outcome_count <= MAX_OUTCOMES);
  assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
  int i = row.token_idx;
  EventType ty = static_cast<EventType>(row.event_type);
  bool has_usd = is_usd_collateral(row.collateral);
  if (is_trade_event(ty) && row.price > 0 && has_usd) {
    st.last_price[i] = row.price;
  }

  auto buy_qty = [&]() {
    assert(row.amount >= 0);
    return row.amount;
  };
  auto sell_qty = [&]() {
    assert(row.amount <= 0);
    return -row.amount;
  };
  // Remove position and cost proportionally; returns cost_removed (0 if non-USD).
  auto remove_cost = [&](int64_t qty) {
    assert(qty > 0);
    int64_t pos = st.positions[i];
    assert(pos > 0 && pos >= qty);
    int64_t cost_removed = 0;
    if (has_usd) {
      __int128 removed = static_cast<__int128>(st.cost[i]) * static_cast<__int128>(qty);
      removed /= pos;
      assert(removed >= std::numeric_limits<int64_t>::min() && removed <= std::numeric_limits<int64_t>::max());
      cost_removed = static_cast<int64_t>(removed);
      st.cost[i] = sub_i64_checked(st.cost[i], cost_removed);
    }
    st.positions[i] = sub_i64_checked(st.positions[i], qty);
    return cost_removed;
  };
  // Sell with price -> realized PnL (only for USD collateral).
  auto apply_sell_by_price = [&]() {
    int64_t qty = sell_qty();
    int64_t cost_removed = remove_cost(qty);
    if (!has_usd) {
      return static_cast<int64_t>(0);
    }
    int64_t proceeds = mul_div_1e6(qty, row.price);
    int64_t realized_delta = sub_i64_checked(proceeds, cost_removed);
    st.realized_pnl = add_i64_checked(st.realized_pnl, realized_delta);
    return realized_delta;
  };

  switch (ty) {
  case EventType::OrderBuy:
  case EventType::FPMMBuy:
  case EventType::SplitNormal:
  case EventType::SplitNegRisk:
  case EventType::SplitNonPoly: {
    int64_t qty = buy_qty();
    st.positions[i] = add_i64_checked(st.positions[i], qty);
    if (has_usd) {
      st.cost[i] = add_i64_checked(st.cost[i], mul_div_1e6(qty, row.price));
    }
    return 0;
  }
  case EventType::FPMMLPAdd:
    // LP 添加流动性：0->FPMM mint，用户没收到 token，不改 pos
    assert(row.amount >= 0);
    return 0;
  case EventType::FPMMLPRemove:
  case EventType::FPMMLPReturn: {
    // LP 移除/返还：FPMM->用户，用户取回 token，计入持仓但不增加 cost
    int64_t qty = buy_qty();
    st.positions[i] = add_i64_checked(st.positions[i], qty);
    return 0;
  }
  case EventType::TransferInNegRisk:
  case EventType::TransferInOther:
  case EventType::TransferInNonPoly: {
    int64_t qty = buy_qty();
    st.positions[i] = add_i64_checked(st.positions[i], qty);
    return 0;
  }
  case EventType::OrderSell:
  case EventType::FPMMSell:
  case EventType::MergeNormal:
  case EventType::MergeNegRisk:
  case EventType::MergeNonPoly: {
    return apply_sell_by_price();
  }
  case EventType::Redemption:
  case EventType::RedemptionNonPoly: {
    return apply_sell_by_price();
  }
  case EventType::Convert: {
    int64_t qty = sell_qty();
    int64_t cost_removed = remove_cost(qty);
    if (!has_usd) {
      return 0;
    }
    int64_t proceeds = convert_payout_amount(cond, qty);
    int64_t realized_delta = sub_i64_checked(proceeds, cost_removed);
    st.realized_pnl = add_i64_checked(st.realized_pnl, realized_delta);
    return realized_delta;
  }
  case EventType::TransferOutNegRisk:
  case EventType::TransferOutOther:
  case EventType::TransferOutNonPoly: {
    int64_t qty = sell_qty();
    (void)remove_cost(qty);
    return 0;
  }
  default:
    assert(false);
    return 0;
  }
}

} // namespace stage3
