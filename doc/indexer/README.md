# PnL Rebuilder

## 核心表

| Entity              | Graph            | 必要字段                                                                                                   | 额外字段                  | 类型 | 增量sync                                                                                                |
| ------------------- | ---------------- | ---------------------------------------------------------------------------------------------------------- | ------------------------- | ---- | ------------------------------------------------------------------------------------------------------- |
| Condition           | Polymarket       | id(hashID), questionId, resolutionTimestamp, payoutNumerators, payoutDenominator, outcomeSlotCount, oracle | positionIds(N \* tokenID) | 状态 | `orderBy: resolutionTimestamp asc, where: {resolutionTimestamp_gte}` + skip, 记录最新非null时间戳，增量 |
| PnlCondition        | Profit and Loss  | id(hashID), positionIds(N \* tokenID)                                                                      |                           | 状态 | `orderBy: id asc, where: {id_gt}`，不可真正增量(hash主键)，需定期全量重拉                               |
| EnrichedOrderFilled | Polymarket       | timestamp, maker.id(usrID), taker.id(usrID), market.id(tokenID), side, size(1e6$), price(0~1)              |                           | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                 |
| Split               | Activity Polygon | timestamp, stakeholder(usrID), condition(questionID), amount(1e6$)                                         |                           | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                 |
| Merge               | Activity Polygon | timestamp, stakeholder(usrID), condition(questionID), amount(1e6$)                                         |                           | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                 |
| Redemption          | Activity Polygon | timestamp, redeemer(usrID), condition(questionID), indexSets("1":yes清零;"2":no清零), payout(1e6$)         |                           | 事件 | `orderBy: timestamp asc, where: {timestamp_gte}` + skip                                                 |

注意同时要支持: 1. 历史markets/questions总和在50W个(10min sync一遍) 2. 单问题多选项(outcomeSlotCount > 2) 3. 多问题相关(NegRisk)

增量sync(自动): 1. 每个entity按照自己的orderBy + skip 正常sync 2. 注意: 1. Condition(Polymarket)的null timestamp部分不全(未结算的condition没有resolutionTimestamp) 2. PnlCondition不可真正增量(hash主键，新数据hash可能比旧的小)

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

## 当前实现 (三阶段全量重建)

```
rebuild_all:
  Phase 1: load_metadata()    — 1次查询 condition 表 → token_map + cond_map
  Phase 2: collect_events()   — 4次全表扫描(EOF/Split/Merge/Redemption) → per-user RawEvent vectors
  Phase 3: replay_all()       — N workers 并行 sort+replay → Snapshot 链, 释放 RawEvent

  总计 5 次 SQL, 预估 20-40秒, 稳态内存 ~1.3GB
```

### Phase 1: 元数据 (~1秒, 1次查询)

```sql
SELECT id, outcomeSlotCount, positionIds, payoutNumerators, payoutDenominator
FROM condition
```

构建:

- `cond_map_`: condition_id → uint32 索引
- `token_map_`: token_id → (cond_idx, token_index) (从 positionIds JSON 解析)
- `conditions_[]`: ConditionInfo (outcome_count, payout 信息)

### Phase 2: 事件收集 (~10-30秒, 4次全表扫描)

每个事件统一为 32 字节 `RawEvent{timestamp, cond_idx, type, token_idx, amount, price}`, 直接 push 到对应用户的 vector:

1. **enriched_order_filled** ORDER BY timestamp → 每行 2 个事件:
   - taker: side=Buy→Buy, side=Sell→Sell (side = taker's direction)
   - maker: side=Buy→Sell, side=Sell→Buy (反向)
   - 通过 `token_map_[market]` 解析 (cond_idx, token_idx)
2. **split** ORDER BY timestamp → stakeholder 得 Split 事件 (all tokens)
3. **merge** ORDER BY timestamp → stakeholder 得 Merge 事件 (all tokens)
4. **redemption** ORDER BY timestamp → redeemer 得 Redemption 事件

用户 ID 通过 `intern_user()` 首次出现时分配 uint32 索引, 同时创建对应 event vector.

### Phase 3: 并行回放 (~5-10秒, 纯计算)

8-16 workers, 每个 worker 处理一批用户:

```cpp
for each user:
  sort(events, by timestamp)       // 几十~几百条, 基本 free
  for each event:
    apply_event(evt, replay_state)  // 更新 positions, cost, realized_pnl
    append Snapshot                 // 112B 固定大小, DDR 连续
  clear raw events                  // 用完即弃
```

### 事件回放 PnL 逻辑

| 事件       | positions 变化                | cost 变化                       | realized_pnl 变化                          |
| ---------- | ----------------------------- | ------------------------------- | ------------------------------------------ |
| Buy [i]    | `pos[i] += amount`            | `cost[i] += amount * price`     | 不变                                       |
| Sell [i]   | `pos[i] -= sell`              | `cost[i] -= proportional`       | `+= (sell * price - cost_removed) / 1e6`   |
| Split      | 所有 `pos[i] += amount`       | `cost[i] += amount * (1e6 / N)` | 不变                                       |
| Merge      | 所有 `pos[i] -= min(amt,pos)` | `cost[i] -= proportional`       | `+= (sell * (1e6/N) - cost_removed) / 1e6` |
| Redemption | 清零有仓位的 token            | `cost[i] = 0`                   | `+= (pos * payout_price - cost) / 1e6`     |

其中 Sell/Merge 的 cost_removed = `cost[i] * sell / pos[i]` (按比例移除)

### 内存生命周期

```
Phase 2 完成:  per-user RawEvent vectors    ~320 MB
Phase 3 逐用户:  RawEvent → Snapshot (边生产边释放)
稳态:          只剩 Snapshot + metadata     ~1.3 GB
```
