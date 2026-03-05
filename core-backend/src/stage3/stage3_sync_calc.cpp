#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

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

bool StageSync::is_effective_holding(double qty_1e6) {
  return is_effective_holding_i64(round_i64(qty_1e6));
}

bool StageSync::is_effective_holding_i64(int64_t qty_1e6) {
  return std::llabs(qty_1e6) >= kMinHoldingQty;
}

bool StageSync::has_any_position(const CondState &st, int outcome_count) {
  assert(outcome_count >= 0 && outcome_count <= MAX_OUTCOMES);
  for (int j = 0; j < outcome_count; ++j) {
    if (round_i64(st.positions[j]) != 0) {
      return true;
    }
  }
  return false;
}

int StageSync::count_effective_holdings(const CondState &st, int outcome_count) {
  assert(outcome_count >= 0 && outcome_count <= MAX_OUTCOMES);
  int count = 0;
  for (int j = 0; j < outcome_count; ++j) {
    if (is_effective_holding(st.positions[j])) {
      count++;
    }
  }
  return count;
}

// Compute unrealized PnL using double arithmetic
// unrealized = Σ(pos * last_price / 1e6 - cost) for all outcomes with known last_price.
// This naturally supports long(pos>0) and short(pos<0) exposure.
double StageSync::compute_unrealized_pnl(const CondState &st) {
  double sum = 0.0;
  for (int j = 0; j < MAX_OUTCOMES; ++j) {
    if (std::abs(st.positions[j]) <= kPosEpsilon || st.last_price[j] <= 0.0) {
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
  double qty = std::abs(static_cast<double>(row.amount));

  // Zero-size rows are semantic no-ops for position/cost/replay accounting.
  if (qty <= kPosEpsilon) {
    return 0.0;
  }

  // Update last_price for trade events
  if (is_trade_event(ty) && row.price > 0 && has_usd) {
    st.last_price[i] = static_cast<double>(row.price);
  }

  double px = static_cast<double>(row.price) / 1e6; // Convert from 1e6 to actual price

  auto normalize_small_residue = [&]() {
    if (std::abs(st.positions[i]) <= kPosEpsilon) {
      st.positions[i] = 0.0;
    }
    if (std::abs(st.cost[i]) <= kPosEpsilon) {
      st.cost[i] = 0.0;
    }
  };

  // Positive delta (buy/inbound): may cover existing short first, then open/increase long.
  auto apply_positive_delta = [&](bool add_cost_for_open_long, bool realize_when_cover_short) -> double {
    assert(qty > 0.0);
    if (!has_usd) {
      st.positions[i] += qty;
      normalize_small_residue();
      return 0.0;
    }

    const double pos_before = st.positions[i];
    const double short_qty = std::max(0.0, -pos_before);
    const double cover_qty = std::min(qty, short_qty);
    const double open_long_qty = qty - cover_qty;

    double realized_delta = 0.0;
    if (cover_qty > 0.0 && short_qty > 0.0) {
      // cost_closed is negative (short book value removed from inventory)
      const double cost_closed = st.cost[i] * (cover_qty / short_qty);
      st.cost[i] -= cost_closed;
      if (realize_when_cover_short) {
        const double entry_credit = -cost_closed;
        const double buy_cost = cover_qty * px;
        realized_delta = entry_credit - buy_cost;
        st.realized_pnl += realized_delta;
      }
    }

    st.positions[i] += qty;
    if (add_cost_for_open_long && open_long_qty > 0.0) {
      st.cost[i] += open_long_qty * px;
    }
    normalize_small_residue();
    return realized_delta;
  };

  // Negative delta (sell/outbound): may close existing long first, then open/increase short.
  auto apply_negative_delta = [&](double proceeds_per_unit,
                                  bool accrue_realized,
                                  bool add_short_entry_cost) -> double {
    assert(qty > 0.0);
    if (!has_usd) {
      st.positions[i] -= qty;
      normalize_small_residue();
      return 0.0;
    }

    const double pos_before = st.positions[i];
    const double long_qty = std::max(0.0, pos_before);
    const double close_qty = std::min(qty, long_qty);
    const double open_short_qty = qty - close_qty;

    double cost_removed = 0.0;
    if (close_qty > 0.0 && long_qty > 0.0) {
      cost_removed = st.cost[i] * (close_qty / long_qty);
      st.cost[i] -= cost_removed;
    }

    st.positions[i] -= qty;
    if (add_short_entry_cost && open_short_qty > 0.0) {
      // Short exposure carries negative cost (entry credit).
      st.cost[i] -= open_short_qty * proceeds_per_unit;
    }

    double realized_delta = 0.0;
    if (accrue_realized) {
      const double proceeds = close_qty * proceeds_per_unit;
      realized_delta = proceeds - cost_removed;
      st.realized_pnl += realized_delta;
    }

    normalize_small_residue();
    return realized_delta;
  };

  switch (ty) {
  // Buy operations: increase position and cost
  case EventType::OrderBuy:
  case EventType::FPMMBuy:
  case EventType::SplitNormal:
  case EventType::SplitNegRisk:
  case EventType::SplitNonPoly: {
    if (row.amount >= 0) {
      return apply_positive_delta(/*add_cost_for_open_long=*/true,
                                  /*realize_when_cover_short=*/true);
    }
    return apply_negative_delta(/*proceeds_per_unit=*/px,
                                /*accrue_realized=*/true,
                                /*add_short_entry_cost=*/true);
  }

  // LP Add: user doesn't receive tokens (0->FPMM mint)
  case EventType::FPMMLPAdd:
    return 0.0;

  // LP Remove/Return: user gets tokens but cost unchanged
  case EventType::FPMMLPRemove:
  case EventType::FPMMLPReturn:
    if (row.amount >= 0) {
      return apply_positive_delta(/*add_cost_for_open_long=*/false,
                                  /*realize_when_cover_short=*/false);
    }
    return apply_negative_delta(/*proceeds_per_unit=*/0.0,
                                /*accrue_realized=*/false,
                                /*add_short_entry_cost=*/false);

  // Transfer In: position increases, cost unchanged
  case EventType::TransferInNegRisk:
  case EventType::TransferInOther:
  case EventType::TransferInNonPoly:
    if (row.amount >= 0) {
      return apply_positive_delta(/*add_cost_for_open_long=*/false,
                                  /*realize_when_cover_short=*/false);
    }
    return apply_negative_delta(/*proceeds_per_unit=*/0.0,
                                /*accrue_realized=*/false,
                                /*add_short_entry_cost=*/false);

  // Sell operations: realize PnL = proceeds - cost_removed
  case EventType::OrderSell:
  case EventType::FPMMSell:
  case EventType::MergeNormal:
  case EventType::MergeNegRisk:
  case EventType::MergeNonPoly:
  case EventType::Redemption:
  case EventType::RedemptionNonPoly: {
    if (row.amount <= 0) {
      return apply_negative_delta(/*proceeds_per_unit=*/px,
                                  /*accrue_realized=*/true,
                                  /*add_short_entry_cost=*/true);
    }
    return apply_positive_delta(/*add_cost_for_open_long=*/true,
                                /*realize_when_cover_short=*/true);
  }

  // Convert: special payout calculation
  // NegRisk convert burns NO tokens and returns (popcount-1)/popcount of collateral
  case EventType::Convert: {
    // Count winning outcomes (non-zero payout numerators)
    int popcount = 0;
    for (int64_t p : cond.payout_numerators) {
      if (p > 0)
        ++popcount;
    }
    assert(popcount > 0);

    // Proceeds from convert: qty * (popcount-1) / popcount
    const double convert_px =
        static_cast<double>(popcount - 1) / static_cast<double>(popcount);
    if (row.amount <= 0) {
      return apply_negative_delta(/*proceeds_per_unit=*/convert_px,
                                  /*accrue_realized=*/true,
                                  /*add_short_entry_cost=*/true);
    }
    return apply_positive_delta(/*add_cost_for_open_long=*/true,
                                /*realize_when_cover_short=*/true);
  }

  // Transfer Out: reduce position/cost, no realized PnL
  case EventType::TransferOutNegRisk:
  case EventType::TransferOutOther:
  case EventType::TransferOutNonPoly: {
    if (row.amount <= 0) {
      return apply_negative_delta(/*proceeds_per_unit=*/0.0,
                                  /*accrue_realized=*/false,
                                  /*add_short_entry_cost=*/false);
    }
    return apply_positive_delta(/*add_cost_for_open_long=*/false,
                                /*realize_when_cover_short=*/false);
  }

  default:
    assert(false);
    return 0.0;
  }
}

} // namespace stage3
