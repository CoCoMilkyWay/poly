# PnL Rebuilder

## 核心表

| Entity              | Graph            | 必要字段                                                                                                   | 额外字段                 | 类型 | 增量sync                                                                                               |
| ------------------- | ---------------- | ---------------------------------------------------------------------------------------------------------- | ------------------------ | ---- | ------------------------------------------------------------------------------------------------------- |
| Condition           | Polymarket       | id(hashID), questionId, resolutionTimestamp, payoutNumerators, payoutDenominator, outcomeSlotCount, oracle | positionIds(N * tokenID) | 状态 | `orderBy: resolutionTimestamp asc, where: {resolutionTimestamp_gte}` + skip, 记录最新非null时间戳，增量 |
| PnlCondition        | Profit and Loss  | id(hashID), positionIds(N * tokenID)                                                                       |                          | 状态 | `orderBy: id asc, where: {id_gt}`，不可真正增量(hash主键)，需定期全量重拉                              |
| EnrichedOrderFilled | Polymarket       | timestamp, maker.id(usrID), taker.id(usrID), market.id(tokenID), side, size(1e6$), price(0~1)              |                          | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                |
| Split               | Activity Polygon | timestamp, stakeholder(usrID), condition(questionID), amount(1e6$)                                         |                          | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                |
| Merge               | Activity Polygon | timestamp, stakeholder(usrID), condition(questionID), amount(1e6$)                                         |                          | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                |
| Redemption          | Activity Polygon | timestamp, redeemer(usrID), condition(questionID), indexSets("1":yes清零;"2":no清零), payout(1e6$)         |                          | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                |

注意同时要支持:
    1. 历史markets/questions总和在50W个(10min sync一遍)
    2. 单问题多选项(outcomeSlotCount > 2)
    3. 多问题相关(NegRisk)

增量sync(自动):
    1. 每个entity按照自己的orderBy + skip 正常sync
    2. 注意:
       1. Condition(Polymarket)的null timestamp部分不全(未结算的condition没有resolutionTimestamp)
       2. PnlCondition不可真正增量(hash主键，新数据hash可能比旧的小)

---
**Split/Merge/Redemption 使用场景**:

1. **Split(铸造)— 市场进行中**
   - 操作: USDC → YES + NO (固定 $0.50/$0.50)
   - 做市: 铸造后挂单卖双边，提供流动性
   - 套利: 当 YES + NO 市场价之和 > $1 时，铸造后卖双边获利
   - 方向性建仓: 当看空方流动性好时，铸造后卖看空方，建仓看多方

2. **Merge(销毁)— 市场进行中**
   - 操作: YES + NO → USDC (固定 $0.50/$0.50)
   - 做市: 买入双边后销毁，退出流动性
   - 套利: 当 YES + NO 市场价之和 < $1 时，买双边后销毁获利
   - 方向性平仓: 当看多方流动性差时，买看空方后销毁双边，平仓看多方

3. **Redemption(赎回)— 市场结算后**
   - 操作: tokens → USDC (只能在市场结算后操作)
   - 用途: 赎回 winning tokens 获得收益，losing tokens 归零 (价格由 payoutNumerators/payoutDenominator 决定)


## 数据库数据结构

业务表(6):
- `condition` (id PK, questionId, oracle, outcomeSlotCount, resolutionTimestamp?, payoutNumerators?, payoutDenominator?, positionIds?) idx: PK
- `enriched_order_filled` (id PK, timestamp, maker, taker, market, side, size, price) idx: timestamp
- `split` (id PK, timestamp, stakeholder, condition, amount) idx: timestamp
- `merge` (id PK, timestamp, stakeholder, condition, amount) idx: timestamp
- `redemption` (id PK, timestamp, redeemer, condition, indexSets, payout) idx: timestamp
- `pnl_condition` (id PK, positionIds?) — positionIds来源表，需定期全量重拉

基础设施表(3):
- `sync_state` (source+entity PK, cursor_value, cursor_skip, last_sync_at)
- `entity_stats_meta` (source+entity PK, total/success/fail计数, total_rows_synced, total_api_time_ms, success_rate, updated_at)
- `indexer_fail_meta` (source+entity+indexer PK, fail_requests, updated_at)

## 当前实现

```
rebuild_all:
  1. load_conditions() → 1 次查询，加载 46万 conditions
  2. get_all_users()   → 1 次查询，获取 82万用户
  3. 72 workers 并行：
     每个 worker 对每个用户调用 load_user_events(user)
       → 4 次 SQL 查询 (maker, taker, merge, redemption)
```

### 步骤 1：加载全局条件数据

**1.1 查询 `condition` 表**

```sql
SELECT id, payoutNumerators, payoutDenominator
FROM condition
```

- 返回：46 万个 condition
- 用途：判断市场是否已结算、结算比例

**1.2 查询 `pnl_condition` 表**

```sql
SELECT id, positionIds, payoutNumerators, payoutDenominator
FROM pnl_condition
```

- 返回：46 万个 condition 配置
- 用途：
  - `positionIds[0]` = YES token ID
  - `positionIds[1]` = NO token ID
  - 将 `enriched_order_filled.market` (token ID) 映射到 conditionId

**内存数据结构**：

```cpp
map<string, Condition> conditions;
// key = conditionId
// value = {positionIds, payoutNumerators, payoutDenominator}
```

---

### 步骤 2：获取所有用户

```sql
SELECT DISTINCT maker FROM enriched_order_filled
UNION
SELECT DISTINCT taker FROM enriched_order_filled
UNION
SELECT DISTINCT stakeholder FROM merge
UNION
SELECT DISTINCT redeemer FROM redemption
```

- 返回：82 万个唯一用户地址

---

### 步骤 3：并行处理每个用户

对每个用户 `user_address`，执行 4 个查询：

**3.1 作为 Maker 的订单**

```sql
SELECT timestamp, market, side, size, price
FROM enriched_order_filled
WHERE maker = 'user_address'
ORDER BY timestamp ASC
```

- `side = "Buy"` → maker 买入：`持仓 += size`, `成本 += size × price`
- `side = "Sell"` → maker 卖出：`持仓 -= size`, `收入 += size × price`

**3.2 作为 Taker 的订单**

```sql
SELECT timestamp, market, side, size, price
FROM enriched_order_filled
WHERE taker = 'user_address'
ORDER BY timestamp ASC
```

- `side = "Buy"` → **taker 卖出**(与 maker 相反)：`持仓 -= size`
- `side = "Sell"` → **taker 买入**：`持仓 += size`

**3.3 Merge(铸造)**

```sql
SELECT timestamp, condition, amount
FROM merge
WHERE stakeholder = 'user_address'
ORDER BY timestamp ASC
```

- 铸造 `amount` USDC → 获得 `amount` YES + `amount` NO
- YES 持仓 += amount, NO 持仓 += amount, 成本 += amount

**3.4 Redemption(赎回)**

```sql
SELECT timestamp, condition, payout
FROM redemption
WHERE redeemer = 'user_address'
ORDER BY timestamp ASC
```

- 销毁 token，实际收入 = `payout`
- 根据 `payoutNumerators` 计算持仓减少量

---

### 步骤 4：按时间回放事件

```cpp
// 合并 4 个查询结果
vector<Event> events = load_user_events(user_address);
sort(events, [](auto& a, auto& b) { return a.timestamp < b.timestamp; });

// 按时间顺序更新持仓
map<string, Position> positions;  // key = conditionId
for (auto& event : events) {
  auto& pos = positions[event.condition_id];

  switch (event.type) {
    case BUY:
      pos.quantity += event.amount;
      pos.cost += event.amount * event.price;
      break;
    case SELL:
      double realized_pnl = event.amount * event.price
                          - (pos.cost / pos.quantity * event.amount);
      pos.quantity -= event.amount;
      pos.realized_pnl += realized_pnl;
      break;
    case MERGE:
      // 同时增加 YES 和 NO
      positions[YES_token].quantity += event.amount;
      positions[NO_token].quantity += event.amount;
      pos.cost += event.amount;
      break;
    case REDEMPTION:
      pos.realized_pnl += event.payout;
      // 持仓根据 payoutNumerators 减少
      break;
  }
}
```

---

### 关键字段映射

| 业务概念     | enriched_order_filled | merge       | redemption | condition        |
| ------------ | --------------------- | ----------- | ---------- | ---------------- |
| **用户地址** | maker / taker         | stakeholder | redeemer   | -                |
| **市场标识** | market (token ID)     | condition   | condition  | id (conditionId) |
| **时间**     | timestamp             | timestamp   | timestamp  | -                |
| **数量**     | size                  | amount      | payout     | -                |
| **价格**     | price                 | -           | -          | payoutNumerators |
| **方向**     | side                  | -           | -          | -                |

---

## 性能瓶颈

82万用户 × 4 次查询 = **328万次 SQL 查询**

计算逻辑(Worker::process_user)很简单，瓶颈在 I/O

## 优化方案

| 改动                                 | 效果                      |
| ------------------------------------ | ------------------------- |
| 一次性加载所有数据，内存中按用户分组 | 328万次查询 → 4次         |
| rebuild_all 时不存 Snapshot          | 省 1100万个 Snapshot 内存 |
| worker 数减少到 8-16                 | 减少 CPU 饥饿             |

## 数据同步问题

### The Graph 索引机制

**主键排序陷阱**：

- Entity `id` 是 **hash**(如 `0x00008da1...`)，按字典序排序
- 新 condition 的 hash 可能比旧的小 → `id > cursor` 会漏数据
- 状态表更新(如 condition 结算)不会改变 id → 拉不到更新

**正确的增量同步**：

| 表                    | 类型     | 同步方式                                    | 原因                      |
| --------------------- | -------- | ------------------------------------------- | ------------------------- |
| enriched_order_filled | 事件日志 | `orderBy: timestamp, where: {timestamp_gt}` | append-only，有时间戳索引 |
| merge                 | 事件日志 | `orderBy: timestamp, where: {timestamp_gt}` | append-only，有时间戳索引 |
| redemption            | 事件日志 | `orderBy: timestamp, where: {timestamp_gt}` | append-only，有时间戳索引 |
| condition             | 状态表   | 全量拉取或 webhook                          | 会更新，无可靠时间戳      |
| pnl_condition         | 状态表   | 全量拉取或 webhook                          | 会更新，无可靠时间戳      |

**The Graph 内部原理**：

- 写入时按 `blockNumber` 顺序处理，`timestamp` 单调递增
- 内部有 `(timestamp, id)` 二级索引，顺序扫描成本低
- 主键 hash 用于唯一性/分布，不用于增量
- 查询限制：`first <= 1000`，禁止深度分页

**Query 示例**(事件表)：

```graphql
{
  enrichedOrderFilleds(
    first: 1000
    orderBy: timestamp
    orderDirection: asc
    where: { timestamp_gte: 1759503806 }
  ) {
    id
    timestamp
    maker
    taker
    market
    side
    size
    price
  }
}
```
