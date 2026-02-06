#pragma once

// ============================================================================
// PnL Rebuilder 并行 Worker
// ============================================================================

#include "rebuilder_types.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace rebuilder {

class Worker {
public:
  using ConfigMap = std::unordered_map<std::string, ConditionConfig>;

  // 处理单个用户的所有事件
  static UserResult process_user(
      const std::string &user,
      const std::vector<Event> &events,
      const ConfigMap &configs) {
    UserResult result;
    result.user = user;

    for (const auto &e : events) {
      // 更新时间戳
      if (result.meta.first_ts == 0 || e.timestamp < result.meta.first_ts) {
        result.meta.first_ts = e.timestamp;
      }
      if (e.timestamp > result.meta.last_ts) {
        result.meta.last_ts = e.timestamp;
      }

      // 分发事件处理
      std::string token_id;
      if (e.event_type == "order_maker" || e.event_type == "order_taker") {
        process_order(e, result);
        token_id = e.token_id;
      } else if (e.event_type == "merge") {
        process_merge(e, result, configs);
        token_id = e.condition_id;
      } else if (e.event_type == "redemption") {
        process_redemption(e, result, configs);
        token_id = e.condition_id;
      }

      // 计算当前总 PnL
      int64_t total_pnl = calc_total_pnl(result.positions);

      // 记录快照
      Snapshot snap;
      snap.timestamp = e.timestamp;
      snap.event_type = e.event_type;
      snap.event_id = e.event_id;
      snap.token_id = token_id;
      snap.side = e.side;
      snap.size = e.size;
      snap.price = e.price;
      snap.total_pnl = total_pnl;
      result.snapshots.push_back(std::move(snap));
    }

    return result;
  }

private:
  // 计算总 PnL(realized + unrealized，假设当前价格为 0.5)
  static int64_t calc_total_pnl(const std::unordered_map<std::string, Position> &positions) {
    int64_t total = 0;
    for (const auto &[_, pos] : positions) {
      total += pos.realized_pnl;
      // unrealized: amount * (current_price - avg_price) / 1e6
      // 假设当前价格为 0.5 ($500000 in 1e6 units)
      int64_t unrealized = pos.amount * (500000 - pos.avg_price) / 1000000;
      total += unrealized;
    }
    return total;
  }

  // 处理订单事件
  static void process_order(const Event &e, UserResult &result) {
    result.meta.order_count++;

    if (e.token_id.empty()) {
      result.meta.skipped_count++;
      return;
    }

    auto &pos = result.positions[e.token_id];

    // price 转为 1e6 单位
    int64_t price_1e6 = static_cast<int64_t>(e.price * 1000000);

    if (e.side == "Buy") {
      process_order_buy(pos, price_1e6, e.size);
    } else if (e.side == "Sell") {
      process_order_sell(pos, price_1e6, e.size, result.meta, e.token_id, e.timestamp);
    }
  }

  // 买入：更新平均价和持仓量
  static void process_order_buy(Position &pos, int64_t price, int64_t amount) {
    if (amount <= 0)
      return;

    // avg_price = (avg_price * amount + price * buy_amount) / (amount + buy_amount)
    int64_t total_value = pos.avg_price * pos.amount + price * amount;
    pos.amount += amount;
    if (pos.amount > 0) {
      pos.avg_price = total_value / pos.amount;
    }
    pos.total_bought += amount;
  }

  // 卖出：计算已实现盈亏
  static void process_order_sell(Position &pos, int64_t price, int64_t amount,
                                 UserMeta &meta, const std::string &token_id, int64_t ts) {
    if (amount <= 0)
      return;

    int64_t sell_amount = std::min(amount, pos.amount);
    // realized_pnl += amount * (price - avg_price) / 1e6
    pos.realized_pnl += sell_amount * (price - pos.avg_price) / 1000000;
    pos.amount -= sell_amount;

    // 检测负持仓
    if (pos.amount < 0) {
      meta.negative_amounts.emplace_back(token_id, ts, pos.amount);
    }
  }

  // 处理 merge 事件
  static void process_merge(const Event &e, UserResult &result, const ConfigMap &configs) {
    result.meta.merge_count++;

    if (e.condition_id.empty()) {
      result.meta.skipped_count++;
      return;
    }

    auto it = configs.find(e.condition_id);
    if (it == configs.end()) {
      result.meta.missing_conditions.push_back(e.condition_id);
      result.meta.skipped_count++;
      return;
    }

    const auto &cfg = it->second;
    if (cfg.position_ids.size() < 2) {
      result.meta.skipped_count++;
      return;
    }

    // merge 操作：用户花费 USDC 铸造 YES + NO token
    // 每个 outcome token 获得 amount 数量
    for (const auto &token_id : cfg.position_ids) {
      auto &pos = result.positions[token_id];
      // merge 价格为 0.5(YES + NO 各占一半)
      int64_t price_1e6 = 500000; // $0.50
      process_order_buy(pos, price_1e6, e.amount);
    }
  }

  // 处理 redemption 事件
  static void process_redemption(const Event &e, UserResult &result, const ConfigMap &configs) {
    result.meta.redemption_count++;

    if (e.condition_id.empty()) {
      result.meta.skipped_count++;
      return;
    }

    auto it = configs.find(e.condition_id);
    if (it == configs.end()) {
      result.meta.missing_conditions.push_back(e.condition_id);
      result.meta.skipped_count++;
      return;
    }

    const auto &cfg = it->second;

    // 检查是否已结算
    if (cfg.payout_denominator == 0) {
      result.meta.unresolved_redemptions.push_back(e.condition_id);
      result.meta.skipped_count++;
      return;
    }

    // redemption 操作：根据 payout 比例清算持仓
    // payout_numerators 表示每个 outcome 的赔付比例
    for (size_t i = 0; i < cfg.position_ids.size() && i < cfg.payout_numerators.size(); ++i) {
      const auto &token_id = cfg.position_ids[i];
      int64_t numerator = cfg.payout_numerators[i];

      auto pos_it = result.positions.find(token_id);
      if (pos_it == result.positions.end())
        continue;

      auto &pos = pos_it->second;
      if (pos.amount <= 0)
        continue;

      // 计算赔付价格：numerator / denominator (转为 1e6 单位)
      int64_t payout_price = (numerator * 1000000) / cfg.payout_denominator;

      // 相当于以 payout_price 卖出全部持仓
      process_order_sell(pos, payout_price, pos.amount, result.meta, token_id, e.timestamp);
    }
  }
};

} // namespace rebuilder
