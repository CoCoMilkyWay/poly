## 交易口径事件
> `qty = abs(amount)`
> `px = price_1e6 / 1e6`
> `cost_before = cost[token_idx]`(本事件处理前的持仓成本)
> `pos_before = pos[token_idx]`(本事件处理前的持仓数量)
> `has_usd = collateral ∈ {USDC(1), USDCe(2), USDT(3), WrappedUSDCe(4)}`

**Collateral 规则**: 只有 USD 类抵押物 (`has_usd=true`) 才计算 cost/realized_delta/last_price；非 USD 只更新 pos。

| EventType                                                  | 是否计算   | 状态更新(pos/cost)                                   | realized_delta 公式                                                | 更新 last_price |
| ---------------------------------------------------------- | ---------- | ---------------------------------------------------- | ------------------------------------------------------------------ | --------------- |
| OrderBuy                                                   | 是         | `pos += qty; cost += qty * px`                       | `0`                                                                | 是              |
| OrderSell                                                  | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        | 是              |
| SplitNormal / SplitNegRisk / SplitNonPoly                  | 是(重分类) | `pos += qty; cost += qty * px`                       | `0`                                                                | 否              |
| MergeNormal / MergeNegRisk / MergeNonPoly                  | 是(重分类) | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        | 否              |
| Redemption / RedemptionNonPoly                             | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        | 否              |
| Convert                                                    | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * (popcount - 1) / popcount - cost_before * qty / pos_before` | 否              |
| FPMMBuy                                                    | 是         | `pos += qty; cost += qty * px`                       | `0`                                                                | 是              |
| FPMMSell                                                   | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        | 是              |
| FPMMLPAdd                                                  | 否(资金流) | 不改 token `pos/cost`(0->FPMM mint)                  | `0`                                                                | 否              |
| FPMMLPRemove                                               | 是(LP取回) | `pos += qty; cost` 不变                              | `0`                                                                | 否              |
| FPMMLPReturn                                               | 是(LP退回) | `pos += qty; cost` 不变                              | `0`                                                                | 否              |
| TransferInNegRisk / TransferInOther / TransferInNonPoly    | 是(转入)   | `pos += qty; cost` 不变                              | `0`                                                                | 否              |
| TransferOutNegRisk / TransferOutOther / TransferOutNonPoly | 是(转出)   | `cost -= cost_before * qty / pos_before; pos -= qty` | `0`                                                                | 否              |

## Unrealized PnL 计算
```
last_price[(cond_idx, token_idx)] = 该token最近一次交易的成交价
unrealized_pnl = Σ(pos[i] * last_price[i] - cost[i])  // 仅对 pos[i] > 0 的token
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
│  │  ├─ event_type 与 amount 方向匹配
│  │  └─ 乘除使用 i128 中间值,回写前断言不溢出
│  ├─ 3.2 路由
│  │  ├─ Buy类: OrderBuy/FPMMBuy/Split*/FPMMLPRemove/FPMMLPReturn
│  │  ├─ Sell类: OrderSell/FPMMSell/Merge*/Redemption*/Convert
│  │  ├─ TransferIn类: TransferIn*
│  │  ├─ TransferOut类: TransferOut*
│  │  └─ LP添加: FPMMLPAdd(仅记录,不改pos)
│  ├─ 3.3 应用规则
│  │  ├─ 公共计算
│  │  │  ├─ qty = abs(amount), px = price_1e6/1e6
│  │  │  └─ 卖出/转出前快照: cost_before=cost[token_idx], pos_before=pos[token_idx]
│  │  ├─ Buy类: pos += qty; cost += qty*px; realized_delta = 0
│  │  ├─ Sell类:
│  │  │  ├─ assert pos_before > 0 && pos_before >= qty
│  │  │  ├─ cost_removed = cost_before * qty / pos_before
│  │  │  ├─ cost -= cost_removed; pos -= qty
│  │  │  ├─ 非 Convert: realized_delta = qty*px - cost_removed
│  │  │  └─ Convert: realized_delta = qty*(popcount-1)/popcount - cost_removed
│  │  ├─ TransferIn类: pos += qty; cost 不变; realized_delta = 0
│  │  ├─ TransferOut类:
│  │  │  ├─ assert pos_before > 0 && pos_before >= qty
│  │  │  ├─ cost_removed = cost_before * qty / pos_before
│  │  │  └─ cost -= cost_removed; pos -= qty; realized_delta = 0
│  │  ├─ FPMMLPRemove/Return: pos += qty; cost 不变; realized_delta = 0
│  │  └─ FPMMLPAdd: 不改 token pos/cost; realized_delta = 0(仅记录事件)
│  ├─ 3.4 更新 last_price (仅 Order/FPMM 交易)
│  │  └─ 若 event_type ∈ {OrderBuy, OrderSell, FPMMBuy, FPMMSell} 且 price > 0:
│  │       last_price[(cond_idx, token_idx)] = price
│  ├─ 3.5 计算 unrealized_pnl
│  │  └─ unrealized_pnl = Σ(pos[i] * last_price[(cond_idx,i)] - cost[i]) for all i where pos[i]>0
│  ├─ 3.6 更新 user_state_map[user_addr]
│  │  ├─ total_events += 1
│  │  ├─ total_realized_pnl += realized_delta
│  │  └─ last_sort_key = evt.sort_key
│  └─ 3.7 产出事实行
│     └─ append EventFactRow(user_addr, sort_key, cond_idx, token_idx, event_type,
│                            realized_delta, realized_cum, unrealized_pnl, token_count)
├─ 4) 批次收尾校验
│  ├─ token 状态非负: pos>=0 且 cost>=0
│  ├─ fact_rows.size == batch_events.size
│  └─ 任一失败 -> rollback,cursor 不推进
├─ 5) 单事务提交
│  ├─ upsert s3_user_cond_state(dirty_cond_keys) (含 last_price)
│  ├─ upsert s3_user_summary(dirty_users)
│  ├─ insert s3_user_event_fact(fact_rows)
│  ├─ checkpoint 规则(每个 chunk 触发)
│  │  └─ 对 touched_users,将其当前 s3_user_cond_state 中“持仓非零或 realized 非零”的 condition 快照写入 s3_user_cond_checkpoint
│  └─ update s3_sync_cursor(...)
└─ 6) 查询路径(只读)
   ├─ users_sorted(limit)           -> s3_user_summary(用户列表)
   └─ user_pnl_data(user, block)    -> 返回 {positions, timeline} 两个紧凑表
```

## 数据结构

```text
type Address20 = bytes20

// Stage2 输入事件 (数据库原始行)
struct UserEvent {
  Address20 user_addr;
  int64   sort_key;
  int32   cond_idx;      // unknown=-1
  int32   event_type;    // EventType
  int32   token_idx;     // unknown=-1
  int32   collateral;
  int64   amount;        // 带符号,6 decimals
  int64   price_1e6;     // 1e6 = $1, invariant: price_1e6 >= 0
}

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

// Stage3 timeline 构建结构 (只承载事实表字段)
struct TimelineEvent {
  int64  sort_key;
  int32  cond_idx;
  int32  event_type;
  int32  token_idx;
  int64  realized_cum;
  int64  unrealized_pnl;
  int32  token_count;
}

// Condition 元数据
struct ConditionMeta {
  uint8 outcome_count;
  int64 payout_numerators[8];
}

// 用户在某 condition 下的状态 (持久化)
struct TokenCondState {
  int64 pos[8];          // 各 outcome 持仓
  int64 cost[8];         // 各 outcome 成本
  int64 last_price[8];   // 各 outcome 最近成交价
  int64 realized_pnl;    // 该 condition 已实现 pnl
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
  int32   token_count;        // 持仓 token 种数
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
```

## API 设计

### GET /api/stage3-status
同步状态
```json
{
  "syncing": true,
  "last_block": 12345678,
  "head_block": 12350000,
  "behind_blocks": 4322,
  "behind_chunks": 1,
  "blocks_per_second": 1234.5,
  "eta_seconds": 3.5,
  "ready": false
}
```

### GET /api/stage3-users?limit=1000
用户列表（按事件数降序）
```json
[
  {"user": "0x1234...", "events": 12345, "rpnl": 1234567890, "upnl": 234567890},
  ...
]
```

### GET /api/stage3-pnl?user=0x...&block=12345678
PnL数据（核心接口）

**输入参数**：
- `user`: 用户地址 (必填)
- `block`: 目标区块号 (可选, 默认=最新)

**返回语义**：
- 若用户不存在或无事件，返回 `404`，body 为 `{"error":"User not found or no events"}`

**输出**：两个紧凑表

```json
{
  "user": "0x1234...",
  "block": 12345678,
  "total_events": 12345,

  "positions": [
    {
      "ci": 123,           // cond_idx
      "ti": 0,             // token_idx
      "qty": 1000000,      // 持仓数量 (1e6)
      "cost": 500000,      // 成本 (1e6)
      "lp": 550000         // last_price (1e6)
    },
    ...
  ],

  "timeline": [
    {
      "sk": 12340000000001,  // sort_key
      "ty": 0,              // event_type
      "rpnl": 100000,       // realized_pnl_cum
      "upnl": 50000,        // unrealized_pnl
      "tk": 5               // token_count
    },
    ...
  ]
}
```

**字段说明**：

| 表        | 字段 | 类型 | 说明                         |
| --------- | ---- | ---- | ---------------------------- |
| positions | ci   | u32  | cond_idx                     |
| positions | ti   | u8   | token_idx                    |
| positions | qty  | i64  | 持仓数量 (1e6 = 1 token)     |
| positions | cost | i64  | 成本基础 (1e6 = $1)          |
| positions | lp   | i64  | 最近成交价 (1e6 = $1)        |
| timeline  | sk   | i64  | sort_key (block*1e9+log_idx) |
| timeline  | ty   | u8   | 事件类型                     |
| timeline  | rpnl | i64  | 累计已实现 PnL               |
| timeline  | upnl | i64  | 该时刻未实现 PnL             |
| timeline  | tk   | i32  | 持仓 token 种数              |

`/api/stage3-users` 字段：

| 字段   | 类型 | 说明               |
| ------ | ---- | ------------------ |
| user   | str  | 用户地址           |
| events | i64  | 事件数             |
| rpnl   | i64  | 用户累计已实现 PnL |
| upnl   | i64  | 用户当前未实现 PnL |

**计算公式**：
```
total_pnl = rpnl + upnl  // 前端绘制第三条线
position_unrealized = (qty * lp - cost) / 1e6  // 单个持仓浮盈
```

## 前端使用

1. **PnL曲线图**：
   - X轴: `timeline[i].sk` 转换为区块号
   - Y轴: 三条线
     - 绿线: `rpnl` (累计已实现)
     - 橙线: `upnl` (未实现)
     - 白线: `rpnl + upnl` (总PnL)

2. **持仓表格**：
   - 仅在用户请求特定时间点时显示
   - 计算列: unrealized = qty * lp / 1e6 - cost / 1e6

3. **交互**：
   - 点击图表某点: 前端带 `block` 参数重新请求, 获取该时刻持仓明细
   - 默认加载最新时刻

## 持久化表结构

```sql
-- 用户-condition 状态 (含 last_price)
CREATE TABLE s3_user_cond_state (
  user_addr BLOB NOT NULL,
  cond_idx INTEGER NOT NULL,
  pos_0 BIGINT, pos_1 BIGINT, pos_2 BIGINT, pos_3 BIGINT,
  pos_4 BIGINT, pos_5 BIGINT, pos_6 BIGINT, pos_7 BIGINT,
  cost_0 BIGINT, cost_1 BIGINT, cost_2 BIGINT, cost_3 BIGINT,
  cost_4 BIGINT, cost_5 BIGINT, cost_6 BIGINT, cost_7 BIGINT,
  lp_0 BIGINT, lp_1 BIGINT, lp_2 BIGINT, lp_3 BIGINT,       -- last_price
  lp_4 BIGINT, lp_5 BIGINT, lp_6 BIGINT, lp_7 BIGINT,
  realized_pnl BIGINT,
  event_count BIGINT,
  last_sort_key BIGINT,
  PRIMARY KEY (user_addr, cond_idx)
);

-- 事件事实表 (用于构建 timeline)
CREATE TABLE s3_user_event_fact (
  user_addr BLOB NOT NULL,
  sort_key BIGINT NOT NULL,
  cond_idx INTEGER NOT NULL,
  token_idx INTEGER NOT NULL,
  event_type INTEGER NOT NULL,
  realized_delta BIGINT NOT NULL,
  realized_cum BIGINT NOT NULL,
  unrealized_pnl BIGINT NOT NULL,
  token_count INTEGER NOT NULL,
  PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
);

-- Checkpoint (用于加速历史回溯)
CREATE TABLE s3_user_cond_checkpoint (
  user_addr BLOB NOT NULL,
  checkpoint_sort_key BIGINT NOT NULL,
  cond_idx INTEGER NOT NULL,
  -- 同 s3_user_cond_state 的所有字段
  PRIMARY KEY (user_addr, checkpoint_sort_key, cond_idx)
);

-- 用户摘要
CREATE TABLE s3_user_summary (
  user_addr BLOB PRIMARY KEY,
  total_events BIGINT NOT NULL,
  total_realized_pnl BIGINT NOT NULL,
  total_unrealized_pnl BIGINT NOT NULL,
  active_conditions BIGINT NOT NULL,
  last_sort_key BIGINT NOT NULL
);

-- 同步游标
CREATE TABLE s3_sync_cursor (
  id INTEGER PRIMARY KEY,
  sort_key BIGINT,
  user_addr BLOB,
  cond_idx INTEGER,
  event_type INTEGER,
  token_idx INTEGER,
  processed_events BIGINT
);
```

## 查询实现伪代码

```cpp
struct PnlResponse {
  std::string user;
  int64_t block;
  int64_t total_events;
  std::vector<PositionRow> positions;
  std::vector<TimelineRow> timeline;
};

PnlResponse get_user_pnl_data(const std::string& user, int64_t target_block) {
  int64_t target_sk = (target_block + 1) * SORT_KEY_SCALE - 1;  // 该block末尾
  
  // 1. 加载 timeline (直接从 fact 表读取)
  auto timeline = db.query(
    "SELECT sort_key, event_type, realized_cum, unrealized_pnl, token_count "
    "FROM s3_user_event_fact "
    "WHERE user_addr = ? AND sort_key <= ? "
    "ORDER BY sort_key", user, target_sk);
  
  // 2. 计算 positions (从 checkpoint 回放)
  auto positions = build_positions_at(user, target_sk);
  
  return {user, target_block, timeline.size(), positions, timeline};
}

std::vector<PositionRow> build_positions_at(const std::string& user, int64_t target_sk) {
  // 找最近 checkpoint
  auto ckpt_sk = db.query_one<int64_t>(
    "SELECT MAX(checkpoint_sort_key) FROM s3_user_cond_checkpoint "
    "WHERE user_addr = ? AND checkpoint_sort_key <= ?", user, target_sk);
  
  // 加载 checkpoint 状态
  std::map<std::pair<int32_t,int32_t>, PositionState> states;  // (cond_idx, token_idx) -> state
  if (ckpt_sk) {
    for (auto& row : db.query("SELECT * FROM s3_user_cond_checkpoint WHERE ...")) {
  // 解析 pos, cost, lp 到 states
    }
  }
  
  // 回放 checkpoint 之后的事件
  for (auto& evt : db.query(
    "SELECT * FROM stage2.user_event "
    "WHERE user_addr = ? AND sort_key > ? AND sort_key <= ? "
    "ORDER BY sort_key", user, ckpt_sk.value_or(-1), target_sk)) {
    apply_event(states, evt);
  }
  
  // 转换为输出格式
  std::vector<PositionRow> result;
  for (auto& [key, st] : states) {
    if (st.qty != 0) {
      result.push_back({key.first, key.second, st.qty, st.cost, st.lp});
    }
  }
  return result;
}
```
