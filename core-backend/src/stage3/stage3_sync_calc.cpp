#include "stage3_sync.hpp"
#include <cmath>

namespace stage3 {

std::string StageSync::normalize_addr(const std::string &addr) {
  std::string lower = to_lower(addr);
  if (lower.rfind("0x", 0) != 0 || lower.size() != 42) {
    return {};
  }
  return lower;
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

// Compute unrealized PnL using double arithmetic
// unrealized = Σ(pos * last_price / 1e6 - cost) for all outcomes with pos > 0
double StageSync::compute_unrealized_pnl(const CondState &st) {
  double sum = 0.0;
  for (int j = 0; j < MAX_OUTCOMES; ++j) {
    if (st.positions[j] <= 0.0 || st.last_price[j] <= 0.0) {
      continue;
    }
    double mtm = st.positions[j] * st.last_price[j] / 1e6;
    sum += mtm - st.cost[j];
  }
  return sum;
}

// Apply event to state using double arithmetic
// Returns realized_delta for this event
double StageSync::apply_event_to_state(const InputEvent &row, CondState &st) const {
  assert(row.cond_idx >= 0);
  assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
  const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
  assert(cond.outcome_count > 0 && cond.outcome_count <= MAX_OUTCOMES);
  assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);

  int i = row.token_idx;
  EventType ty = static_cast<EventType>(row.event_type);
  bool has_usd = is_usd_collateral(row.collateral);

  // Update last_price for trade events
  if (is_trade_event(ty) && row.price > 0 && has_usd) {
    st.last_price[i] = static_cast<double>(row.price);
  }

  double qty = std::abs(static_cast<double>(row.amount));
  double px = static_cast<double>(row.price) / 1e6; // Convert from 1e6 to actual price

  // Remove position and cost proportionally, returns cost_removed
  auto remove_cost = [&]() -> double {
    assert(qty > 0.0);
    double pos = st.positions[i];
    assert(pos > 0.0 && pos >= qty - kPosEpsilon);
    double cost_removed = 0.0;
    if (has_usd && pos > 0.0) {
      cost_removed = st.cost[i] * qty / pos;
      st.cost[i] -= cost_removed;
    }
    st.positions[i] -= qty;
    // Clamp to zero to avoid negative due to floating point errors
    if (st.positions[i] < 0.0)
      st.positions[i] = 0.0;
    if (st.cost[i] < 0.0)
      st.cost[i] = 0.0;
    return cost_removed;
  };

  switch (ty) {
  // Buy operations: increase position and cost
  case EventType::OrderBuy:
  case EventType::FPMMBuy:
  case EventType::SplitNormal:
  case EventType::SplitNegRisk:
  case EventType::SplitNonPoly: {
    assert(row.amount >= 0);
    st.positions[i] += qty;
    if (has_usd) {
      st.cost[i] += qty * px;
    }
    return 0.0;
  }

  // LP Add: user doesn't receive tokens (0->FPMM mint)
  case EventType::FPMMLPAdd:
    assert(row.amount >= 0);
    return 0.0;

  // LP Remove/Return: user gets tokens but cost unchanged
  case EventType::FPMMLPRemove:
  case EventType::FPMMLPReturn:
    assert(row.amount >= 0);
    st.positions[i] += qty;
    return 0.0;

  // Transfer In: position increases, cost unchanged
  case EventType::TransferInNegRisk:
  case EventType::TransferInOther:
  case EventType::TransferInNonPoly:
    assert(row.amount >= 0);
    st.positions[i] += qty;
    return 0.0;

  // Sell operations: realize PnL = proceeds - cost_removed
  case EventType::OrderSell:
  case EventType::FPMMSell:
  case EventType::MergeNormal:
  case EventType::MergeNegRisk:
  case EventType::MergeNonPoly:
  case EventType::Redemption:
  case EventType::RedemptionNonPoly: {
    assert(row.amount <= 0);
    double cost_removed = remove_cost();
    if (!has_usd)
      return 0.0;
    double proceeds = qty * px;
    double realized_delta = proceeds - cost_removed;
    st.realized_pnl += realized_delta;
    return realized_delta;
  }

  // Convert: special payout calculation
  // NegRisk convert burns NO tokens and returns (popcount-1)/popcount of collateral
  case EventType::Convert: {
    assert(row.amount <= 0);
    double cost_removed = remove_cost();
    if (!has_usd)
      return 0.0;

    // Count winning outcomes (non-zero payout numerators)
    int popcount = 0;
    for (int64_t p : cond.payout_numerators) {
      if (p > 0)
        ++popcount;
    }
    if (popcount <= 0)
      popcount = 1; // Safety fallback

    // Proceeds from convert: qty * (popcount-1) / popcount
    double proceeds = qty * static_cast<double>(popcount - 1) / static_cast<double>(popcount);
    double realized_delta = proceeds - cost_removed;
    st.realized_pnl += realized_delta;
    return realized_delta;
  }

  // Transfer Out: reduce position/cost, no realized PnL
  case EventType::TransferOutNegRisk:
  case EventType::TransferOutOther:
  case EventType::TransferOutNonPoly: {
    assert(row.amount <= 0);
    (void)remove_cost();
    return 0.0;
  }

  default:
    assert(false);
    return 0.0;
  }
}

} // namespace stage3
