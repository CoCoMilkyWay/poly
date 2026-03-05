## 交易口径事件（符号驱动，支持做空）
> `qty = abs(amount)`
> `dir = sign(amount)`（`+1 / -1 / 0`）
> `px = price_1e6 / 1e6`
> `cost_before = cost[token_idx]`（本事件前）
> `pos_before = pos[token_idx]`（本事件前）
> `has_usd = collateral ∈ {USDC(1), USDCe(2), USDT(3), WrappedUSDCe(4)}`

**Collateral 规则**: 只有 USD 类抵押物 (`has_usd=true`) 才计算 `cost / realized_delta / last_price`；非 USD 仅更新 `pos`。  
**方向规则**: `EventType` 决定“经济语义”，`amount` 符号决定“方向”；同一 `EventType` 可出现正负。

| 事件族                                                     | `amount > 0` (正向腿)                                        | `amount < 0` (反向腿)                                         | realized 规则                                                | last_price               |
| ---------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------- | ------------------------------------------------------------ | ------------------------ |
| `Order* / FPMM* / Split* / Merge* / Redemption*`           | 先平空(`pos<0`)再开多；开多部分 `cost += open_long_qty * px` | 先平多(`pos>0`)再开空；开空部分 `cost -= open_short_qty * px` | 仅“平掉已有仓位”部分计 realized；开新仓部分不计当期 realized | 仅 `Order* / FPMM*` 更新 |
| `Convert`                                                  | 先平空再开多；开多部分 `cost += open_long_qty * convert_px`  | 先平多再开空；开空部分 `cost -= open_short_qty * convert_px`  | `convert_px = (popcount-1)/popcount`，仅平仓部分计 realized  | 否                       |
| `TransferIn* / TransferOut* / FPMMLPRemove / FPMMLPReturn` | 先平空再开多；`cost` 不增加                                  | 先平多再开空；`cost` 不增加（保持 transfer 语义）             | 始终 `0`                                                     | 否                       |
| `FPMMLPAdd`                                                | 不改 `pos/cost`                                              | 不改 `pos/cost`                                               | `0`                                                          | 否                       |

`amount == 0` 视为 no-op（仅事件计数推进，不改持仓/成本/PnL）。

## Unrealized PnL 计算
```
last_price[(cond_idx, token_idx)] = 该token最近一次交易的成交价
unrealized_pnl = Σ(pos[i] * last_price[i] / 1e6 - cost[i])  // 对所有 pos[i] != 0 且 last_price[i] > 0
```
- 只有 Order/FPMM 交易事件会更新 last_price
- Split/Merge/Transfer 等不更新 last_price
- 若某token从未交易过（如TransferIn获得）,该token不计入unrealized（last_price=0）

## Flow 图

```text
stage3_bootstrap
└─ 初始化 schema 与索引；若游标不存在则写入初始 cursor

stage3_sync_tick
├─ 0) 读取游标与元数据
│  ├─ 读 s3_sync_cursor(id=1) -> SyncCursor
│  ├─ 刷新 condition_meta(只读缓存)
│  └─ 计算批次游标起点 (last_sort_key + last_pk_tiebreak)
├─ 1) 拉取输入事件(Stage2 输出)
│  ├─ SELECT * FROM stage2.user_event
│  │    WHERE (sort_key, user_addr, cond_idx, event_type, token_idx) > cursor
│  │    ORDER BY sort_key, user_addr, cond_idx, event_type, token_idx
│  └─ 若无事件 -> 结束本 tick
├─ 2) 预加载状态
│  ├─ 扫描 batch_events,提取 touched keys
│  │  ├─ touched_cond_keys = {(user_addr, cond_idx) | cond_idx >= 0}
│  │  └─ touched_users     = {user_addr}
│  ├─ 读 s3_user_cond_state -> token_state_map (含 last_price)
│  ├─ 读 s3_user_summary    -> user_state_map
│  └─ 初始化临时结构
│     ├─ fact_rows
│     └─ dirty_cond_keys / dirty_users
├─ 3) 回放循环(for evt in batch_events, 按 sort_key + PK 升序)
│  ├─ 3.1 输入断言
│  │  ├─ (sort_key, user_addr, cond_idx, event_type, token_idx) 严格递增
│  │  ├─ cond_idx < 0: 不进入状态机(仅事实行)
│  │  ├─ cond_idx >= 0: condition_meta 必须存在
│  │  ├─ token_idx ∈ [0, outcome_count)
│  │  ├─ event_type 合法；amount 符号不做类型硬断言（允许同类型正负共存）
│  │  ├─ amount==0 允许（no-op）
│  │  └─ 内部使用 double 计算,写入 DB 时 round() 转 int64
│  ├─ 3.2 路由
│  │  ├─ 方向由 amount 符号决定:正向腿(amount>0) / 反向腿(amount<0)
│  │  ├─ 事件族决定经济语义:price类 / convert类 / transfer类 / lp_add类
│  │  └─ LP添加: FPMMLPAdd(仅记录,不改pos/cost)
│  ├─ 3.3 应用规则
│  │  ├─ 公共计算
│  │  │  ├─ qty = abs(amount), px = price_1e6/1e6
│  │  │  └─ amount==0: no-op
│  │  ├─ 正向腿(amount>0): 先平空(pos<0)再开多
│  │  │  ├─ price/convert 类: 开多部分增加 cost
│  │  │  └─ transfer/LP-return 类: 开多部分不增加 cost
│  │  ├─ 反向腿(amount<0): 先平多(pos>0)再开空
│  │  │  ├─ price/convert 类: 开空部分记负 cost（入场信用）
│  │  │  └─ transfer/LP-return 类: 开空部分不改 cost
│  │  ├─ realized 仅在“平掉已有仓位”时产生
│  │  │  ├─ price 类: realized = closed_qty * px - cost_removed
│  │  │  └─ convert 类: realized = closed_qty * ((popcount-1)/popcount) - cost_removed
│  │  └─ FPMMLPAdd: 不改 token pos/cost; realized_delta=0
│  ├─ 3.4 更新 last_price (仅 Order/FPMM 交易)
│  │  └─ 若 event_type ∈ {OrderBuy, OrderSell, FPMMBuy, FPMMSell} 且 price > 0:
│  │       last_price[(cond_idx, token_idx)] = price
│  ├─ 3.5 计算 unrealized_pnl
│  │  └─ unrealized_pnl = Σ(pos[i] * last_price[(cond_idx,i)] / 1e6 - cost[i]) for all i where pos[i]!=0 && last_price[i]>0
│  ├─ 3.6 更新 user_state_map[user_addr]
│  │  ├─ total_events += 1
│  │  ├─ total_realized_pnl += realized_delta
│  │  └─ last_sort_key = evt.sort_key
│  └─ 3.7 产出事实行
│     └─ append EventFactRow(user_addr, sort_key, cond_idx, token_idx, event_type,
│                            realized_delta, realized_cum, unrealized_pnl, token_count)
├─ 4) 批次收尾校验
│  ├─ token 状态有限: pos/cost 必须是 finite（允许负数,表示短仓/负成本）
│  ├─ fact_rows.size == batch_events.size
│  └─ 任一失败 -> rollback,cursor 不推进
├─ 5) 单事务提交
│  ├─ upsert s3_user_cond_state(dirty_cond_keys) (含 last_price)
│  ├─ upsert s3_user_summary(dirty_users)
│  ├─ insert s3_user_event_fact(fact_rows)
│  └─ update s3_sync_cursor(...)
└─ 6) 查询路径(只读)
   ├─ users_sorted(limit)           -> s3_user_summary(用户列表)
   ├─ user_pnl_timeline(user)       -> 预热并缓存该用户 timeline + snapshots
   └─ user_positions_at(user, sk)   -> 命中内存 snapshots 返回 positions
```

## 数据结构

```text
符号: E=事件总数, U=有事件用户数, C=condition数, K=活跃(user,cond)对数

InputEvent (实例数=E)
ConditionMeta (实例数=C)
  -> TokenCondState (实例数=K, K=|{(u,c) | 对应状态存在}|, 且 K<=U*C)
      -> EventFactRow (实例数=E, 每个InputEvent对应1行)
      -> UserSummaryState (实例数=U)
      -> SyncCursor (实例数=1)
      -> UserQueryCache (实例数<=U, 命中后按用户懒加载)
```

```text
// Stage3 内部统一输入结构 (用于回放/状态机)
struct InputEvent {
  string user_hex;        // 查询路径可为空，批处理路径必填
  int64  sort_key;
  int32  cond_idx;
  int32  event_type;
  int32  token_idx;
  int32  collateral;
  int64  amount;          // signed, 1e6
  int64  price_1e6;
}

// Condition 元数据
struct ConditionMeta {
  uint8 outcome_count;
  int64 payout_numerators[8];
}

// 用户在某 condition 下的状态
// 内部计算使用 double，持久化时 round() 转 int64
struct TokenCondState {
  double pos[8];          // 各 outcome 持仓 (内部 double，DB int64; 可负=短仓)
  double cost[8];         // 各 outcome 成本 (内部 double，DB int64; 可负=短仓信用)
  double last_price[8];   // 各 outcome 最近成交价 (内部 double，DB int64)
  double realized_pnl;    // 该 condition 已实现 pnl (内部 double，DB int64)
  int64 event_count;
  int64 last_sort_key;
}

// 用户摘要 (持久化)
struct UserSummaryState {
  int64  total_events;
  int64  total_realized_pnl;
  int32  active_conditions;
  int64  last_sort_key;
}

// 事件事实行 (持久化, 用于构建 timeline)
struct EventFactRow {
  Address20 user_addr;
  int64   sort_key;
  int32   cond_idx;
  int32   token_idx;
  int32   event_type;
  int64   realized_delta;     // 本事件 realized 增量
  int64   realized_cum;       // 累计 realized pnl
  int64   unrealized_pnl;     // 本事件后的 unrealized pnl
  int32   token_count;        // 持仓 token 种数(|pos|>0)
}

// 游标
struct SyncCursor {
  int64 last_sort_key;
  Address20 last_user_addr;
  int32 last_cond_idx;
  int32 last_event_type;
  int32 last_token_idx;
  int64 processed_events;
}

// 查询缓存（单用户）
struct UserQueryCache {
  string user_addr_lower;
  vector<TimelineRow> timeline;
  vector<{int64 sort_key; vector<PositionRow> positions;}> snapshots;
}
```

## API 设计

### GET /api/stage3-status
| entry                  | type   | required | description          |
| ---------------------- | ------ | -------- | -------------------- |
| query                  | object | 否       | 无参数               |
| resp.syncing           | bool   | 是       | 是否正在同步         |
| resp.last_block        | i64    | 是       | 已同步到的最新 block |
| resp.head_block        | i64    | 是       | 链上当前 head block  |
| resp.behind_blocks     | i64    | 是       | 落后 block 数        |
| resp.behind_chunks     | i64    | 是       | 预估剩余 chunk 数    |
| resp.blocks_per_second | f64    | 是       | 同步速度             |
| resp.eta_seconds       | f64    | 是       | 预计剩余秒数         |
| resp.ready             | bool   | 是       | 是否可用于查询       |

### GET /api/stage3-users?limit=1000
| entry         | type   | required | description                  |
| ------------- | ------ | -------- | ---------------------------- |
| query.limit   | i32    | 否       | 返回条数上限，默认 `1000`    |
| resp[]        | array  | 是       | 用户列表（按 `events` 降序） |
| resp[].user   | string | 是       | 用户地址（lowercase）        |
| resp[].events | i64    | 是       | 用户事件总数                 |
| resp[].rpnl   | i64    | 是       | 累计已实现 PnL               |
| resp[].upnl   | i64    | 是       | 当前未实现 PnL               |

### GET /api/stage3-pnl?user=0x...
| entry                | type   | required | description                      |
| -------------------- | ------ | -------- | -------------------------------- |
| query.user           | string | 是       | 用户地址                         |
| resp.user            | string | 是       | 用户地址                         |
| resp.block           | i64    | 是       | 该用户最新事件所在 block         |
| resp.total_events    | i64    | 是       | 该用户总事件数                   |
| resp.timeline[]      | array  | 是       | 全量事件序列（不因 cursor 截断） |
| resp.timeline[].sk   | i64    | 是       | sort_key（`block*1e9+log_idx`）  |
| resp.timeline[].ty   | u8     | 是       | 事件类型                         |
| resp.timeline[].rpnl | i64    | 是       | 累计已实现 PnL                   |
| resp.timeline[].upnl | i64    | 是       | 该时刻未实现 PnL                 |
| resp.timeline[].tk   | i32    | 是       | 持仓 token 种数                  |
| error.404            | object | 条件     | 用户不存在或无事件时返回         |

### GET /api/stage3-positions?user=0x...&sort_key=...
| entry                 | type   | required | description                            |
| --------------------- | ------ | -------- | -------------------------------------- |
| query.user            | string | 是       | 用户地址                               |
| query.sort_key        | i64    | 是       | 目标 cursor                            |
| resp.user             | string | 是       | 用户地址                               |
| resp.sort_key         | i64    | 是       | 回显请求的 cursor                      |
| resp.block            | i64    | 是       | 快照所在 block                         |
| resp.positions[]      | array  | 是       | 指定 cursor 的持仓快照（仅 positions） |
| resp.positions[].ci   | u32    | 是       | cond_idx                               |
| resp.positions[].ti   | u8     | 是       | token_idx                              |
| resp.positions[].qty  | i64    | 是       | 持仓数量（`1e6=1 token`，可负）        |
| resp.positions[].cost | i64    | 是       | 成本基础（`1e6=$1`，可负）             |
| resp.positions[].lp   | i64    | 是       | 最近成交价（`1e6=$1`）                 |
