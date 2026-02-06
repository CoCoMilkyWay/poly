#pragma once

// ============================================================================
// PnL Rebuilder 数据结构定义
// ============================================================================

#include <cstdint>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace rebuilder {

// ============================================================================
// 持仓
// ============================================================================
struct Position {
  int64_t amount = 0;       // 持仓量
  int64_t avg_price = 0;    // 平均价 (1e6 = $1)
  int64_t realized_pnl = 0; // 已实现盈亏
  int64_t total_bought = 0; // 累计买入
};

// ============================================================================
// 快照(记录每个事件后的状态)
// ============================================================================
struct Snapshot {
  int64_t timestamp;
  std::string event_type;
  std::string event_id;
  std::string token_id;   // 本次事件涉及的 token
  std::string side;       // order: Buy/Sell
  int64_t size = 0;       // order: size
  double price = 0.0;     // order: price
  int64_t total_pnl = 0;  // 事件后的总 PnL(realized + unrealized)
};

// ============================================================================
// 用户元数据
// ============================================================================
struct UserMeta {
  int64_t first_ts = 0;
  int64_t last_ts = 0;

  // 异常统计
  std::vector<std::string> missing_conditions;
  std::vector<std::string> unresolved_redemptions;
  std::vector<std::tuple<std::string, int64_t, int64_t>> negative_amounts; // token_id, ts, amount

  // 计数
  int64_t order_count = 0;
  int64_t merge_count = 0;
  int64_t redemption_count = 0;
  int64_t skipped_count = 0;
};

// ============================================================================
// 用户重建结果
// ============================================================================
struct UserResult {
  std::string user;
  UserMeta meta;
  std::unordered_map<std::string, Position> positions; // token_id -> Position
  std::vector<Snapshot> snapshots;                     // 事件快照序列
};

// ============================================================================
// Condition 配置
// ============================================================================
struct ConditionConfig {
  std::vector<std::string> position_ids; // [yes_token, no_token]
  std::vector<int64_t> payout_numerators;
  int64_t payout_denominator = 0;
};

// ============================================================================
// 统一事件结构
// ============================================================================
struct Event {
  int64_t timestamp;
  std::string event_type; // "order_maker", "order_taker", "merge", "redemption"
  std::string event_id;
  std::string user;

  // order 字段
  std::string token_id;
  std::string side; // "Buy" / "Sell"
  int64_t size = 0;
  double price = 0.0;

  // merge/redemption 字段
  std::string condition_id;
  int64_t amount = 0; // merge.amount 或 redemption.payout
};

// ============================================================================
// 重建进度状态
// ============================================================================
struct RebuildProgress {
  int64_t total_users = 0;
  int64_t processed_users = 0;
  int64_t total_events = 0;
  int64_t processed_events = 0;
  bool running = false;
  std::string error;
};

} // namespace rebuilder
