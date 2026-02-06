#pragma once

// ============================================================================
// PnL Rebuilder 数据加载器
// ============================================================================

#include "rebuilder_types.hpp"
#include <algorithm>
#include <cassert>
#include <duckdb.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::json;

namespace rebuilder {

class Loader {
public:
  explicit Loader(duckdb::Connection &conn) : conn_(conn) {}

  // 加载 pnl_condition，建立 positionId → conditionId 反向映射
  std::unordered_map<std::string, ConditionConfig> load_conditions() {
    std::unordered_map<std::string, ConditionConfig> configs;

    auto result = conn_.Query(R"(
      SELECT id, positionIds, payoutNumerators, payoutDenominator
      FROM pnl_condition
    )");
    assert(!result->HasError() && "load_conditions failed");

    for (size_t row = 0; row < result->RowCount(); ++row) {
      std::string id = result->GetValue(0, row).ToString();
      std::string position_ids_str = result->GetValue(1, row).ToString();
      std::string payout_nums_str = result->GetValue(2, row).ToString();
      auto payout_denom_val = result->GetValue(3, row);

      ConditionConfig cfg;

      // 解析 position_ids JSON 数组
      if (!position_ids_str.empty() && position_ids_str != "NULL") {
        auto pos_arr = json::parse(position_ids_str);
        for (const auto &p : pos_arr) {
          cfg.position_ids.push_back(p.get<std::string>());
        }
      }

      // 解析 payout_numerators JSON 数组
      if (!payout_nums_str.empty() && payout_nums_str != "NULL") {
        auto nums_arr = json::parse(payout_nums_str);
        for (const auto &n : nums_arr) {
          if (n.is_string()) {
            cfg.payout_numerators.push_back(std::stoll(n.get<std::string>()));
          } else {
            cfg.payout_numerators.push_back(n.get<int64_t>());
          }
        }
      }

      // payout_denominator
      if (!payout_denom_val.IsNull()) {
        std::string denom_str = payout_denom_val.ToString();
        cfg.payout_denominator = denom_str.empty() ? 0 : std::stoll(denom_str);
      }

      configs[id] = std::move(cfg);
    }

    return configs;
  }

  // 构建 positionId → (conditionId, outcomeIndex) 反向映射
  std::unordered_map<std::string, std::pair<std::string, int>> build_position_to_condition_map(
      const std::unordered_map<std::string, ConditionConfig> &conditions) {
    std::unordered_map<std::string, std::pair<std::string, int>> map;
    for (const auto &[cond_id, cfg] : conditions) {
      for (size_t i = 0; i < cfg.position_ids.size(); ++i) {
        map[cfg.position_ids[i]] = {cond_id, static_cast<int>(i)};
      }
    }
    return map;
  }

  // 获取所有用户列表(从 order、merge、redemption 中提取)
  std::vector<std::string> get_all_users() {
    auto result = conn_.Query(R"(
      SELECT DISTINCT user_addr FROM (
        SELECT maker as user_addr FROM enriched_order_filled WHERE maker IS NOT NULL
        UNION
        SELECT taker as user_addr FROM enriched_order_filled WHERE taker IS NOT NULL
        UNION
        SELECT stakeholder as user_addr FROM merge WHERE stakeholder IS NOT NULL
        UNION
        SELECT redeemer as user_addr FROM redemption WHERE redeemer IS NOT NULL
      )
    )");
    assert(!result->HasError() && "get_all_users failed");

    std::vector<std::string> users;
    users.reserve(result->RowCount());
    for (size_t row = 0; row < result->RowCount(); ++row) {
      users.push_back(result->GetValue(0, row).ToString());
    }
    return users;
  }

  // 加载指定用户的所有事件，按 timestamp 排序
  std::vector<Event> load_user_events(const std::string &user) {
    std::vector<Event> events;

    // 1. 加载订单事件(maker 视角: maker 是挂单方)
    // side=Buy: maker 买入 token, 支付 USDC
    // side=Sell: maker 卖出 token, 收取 USDC
    auto maker_result = conn_.Query(
        "SELECT timestamp, id, market, side, size, price "
        "FROM enriched_order_filled "
        "WHERE maker = '" +
        escape_sql(user) +
        "' "
        "ORDER BY timestamp, id");
    assert(!maker_result->HasError());

    for (size_t row = 0; row < maker_result->RowCount(); ++row) {
      Event e;
      e.timestamp = maker_result->GetValue(0, row).GetValue<int64_t>();
      e.event_type = "order_maker";
      e.event_id = maker_result->GetValue(1, row).ToString();
      e.user = user;
      e.token_id = maker_result->GetValue(2, row).ToString();
      e.side = maker_result->GetValue(3, row).ToString();

      auto size_val = maker_result->GetValue(4, row);
      e.size = size_val.IsNull() ? 0 : std::stoll(size_val.ToString());

      auto price_val = maker_result->GetValue(5, row);
      e.price = price_val.IsNull() ? 0.0 : price_val.GetValue<double>();

      events.push_back(std::move(e));
    }

    // 2. 加载订单事件(taker 视角: taker 是吃单方，side 相反)
    // maker side=Buy: taker 卖出 token
    // maker side=Sell: taker 买入 token
    auto taker_result = conn_.Query(
        "SELECT timestamp, id, market, side, size, price "
        "FROM enriched_order_filled "
        "WHERE taker = '" +
        escape_sql(user) +
        "' "
        "ORDER BY timestamp, id");
    assert(!taker_result->HasError());

    for (size_t row = 0; row < taker_result->RowCount(); ++row) {
      Event e;
      e.timestamp = taker_result->GetValue(0, row).GetValue<int64_t>();
      e.event_type = "order_taker";
      e.event_id = taker_result->GetValue(1, row).ToString();
      e.user = user;
      e.token_id = taker_result->GetValue(2, row).ToString();
      // taker 的 side 是 maker side 的反向
      std::string maker_side = taker_result->GetValue(3, row).ToString();
      e.side = (maker_side == "Buy") ? "Sell" : "Buy";

      auto size_val = taker_result->GetValue(4, row);
      e.size = size_val.IsNull() ? 0 : std::stoll(size_val.ToString());

      auto price_val = taker_result->GetValue(5, row);
      e.price = price_val.IsNull() ? 0.0 : price_val.GetValue<double>();

      events.push_back(std::move(e));
    }

    // 3. 加载 merge 事件 (用 collateral 铸造 condition 的所有 outcome tokens)
    auto merge_result = conn_.Query(
        "SELECT timestamp, id, condition, amount "
        "FROM merge "
        "WHERE stakeholder = '" +
        escape_sql(user) +
        "' "
        "ORDER BY timestamp, id");
    assert(!merge_result->HasError());

    for (size_t row = 0; row < merge_result->RowCount(); ++row) {
      Event e;
      e.timestamp = merge_result->GetValue(0, row).GetValue<int64_t>();
      e.event_type = "merge";
      e.event_id = merge_result->GetValue(1, row).ToString();
      e.user = user;
      e.condition_id = merge_result->GetValue(2, row).ToString();

      auto amount_val = merge_result->GetValue(3, row);
      e.amount = amount_val.IsNull() ? 0 : std::stoll(amount_val.ToString());

      events.push_back(std::move(e));
    }

    // 4. 加载 redemption 事件 (用 tokens 换回 collateral)
    auto redeem_result = conn_.Query(
        "SELECT timestamp, id, condition, payout "
        "FROM redemption "
        "WHERE redeemer = '" +
        escape_sql(user) +
        "' "
        "ORDER BY timestamp, id");
    assert(!redeem_result->HasError());

    for (size_t row = 0; row < redeem_result->RowCount(); ++row) {
      Event e;
      e.timestamp = redeem_result->GetValue(0, row).GetValue<int64_t>();
      e.event_type = "redemption";
      e.event_id = redeem_result->GetValue(1, row).ToString();
      e.user = user;
      e.condition_id = redeem_result->GetValue(2, row).ToString();

      auto payout_val = redeem_result->GetValue(3, row);
      e.amount = payout_val.IsNull() ? 0 : std::stoll(payout_val.ToString());

      events.push_back(std::move(e));
    }

    // 按 timestamp 和 event_id 排序
    std::sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
      if (a.timestamp != b.timestamp)
        return a.timestamp < b.timestamp;
      return a.event_id < b.event_id;
    });

    return events;
  }

  // 统计总事件数
  int64_t count_total_events() {
    auto result = conn_.Query(R"(
      SELECT
        (SELECT COUNT(*) FROM enriched_order_filled) +
        (SELECT COUNT(*) FROM merge) +
        (SELECT COUNT(*) FROM redemption)
    )");
    assert(!result->HasError());
    return result->GetValue(0, 0).GetValue<int64_t>();
  }

private:
  static std::string escape_sql(const std::string &s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
      if (c == '\'')
        r += "''";
      else
        r += c;
    }
    return r;
  }

  duckdb::Connection &conn_;
};

} // namespace rebuilder
