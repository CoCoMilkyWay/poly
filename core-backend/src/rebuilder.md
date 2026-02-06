# PnL Rebuilder

## 核心表

### condition — 市场条件 (状态表，会更新)

| 字段                     | 类型    | Rebuild | 含义                            |
| ------------------------ | ------- | ------- | ------------------------------- |
| id                       | VARCHAR | ✅      | conditionId，预测问题的唯一标识 |
| oracle                   | VARCHAR |         | 预言机地址                      |
| questionId               | VARCHAR |         | 问题ID                          |
| outcomeSlotCount         | INT     |         | outcome数量，通常是2            |
| resolutionTimestamp      | BIGINT  |         | 结算时间戳                      |
| payouts                  | JSON    |         | 结算比例(带格式)                |
| payoutNumerators         | JSON    | ✅      | 结算比例分子，如 `["1","0"]`    |
| payoutDenominator        | VARCHAR | ✅      | 结算比例分母                    |
| fixedProductMarketMakers | VARCHAR |         | AMM地址                         |
| resolutionHash           | VARCHAR |         | 结算哈希                        |

**Corner Cases**:

- `resolutionTimestamp, payouts, payoutNumerators, payoutDenominator 等为 null` → **未结算**，市场进行中

---

### pnl_condition — 条件配置 (状态表，会更新)

| 字段              | 类型    | Rebuild | 含义                                              |
| ----------------- | ------- | ------- | ------------------------------------------------- |
| id                | VARCHAR | ✅      | conditionId，预测问题的唯一标识                   |
| positionIds       | JSON    | ✅      | `[YES_token, NO_token]`，两个 outcome token 的 ID |
| payoutNumerators  | JSON    | ✅      | 结算比例分子，如 `["1","0"]`                      |
| payoutDenominator | VARCHAR | ✅      | 结算比例分母                                      |

**Corner Cases**:

- `payoutNumerators=[], payoutDenominator=0` → **未结算**，市场进行中
- `payoutNumerators=["1","0"], payoutDenominator=1` → **YES 赢**，YES=$1, NO=$0
- `payoutNumerators=["0","1"], payoutDenominator=1` → **NO 赢**，YES=$0, NO=$1
- `payoutNumerators=["1000000","0"], payoutDenominator=1000000` → 等价于 YES 赢(需要除以分母)

---

### enriched_order_filled — 订单成交 (事件日志，不变)

| 字段            | 类型    | Rebuild | 含义                                                |
| --------------- | ------- | ------- | --------------------------------------------------- |
| id              | VARCHAR |         | txHash_orderHash 组合                               |
| transactionHash | VARCHAR |         | 交易哈希                                            |
| timestamp       | BIGINT  | ✅      | Unix 时间戳                                         |
| maker           | VARCHAR | ✅      | 挂单方地址                                          |
| taker           | VARCHAR | ✅      | 吃单方地址                                          |
| orderHash       | VARCHAR |         | 订单哈希                                            |
| market          | VARCHAR | ✅      | **token ID (positionId)**，不是 conditionId！       |
| side            | VARCHAR | ✅      | **maker 的方向**：`Buy`=maker买入, `Sell`=maker卖出 |
| size            | VARCHAR | ✅      | 数量，单位 1e-6。`39600000` = 39.6 个 token         |
| price           | DOUBLE  | ✅      | 价格，0-1 之间。`0.22` = $0.22                      |

**Corner Cases**:

- `side` 是 **maker 的方向**，taker 方向相反
- `price` 可能有很多小数位(如 `0.1269999908496892883532907019005015`)
- 同一笔交易可能产生多条记录(多个 maker 撮合)

---

### merge — 铸造 (事件日志，不变)

| 字段               | 类型    | Rebuild | 含义                                   |
| ------------------ | ------- | ------- | -------------------------------------- |
| id                 | VARCHAR |         | 交易哈希                               |
| timestamp          | BIGINT  | ✅      | Unix 时间戳                            |
| stakeholder        | VARCHAR | ✅      | 用户地址                               |
| collateralToken    | VARCHAR |         | 抵押品地址 (USDC)                      |
| parentCollectionId | VARCHAR |         | 通常是 0x000...000                     |
| condition          | VARCHAR | ✅      | conditionId                            |
| partition          | JSON    |         | 分区，通常是 `["1","2"]`               |
| amount             | VARCHAR | ✅      | USDC 数量，单位 1e-6。`31000000` = $31 |

**业务含义**: 用户花 $X 铸造 X 个 YES + X 个 NO(等价于各 $0.50 买入)

**Corner Cases**:

- `collateralToken` 有两种：
  - `0x2791bca1f2de4661ed88a30c99a7a9449aa84174` — **USDC.e**(Bridged USDC，通过桥从以太坊转过来)
  - `0x3a3bd7bb9528e159577f7c2e685cc81a765002e2` — **Native USDC**(Circle 在 Polygon 原生发行)
- `partition` 固定是 `["1","2"]`

---

### redemption — 赎回 (事件日志，不变)

| 字段               | 类型    | Rebuild | 含义                       |
| ------------------ | ------- | ------- | -------------------------- |
| id                 | VARCHAR |         | 交易哈希                   |
| timestamp          | BIGINT  | ✅      | Unix 时间戳                |
| redeemer           | VARCHAR | ✅      | 用户地址                   |
| collateralToken    | VARCHAR |         | 抵押品地址 (USDC)          |
| parentCollectionId | VARCHAR |         | 通常是 0x000...000         |
| condition          | VARCHAR | ✅      | conditionId                |
| indexSets          | JSON    |         | 赎回的 outcome 集合        |
| payout             | VARCHAR | ✅      | 实际收到的 USDC，单位 1e-6 |

**业务含义**: 市场结算后，用户用 tokens 换回 USDC

**Corner Cases**:

- `payout=0` → 用户押的一方输了，tokens 归零
- `indexSets=["1"]` → 只赎回一个 outcome(部分赎回)
- `indexSets=["1","2"]` → 赎回两个 outcome(常见)
- rebuild 时 payout 不直接使用，而是根据持仓量 × payout_price 计算

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
