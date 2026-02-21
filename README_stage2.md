# Stage 2: 构建映射 + 写 user_event

## 运行模式

### 首次 rebuild

```
条件: sync_state['rebuild_last_block'] 不存在或为 0
行为:
  - Phase 1A 跳过 (rb_* 表为空)
  - Phase 1B 处理全量 (last_block=0)
  - Phase 2 处理全量
  - Phase 3 处理全量
耗时: 取决于历史数据量，预计 5-15 分钟
```

### 增量 rebuild

```
条件: sync_state['rebuild_last_block'] > 0
行为:
  - Phase 1A 从 rb_* 表加载已有映射
  - Phase 1B 只处理 (last_block, target_block] 的新数据
  - Phase 2 只处理 new_range
  - Phase 3 只处理 new_range
耗时: 取决于增量区块数，通常 < 1 分钟
```

## 执行顺序

```
rebuild(target_block):
    last_block = get_sync_state('rebuild_last_block') ?? 0
    if target_block <= last_block: return  // 无需更新

    // Phase 1A: 加载已有映射 (首次跳过)
    load_existing_mappings()

    // Phase 1B: 增量更新映射
    // 步骤 1: 先处理 condition_preparation (其他都依赖 cond_map_)
    update_cond_map(last_block, target_block)

    // 步骤 2: 并行处理其他 4 个 (都依赖 cond_map_, 互不依赖)
    parallel:
        update_conditions(last_block, target_block)    // condition_resolution → payout
        update_token_map(last_block, target_block)     // token_map → token_map_
        update_fpmm_map(last_block, target_block)      // fpmm → fpmm_map_ + 补全 token_map_
        update_neg_risk_map(last_block, target_block)  // neg_risk_question → neg_risk_map_

    // Phase 2: 构建语义索引 (并行扫 7 个表, 内存)
    parallel:
        scan_split(last_block, target_block)
        scan_merge(last_block, target_block)
        scan_redemption(last_block, target_block)
        scan_convert(last_block, target_block)
        scan_order_filled(last_block, target_block)
        scan_fpmm_trade(last_block, target_block)
        scan_fpmm_funding(last_block, target_block)

    // Phase 3: 处理 Transfer (顺序扫, 批量写 user_event)
    process_transfer(last_block, target_block)

    // 更新同步状态
    set_sync_state('rebuild_last_block', target_block)
```

## 数据流总览

```
输入参数：
  last_block = sync_state['rebuild_last_block']  // 上次处理到的区块，首次为 0
  target_block = 目标区块号                       // 通常是 stage1 的 last_block
  new_range = (last_block, target_block]         // 本次处理的区块范围

┌────────────────────────────────────────────────────────────────────┐
│                    Phase 1A: 加载已有映射                          │
├────────────────────────────────────────────────────────────────────┤
│  rb_cond_map ────────────────► cond_map_ (内存)                    │
│  rb_condition ───────────────► conditions_[] (内存)                │
│  rb_token_map ───────────────► token_map_ (内存)                   │
│  rb_fpmm_map ────────────────► fpmm_map_ (内存)                    │
│  rb_neg_risk_map ────────────► neg_risk_map_ (内存)                │
│                                                                    │
│  【首次 rebuild】: 表为空，跳过                                    │
│  【增量 rebuild】: 从持久化表加载已有映射                          │
└────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│              Phase 1B: 增量更新映射 (只处理 new_range)             │
├────────────────────────────────────────────────────────────────────┤
│  condition_preparation ─────► 新 condition 追加到 cond_map_        │
│  condition_resolution ──────► 更新 conditions_[].payout            │
│  token_map ─────────────────► 新 token 追加到 token_map_           │
│  fpmm ──────────────────────► 新 fpmm 追加到 fpmm_map_             │
│                              └──► 补全 token_map_ (keccak256)      │
│  neg_risk_question ─────────► 新映射追加到 neg_risk_map_           │
│                                                                    │
│  【持久化】: INSERT 新增项到 rb_* 表                               │
└────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│           Phase 2: 构建语义索引 (只处理 new_range, 内存)           │
├────────────────────────────────────────────────────────────────────┤
│  split ─────────────► tx_split_[(block, tx_hash)]                  │
│  merge ─────────────► tx_merge_[(block, tx_hash)]                  │
│  redemption ────────► tx_redemption_[(block, tx_hash)]             │
│  convert ───────────► tx_convert_[(block, tx_hash)]                │
│  order_filled ──────► tx_order_[(block, tx_hash, token_id)]        │
│  fpmm_trade ────────► tx_fpmm_trade_[(block, tx_hash)]             │
│  fpmm_funding ──────► tx_fpmm_funding_[(block, tx_hash)]           │
│                                                                    │
│  【关键】: 语义事件和 Transfer 在同一个 tx，所以只需索引 new_range │
└────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│           Phase 3: 处理 Transfer 并持久化 (只处理 new_range)       │
├────────────────────────────────────────────────────────────────────┤
│  for each transfer in new_range:                                   │
│    classify by (operator, from, to)                                │
│    lookup semantic index → get price/context                       │
│    INSERT INTO user_event (user_addr, sort_key, ...)               │
└────────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌────────────────────────────────────────────────────────────────────┐
│                     更新同步状态                                   │
├────────────────────────────────────────────────────────────────────┤
│  UPDATE sync_state SET value = target_block                        │
│  WHERE key = 'rebuild_last_block'                                  │
└────────────────────────────────────────────────────────────────────┘

【Stage 3】: 从 user_event 回放计算 PnL (全量或按需)

原则：
1. 任何 token 流水都被包括
2. 任何 token 流水不被 double count
3. token 流水被精确还原，不含近似假设

核心设计：
- Transfer 是持仓变化的【唯一来源】(TransferSingle + TransferBatch 覆盖所有持仓变化)
- 其他事件(Split/Merge/OrderFilled等)只提供【语义和价格信息】
- 通过 (block_number, tx_hash) 关联 Transfer 和语义事件
```

## 数据结构

### 持久化表 (rebuild 专用)

```sql
-- 映射表持久化 (Phase 1 产出)
CREATE TABLE rb_cond_map (
    condition_id BLOB(32) PRIMARY KEY,
    cond_idx     INTEGER NOT NULL
);

CREATE TABLE rb_condition (
    cond_idx          INTEGER PRIMARY KEY,
    outcome_count     INTEGER NOT NULL,
    payout_numerators TEXT    -- NULL = 未结算, "[1,0]" = YES赢, "[0,1]" = NO赢
);

CREATE TABLE rb_token_map (
    token_id BLOB(32) PRIMARY KEY,
    cond_idx INTEGER NOT NULL,
    is_yes   INTEGER NOT NULL  -- 1=YES, 0=NO
);

CREATE TABLE rb_fpmm_map (
    fpmm_addr BLOB(20) PRIMARY KEY,
    cond_idx  INTEGER NOT NULL,
    token_yes BLOB(32) NOT NULL,
    token_no  BLOB(32) NOT NULL
);

CREATE TABLE rb_neg_risk_map (
    market_id      BLOB(32) NOT NULL,
    question_index INTEGER NOT NULL,
    cond_idx       INTEGER NOT NULL,
    PRIMARY KEY (market_id, question_index)
);

-- 用户事件表 (Phase 3 产出)
CREATE TABLE user_event (
    user_addr  BLOB(20) NOT NULL,
    sort_key   INTEGER NOT NULL,   -- block_number * 1e9 + log_index
    cond_idx   INTEGER NOT NULL,
    event_type INTEGER NOT NULL,   -- EventType enum
    token_idx  INTEGER NOT NULL,   -- 0=YES, 1=NO, 255=全部
    amount     INTEGER NOT NULL,
    price      INTEGER NOT NULL,
    PRIMARY KEY (user_addr, sort_key, cond_idx, event_type)
);
CREATE INDEX idx_user_event_user ON user_event(user_addr);

-- 同步状态 (已有表，新增 key)
-- sync_state: key='rebuild_last_block', value=<已处理到的区块>
```

### 内存结构 (运行时)

```
映射表 (Phase 1A 加载, Phase 1B 更新):

cond_map_: map<condition_id(hex), cond_idx(u32)>
    持久化: rb_cond_map

token_map_: map<token_id(hex), TokenInfo>
    TokenInfo = { cond_idx: u32, is_yes: bool }
    持久化: rb_token_map

fpmm_map_: map<fpmm_addr(hex), FPMMInfo>
    FPMMInfo = { cond_idx: u32, token_yes: hex, token_no: hex }
    持久化: rb_fpmm_map

neg_risk_map_: map<(market_id, question_index), cond_idx(u32)>
    持久化: rb_neg_risk_map

conditions_[cond_idx]: ConditionInfo
    ConditionInfo = { outcome_count: u8, payout_numerators: Option<vec<i64>> }
    持久化: rb_condition

next_cond_idx_: u32
    下一个可分配的 cond_idx，= max(已有 cond_idx) + 1
```

### 语义索引 (Phase 2 产出, 仅内存)

```
tx_split_: map<(block_number, tx_hash), SplitInfo>
    SplitInfo = { condition_id, amount, stakeholder }

tx_merge_: map<(block_number, tx_hash), MergeInfo>
    MergeInfo = { condition_id, amount, stakeholder }

tx_redemption_: map<(block_number, tx_hash), RedemptionInfo>
    RedemptionInfo = { condition_id, index_sets, payout, redeemer }

tx_convert_: map<(block_number, tx_hash), ConvertInfo>
    ConvertInfo = { market_id, index_set, amount, stakeholder }

tx_order_: map<(block_number, tx_hash, token_id), OrderInfo>
    OrderInfo = { maker, taker, side, token_amount, usdc_amount, fee }
    side: 1=maker买入, 2=maker卖出

tx_fpmm_trade_: map<(block_number, tx_hash), FPMMTradeInfo>
    FPMMTradeInfo = { fpmm_addr, trader, side, outcome_index, token_amount, usdc_amount, fee }

tx_fpmm_funding_: map<(block_number, tx_hash), FPMMFundingInfo>
    FPMMFundingInfo = { fpmm_addr, funder, side, amounts[], collateral_from_fee_pool }

【不需要持久化】: 语义事件和 Transfer 在同一个 tx，每次增量只需索引 new_range
```

### 事件格式

```
user_event 表的字段对应 RawEvent:
    sort_key:   block_number * 1e9 + log_index
    cond_idx:   condition 索引
    event_type: EventType
    token_idx:  0=YES, 1=NO, 255=全部
    amount:     数量 (6 decimals, 1e6 = $1)
    price:      价格*1e6 或额外数据

EventType:
    Buy=0, Sell=1, Split=2, Merge=3, Redemption=4,
    FPMMBuy=5, FPMMSell=6, FPMMLPAdd=7, FPMMLPRemove=8,
    Convert=9, TransferIn=10, TransferOut=11
```

### 用户状态 (Stage 3 计算)

```
ReplayState (回放中间态):
    positions[8]: i64   每个 outcome 的持仓
    cost[8]:      i64   每个 outcome 的成本
    realized_pnl: i64   已实现盈亏

Snapshot (快照):
    sort_key:      i64
    delta:         i64   本次变动量
    price:         i64   成交价格
    positions[8]:  i64   事件后持仓
    cost_basis:    i64   事件后总成本
    realized_pnl:  i64   累计已实现盈亏
    event_type:    u8
    token_idx:     u8
    outcome_count: u8
```

## Phase 1A: 加载已有映射

从持久化表加载已有映射到内存。首次 rebuild 时表为空，跳过。

```
def load_existing_mappings():
    // 1. 加载 cond_map_ 和 conditions_
    for row in SELECT condition_id, cond_idx FROM rb_cond_map:
        cond_map_[lowercase(condition_id)] = cond_idx

    for row in SELECT cond_idx, outcome_count, payout_numerators FROM rb_condition:
        conditions_[cond_idx] = ConditionInfo {
            outcome_count,
            payout_numerators: parse_or_none(payout_numerators)
        }

    // 2. 计算 next_cond_idx_
    next_cond_idx_ = max(conditions_.keys()) + 1  // 若为空则为 0

    // 3. 加载 token_map_
    for row in SELECT token_id, cond_idx, is_yes FROM rb_token_map:
        token_map_[lowercase(token_id)] = TokenInfo { cond_idx, is_yes: is_yes == 1 }

    // 4. 加载 fpmm_map_
    for row in SELECT fpmm_addr, cond_idx, token_yes, token_no FROM rb_fpmm_map:
        fpmm_map_[lowercase(fpmm_addr)] = FPMMInfo { cond_idx, token_yes, token_no }

    // 5. 加载 neg_risk_map_
    for row in SELECT market_id, question_index, cond_idx FROM rb_neg_risk_map:
        neg_risk_map_[(lowercase(market_id), question_index)] = cond_idx
```

## Phase 1B: 增量更新映射

只处理 new_range = (last_block, target_block] 的新数据。

### 1B.1 处理新的 condition_preparation

```sql
SELECT condition_id, outcome_slot_count, block_number
FROM condition_preparation
WHERE block_number > :last_block AND block_number <= :target_block
ORDER BY block_number
```

```
new_cond_map_rows = []
new_condition_rows = []

for row in results:
    cond_id = lowercase(condition_id)
    if cond_id in cond_map_: continue  // 已存在，跳过

    cond_idx = next_cond_idx_++

    cond_map_[cond_id] = cond_idx
    conditions_[cond_idx] = ConditionInfo { outcome_count: outcome_slot_count, payout_numerators: None }

    new_cond_map_rows.push((cond_id, cond_idx))
    new_condition_rows.push((cond_idx, outcome_slot_count, NULL))

// 批量持久化
INSERT INTO rb_cond_map (condition_id, cond_idx) VALUES ...
INSERT INTO rb_condition (cond_idx, outcome_count, payout_numerators) VALUES ...
```

### 1B.2 处理新的 condition_resolution

```sql
SELECT condition_id, payout_numerators, block_number
FROM condition_resolution
WHERE block_number > :last_block AND block_number <= :target_block
```

```
update_rows = []

for row in results:
    cond_id = lowercase(condition_id)
    if cond_id not in cond_map_: continue  // 未知 condition，跳过

    cond_idx = cond_map_[cond_id]
    payout = parse(payout_numerators)

    conditions_[cond_idx].payout_numerators = payout
    update_rows.push((cond_idx, payout_numerators))

// 批量更新
for (cond_idx, payout) in update_rows:
    UPDATE rb_condition SET payout_numerators = :payout WHERE cond_idx = :cond_idx
```

### 1B.3 处理新的 token_map

```sql
SELECT token0, token1, condition_id, block_number
FROM token_map
WHERE block_number > :last_block AND block_number <= :target_block
```

```
new_token_rows = []

for row in results:
    token0 = lowercase(token0)
    token1 = lowercase(token1)
    cond_id = lowercase(condition_id)

    if cond_id not in cond_map_: continue
    cond_idx = cond_map_[cond_id]

    if token0 not in token_map_:
        token_map_[token0] = TokenInfo { cond_idx, is_yes: true }
        new_token_rows.push((token0, cond_idx, 1))

    if token1 not in token_map_:
        token_map_[token1] = TokenInfo { cond_idx, is_yes: false }
        new_token_rows.push((token1, cond_idx, 0))

// 批量持久化
INSERT INTO rb_token_map (token_id, cond_idx, is_yes) VALUES ...
```

### 1B.4 处理新的 fpmm 并补全 token*map*

```sql
SELECT fpmm_addr, condition_ids, collateral_token, block_number
FROM fpmm
WHERE block_number > :last_block AND block_number <= :target_block
```

```
new_fpmm_rows = []
new_token_rows = []  // 从 fpmm 计算出的 token

for row in results:
    fpmm_addr = lowercase(fpmm_addr)
    if fpmm_addr in fpmm_map_: continue  // 已存在

    cond_ids = parse_json(condition_ids)
    collateral = lowercase(collateral_token)

    for cond_id in cond_ids:
        cond_id = lowercase(cond_id)
        if cond_id not in cond_map_: continue

        cond_idx = cond_map_[cond_id]

        // 计算 token_id (keccak256)
        collection_yes = keccak256(cond_id, 1)
        collection_no  = keccak256(cond_id, 2)
        token_yes = keccak256(collateral, collection_yes)
        token_no  = keccak256(collateral, collection_no)

        fpmm_map_[fpmm_addr] = FPMMInfo { cond_idx, token_yes, token_no }
        new_fpmm_rows.push((fpmm_addr, cond_idx, token_yes, token_no))

        // 补全 token_map_
        if token_yes not in token_map_:
            token_map_[token_yes] = TokenInfo { cond_idx, is_yes: true }
            new_token_rows.push((token_yes, cond_idx, 1))

        if token_no not in token_map_:
            token_map_[token_no] = TokenInfo { cond_idx, is_yes: false }
            new_token_rows.push((token_no, cond_idx, 0))

// 批量持久化
INSERT INTO rb_fpmm_map (fpmm_addr, cond_idx, token_yes, token_no) VALUES ...
INSERT INTO rb_token_map (token_id, cond_idx, is_yes) VALUES ...  // 去重
```

### 1B.5 处理新的 neg_risk_question

```sql
SELECT nrq.market_id, nrq.question_index, cp.condition_id, nrq.block_number
FROM neg_risk_question nrq
JOIN condition_preparation cp ON nrq.question_id = cp.question_id
WHERE nrq.block_number > :last_block AND nrq.block_number <= :target_block
```

```
new_neg_risk_rows = []

for row in results:
    market_id = lowercase(market_id)
    cond_id = lowercase(condition_id)
    key = (market_id, question_index)

    if key in neg_risk_map_: continue  // 已存在
    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    neg_risk_map_[key] = cond_idx
    new_neg_risk_rows.push((market_id, question_index, cond_idx))

// 批量持久化
INSERT INTO rb_neg_risk_map (market_id, question_index, cond_idx) VALUES ...
```

## Phase 2: 构建语义索引 (只处理 new_range)

构建语义索引，供 Phase 3 关联查询。

**关键优化**：语义事件和 Transfer 在同一个 tx（同一个 block），所以只需索引 new_range。

**并行执行**：所有 scan 函数相互独立，可并行执行。

### scan_split

```sql
SELECT block_number, tx_hash, condition_id, amount, stakeholder
FROM split
WHERE block_number > :last_block AND block_number <= :target_block
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_split_[key] = SplitInfo { condition_id, amount, stakeholder }
```

### scan_merge

```sql
SELECT block_number, tx_hash, condition_id, amount, stakeholder
FROM merge
WHERE block_number > :last_block AND block_number <= :target_block
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_merge_[key] = MergeInfo { condition_id, amount, stakeholder }
```

### scan_redemption

```sql
SELECT block_number, tx_hash, condition_id, index_sets, payout, redeemer
FROM redemption
WHERE block_number > :last_block AND block_number <= :target_block
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_redemption_[key] = RedemptionInfo { condition_id, index_sets, payout, redeemer }
```

### scan_convert

```sql
SELECT block_number, tx_hash, market_id, index_set, amount, stakeholder
FROM convert
WHERE block_number > :last_block AND block_number <= :target_block
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_convert_[key] = ConvertInfo { market_id, index_set, amount, stakeholder }
```

### scan_order_filled

```sql
SELECT block_number, tx_hash, maker, taker, maker_asset_id, taker_asset_id,
       maker_amount, taker_amount, fee
FROM order_filled
WHERE block_number > :last_block AND block_number <= :target_block
```

```
for row in results:
    // 确定 token_id 和交易方向
    if maker_asset_id == 0x0:  // maker 出 USDC 换 token → maker 买入
        token_id = taker_asset_id
        side = 1  // maker buy
        token_amount = taker_amount
        usdc_amount = maker_amount
    else:  // maker 出 token 换 USDC → maker 卖出
        token_id = maker_asset_id
        side = 2  // maker sell
        token_amount = maker_amount
        usdc_amount = taker_amount

    key = (block_number, lowercase(tx_hash), lowercase(token_id))
    tx_order_[key] = OrderInfo { maker, taker, side, token_amount, usdc_amount, fee }
```

### scan_fpmm_trade

```sql
SELECT block_number, tx_hash, fpmm_addr, trader, side, outcome_index,
       usdc_amount, token_amount, fee
FROM fpmm_trade
WHERE block_number > :last_block AND block_number <= :target_block
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_fpmm_trade_[key] = FPMMTradeInfo {
        fpmm_addr, trader, side, outcome_index, token_amount, usdc_amount, fee
    }
```

### scan_fpmm_funding

```sql
SELECT block_number, tx_hash, fpmm_addr, funder, side, amounts, collateral_from_fee_pool
FROM fpmm_funding
WHERE block_number > :last_block AND block_number <= :target_block
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    amounts_arr = parse_json(amounts)  // "[yes, no]"
    tx_fpmm_funding_[key] = FPMMFundingInfo {
        fpmm_addr, funder, side, amounts_arr, collateral_from_fee_pool
    }
```

## Phase 3: 处理 Transfer 并持久化 (只处理 new_range)

**核心逻辑**：遍历 new_range 的 transfer 记录，关联语义索引，生成 user_event 并持久化。

### 已知合约地址

```
ZERO_ADDR = 0x0000000000000000000000000000000000000000
CTF_EXCHANGE = 0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e
NEG_RISK_CTF_EXCHANGE = 0xc5d563a36ae78145c45a50134d48a1215220f80a
NEG_RISK_ADAPTER = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296
CONDITIONAL_TOKENS = 0x4d97dcd97ec945f40cf65f87097ace5ea0476045
```

### 批量写入缓冲

```
pending_events: vec<(user_addr, sort_key, cond_idx, event_type, token_idx, amount, price)>
BATCH_SIZE = 10000  // 每 10000 条写入一次

def flush_events():
    if pending_events.empty(): return
    INSERT INTO user_event (user_addr, sort_key, cond_idx, event_type, token_idx, amount, price)
    VALUES ... (pending_events)
    pending_events.clear()

def push_event(user_addr, sort_key, cond_idx, event_type, token_idx, amount, price):
    pending_events.push((user_addr, sort_key, cond_idx, event_type, token_idx, amount, price))
    if pending_events.len() >= BATCH_SIZE:
        flush_events()
```

### 主流程

```sql
SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount
FROM transfer
WHERE block_number > :last_block AND block_number <= :target_block
ORDER BY block_number, log_index
```

```
for row in results:
    token_id = lowercase(token_id)
    if token_id not in token_map_: continue

    info = token_map_[token_id]
    cond_idx = info.cond_idx
    token_idx = info.is_yes ? 0 : 1
    sort_key = block_number * 1e9 + log_index
    tx_key = (block_number, lowercase(tx_hash))

    op = lowercase(operator)
    from = lowercase(from_addr)
    to = lowercase(to_addr)

    // 分类处理
    if from == ZERO_ADDR:
        process_mint(tx_key, to, op, cond_idx, token_idx, amount, sort_key)
    elif to == ZERO_ADDR:
        process_burn(tx_key, from, op, cond_idx, token_idx, amount, sort_key, token_id)
    elif op == CTF_EXCHANGE or op == NEG_RISK_CTF_EXCHANGE:
        process_exchange_trade(tx_key, from, to, token_id, cond_idx, token_idx, amount, sort_key)
    elif op in fpmm_map_:
        process_fpmm_trade(tx_key, from, to, op, cond_idx, token_idx, amount, sort_key)
    else:
        process_direct_transfer(from, to, cond_idx, token_idx, amount, sort_key)

// 处理完毕，刷新剩余缓冲
flush_events()

// 更新同步状态
UPDATE sync_state SET value = :target_block WHERE key = 'rebuild_last_block'
```

### process_mint (from=0x0)

mint 来源：Split 或 FPMMLPAdd (内部 Split)

```
def process_mint(tx_key, to, op, cond_idx, token_idx, amount, sort_key):
    // 尝试匹配 Split (用户直接操作)
    if tx_key in tx_split_:
        split = tx_split_[tx_key]
        // 如果 to 是用户，直接记录
        if not is_known_contract(to):
            push_event(to, sort_key, cond_idx, Split, token_idx, amount, 500000)
        return

    // 尝试匹配 FPMMLPAdd (FPMM 内部 Split)
    if tx_key in tx_fpmm_funding_ and tx_fpmm_funding_[tx_key].side == 1:
        funding = tx_fpmm_funding_[tx_key]
        // mint to FPMM 的 Transfer，用户是 funder
        // FPMMLPAdd 只处理一次（token_idx=0 时）
        if token_idx == 0:
            user = lowercase(funding.funder)
            // amount 是 mint 的总量 (max)，用于计算成本
            // funding.amounts 是进池子的量
            // 注意：同 tx 可能有多条 mint Transfer (YES/NO 各一条)
            // 这里用 Transfer 的 amount 作为 max，假设 YES/NO 数量相等
            push_event(user, sort_key, cond_idx, FPMMLPAdd, 0xFF, funding.amounts[0], funding.amounts[1])
        return

    // 如果 to 是已知合约，跳过
    if to in fpmm_map_ or is_known_contract(to):
        return

    // 未匹配到语义事件，作为无成本 TransferIn
    push_event(to, sort_key, cond_idx, TransferIn, token_idx, amount, 0)
```

**is_known_contract**: CTF_EXCHANGE, NEG_RISK_CTF_EXCHANGE, NEG_RISK_ADAPTER, CONDITIONAL_TOKENS

### process_burn (to=0x0)

burn 来源：Merge 或 Redemption 或 Convert 或 FPMMLPRemove

```
def process_burn(tx_key, from, op, cond_idx, token_idx, amount, sort_key, token_id):
    // 如果 from 是已知合约，跳过（合约内部操作，不是用户流水）
    if from in fpmm_map_ or is_known_contract(from):
        return

    // 尝试匹配 Merge
    if tx_key in tx_merge_:
        // Merge: 1 YES + 1 NO → 1 USDC, 每个 token 换回 0.5
        // amount 是单个 token 数量，price = 0.5 * 1e6 = 500000
        push_event(from, sort_key, cond_idx, Merge, token_idx, amount, 500000)
        return

    // 尝试匹配 Redemption
    if tx_key in tx_redemption_:
        redemption = tx_redemption_[tx_key]
        // Redemption: 需要从 payout 和 positions 计算 price
        // 为避免重复，只在第一个被赎回的 token 时处理
        index_sets = parse_json(redemption.index_sets)
        first_idx = find_first_set_bit(index_sets)
        if token_idx == first_idx:
            push_event(from, sort_key, cond_idx, Redemption, pack_index_sets(index_sets), redemption.payout, 0)
        return

    // 尝试匹配 Convert
    if tx_key in tx_convert_:
        convert = tx_convert_[tx_key]
        // Convert: M 个 NO → (M-1) USDC
        // price 存 index_set 供 apply_event 计算 popcount
        // 只处理 NO token (token_idx=1)
        if token_idx == 1:
            push_event(from, sort_key, cond_idx, Convert, 1, amount, convert.index_set)
        return

    // 尝试匹配 FPMMLPRemove
    if tx_key in tx_fpmm_funding_:
        funding = tx_fpmm_funding_[tx_key]
        if funding.side == 2:  // Remove
            if token_idx == 0:
                push_event(from, sort_key, cond_idx, FPMMLPRemove, 0xFF, funding.amounts[0], funding.amounts[1])
            return

    // 未匹配到语义事件，作为 TransferOut
    push_event(from, sort_key, cond_idx, TransferOut, token_idx, amount, 0)
```

### process_exchange_trade (operator=Exchange)

交易所撮合的 Transfer

```
def process_exchange_trade(tx_key, from, to, token_id, cond_idx, token_idx, amount, sort_key):
    order_key = (tx_key[0], tx_key[1], lowercase(token_id))

    if order_key not in tx_order_:
        // 无对应 OrderFilled，作为直接转账处理
        process_direct_transfer(from, to, cond_idx, token_idx, amount, sort_key)
        return

    order = tx_order_[order_key]
    price = order.usdc_amount * 1e6 / order.token_amount

    // from 是卖方，to 是买方
    push_event(from, sort_key, cond_idx, Sell, token_idx, amount, price)
    push_event(to,   sort_key, cond_idx, Buy,  token_idx, amount, price)
```

### process_fpmm_trade (operator=FPMM)

FPMM 相关的 Transfer

```
def process_fpmm_trade(tx_key, from, to, fpmm_addr, cond_idx, token_idx, amount, sort_key):
    // 先检查是否是 FPMMTrade (Buy/Sell)
    if tx_key in tx_fpmm_trade_:
        trade = tx_fpmm_trade_[tx_key]
        price = trade.usdc_amount * 1e6 / trade.token_amount

        if trade.side == 1:  // Buy: FPMM → user
            push_event(to, sort_key, cond_idx, FPMMBuy, token_idx, amount, price)
        else:  // Sell: user → FPMM
            push_event(from, sort_key, cond_idx, FPMMSell, token_idx, amount, price)
        return

    // 检查是否是 FPMMFunding 相关的 Transfer
    if tx_key in tx_fpmm_funding_:
        funding = tx_fpmm_funding_[tx_key]
        fpmm = lowercase(fpmm_addr)

        if from == fpmm:
            // FPMM → user: LP Add 返还多余 token，作为 0 成本 TransferIn
            push_event(to, sort_key, cond_idx, TransferIn, token_idx, amount, 0)
        else:
            // user → FPMM: LP Remove 前用户转入 token 给 FPMM burn
            // 这些 token 会在 process_burn 中作为 FPMMLPRemove 处理，跳过避免重复
            pass
        return

    // 其他情况作为直接转账
    process_direct_transfer(from, to, cond_idx, token_idx, amount, sort_key)
```

### process_direct_transfer

用户间直接转账

```
def process_direct_transfer(from, to, cond_idx, token_idx, amount, sort_key):
    push_event(from, sort_key, cond_idx, TransferOut, token_idx, amount, 0)
    push_event(to,   sort_key, cond_idx, TransferIn,  token_idx, amount, 0)
```
