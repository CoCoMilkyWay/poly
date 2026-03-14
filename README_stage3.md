## 交易口径事件（符号驱动, 支持做空）
> `qty = abs(amount)`
> `dir = sign(amount)`（`+1 / -1 / 0`）
> `px = price_1e6 / 1e6`
> `cost_before = cost`（本事件前该token的cost）
> `pos_before = pos`（本事件前该token的pos）
> `has_usd = collateral ∈ {USDC(1), USDCe(2), USDT(3), WrappedUSDCe(4)}`

**Collateral 规则**: 只有 USD 类抵押物 (`has_usd=true`) 才计算 `cost / realized_delta / lp`；非 USD 仅更新 `pos`。  
**方向规则**: `EventType` 决定“经济语义”, `amount` 符号决定“方向”；同一 `EventType` 可出现正负。

| 事件族                                                     | `amount > 0` (正向腿)                                        | `amount < 0` (反向腿)                                         | realized 规则                                                | lp更新                   |
| ---------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------- | ------------------------------------------------------------ | ------------------------ |
| `Order* / FPMM* / Split* / Merge* / Redemption*`           | 先平空(`pos<0`)再开多；开多部分 `cost += open_long_qty * px` | 先平多(`pos>0`)再开空；开空部分 `cost -= open_short_qty * px` | 仅"平掉已有仓位"部分计 realized；开新仓部分不计当期 realized | 仅 `Order* / FPMM*` 更新 |
| `Convert`                                                  | 先平空再开多；开多部分 `cost += open_long_qty * convert_px`  | 先平多再开空；开空部分 `cost -= open_short_qty * convert_px`  | `convert_px = (popcount-1)/popcount`, 仅平仓部分计 realized  | 否                       |
| `TransferIn* / TransferOut* / FPMMLPRemove / FPMMLPReturn` | 先平空再开多；`cost` 不增加                                  | 先平多再开空；`cost` 不增加（保持 transfer 语义）             | 始终 `0`                                                     | 否                       |
| `FPMMLPAdd`                                                | 不改 `pos/cost`                                              | 不改 `pos/cost`                                               | `0`                                                          | 否                       |

`amount == 0` 视为 no-op（仅事件计数推进, 不改持仓/成本/PnL）。

## Unrealized PnL 计算
```
// 对该用户所有有持仓的token求和
unrealized_pnl = Σ(pos * lp / 1e6 - cost)  // 对所有 token_state 中 pos != 0 且 lp > 0 的token
```
- 只有 Order/FPMM 交易事件会更新 lp
- Split/Merge/Transfer 等不更新 lp
- 若某token从未交易过（如TransferIn获得）,该token不计入unrealized（lp=0）

## Flow 图

```text
stage3_bootstrap
└─ 初始化 schema 与索引；若游标不存在则写入初始 cursor

stage3_sync_tick
├─ 0) 读取游标与元数据
│  ├─ 读 sync_cursor_state(id=1) -> SyncCursorState
│  ├─ 刷新 condition_meta(只读缓存)
│  └─ 计算批次游标起点 (sort_key_exclusive = cursor.sort_key)
├─ 1) 拉取输入事件(Stage2 输出, RocksDB sort 索引)
│  ├─ scan_by_sort_key(sort_key_exclusive=cursor.sort_key,
│  │                  sort_key_inclusive=head_block*1e9+(1e9-1),
│  │                  limit=batch_events)
│  ├─ 返回顺序按 key: (sort_key, user_addr, cond_idx, event_type, token_idx)
│  └─ 若无事件 -> 将 cursor.sort_key 推进到 head_sort_key 后结束本 tick
├─ 2) 预加载状态
│  ├─ 扫描 batch_events,提取 touched keys
│  │  ├─ touched_token_keys = {(user_addr, cond_idx, token_idx)}
│  │  └─ touched_users      = {user_addr}
│  ├─ 读 token_state  -> token_state_map
│  ├─ 读 user_summary_state -> user_state_map
│  └─ 初始化临时结构
│     ├─ fact_rows
│     └─ dirty_token_keys / dirty_users
├─ 3) 回放循环(for evt in batch_events, 按 sort_key + PK 升序)
│  ├─ 3.1 输入断言
│  │  ├─ (sort_key, user_addr, cond_idx, event_type, token_idx) 严格递增
│  │  ├─ cond_idx < 0: 不进入状态机(仅事实行)
│  │  ├─ cond_idx >= 0: condition_meta 必须存在
│  │  ├─ token_idx ∈ [0, outcome_count)
│  │  ├─ event_type 合法；amount 符号不做类型硬断言（允许同类型正负共存）
│  │  ├─ amount==0 允许（no-op）
│  │  └─ 常规指标使用 double 计算并 round() 写入 int64；大数累计项直接写 HUGEINT
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
│  ├─ 3.4 更新 token state
│  │  ├─ 更新 pos, cost, lp (若Order/FPMM且price>0)
│  │  └─ 更新 entry_block:
│  │       建仓 (pos: 0 → 非0):    entry_block = current_block
│  │       加仓 (|pos| 增加):      entry_block = (|old_pos| * old_entry + delta * current_block) / |new_pos|
│  │       减仓 (|pos| 减少):      entry_block 不变
│  │       平仓 (pos → 0):         删除该行
│  ├─ 3.5 计算 user 级别聚合
│  │  ├─ unrealized_pnl = Σ(pos * lp / 1e6 - cost) for all tokens
│  │  ├─ token_count = 有效持仓token数（过滤dust, |qty| >= 10 token）
│  │  └─ 计算 exposure, holding_period (token级别)
│  ├─ 3.6 更新 user_state_map[user_addr]
│  │  ├─ total_events += 1
│  │  ├─ total_realized_pnl += realized_delta
│  │  └─ last_sort_key = evt.sort_key
│  ├─ 3.7 产出事实行
│  │  └─ append EventFact(user_addr, sort_key, cond_idx, token_idx, event_type,
│  │                         realized_delta, realized_cum, unrealized_pnl, token_count,
│  │                         tag_id, exposure, volume, holding_period)
│  └─ 3.8 统一特征图更新 (按 bucket 增量)
│     ├─ 将事件映射为 (user_addr, tag_id, block_bucket) dirty key
│     ├─ 更新该 bucket 的 Node-A0 续算锚点 + Node-A 原子统计节点
│     ├─ 依赖前序 bucket 前缀节点, 计算 100w/1000w 窗口节点
│     └─ 产出并覆盖 feature_tensor_state 对应行
├─ 4) 批次收尾校验
│  ├─ token 状态有限: pos/cost 必须是 finite（允许负数,表示短仓/负成本）
│  ├─ fact_rows.size == batch_events.size
│  └─ 任一失败 -> rollback,cursor 不推进
├─ 5) 单事务提交
│  ├─ upsert token_state(dirty_token_keys)
│  ├─ upsert user_summary_state(dirty_users)
│  ├─ write event_fact.rocks/timeline(fact_rows)
│  ├─ upsert feature_tensor_state(dirty_user_bucket_tag, 含 Node-A0/A/B/C/D)
│  └─ update sync_cursor_state(sort_key, processed_events)
└─ 6) 查询路径(只读)
   ├─ users_sorted(limit)                 -> user_summary_state(用户列表)
   ├─ user_pnl_timeline(user)             -> 预热并缓存该用户 timeline + snapshots
   ├─ user_positions_at(user, sk)         -> 命中内存 snapshots 返回 positions
   └─ user_feature_filter(anchor, exprs)  -> feature_tensor_state(截面 top users)
```

## 数据结构

符号: `E=事件总数`, `U=有事件用户数`, `T=活跃token数(user,cond,token_idx)`, `F≈E/20`  
Size估算基准: E=2B, U=2M, T=1M, F=100M

| 数据结构                                      | 层级/实例数       | 行大小 | Size   | 主要用途                                          | Persist      | ORDER BY                          |
| --------------------------------------------- | ----------------- | ------ | ------ | ------------------------------------------------- | ------------ | --------------------------------- |
| `EventInput`                                  | 输入流 / `E`      | -      | -      | 回放状态机输入                                    | 否（临时）   | -                                 |
| `ConditionMeta`                               | 只读缓存 / `C`    | -      | -      | `outcome_count` 与 `tag_id` 元信息                | 否（可重载） | -                                 |
| `TokenState` (`token_state`)                  | Token级 / `T`     | 60B    | ~60MB  | 当前持仓 `pos/cost/lp/entry_block`                | 是           | `(user_addr,cond_idx,token_idx)`  |
| `EventFact` (`event_fact.rocks/timeline`)     | Token级 / `E`     | 96B    | ~190GB | 事件事实、timeline、特征原料                      | 是           | `(user_addr,sort_key,cond_idx,event_type,token_idx)` |
| `UserSummaryState` (`user_summary_state`)     | User级 / `U`      | 60B    | ~120MB | 用户总览查询加速                                  | 是           | `(user_addr)`                     |
| `FeatureTensorState` (`feature_tensor_state`) | User*Bucket*Tag级 | 536B   | ~50GB  | 统一特征张量（含原子/窗口/前缀缓存/增量续算锚点） | 是           | `(block_bucket,tag_id,user_addr)` |
| `SyncCursorState` (`sync_cursor_state`)       | 全局 / `1`        | <1KB   | <1KB   | 增量同步断点                                      | 是           | -                                 |
| `UserQueryCache`                              | 进程内 / `<=U`    | -      | -      | 查询缓存（timeline/snapshot）                     | 否（内存）   | -                                 |

```text
// Stage3 内部统一输入结构 (用于回放/状态机)
struct EventInput {
  string user_hex;        // 查询路径可为空, 批处理路径必填
  int64  sort_key;
  int32  cond_idx;
  int32  event_type;
  int32  token_idx;
  int32  collateral;
  int64  amount;          // signed, 1e6
  int64  price_1e6;
}

// Condition 元数据 (只读缓存)
struct ConditionMeta {
  uint8 outcome_count;
  int64 payout_numerators[256];  // 支持任意outcome数
  int8 tag_id;                   // 行业分类
}

// Token 状态 (稀疏存储, 只存有持仓的token)
// PK: (user_addr, cond_idx, token_idx)
struct TokenState {
  double pos;           // 持仓量 (可负=空头)
  double cost;          // 成本基础 (可负=空头信用)
  double lp;            // 最近成交价
  double entry_block;   // 加权平均建仓block
}

// 事件事实行 (持久化, 用于构建 timeline)
struct EventFact {
  Address20 user_addr;
  int64   sort_key;
  int32   cond_idx;
  int32   token_idx;
  int32   event_type;
  int64   realized_delta;     // 本事件 realized 增量
  int64   realized_cum;       // user级别累计 realized pnl
  int64   unrealized_pnl;     // user级别 unrealized pnl
  int32   token_count;        // user级别有效持仓 token 种数（|qty| >= 10 token）
  // 新增字段
  int8    tag_id;             // 行业分类 (0-13)
  int64   exposure;           // 该token暴露额 |pos*lp|
  int64   volume;             // 交易额 |amount*price|
  int64   holding_period;     // 该token持仓周期 (blocks)
}

// 用户摘要 (持久化)
struct UserSummaryState {
  int64  total_events;
  int64  total_realized_pnl;
  int64  total_unrealized_pnl;
  int32  active_tokens;       // 有持仓的token数
  int64  last_sort_key;
}

// 统一特征张量状态 (持久化)
// PK: (user_addr, block_bucket, tag_id)
// 注: tag_id = -1 表示全行业聚合行（用于总夏普等 1 维特征）
struct FeatureTensorState {
  Address20 user_addr;
  int64  block_bucket;
  int32  tag_id;

  // Node-A0: 增量续算锚点
  // 跨 chunk / 重启后继续做 tail 修正, 无需回扫 event_fact
  int64  last_sort_key_10w;
  int64  last_block_10w;
  int64  last_exposure_10w;
  int128 last_holding_period_10w;        // DuckDB: HUGEINT
  int64  last_token_count_10w;

  // Node-A: 10w 原子统计（事件增量累加）
  int64  time_weight_sum_10w;
  int64  token_count_tw_sum_10w;
  int128 exposure_tw_sum_10w;            // DuckDB: HUGEINT
  int64  volume_sum_10w;
  int128 holding_period_exp_tw_sum_10w;  // DuckDB: HUGEINT
  int64  realized_sum_10w;
  int128 realized_sq_sum_10w;            // DuckDB: HUGEINT
  int64  realized_count_10w;

  // Node-B: 10w 归一化输出
  int64  token_avg_10w;
  int64  exposure_avg_10w;
  int64  volume_10w;
  int64  holding_period_avg_10w;
  float  sharpe_10w;

  // Node-C: 前缀缓存（按 bucket 单调推进）
  int64  ps_token_avg_10w;
  int64  ps_exposure_avg_10w;
  int64  ps_volume_10w;
  int64  ps_holding_period_avg_10w;
  int64  ps_realized_sum_10w;
  int128 ps_realized_sq_sum_10w;         // DuckDB: HUGEINT
  int64  ps_realized_count_10w;

  // Node-D: 窗口投影输出（由 Node-C O(1) 计算）
  int64  token_avg_100w;
  int64  token_avg_1000w;
  int64  exposure_avg_100w;
  int64  exposure_avg_1000w;
  int64  volume_avg_100w;
  int64  volume_avg_1000w;
  int64  holding_period_avg_100w;
  int64  holding_period_avg_1000w;
  float  sharpe_100w;
  float  sharpe_1000w;

  int64  updated_sort_key;
}

// 游标
struct SyncCursorState {
  int64 sort_key;
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
| resp.timeline[].tk   | i32    | 是       | 有效持仓 token 种数（过滤dust）  |
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
| resp.positions[].qty  | i64    | 是       | 持仓数量（`1e6=1 token`, 可负）        |
| resp.positions[].cost | i64    | 是       | 成本基础（`1e6=$1`, 可负）             |
| resp.positions[].lp   | i64    | 是       | 最近成交价（`1e6=$1`）                 |
| resp.positions[].eb   | i64    | 是       | 建仓block                              |

### POST /api/stage3-filter
| entry                   | type     | required | description                                    |
| ----------------------- | -------- | -------- | ---------------------------------------------- |
| req.anchor_bucket       | i64      | 是       | 锚定 bucket（必须显式传入）                    |
| req.filters[]           | string[] | 否       | 过滤表达式列表（按 `AND` 连接）                |
| req.sort_expr           | string   | 是       | 排序表达式                                     |
| req.sort_asc            | bool     | 是       | 排序方向（必须显式传入）                       |
| req.limit               | i32      | 是       | 返回条数（必须显式传入，当前实现固定传 `100`） |
| resp.anchor_bucket      | i64      | 是       | 实际执行使用的 anchor bucket                   |
| resp.users[]            | array    | 是       | 过滤 + 排序后的用户列表                        |
| resp.users[].addr       | string   | 是       | 用户地址                                       |
| resp.users[].sort_value | f64      | 是       | 排序表达式值                                   |

## 建立动态高玩池(特征工程, 统一表 + Compute Graph)

10w blk ~ 2.3day ~ 日线  
100w blk ~ 23day ~ 月线  
1000w blk ~ 230day ~ 年线

特征张量索引: `User * Time(per 10w bucket) * Feature`

行业(N):
Crypto_Price
Crypto_Market
Sports_Basketball
Sports_Football
Sports_Soccer
Sports_Individual
Politics_US
Politics_World
Economy_Finance
Tech
Entertainment
Weather
Society
Unknown

时序特征(名称,数量,统计方式,描述):
    近期行业平均持仓token数    N    时间加权           近10w块移动平均持仓token数(分行业)    
    近月行业平均持仓token数    N    等权               近100w块移动平均持仓token数(分行业)   
    近年行业平均持仓token数    N    等权               近1000w块移动平均持仓token数(分行业)  
    近期行业平均持仓暴露额     N    时间加权           近10w块移动平均持仓暴露额(分行业)     
    近月行业平均持仓暴露额     N    等权               近100w块移动平均持仓暴露额(分行业)    
    近年行业平均持仓暴露额     N    等权               近1000w块移动平均持仓暴露额(分行业)   
    近期行业总和交易额         N    加总               近10w块总和交易额(分行业)             
    近月行业平均总和交易额     N    等权               近100w块总和交易额(分行业)            
    近年行业平均总和交易额     N    等权               近1000w块总和交易额(分行业)           
    近期行业平均持仓周期       N    时间,金额加权      近10w块移动平均持仓周期(分行业)       
    近月行业平均持仓周期       N    等权               近100w块移动平均持仓周期(分行业)      
    近年行业平均持仓周期       N    等权               近1000w块移动平均持仓周期(分行业)     
    近期总夏普                 1    online std         近10w块夏普                           
    近月总夏普                 1    前缀和差分         近100w块夏普                          
    近年总夏普                 1    前缀和差分         近1000w块夏普                         
截面特征:
    输入: anchor bucket + 多条自由表达式(filters) + 排序表达式(sort_expr)
    输出: 过滤 + 排序后的 top 100 用户地址
    目标: 支持“分行业 nested 特征”的横截面对比与选人

注:
  1. 交易额里: 只记录会直接创造头寸暴露的操作(比如铸币, 合币就不应该记入), 暴露方向不重要
  2. 平均持仓: 需要统计周期内多个事件(非均匀)的持仓快照(记录不同token的平均持仓周期), 再按照token金额, 事件时间加权
  3. 夏普: 无风险=0, 基于事件驱动计算, 使用 online std 算法 (sum + sq_sum + count) 精确计算均值和标准差, 窗口聚合通过前缀和 O(1) 差分实现
