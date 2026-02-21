# PnL 重建流程

## 数据流总览

```
┌─────────────────────────────────────────────────────────────────┐
│                         Phase 1: Metadata                       │
├─────────────────────────────────────────────────────────────────┤
│  condition_preparation ─────────────► cond_map_                 │
│  condition_resolution ──────────────► conditions_[] (payout)    │
│                                                                 │
│  token_map ─────► (join cond_map_) ─► token_map_                │
│                                           ▲                     │
│  fpmm ──────────► (join cond_map_) ─► fpmm_map_                 │
│       └──────────► (keccak256)  ──────────┘ (补全FPMM token)    │
│                                                                 │
│  neg_risk_question ► (join condition) ► neg_risk_map_           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                   Phase 2: Build Semantic Index                 │
├─────────────────────────────────────────────────────────────────┤
│  split ─────────────► tx_split_[(block, tx_hash)]               │
│  merge ─────────────► tx_merge_[(block, tx_hash)]               │
│  redemption ────────► tx_redemption_[(block, tx_hash)]          │
│  convert ───────────► tx_convert_[(block, tx_hash)]             │
│  order_filled ──────► tx_order_[(block, tx_hash, token_id)]     │
│  fpmm_trade ────────► tx_fpmm_trade_[(block, tx_hash)]          │
│  fpmm_funding ──────► tx_fpmm_funding_[(block, tx_hash)]        │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                   Phase 3: Process Transfer                     │
├─────────────────────────────────────────────────────────────────┤
│  for each transfer:                                             │
│    classify by (operator, from, to)                             │
│    lookup semantic event → get price/context                    │
│    push to user_events_[from] and/or user_events_[to]           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Phase 4: Replay                          │
├─────────────────────────────────────────────────────────────────┤
│  for each user (parallel):                                      │
│    sort events by sort_key                                      │
│    for each event:                                              │
│      apply_event → update ReplayState                           │
│      snapshot → append to Snapshot chain                        │
│    store user_states_[uid]                                      │
└─────────────────────────────────────────────────────────────────┘

原则：
1. 任何token流水都被包括
2. 任何token流水不被double count
3. token流水被精确还原， 不含近似假设

核心设计：
- Transfer 是持仓变化的【唯一来源】(TransferSingle + TransferBatch 覆盖所有持仓变化)
- 其他事件(Split/Merge/OrderFilled等)只提供【语义和价格信息】
- 通过 (block_number, tx_hash) 关联 Transfer 和语义事件

```

## 数据结构

### 映射表 (Phase 1 产出)

```
cond_map_: map<condition_id(hex), cond_idx(u32)>
    来源: condition_preparation 表

token_map_: map<token_id(hex), TokenInfo>
    来源: token_map 表 + fpmm 表计算补全
    TokenInfo = { cond_idx: u32, is_yes: bool }

fpmm_map_: map<fpmm_addr(hex), FPMMInfo>
    来源: fpmm 表
    FPMMInfo = { cond_idx: u32, token_yes: hex, token_no: hex }

neg_risk_map_: map<(market_id, question_index), cond_idx(u32)>
    来源: neg_risk_question JOIN condition_preparation ON question_id

conditions_[cond_idx]: ConditionInfo
    ConditionInfo = { outcome_count: u8, payout_numerators: vec<i64> }
```

### 语义索引 (Phase 2 产出)

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
```

### 事件

```
RawEvent (32 bytes):
    sort_key:   i64   block_number * 1e9 + log_index
    cond_idx:   u32   condition 索引
    type:       u8    EventType
    token_idx:  u8    0=YES, 1=NO, 0xFF=全部
    _pad:       u16
    amount:     i64   数量 (1e6 = $1)
    price:      i64   价格*1e6 或额外数据

EventType:
    Buy=0, Sell=1, Split=2, Merge=3, Redemption=4,
    FPMMBuy=5, FPMMSell=6, FPMMLPAdd=7, FPMMLPRemove=8,
    Convert=9, TransferIn=10, TransferOut=11
```

### 用户状态

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

## Phase 1: load_metadata

### 1.1 从 condition_preparation 表构建 cond_map_

```sql
SELECT condition_id, outcome_slot_count FROM condition_preparation
```

```
for row in results:
    cond_id = lowercase(condition_id)
    cond_idx = conditions_.size()

    cond_map_[cond_id] = cond_idx
    conditions_.push(ConditionInfo { outcome_count: outcome_slot_count })
```

### 1.2 从 condition_resolution 表填充 payout_numerators

```sql
SELECT condition_id, payout_numerators FROM condition_resolution
```

```
for row in results:
    cond_id = lowercase(condition_id)
    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    conditions_[cond_idx].payout_numerators = parse(payout_numerators)
```

### 1.3 从 token_map 表构建 token_map_

```sql
SELECT token0, token1, condition_id FROM token_map
```

```
for row in results:
    token0 = lowercase(token0)  // YES token
    token1 = lowercase(token1)  // NO token
    cond_id = lowercase(condition_id)

    if cond_id not in cond_map_: continue
    cond_idx = cond_map_[cond_id]

    token_map_[token0] = TokenInfo { cond_idx, is_yes: true }
    token_map_[token1] = TokenInfo { cond_idx, is_yes: false }
```

### 1.4 从 fpmm 表构建 fpmm_map_ 并补全 token_map_

```sql
SELECT fpmm_addr, condition_ids, collateral_token FROM fpmm
```

```
for row in results:
    fpmm_addr = lowercase(fpmm_addr)
    cond_ids = parse_json(condition_ids)  // "[\"0x...\"]"
    collateral = lowercase(collateral_token)

    for cond_id in cond_ids:
        cond_id = lowercase(cond_id)
        if cond_id not in cond_map_: continue

        cond_idx = cond_map_[cond_id]

        // 补全 FPMM 市场的 token_map (可能未在订单簿注册)
        // positionId = keccak256(collateralToken, collectionId)
        // collectionId = keccak256(conditionId, indexSet)
        collection_yes = keccak256(cond_id, 1)
        collection_no  = keccak256(cond_id, 2)
        token_yes = keccak256(collateral, collection_yes)
        token_no  = keccak256(collateral, collection_no)

        fpmm_map_[fpmm_addr] = FPMMInfo { cond_idx, token_yes, token_no }

        if token_yes not in token_map_:
            token_map_[token_yes] = TokenInfo { cond_idx, is_yes: true }
        if token_no not in token_map_:
            token_map_[token_no] = TokenInfo { cond_idx, is_yes: false }
```

### 1.5 从 neg_risk_question 构建 neg_risk_map_

```sql
SELECT nrq.market_id, nrq.question_index, cp.condition_id
FROM neg_risk_question nrq
JOIN condition_preparation cp ON nrq.question_id = cp.question_id
```

```
for row in results:
    market_id = lowercase(market_id)
    cond_id = lowercase(condition_id)

    if cond_id not in cond_map_: continue

    neg_risk_map_[(market_id, question_index)] = cond_map_[cond_id]
```

## Phase 2: build_semantic_index

构建语义索引，供 Phase 3 关联查询。所有 scan 函数可并行执行。

### scan_split

```sql
SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM split
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_split_[key] = SplitInfo { condition_id, amount, stakeholder }
```

### scan_merge

```sql
SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM merge
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_merge_[key] = MergeInfo { condition_id, amount, stakeholder }
```

### scan_redemption

```sql
SELECT block_number, tx_hash, condition_id, index_sets, payout, redeemer FROM redemption
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    tx_redemption_[key] = RedemptionInfo { condition_id, index_sets, payout, redeemer }
```

### scan_convert

```sql
SELECT block_number, tx_hash, market_id, index_set, amount, stakeholder FROM convert
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
```

```
for row in results:
    key = (block_number, lowercase(tx_hash))
    amounts_arr = parse_json(amounts)  // "[yes, no]"
    tx_fpmm_funding_[key] = FPMMFundingInfo {
        fpmm_addr, funder, side, amounts_arr, collateral_from_fee_pool
    }
```

## Phase 3: process_transfer

**核心逻辑**：遍历 transfer 表的每条记录，根据 (operator, from_addr, to_addr) 判断类型，关联语义索引获取价格，生成用户事件。

### 已知合约地址

```
ZERO_ADDR = 0x0000000000000000000000000000000000000000
CTF_EXCHANGE = 0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e
NEG_RISK_CTF_EXCHANGE = 0xc5d563a36ae78145c45a50134d48a1215220f80a
NEG_RISK_ADAPTER = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296
CONDITIONAL_TOKENS = 0x4d97dcd97ec945f40cf65f87097ace5ea0476045
```

### 主流程

```sql
SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount
FROM transfer ORDER BY block_number, log_index
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
            user = to
            uid = intern_user(user)
            push(uid, { sort_key, cond_idx, Split, token_idx, amount, 500000 })
        return

    // 尝试匹配 FPMMLPAdd (FPMM 内部 Split)
    if tx_key in tx_fpmm_funding_ and tx_fpmm_funding_[tx_key].side == 1:
        funding = tx_fpmm_funding_[tx_key]
        // mint to FPMM 的 Transfer，用户是 funder
        // FPMMLPAdd 只处理一次（token_idx=0 时）
        if token_idx == 0:
            user = lowercase(funding.funder)
            uid = intern_user(user)
            // amount 是 mint 的总量 (max)，用于计算成本
            // funding.amounts 是进池子的量
            // 注意：同 tx 可能有多条 mint Transfer (YES/NO 各一条)
            // 这里用 Transfer 的 amount 作为 max，假设 YES/NO 数量相等
            push(uid, { sort_key, cond_idx, FPMMLPAdd, 0xFF, funding.amounts[0], funding.amounts[1] })
        return

    // 如果 to 是已知合约，跳过
    if to in fpmm_map_ or is_known_contract(to):
        return

    // 未匹配到语义事件，作为无成本 TransferIn
    uid = intern_user(to)
    push(uid, { sort_key, cond_idx, TransferIn, token_idx, amount, 0 })
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
        merge = tx_merge_[tx_key]
        user = from
        uid = intern_user(user)
        // Merge: 1 YES + 1 NO → 1 USDC, 每个 token 换回 0.5
        // amount 是单个 token 数量，price = 0.5 * 1e6 = 500000
        push(uid, { sort_key, cond_idx, Merge, token_idx, amount, 500000 })
        return

    // 尝试匹配 Redemption
    if tx_key in tx_redemption_:
        redemption = tx_redemption_[tx_key]
        user = from
        uid = intern_user(user)
        // Redemption: 需要从 payout 和 positions 计算 price
        // 为避免重复，只在第一个被赎回的 token 时处理
        index_sets = parse_json(redemption.index_sets)
        first_idx = find_first_set_bit(index_sets)
        if token_idx == first_idx:
            push(uid, { sort_key, cond_idx, Redemption, pack_index_sets(index_sets), redemption.payout, 0 })
        return

    // 尝试匹配 Convert
    if tx_key in tx_convert_:
        convert = tx_convert_[tx_key]
        user = from
        uid = intern_user(user)
        // Convert: M 个 NO → (M-1) USDC
        // price 存 index_set 供 apply_event 计算 popcount
        // 只处理 NO token (token_idx=1)
        if token_idx == 1:
            push(uid, { sort_key, cond_idx, Convert, 1, amount, convert.index_set })
        return

    // 尝试匹配 FPMMLPRemove
    if tx_key in tx_fpmm_funding_:
        funding = tx_fpmm_funding_[tx_key]
        if funding.side == 2:  // Remove
            user = from
            uid = intern_user(user)
            if token_idx == 0:
                push(uid, { sort_key, cond_idx, FPMMLPRemove, 0xFF, funding.amounts[0], funding.amounts[1] })
            return

    // 未匹配到语义事件，作为 TransferOut
    uid = intern_user(from)
    push(uid, { sort_key, cond_idx, TransferOut, token_idx, amount, 0 })
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
    seller_uid = intern_user(from)
    buyer_uid = intern_user(to)

    push(seller_uid, { sort_key, cond_idx, Sell, token_idx, amount, price })
    push(buyer_uid,  { sort_key, cond_idx, Buy,  token_idx, amount, price })
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
            user_uid = intern_user(to)
            push(user_uid, { sort_key, cond_idx, FPMMBuy, token_idx, amount, price })
        else:  // Sell: user → FPMM
            user_uid = intern_user(from)
            push(user_uid, { sort_key, cond_idx, FPMMSell, token_idx, amount, price })
        return

    // 检查是否是 FPMMFunding 相关的 Transfer
    if tx_key in tx_fpmm_funding_:
        funding = tx_fpmm_funding_[tx_key]
        fpmm = lowercase(fpmm_addr)

        if from == fpmm:
            // FPMM → user: LP Add 返还多余 token，作为 0 成本 TransferIn
            uid = intern_user(to)
            push(uid, { sort_key, cond_idx, TransferIn, token_idx, amount, 0 })
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
    from_uid = intern_user(from)
    to_uid = intern_user(to)

    push(from_uid, { sort_key, cond_idx, TransferOut, token_idx, amount, 0 })
    push(to_uid,   { sort_key, cond_idx, TransferIn,  token_idx, amount, 0 })
```

---

## Phase 4: replay_all

### 主流程

```
for uid in 0..users_.size() (并行):
    replay_user(uid)

replay_user(uid):
    events = user_events_[uid]
    if events.empty(): return

    sort(events, by sort_key)

    states: map<cond_idx, ReplayState>
    snaps:  map<cond_idx, vec<Snapshot>>

    for evt in events:
        st = &states[evt.cond_idx]
        cond = &conditions_[evt.cond_idx]

        apply_event(evt, st, cond)

        snap = Snapshot {
            sort_key, delta: evt.amount, price: evt.price,
            event_type: evt.type, token_idx: evt.token_idx,
            outcome_count: cond.outcome_count,
            positions: copy(st.positions),
            cost_basis: sum(st.cost),
            realized_pnl: st.realized_pnl,
        }
        snaps[evt.cond_idx].push(snap)

    user_states_[uid] = convert(snaps)
```

### apply_event

**核心原则**：所有操作只做简单的 +/- 仓位，不做任何持仓检查。负持仓说明有 bug。

---

#### Buy / FPMMBuy

**步骤**:
1. 计算成本 = amount * price / 1e6
2. cost[i] += 成本
3. positions[i] += amount

**注意**:
- price 单位是 1e6 = $1，amount 也是 6 decimals
- 除以 1e6 是为了让 cost 的单位也是 6 decimals

```
i = token_idx
cost[i] += amount * price / 1e6
positions[i] += amount
```

---

#### Sell / FPMMSell

**步骤**:
1. 计算按比例移除的成本 = cost[i] * amount / positions[i]
2. 计算卖出收入 = amount * price / 1e6
3. realized_pnl += 收入 - 成本
4. cost[i] -= cost_removed
5. positions[i] -= amount

**注意**:
- **不检查 positions[i] 是否足够**：如果 positions[i] < amount，会产生负持仓
- 负持仓 = bug 信号，需要排查事件流

```
i = token_idx
cost_removed = cost[i] * amount / positions[i]
realized_pnl += amount * price / 1e6 - cost_removed
cost[i] -= cost_removed
positions[i] -= amount
```

---

#### Split

**步骤**:
1. 计算成本 = amount * price / 1e6 (price = 500000 = $0.50)
2. cost[i] += 成本
3. positions[i] += amount

**注意**:
- 新设计：每个 mint Transfer 单独处理，YES 和 NO 各一次
- price = 500000 ($0.50)，因为 1 USDC → 1 YES + 1 NO
- amount 是单个 token 的数量

```
i = token_idx
cost[i] += amount * price / 1e6  // = amount * 0.5
positions[i] += amount
```

---

#### Merge

**步骤**:
1. 计算按比例移除的成本
2. 计算收入 = amount * price / 1e6 (price = 500000 = $0.50)
3. realized_pnl += 收入 - 成本
4. 减少 cost 和 positions

**注意**:
- 新设计：每个 burn Transfer 单独处理，YES 和 NO 各一次
- price = 500000 ($0.50)，因为 1 YES + 1 NO → 1 USDC
- 是 Split 的逆操作

```
i = token_idx
cost_removed = cost[i] * amount / positions[i]
realized_pnl += amount * price / 1e6 - cost_removed  // = amount * 0.5 - cost_removed
cost[i] -= cost_removed
positions[i] -= amount
```

---

#### Redemption

**步骤**:
1. 解析 index_sets bitmap
2. 对每个涉及的 outcome：
   - realized_pnl += 持仓 * payout_price - 成本
   - 清零 cost 和 positions

**注意**:
- **假设全量赎回**：清零所有涉及的 positions
- payout_price = payout_numerators[i]（0 或 1）
- 输家 token (payout=0)：realized_pnl -= cost（亏损全部成本）
- 赢家 token (payout=1)：realized_pnl += positions - cost

```
index_sets = token_idx

for i in 0..outcome_count:
    if not ((index_sets >> i) & 1): continue
    
    payout_price = payout_numerators[i]
    realized_pnl += positions[i] * payout_price - cost[i]
    cost[i] = 0
    positions[i] = 0
```

---

#### FPMMLPAdd

**步骤**:
1. 计算实际 USDC 投入 = max(amount0, amount1)
2. 按 token 比例分配成本（只针对进池子的部分）
3. 增加 positions（只记录进池子的部分）

**注意**:
- LP 投入 USDC → Split 成 YES+NO → 按池子比例添加 → 多余 token 返还
- **usdc_spent = max(amount0, amount1)**：这是 Split 的 USDC 数量
- amount0/amount1 是进入池子的 token 数量
- **返还 token 单独处理**：返还给用户的 (max-amount0) YES + (max-amount1) NO 通过 Transfer(from=FPMM) 被 process_fpmm_trade 处理为 TransferIn（成本=0）
- **成本近似**：返还 token 成本为 0，不完美但简化了逻辑。用户的总 USDC 支出 = usdc_spent，其中大部分成本分配给进池子的 token

```
amount0 = amount   // 添加到池子的 YES 数量
amount1 = price    // 添加到池子的 NO 数量
usdc_spent = max(amount0, amount1)
total = amount0 + amount1

cost[0] += usdc_spent * amount0 / total
cost[1] += usdc_spent * amount1 / total
positions[0] += amount0
positions[1] += amount1
```

---

#### FPMMLPRemove

**步骤**:
1. 按比例移除成本
2. 减少 cost 和 positions
3. **不计算 realized_pnl**

**注意**:
- LP 取回的是 YES+NO token，**不是 USDC**
- **不计算 realized_pnl 的原因**：Remove 只是把"池子份额"换成"手持 token"，没有发生 USDC 交换
- 用户后续可能：(1) 保留 token (2) 手动 Merge (3) 在交易所卖出
- 这些操作会通过 Transfer 事件被捕获，届时再计入 realized_pnl

```
amount0 = amount   // YES 数量
amount1 = price    // NO 数量

cost_removed0 = cost[0] * amount0 / positions[0]
cost_removed1 = cost[1] * amount1 / positions[1]
cost[0] -= cost_removed0
cost[1] -= cost_removed1
positions[0] -= amount0
positions[1] -= amount1
// realized_pnl 不变
```

---

#### Convert

**步骤**:
1. 解析 index_set，计算 popcount
2. 按比例移除 NO 成本
3. 减少 NO 持仓
4. 计算分摊收益 = (popcount-1)/popcount * amount

**注意**:
- **仅限 NegRisk**：M 个 NO → (M-1) USDC
- 每个涉及的 condition 都会收到一个 Convert 事件
- **收益分摊**：总收益 (M-1)*amount 平均分到 M 个 condition
- 每个 condition 销毁 amount 个 NO token

```
index_set = price
popcount = bitcount(index_set)

cost_removed = cost[1] * amount / positions[1]
cost[1] -= cost_removed
positions[1] -= amount

realized_pnl += (popcount - 1) * amount / popcount - cost_removed
```

---

#### TransferIn

**步骤**:
1. 增加 positions

**注意**:
- **0 成本获得 token**：可能是赠与、空投、从其他账户转入
- 不增加 cost（成本为 0）

```
positions[token_idx] += amount
```

---

#### TransferOut

**步骤**:
1. 按比例移除成本
2. 减少 cost 和 positions

**注意**:
- **不产生 realized_pnl**：转出不是卖出，只是把 token 和对应成本转移走
- 如果转给自己的另一个账户，那边会收到 TransferIn（成本为 0）

```
cost_removed = cost[token_idx] * amount / positions[token_idx]
cost[token_idx] -= cost_removed
positions[token_idx] -= amount
```
