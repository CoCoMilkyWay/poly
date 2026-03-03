## 交易口径事件
> `qty = abs(amount)`
> `px = price_1e6 / 1e6`
> `cost_before = cost[token_idx]`(本事件处理前的持仓成本)
> `pos_before = pos[token_idx]`(本事件处理前的持仓数量)

| EventType                                                  | 是否计算   | 状态更新(pos/cost)                                   | realized_delta 公式                                                |
| ---------------------------------------------------------- | ---------- | ---------------------------------------------------- | ------------------------------------------------------------------ |
| OrderBuy                                                   | 是         | `pos += qty; cost += qty * px`                       | `0`                                                                |
| OrderSell                                                  | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        |
| SplitNormal / SplitNegRisk / SplitNonPoly                  | 是(重分类) | `pos += qty; cost += qty * px`                       | `0`                                                                |
| MergeNormal / MergeNegRisk / MergeNonPoly                  | 是(重分类) | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        |
| Redemption / RedemptionNonPoly                             | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        |
| Convert                                                    | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * (popcount - 1) / popcount - cost_before * qty / pos_before` |
| FPMMBuy                                                    | 是         | `pos += qty; cost += qty * px`                       | `0`                                                                |
| FPMMSell                                                   | 是         | `cost -= cost_before * qty / pos_before; pos -= qty` | `qty * px - cost_before * qty / pos_before`                        |
| FPMMLPAdd                                                  | 否(资金流) | 不改 token `pos/cost`(仅记录事件)                    | `0`                                                                |
| FPMMLPRemove                                               | 否(资金流) | 不改 token `pos/cost`(仅记录事件)                    | `0`                                                                |
| FPMMLPReturn                                               | 否(资金流) | 不改 token `pos/cost`(仅记录事件)                    | `0`                                                                |
| TransferInNegRisk / TransferInOther / TransferInNonPoly    | 是(转入)   | `pos += qty; cost` 不变                              | `0`                                                                |
| TransferOutNegRisk / TransferOutOther / TransferOutNonPoly | 是(转出)   | `cost -= cost_before * qty / pos_before; pos -= qty` | `0`                                                                |

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
│  ├─ 读 s3_user_cond_state -> token_state_map
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
│  │  ├─ Buy类: OrderBuy/FPMMBuy/Split*
│  │  ├─ Sell类: OrderSell/FPMMSell/Merge*/Redemption*/Convert
│  │  ├─ TransferIn类: TransferIn*
│  │  ├─ TransferOut类: TransferOut*
│  │  └─ LP资金流: FPMMLPAdd/FPMMLPRemove/FPMMLPReturn(仅记录,不计入 realized)
│  ├─ 3.3 应用规则(trade-only)
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
│  │  └─ LP资金流: 不改 token pos/cost; realized_delta = 0(但写入事实行)
│  ├─ 3.4 更新 user_state_map[user_addr]
│  │  ├─ total_events += 1
│  │  ├─ total_realized_pnl += realized_delta
│  │  └─ last_sort_key = evt.sort_key
│  └─ 3.5 产出事实行
│     └─ append EventFactRow(user_addr,sort_key,cond_idx,token_idx,event_type,collateral,amount,price_1e6,realized_delta,realized_cum)
├─ 4) 批次收尾校验
│  ├─ token 状态非负: pos>=0 且 cost>=0
│  ├─ fact_rows.size == batch_events.size
│  └─ 任一失败 -> rollback,cursor 不推进
├─ 5) 单事务提交
│  ├─ upsert s3_user_cond_state(dirty_cond_keys)
│  ├─ upsert s3_user_summary(dirty_users)
│  │  └─ active_conditions = count(cond where any(pos)!=0 or realized!=0)
│  ├─ insert s3_user_event_fact(fact_rows)
│  ├─ checkpoint 规则(每个 chunk 触发)
│  │  └─ 对 touched_users,将其当前 s3_user_cond_state 全量快照写入 s3_user_cond_checkpoint(checkpoint_sort_key=cursor.sort_key)
│  └─ update s3_sync_cursor(last_sort_key,last_user_addr,last_cond_idx,last_event_type,last_token_idx,processed_events)
└─ 6) 查询路径(只读)
   ├─ users() / users_sorted()         -> s3_user_summary(用户列表)
   ├─ user_timeline(user)              -> 由 s3_user_event_fact 构建 timeline(含 rpnl/tk)
   ├─ positions_at(user, sort_key)     -> 先读最近 checkpoint,再回放 checkpoint 之后的事实事件
   └─ events_near(user, sort_key, N)   -> 目标时刻附近事件窗口
```

## Flow 使用的数据结构详细定义

```text
type Address20 = bytes20

// Stage3 直接读取 Stage2 事件行(多用户全局排序)
struct UserEvent {
  Address20 user_addr;
  int64   sort_key;
  int32   cond_idx;      // unknown=-1
  int32   event_type;    // EventType
  int32   token_idx;     // unknown=-1
  int32   collateral;    // collateral 枚举值
  int64   amount;        // 带符号,6 decimals
  int64   price_1e6;     // 1e6 = $1
}

// 说明：
// - Stage3 trade-only 仅依赖以上字段,直接沿用 Stage2 输出。
// - 与口径无关的小差异(unknown/sentinel)在计算时顺手处理,不单独建中间结构。

struct ConditionMeta {
  uint8 outcome_count;
  int64 payout_numerators[8];
}

struct TokenCondState {
  int64 pos[8];
  int64 cost[8];
  int64 realized;
  int64 event_count;
  int64 last_sort_key;
}

struct UserSummaryState {
  int64  total_events;
  int64  total_realized_pnl;
  int32  active_conditions;
  int64  last_sort_key;
}

struct EventFactRow {
  Address20 user_addr;
  int64   sort_key;
  int32   cond_idx;
  int32   token_idx;
  int32   event_type;
  int32   collateral;
  int64   amount;
  int64   price_1e6;
  int64   realized_delta;
  int64   realized_cum;
  int32   token_count_cum; // 当前用户“非零 token 持仓种数”累计值(用于前端 timeline.tk 直读)
}

struct SyncCursor {
  int64 last_sort_key;
  Address20 last_user_addr;
  int32 last_cond_idx;
  int32 last_event_type;
  int32 last_token_idx;
  int64 processed_events;
}

struct ReplayContext {
  map<(Address20,int32), TokenCondState> token_state_map;
  map<Address20, UserSummaryState>       user_state_map;

  set<(Address20,int32)> dirty_cond_keys;
  set<Address20>         dirty_users;

  vector<EventFactRow> fact_rows;
}
```

## API 返回数据结构

```text
GET /api/stage3-status
{
  syncing: bool,
  last_block: int64,
  head_block: int64,
  behind_blocks: int64,
  behind_chunks: int64,
  blocks_per_second: float64,
  eta_seconds: float64,
  ready: bool
}

GET /api/stage3-users?limit=1000
[
  {
    user_addr: string,      // "0x..."
    event_count: int64,
    realized_pnl: int64     // 1e6
  },
  ...
]

GET /api/stage3-data?user=0x...&sk=<optional>
{
  total_events: int64,
  first_ts: int64,          // sort_key / 1e9
  last_ts: int64,           // sort_key / 1e9
  timeline: [
    {
      sk: int64,            // sort_key
      ty: uint8,            // 事件类型
      rpnl: int64,          // 累计 realized pnl, 1e6
      d: int64,             // delta(amount), 1e6
      p: int64,             // price_1e6
      ci: uint32,           // cond_idx (unknown 映射为 UNKNOWN_COND_IDX)
      ti: uint8,            // token_idx (unknown 映射为 UNKNOWN_TOKEN_IDX)
      tk: int32             // 当前持仓 token 种数
    },
    ...
  ],
  positions: [
    {
      id: string,           // condition_id
      pos: [int64...],      // 各 outcome 持仓
      cost: int64,          // 当前总成本
      rpnl: int64           // 当前 condition realized pnl
    },
    ...
  ],
  events: [
    {
      sk: int64,
      ty: uint8,
      d: int64,
      p: int64,
      ci: uint32,
      ti: uint8
    },
    ...
  ],
  center: int32             // events 数组中的中心索引
}
```
