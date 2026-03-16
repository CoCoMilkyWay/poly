#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace stage3 {

std::string StageSync::normalize_addr(const std::string &addr) {
  std::string addr_lower = to_lower(addr);
  if (addr_lower.rfind("0x", 0) != 0 || addr_lower.size() != 42) {
    return {};
  }
  return addr_lower;
}

std::string StageSync::normalize_tag_key(const std::string &raw) {
  std::string normalized_key;
  normalized_key.reserve(raw.size());
  bool previous_is_separator = false;
  for (char c : to_lower(raw)) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (keep) {
      normalized_key.push_back(c);
      previous_is_separator = false;
      continue;
    }
    if (!previous_is_separator && !normalized_key.empty()) {
      normalized_key.push_back('_');
      previous_is_separator = true;
    }
  }
  while (!normalized_key.empty() && normalized_key.back() == '_') {
    normalized_key.pop_back();
  }
  return normalized_key;
}

bool StageSync::is_trade_event(EventType event_type) {
  return event_type == EventType::OrderBuy || event_type == EventType::OrderSell ||
         event_type == EventType::FPMMBuy || event_type == EventType::FPMMSell;
}

bool StageSync::is_usd_collateral(int32_t collateral) {
  return collateral == static_cast<int32_t>(Collateral::USDC) ||
         collateral == static_cast<int32_t>(Collateral::USDCe) ||
         collateral == static_cast<int32_t>(Collateral::USDT) ||
         collateral == static_cast<int32_t>(Collateral::WrappedUSDCe);
}

int8_t StageSync::tag_name_to_id(const std::string &tag_name) const {
  assert(!tag_to_industry_id_.empty());
  const std::string normalized_tag_key = normalize_tag_key(tag_name);
  auto tag_id_it = tag_to_industry_id_.find(normalized_tag_key);
  if (tag_id_it != tag_to_industry_id_.end()) {
    return tag_id_it->second;
  }
  return 13;
}

int64_t StageSync::sort_key_to_block(int64_t sort_key) {
  return feature_comp::sort_key_to_block(sort_key, SORT_KEY_SCALE);
}

bool StageSync::is_effective_holding_i64(int64_t qty_1e6) { return std::llabs(qty_1e6) >= kMinHoldingQty; }

int64_t StageSync::calc_volume_1e6(const EventInput &row) {
  return feature_comp::calc_volume_1e6(static_cast<EventType>(row.event_type), row.amount, row.price);
}

double StageSync::calc_convert_price_for_cond(int32_t cond_idx) const {
  assert(cond_idx >= 0);
  assert(static_cast<size_t>(cond_idx) < cond_market_question_counts_.size());
  const uint16_t question_count = cond_market_question_counts_[static_cast<size_t>(cond_idx)];
  assert(question_count >= 2);
  return static_cast<double>(question_count - 1) / static_cast<double>(question_count);
}

uint64_t StageSync::pack_cond_token_key(int32_t cond_idx, int32_t token_idx) {
  assert(cond_idx >= 0);
  assert(token_idx >= 0);
  return (static_cast<uint64_t>(static_cast<uint32_t>(cond_idx)) << 32) |
         static_cast<uint32_t>(token_idx);
}

double StageSync::apply_event_input(const EventInput &row, TokenState &st) const {
  assert(row.cond_idx >= 0);
  assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
  const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
  assert(cond.outcome_count > 0);
  assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);

  const EventType event_type = static_cast<EventType>(row.event_type);
  const bool is_usd_collateral_event = is_usd_collateral(row.collateral);
  const double quantity = std::abs(static_cast<double>(row.amount));
  if (quantity <= kPosEpsilon) {
    return 0.0;
  }

  if (is_trade_event(event_type) && row.price > 0 && is_usd_collateral_event) {
    st.lp = static_cast<double>(row.price);
  }

  const double pos_before = st.pos;
  const double entry_before = st.entry_block;
  const double event_price = static_cast<double>(row.price) / 1e6;
  const int64_t current_block = sort_key_to_block(row.sort_key);

  auto normalize_small_residue = [&]() {
    if (std::abs(st.pos) <= kPosEpsilon) {
      st.pos = 0.0;
    }
    if (std::abs(st.cost) <= kPosEpsilon) {
      st.cost = 0.0;
    }
  };

  auto apply_positive_delta = [&](double price_per_unit,
                                  bool add_cost_for_open_long,
                                  bool realize_when_cover_short) -> double {
    assert(quantity > 0.0);
    if (!is_usd_collateral_event) {
      st.pos += quantity;
      normalize_small_residue();
      return 0.0;
    }

    const double short_qty = std::max(0.0, -st.pos);
    const double cover_qty = std::min(quantity, short_qty);
    const double open_long_qty = quantity - cover_qty;

    double realized_delta = 0.0;
    if (cover_qty > 0.0 && short_qty > 0.0) {
      const double cost_closed = st.cost * (cover_qty / short_qty);
      st.cost -= cost_closed;
      if (realize_when_cover_short) {
        const double entry_credit = -cost_closed;
        const double buy_cost = cover_qty * price_per_unit;
        realized_delta = entry_credit - buy_cost;
      }
    }

    st.pos += quantity;
    if (add_cost_for_open_long && open_long_qty > 0.0) {
      st.cost += open_long_qty * price_per_unit;
    }
    normalize_small_residue();
    return realized_delta;
  };

  auto apply_negative_delta = [&](double proceeds_per_unit,
                                  bool accrue_realized,
                                  bool add_short_entry_cost) -> double {
    assert(quantity > 0.0);
    if (!is_usd_collateral_event) {
      st.pos -= quantity;
      normalize_small_residue();
      return 0.0;
    }

    const double long_qty = std::max(0.0, st.pos);
    const double close_qty = std::min(quantity, long_qty);
    const double open_short_qty = quantity - close_qty;

    double cost_removed = 0.0;
    if (close_qty > 0.0 && long_qty > 0.0) {
      cost_removed = st.cost * (close_qty / long_qty);
      st.cost -= cost_removed;
    }

    st.pos -= quantity;
    if (add_short_entry_cost && open_short_qty > 0.0) {
      st.cost -= open_short_qty * proceeds_per_unit;
    }

    double realized_delta = 0.0;
    if (accrue_realized) {
      const double proceeds = close_qty * proceeds_per_unit;
      realized_delta = proceeds - cost_removed;
    }

    normalize_small_residue();
    return realized_delta;
  };

  double realized_delta = 0.0;
  switch (event_type) {
  case EventType::OrderBuy:
  case EventType::FPMMBuy:
  case EventType::SplitNormal:
  case EventType::SplitNegRisk:
  case EventType::SplitNonPoly: {
    if (row.amount >= 0) {
      realized_delta = apply_positive_delta(/*price_per_unit=*/event_price,
                                            /*add_cost_for_open_long=*/true,
                                            /*realize_when_cover_short=*/true);
      break;
    }
    realized_delta = apply_negative_delta(/*proceeds_per_unit=*/event_price,
                                          /*accrue_realized=*/true,
                                          /*add_short_entry_cost=*/true);
    break;
  }

  case EventType::FPMMLPAdd:
  case EventType::FPMMLPRemove:
  case EventType::FPMMLPReturn:
    // LP 事件仅记录行为，不进入 token 持仓/成本/PnL 状态机.
    realized_delta = 0.0;
    break;

  case EventType::TransferInNegRisk:
  case EventType::TransferInOther:
  case EventType::TransferInNonPoly:
    if (row.amount >= 0) {
      realized_delta = apply_positive_delta(/*price_per_unit=*/0.0,
                                            /*add_cost_for_open_long=*/false,
                                            /*realize_when_cover_short=*/false);
      break;
    }
    realized_delta = apply_negative_delta(/*proceeds_per_unit=*/0.0,
                                          /*accrue_realized=*/false,
                                          /*add_short_entry_cost=*/false);
    break;

  case EventType::OrderSell:
  case EventType::FPMMSell:
  case EventType::MergeNormal:
  case EventType::MergeNegRisk:
  case EventType::MergeNonPoly:
  case EventType::Redemption:
  case EventType::RedemptionNonPoly: {
    if (row.amount <= 0) {
      realized_delta = apply_negative_delta(/*proceeds_per_unit=*/event_price,
                                            /*accrue_realized=*/true,
                                            /*add_short_entry_cost=*/true);
      break;
    }
    realized_delta = apply_positive_delta(/*price_per_unit=*/event_price,
                                          /*add_cost_for_open_long=*/true,
                                          /*realize_when_cover_short=*/true);
    break;
  }

  case EventType::Convert: {
    const double convert_px = calc_convert_price_for_cond(row.cond_idx);
    if (row.amount <= 0) {
      realized_delta = apply_negative_delta(/*proceeds_per_unit=*/convert_px,
                                            /*accrue_realized=*/true,
                                            /*add_short_entry_cost=*/true);
      break;
    }
    realized_delta = apply_positive_delta(/*price_per_unit=*/convert_px,
                                          /*add_cost_for_open_long=*/true,
                                          /*realize_when_cover_short=*/true);
    break;
  }

  case EventType::TransferOutNegRisk:
  case EventType::TransferOutOther:
  case EventType::TransferOutNonPoly: {
    if (row.amount <= 0) {
      realized_delta = apply_negative_delta(/*proceeds_per_unit=*/0.0,
                                            /*accrue_realized=*/false,
                                            /*add_short_entry_cost=*/false);
      break;
    }
    realized_delta = apply_positive_delta(/*price_per_unit=*/0.0,
                                          /*add_cost_for_open_long=*/false,
                                          /*realize_when_cover_short=*/false);
    break;
  }

  default:
    assert(false);
    realized_delta = 0.0;
    break;
  }

  normalize_small_residue();
  const double pos_after = st.pos;
  if (std::abs(pos_after) <= kPosEpsilon) {
    st.pos = 0.0;
    st.cost = 0.0;
    st.entry_block = 0.0;
    return realized_delta;
  }

  const double abs_before = std::abs(pos_before);
  const double abs_after = std::abs(pos_after);
  if (abs_before <= kPosEpsilon) {
    st.entry_block = static_cast<double>(current_block);
    return realized_delta;
  }

  if (pos_before * pos_after < 0.0) {
    st.entry_block = static_cast<double>(current_block);
    return realized_delta;
  }

  if (abs_after > abs_before + kPosEpsilon) {
    const double delta_abs = abs_after - abs_before;
    if (entry_before > 0.0) {
      st.entry_block = (abs_before * entry_before + delta_abs * static_cast<double>(current_block)) / abs_after;
    } else {
      st.entry_block = static_cast<double>(current_block);
    }
  }
  return realized_delta;
}

} // namespace stage3
