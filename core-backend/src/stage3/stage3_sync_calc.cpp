#include "stage3_sync.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

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

int8_t StageSync::tag_name_to_id(const std::string &tag_name) const {
  auto normalize = [](const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    bool prev_sep = false;
    for (char c : to_lower(raw)) {
      const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (keep) {
        out.push_back(c);
        prev_sep = false;
        continue;
      }
      if (!prev_sep && !out.empty()) {
        out.push_back('_');
        prev_sep = true;
      }
    }
    while (!out.empty() && out.back() == '_') {
      out.pop_back();
    }
    return out;
  };

  assert(!tag_to_industry_id_.empty());
  const std::string whole = normalize(tag_name);
  auto it = tag_to_industry_id_.find(whole);
  if (it != tag_to_industry_id_.end()) {
    return it->second;
  }

  size_t sep = tag_name.find(" - ");
  if (sep != std::string::npos) {
    const std::string left = normalize(tag_name.substr(0, sep));
    const std::string right = normalize(tag_name.substr(sep + 3));
    auto it_right = tag_to_industry_id_.find(right);
    if (it_right != tag_to_industry_id_.end()) {
      return it_right->second;
    }
    auto it_left = tag_to_industry_id_.find(left);
    if (it_left != tag_to_industry_id_.end()) {
      return it_left->second;
    }
  }
  return 13;
}

int64_t StageSync::sort_key_to_block(int64_t sort_key) {
  return feature_comp::sort_key_to_block(sort_key, SORT_KEY_SCALE);
}

int64_t StageSync::sort_key_to_block_bucket(int64_t sort_key) {
  return feature_comp::sort_key_to_block_bucket(sort_key, SORT_KEY_SCALE, kBlockBucketSize);
}

int64_t StageSync::bucket_end_block(int64_t block_bucket) {
  return feature_comp::bucket_end_block(block_bucket, kBlockBucketSize);
}

bool StageSync::is_effective_holding(double qty_1e6) {
  return is_effective_holding_i64(round_i64(qty_1e6));
}

bool StageSync::is_effective_holding_i64(int64_t qty_1e6) {
  return std::llabs(qty_1e6) >= kMinHoldingQty;
}

double StageSync::calc_unrealized_pnl(const TokenState &st) {
  return feature_comp::calc_unrealized_pnl(
      feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
}

int64_t StageSync::calc_exposure_1e6(const TokenState &st) {
  return feature_comp::calc_exposure_1e6(
      feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
}

int64_t StageSync::calc_holding_period_blocks(int64_t current_block, const TokenState &st) {
  return feature_comp::calc_holding_period_blocks(
      current_block, feature_comp::TokenSnapshot{st.pos, st.cost, st.lp, st.entry_block}, kPosEpsilon);
}

int64_t StageSync::calc_volume_1e6(const EventInput &row) {
  return feature_comp::calc_volume_1e6(static_cast<EventType>(row.event_type), row.amount, row.price);
}

uint64_t StageSync::pack_cond_token_key(int32_t cond_idx, int32_t token_idx) {
  assert(cond_idx >= 0);
  assert(token_idx >= 0);
  return (static_cast<uint64_t>(static_cast<uint32_t>(cond_idx)) << 32) |
         static_cast<uint32_t>(token_idx);
}

void StageSync::adjust_tail_window(AggRuntime &agg,
                                   int64_t block_bucket,
                                   int64_t current_block,
                                   int64_t current_exposure,
                                   int64_t current_holding_period,
                                   int64_t current_token_count) {
  feature_comp::adjust_tail_window(
      agg, block_bucket, current_block, current_exposure, current_holding_period, current_token_count, kBlockBucketSize);
}

double StageSync::apply_event_input(const EventInput &row, TokenState &st) const {
  assert(row.cond_idx >= 0);
  assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
  const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
  assert(cond.outcome_count > 0);
  assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);

  const EventType ty = static_cast<EventType>(row.event_type);
  const bool has_usd = is_usd_collateral(row.collateral);
  const double qty = std::abs(static_cast<double>(row.amount));
  if (qty <= kPosEpsilon) {
    return 0.0;
  }

  if (is_trade_event(ty) && row.price > 0 && has_usd) {
    st.lp = static_cast<double>(row.price);
  }

  const double pos_before = st.pos;
  const double entry_before = st.entry_block;
  const double px = static_cast<double>(row.price) / 1e6;
  const int64_t current_block = sort_key_to_block(row.sort_key);

  auto normalize_small_residue = [&]() {
    if (std::abs(st.pos) <= kPosEpsilon) {
      st.pos = 0.0;
    }
    if (std::abs(st.cost) <= kPosEpsilon) {
      st.cost = 0.0;
    }
  };

  auto apply_positive_delta = [&](bool add_cost_for_open_long, bool realize_when_cover_short) -> double {
    assert(qty > 0.0);
    if (!has_usd) {
      st.pos += qty;
      normalize_small_residue();
      return 0.0;
    }

    const double short_qty = std::max(0.0, -st.pos);
    const double cover_qty = std::min(qty, short_qty);
    const double open_long_qty = qty - cover_qty;

    double realized_delta = 0.0;
    if (cover_qty > 0.0 && short_qty > 0.0) {
      const double cost_closed = st.cost * (cover_qty / short_qty);
      st.cost -= cost_closed;
      if (realize_when_cover_short) {
        const double entry_credit = -cost_closed;
        const double buy_cost = cover_qty * px;
        realized_delta = entry_credit - buy_cost;
      }
    }

    st.pos += qty;
    if (add_cost_for_open_long && open_long_qty > 0.0) {
      st.cost += open_long_qty * px;
    }
    normalize_small_residue();
    return realized_delta;
  };

  auto apply_negative_delta = [&](double proceeds_per_unit,
                                  bool accrue_realized,
                                  bool add_short_entry_cost) -> double {
    assert(qty > 0.0);
    if (!has_usd) {
      st.pos -= qty;
      normalize_small_residue();
      return 0.0;
    }

    const double long_qty = std::max(0.0, st.pos);
    const double close_qty = std::min(qty, long_qty);
    const double open_short_qty = qty - close_qty;

    double cost_removed = 0.0;
    if (close_qty > 0.0 && long_qty > 0.0) {
      cost_removed = st.cost * (close_qty / long_qty);
      st.cost -= cost_removed;
    }

    st.pos -= qty;
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
  switch (ty) {
  case EventType::OrderBuy:
  case EventType::FPMMBuy:
  case EventType::SplitNormal:
  case EventType::SplitNegRisk:
  case EventType::SplitNonPoly: {
    if (row.amount >= 0) {
      realized_delta = apply_positive_delta(/*add_cost_for_open_long=*/true,
                                            /*realize_when_cover_short=*/true);
      break;
    }
    realized_delta = apply_negative_delta(/*proceeds_per_unit=*/px,
                                          /*accrue_realized=*/true,
                                          /*add_short_entry_cost=*/true);
    break;
  }

  case EventType::FPMMLPAdd:
    realized_delta = 0.0;
    break;

  case EventType::FPMMLPRemove:
  case EventType::FPMMLPReturn:
    if (row.amount >= 0) {
      realized_delta = apply_positive_delta(/*add_cost_for_open_long=*/false,
                                            /*realize_when_cover_short=*/false);
      break;
    }
    realized_delta = apply_negative_delta(/*proceeds_per_unit=*/0.0,
                                          /*accrue_realized=*/false,
                                          /*add_short_entry_cost=*/false);
    break;

  case EventType::TransferInNegRisk:
  case EventType::TransferInOther:
  case EventType::TransferInNonPoly:
    if (row.amount >= 0) {
      realized_delta = apply_positive_delta(/*add_cost_for_open_long=*/false,
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
      realized_delta = apply_negative_delta(/*proceeds_per_unit=*/px,
                                            /*accrue_realized=*/true,
                                            /*add_short_entry_cost=*/true);
      break;
    }
    realized_delta = apply_positive_delta(/*add_cost_for_open_long=*/true,
                                          /*realize_when_cover_short=*/true);
    break;
  }

  case EventType::Convert: {
    int popcount = 0;
    for (int64_t p : cond.payout_numerators) {
      if (p > 0) {
        ++popcount;
      }
    }
    if (popcount <= 0) { // this is allowed
      popcount = 1;
    }
    const double convert_px = static_cast<double>(popcount - 1) / static_cast<double>(popcount);
    if (row.amount <= 0) {
      realized_delta = apply_negative_delta(/*proceeds_per_unit=*/convert_px,
                                            /*accrue_realized=*/true,
                                            /*add_short_entry_cost=*/true);
      break;
    }
    realized_delta = apply_positive_delta(/*add_cost_for_open_long=*/true,
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
    realized_delta = apply_positive_delta(/*add_cost_for_open_long=*/false,
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
