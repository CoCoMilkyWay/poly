#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>

namespace stage3 {

namespace {

inline bool is_trade_event(int32_t event_type) {
  return event_type == EVT_ORDER_BUY || event_type == EVT_ORDER_SELL ||
         event_type == EVT_FPMM_BUY || event_type == EVT_FPMM_SELL;
}

inline bool is_lp_event(int32_t event_type) {
  return event_type == EVT_FPMM_LP_ADD ||
         event_type == EVT_FPMM_LP_REMOVE ||
         event_type == EVT_FPMM_LP_RETURN;
}

inline bool is_transfer_in_event(int32_t event_type) {
  return event_type == EVT_TRANSFER_IN_NEGRISK ||
         event_type == EVT_TRANSFER_IN_OTHER ||
         event_type == EVT_TRANSFER_IN_NON_POLY;
}

inline bool is_transfer_out_event(int32_t event_type) {
  return event_type == EVT_TRANSFER_OUT_NEGRISK ||
         event_type == EVT_TRANSFER_OUT_OTHER ||
         event_type == EVT_TRANSFER_OUT_NON_POLY;
}

inline bool is_transfer_event(int32_t event_type) {
  return is_transfer_in_event(event_type) || is_transfer_out_event(event_type);
}

// price 类: Order* / FPMM* / Split* / Merge* / Redemption*
inline bool is_price_event(int32_t event_type) {
  return event_type == EVT_ORDER_BUY || event_type == EVT_ORDER_SELL ||
         event_type == EVT_FPMM_BUY || event_type == EVT_FPMM_SELL ||
         event_type == EVT_SPLIT_NORMAL || event_type == EVT_SPLIT_NEGRISK ||
         event_type == EVT_SPLIT_NON_POLY ||
         event_type == EVT_MERGE_NORMAL || event_type == EVT_MERGE_NEGRISK ||
         event_type == EVT_MERGE_NON_POLY ||
         event_type == EVT_REDEMPTION || event_type == EVT_REDEMPTION_NON_POLY;
}

// 正向腿典型事件: OrderBuy, FPMMBuy, Split*, TransferIn*
inline bool is_positive_typical(int32_t event_type) {
  return event_type == EVT_ORDER_BUY || event_type == EVT_FPMM_BUY ||
         event_type == EVT_SPLIT_NORMAL || event_type == EVT_SPLIT_NEGRISK ||
         event_type == EVT_SPLIT_NON_POLY ||
         is_transfer_in_event(event_type);
}

// 反向腿典型事件: OrderSell, FPMMSell, Merge*, Redemption*, Convert, TransferOut*
inline bool is_negative_typical(int32_t event_type) {
  return event_type == EVT_ORDER_SELL || event_type == EVT_FPMM_SELL ||
         event_type == EVT_MERGE_NORMAL || event_type == EVT_MERGE_NEGRISK ||
         event_type == EVT_MERGE_NON_POLY ||
         event_type == EVT_REDEMPTION || event_type == EVT_REDEMPTION_NON_POLY ||
         event_type == EVT_CONVERT ||
         is_transfer_out_event(event_type);
}

double calc_convert_price(const Stage3Runtime *rt, int32_t cond_idx, const ConditionMeta &cond) {
  uint16_t question_count = 0;
  if (cond_idx >= 0 && static_cast<size_t>(cond_idx) < rt->cond_market_question_counts.size()) {
    question_count = rt->cond_market_question_counts[cond_idx];
  }
  const uint16_t denominator = (question_count >= 2) ? question_count : cond.outcome_count;
  assert(denominator >= 2);
  return static_cast<double>(denominator - 1) / static_cast<double>(denominator);
}

// 小残差归零
inline void normalize_small_residue(TokenSlot *tok) {
  constexpr int64_t EPSILON_1E6 = 1; // 1e-6 in 1e6 scale
  if (std::abs(tok->pos) <= EPSILON_1E6) {
    tok->pos = 0;
  }
  if (std::abs(tok->cost) <= EPSILON_1E6) {
    tok->cost = 0;
  }
}

} // namespace

// ============================================================================
// apply_trade_event - 交易状态机核心
// 返回 realized_delta (1e6 scale)
// ============================================================================

int64_t apply_trade_event(Stage3Runtime *rt, const EventInput &evt, TokenSlot *tok) {
  assert(evt.cond_idx >= 0);
  assert(static_cast<size_t>(evt.cond_idx) < MAX_CONDITIONS);

  const ConditionMeta &cond = rt->conditions[evt.cond_idx];
  assert(cond.outcome_count > 0);
  assert(evt.token_idx >= 0 && evt.token_idx < cond.outcome_count);

  const int32_t event_type = evt.event_type;
  const bool has_usd = is_usd_collateral(evt.collateral);
  const int64_t amount = evt.amount;
  const int64_t qty = std::abs(amount);

  // No-op for zero amount
  if (qty == 0) {
    return 0;
  }

  // FPMMLPAdd: 不改 pos/cost, realized = 0
  if (event_type == EVT_FPMM_LP_ADD) {
    return 0;
  }

  // LP Remove/Return: 不改 pos/cost (按新架构规则)
  if (event_type == EVT_FPMM_LP_REMOVE || event_type == EVT_FPMM_LP_RETURN) {
    return 0;
  }

  // 更新 lp (仅 Order/FPMM 交易且 price > 0)
  if (is_trade_event(event_type) && evt.price_1e6 > 0 && has_usd) {
    tok->lp = evt.price_1e6;
  }

  const int64_t pos_before = tok->pos;
  const int64_t cost_before = tok->cost;
  const int64_t entry_before = tok->entry_block;
  const int64_t current_block = sort_key_to_block(evt.sort_key);

  // 确定价格
  double price_per_unit = 0.0;
  if (event_type == EVT_CONVERT) {
    price_per_unit = calc_convert_price(rt, evt.cond_idx, cond);
  } else if (is_price_event(event_type)) {
    price_per_unit = static_cast<double>(evt.price_1e6) / 1e6;
  }
  // transfer 类 price = 0

  int64_t realized_delta = 0;

  if (amount > 0) {
    // 正向腿: 先平空再开多
    if (!has_usd) {
      tok->pos += qty;
      normalize_small_residue(tok);
    } else {
      const int64_t short_qty = std::max<int64_t>(0, -pos_before);
      const int64_t cover_qty = std::min(qty, short_qty);
      const int64_t open_long_qty = qty - cover_qty;

      // 平空部分
      if (cover_qty > 0 && short_qty > 0) {
        const int64_t cost_removed = static_cast<int64_t>(
            static_cast<double>(cost_before) * static_cast<double>(cover_qty) / static_cast<double>(short_qty));
        tok->cost -= cost_removed;

        // realized 计算 (仅 price/convert 类)
        if (!is_transfer_event(event_type)) {
          // 空头平仓: realized = entry_credit - buy_cost = (-cost_removed) - (cover_qty * price)
          const int64_t entry_credit = -cost_removed;
          const int64_t buy_cost = static_cast<int64_t>(static_cast<double>(cover_qty) * price_per_unit);
          realized_delta = entry_credit - buy_cost;
        }
      }

      tok->pos += qty;

      // 开多部分增加 cost (仅 price/convert 类)
      if (open_long_qty > 0 && !is_transfer_event(event_type)) {
        tok->cost += static_cast<int64_t>(static_cast<double>(open_long_qty) * price_per_unit);
      }

      normalize_small_residue(tok);
    }
  } else {
    // 反向腿 (amount < 0): 先平多再开空
    if (!has_usd) {
      tok->pos -= qty;
      normalize_small_residue(tok);
    } else {
      const int64_t long_qty = std::max<int64_t>(0, pos_before);
      const int64_t close_qty = std::min(qty, long_qty);
      const int64_t open_short_qty = qty - close_qty;

      // 平多部分
      int64_t cost_removed = 0;
      if (close_qty > 0 && long_qty > 0) {
        cost_removed = static_cast<int64_t>(
            static_cast<double>(cost_before) * static_cast<double>(close_qty) / static_cast<double>(long_qty));
        tok->cost -= cost_removed;

        // realized 计算 (仅 price/convert 类)
        if (!is_transfer_event(event_type)) {
          // 多头平仓: realized = proceeds - cost_removed
          const int64_t proceeds = static_cast<int64_t>(static_cast<double>(close_qty) * price_per_unit);
          realized_delta = proceeds - cost_removed;
        }
      }

      tok->pos -= qty;

      // 开空部分记负 cost (仅 price/convert 类)
      if (open_short_qty > 0 && !is_transfer_event(event_type)) {
        tok->cost -= static_cast<int64_t>(static_cast<double>(open_short_qty) * price_per_unit);
      }

      normalize_small_residue(tok);
    }
  }

  // 更新 entry_block
  const int64_t pos_after = tok->pos;
  if (pos_after == 0) {
    // 平仓完毕
    tok->cost = 0;
    tok->entry_block = 0;
  } else if (pos_before == 0) {
    // 建仓
    tok->entry_block = current_block;
  } else if ((pos_before > 0) != (pos_after > 0)) {
    // 方向反转
    tok->entry_block = current_block;
  } else if (std::abs(pos_after) > std::abs(pos_before)) {
    // 加仓: 加权平均
    const int64_t abs_before = std::abs(pos_before);
    const int64_t abs_after = std::abs(pos_after);
    const int64_t delta_abs = abs_after - abs_before;
    if (entry_before > 0) {
      tok->entry_block = (abs_before * entry_before + delta_abs * current_block) / abs_after;
    } else {
      tok->entry_block = current_block;
    }
  }
  // 减仓: entry_block 不变

  return realized_delta;
}

} // namespace stage3
