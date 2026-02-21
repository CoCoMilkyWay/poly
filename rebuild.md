# PnL 重建流程

## 数据流总览

```
┌─────────────────────────────────────────────────────────────────┐
│                         Phase 1: Metadata                       │
├─────────────────────────────────────────────────────────────────┤
│  condition ──────────────────────► cond_map_                    │
│      │                               conditions_[]              │
│      │                               cond_ids_[]                │
│      │                                                          │
│  token_map ─────► (join cond_map_) ─► token_map_                │
│                                                                 │
│  fpmm ──────────► (join cond_map_) ─► fpmm_map_                 │
│                                                                 │
│  neg_risk_question ► (join condition) ► neg_risk_map_           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      Phase 2: Collect Events                    │
├─────────────────────────────────────────────────────────────────┤
│  order_filled ──► token_map_    ──► user_events_[uid]           │
│  split ─────────► cond_map_     ──► user_events_[uid]           │
│  merge ─────────► cond_map_     ──► user_events_[uid]           │
│  redemption ────► cond_map_     ──► user_events_[uid]           │
│  fpmm_trade ────► fpmm_map_     ──► user_events_[uid]           │
│  fpmm_funding ──► fpmm_map_     ──► user_events_[uid]           │
│  convert ───────► neg_risk_map_ ──► user_events_[uid]           │
│  transfer ──────► token_map_    ──► user_events_[uid]           │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Phase 3: Replay                          │
├─────────────────────────────────────────────────────────────────┤
│  for each user (parallel):                                      │
│    sort events by sort_key                                      │
│    for each event:                                              │
│      apply_event → update ReplayState                           │
│      snapshot → append to Snapshot chain                        │
│    store user_states_[uid]                                      │
└─────────────────────────────────────────────────────────────────┘
```

## 数据结构

### 映射表

```
cond_map_: map<condition_id(hex), cond_idx(u32)>
    来源: condition 表 (包含所有 condition)

token_map_: map<token_id(hex), TokenInfo>
    来源: token_map 表
    TokenInfo = { cond_idx: u32, is_yes: u8 }

fpmm_map_: map<fpmm_addr(hex), cond_idx(u32)>
    来源: fpmm 表

neg_risk_map_: map<(market_id, question_index), cond_idx(u32)>
    来源: neg_risk_question JOIN condition ON question_id

conditions_[cond_idx]: ConditionInfo
    ConditionInfo = { outcome_count: u8, payout_numerators: vec<i64> }

cond_ids_[cond_idx]: string
    condition_id 反查
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

### 1.1 从 condition 表构建 cond*map*

```sql
SELECT condition_id, payout_numerators FROM condition
```

```
for row in results:
    cond_id = lowercase(condition_id)
    cond_idx = conditions_.size()

    cond_map_[cond_id] = cond_idx
    cond_ids_.push(cond_id)

    cond = ConditionInfo { outcome_count: 2 }
    if payout_numerators != NULL:
        cond.payout_numerators = parse(payout_numerators)
    conditions_.push(cond)
```

### 1.2 从 token*map 表构建 token_map*

```sql
SELECT token_id, condition_id, is_yes FROM token_map
```

```
for row in results:
    token_id = lowercase(token_id)
    cond_id = lowercase(condition_id)

    if cond_id not in cond_map_: continue

    token_map_[token_id] = TokenInfo {
        cond_idx: cond_map_[cond_id],
        is_yes: is_yes
    }
```

### 1.3 从 fpmm 表构建 fpmm_map_ 并补全 token_map_

```sql
SELECT fpmm_addr, condition_id FROM fpmm
```

```
USDC_E = 0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174

for row in results:
    fpmm_addr = lowercase(fpmm_addr)
    cond_id = lowercase(condition_id)

    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    fpmm_map_[fpmm_addr] = cond_idx

    // 补全 FPMM 市场的 token_map (可能未在订单簿注册)
    // positionId = keccak256(collateralToken, collectionId)
    // collectionId = keccak256(conditionId, indexSet)
    collection_yes = keccak256(cond_id, 1)
    collection_no  = keccak256(cond_id, 2)
    token_yes = keccak256(USDC_E, collection_yes)
    token_no  = keccak256(USDC_E, collection_no)

    if token_yes not in token_map_:
        token_map_[token_yes] = TokenInfo { cond_idx, is_yes: 1 }
    if token_no not in token_map_:
        token_map_[token_no] = TokenInfo { cond_idx, is_yes: 0 }
```

### 1.4 从 neg_risk_question 构建 neg_risk_map_

```sql
SELECT nrq.market_id, nrq.question_index, c.condition_id
FROM neg_risk_question nrq
JOIN condition c ON nrq.question_id = c.question_id
```

```
for row in results:
    market_id = lowercase(market_id)
    cond_id = lowercase(condition_id)

    if cond_id not in cond_map_: continue

    neg_risk_map_[(market_id, question_index)] = cond_map_[cond_id]
```

## Phase 2: collect_events

8 个 scan 函数并行执行。

### scan_order_filled

```sql
SELECT block_number, log_index, maker, taker, token_id, side, usdc_amount, token_amount
FROM order_filled ORDER BY block_number, log_index
```

```
for row in results:
    token_id = lowercase(token_id)
    if token_id not in token_map_: continue

    info = token_map_[token_id]
    sort_key = block_number * 1e9 + log_index
    price = usdc_amount * 1e6 / token_amount
    token_idx = info.is_yes ? 0 : 1

    maker_uid = intern_user(maker)
    taker_uid = intern_user(taker)

    if side == 1:  // maker 买入
        push(maker_uid, { sort_key, info.cond_idx, Buy,  token_idx, token_amount, price })
        push(taker_uid, { sort_key, info.cond_idx, Sell, token_idx, token_amount, price })
    else:          // maker 卖出
        push(maker_uid, { sort_key, info.cond_idx, Sell, token_idx, token_amount, price })
        push(taker_uid, { sort_key, info.cond_idx, Buy,  token_idx, token_amount, price })
```

### scan_split

```sql
SELECT block_number, log_index, stakeholder, condition_id, amount
FROM split ORDER BY block_number, log_index
```

```
NEGRISK_ADAPTER = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296

for row in results:
    // NegRisk 用户通过 Adapter 操作，实际流水已在 order_filled 中
    if stakeholder == NEGRISK_ADAPTER: continue

    cond_id = lowercase(condition_id)
    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(stakeholder)

    push(uid, { sort_key, cond_idx, Split, 0xFF, amount, 0 })
```

### scan_merge

```sql
SELECT block_number, log_index, stakeholder, condition_id, amount
FROM merge ORDER BY block_number, log_index
```

```
NEGRISK_ADAPTER = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296

for row in results:
    // NegRisk 用户通过 Adapter 操作，实际流水已在 order_filled 中
    if stakeholder == NEGRISK_ADAPTER: continue

    cond_id = lowercase(condition_id)
    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(stakeholder)

    push(uid, { sort_key, cond_idx, Merge, 0xFF, amount, 0 })
```

### scan_redemption

```sql
SELECT block_number, log_index, redeemer, condition_id, index_sets, payout
FROM redemption ORDER BY block_number, log_index
```

```
for row in results:
    cond_id = lowercase(condition_id)
    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(redeemer)

    push(uid, { sort_key, cond_idx, Redemption, index_sets, payout, 0 })
```

### scan_fpmm_trade

```sql
SELECT block_number, log_index, fpmm_addr, trader, side, outcome_index, usdc_amount, token_amount
FROM fpmm_trade ORDER BY block_number, log_index
```

```
for row in results:
    fpmm_addr = lowercase(fpmm_addr)
    if fpmm_addr not in fpmm_map_: continue

    cond_idx = fpmm_map_[fpmm_addr]
    sort_key = block_number * 1e9 + log_index
    price = usdc_amount * 1e6 / token_amount
    token_idx = (outcome_index == 0) ? 0 : 1
    uid = intern_user(trader)

    type = (side == 1) ? FPMMBuy : FPMMSell
    push(uid, { sort_key, cond_idx, type, token_idx, token_amount, price })
```

### scan_fpmm_funding

```sql
SELECT block_number, log_index, fpmm_addr, funder, side, amount0, amount1
FROM fpmm_funding ORDER BY block_number, log_index
```

```
for row in results:
    fpmm_addr = lowercase(fpmm_addr)
    if fpmm_addr not in fpmm_map_: continue

    cond_idx = fpmm_map_[fpmm_addr]
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(funder)

    type = (side == 1) ? FPMMLPAdd : FPMMLPRemove
    push(uid, { sort_key, cond_idx, type, 0xFF, amount0, amount1 })
```

### scan_convert

```sql
SELECT block_number, log_index, stakeholder, market_id, index_set, amount
FROM convert ORDER BY block_number, log_index
```

```
for row in results:
    market_id = lowercase(market_id)
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(stakeholder)

    // Convert: M 个 NO → (M-1) USDC
    // index_set 是 bitmap，表示哪些 question 的 NO 被转换
    // 每个 bit 对应一个 question_index

    // 遍历 index_set 的每个 bit，为每个涉及的 condition 生成事件
    for bit_idx in 0..64:
        if not ((index_set >> bit_idx) & 1): continue

        key = (market_id, bit_idx)
        if key not in neg_risk_map_: continue

        cond_idx = neg_risk_map_[key]
        // NO token 被销毁
        push(uid, { sort_key, cond_idx, Convert, 1, amount, index_set })

    // 总收益 = (popcount - 1) * amount，记录在第一个 condition
    // 或者在 replay 时统一计算
```

### scan_transfer

```sql
SELECT block_number, log_index, from_addr, to_addr, token_id, amount
FROM transfer ORDER BY block_number, log_index
```

```
for row in results:
    token_id = lowercase(token_id)
    if token_id not in token_map_: continue

    info = token_map_[token_id]
    sort_key = block_number * 1e9 + log_index
    token_idx = info.is_yes ? 0 : 1

    from_uid = intern_user(from_addr)
    to_uid = intern_user(to_addr)

    push(from_uid, { sort_key, info.cond_idx, TransferOut, token_idx, amount, 0 })
    push(to_uid,   { sort_key, info.cond_idx, TransferIn,  token_idx, amount, 0 })
```

## Phase 3: replay_all

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

所有操作只做简单的 +/- 仓位，不做任何持仓检查。负持仓说明有 bug。

```
Buy / FPMMBuy:
    i = token_idx
    cost[i] += amount * price / 1e6
    positions[i] += amount

Sell / FPMMSell:
    i = token_idx
    cost_removed = cost[i] * amount / positions[i]
    realized_pnl += amount * price / 1e6 - cost_removed
    cost[i] -= cost_removed
    positions[i] -= amount

Split:
    // USDC → YES + NO (1 USDC → 1 YES + 1 NO)
    for i in 0..outcome_count:
        cost[i] += amount / outcome_count
        positions[i] += amount

Merge:
    // YES + NO → USDC (1 YES + 1 NO → 1 USDC)
    for i in 0..outcome_count:
        cost_removed = cost[i] * amount / positions[i]
        realized_pnl += amount / outcome_count - cost_removed
        cost[i] -= cost_removed
        positions[i] -= amount

Redemption:
    // token → USDC (按 payout 结算，payout 是实际获得的 USDC)
    // index_sets: bitmap 表示赎回哪些 outcome
    // payout: 实际获得的 USDC 总额
    index_sets = token_idx
    
    // 从 payout 反算赎回数量
    // binary market: payout_numerators = [1,0] 或 [0,1] 或 [1,1]
    // redeemed_amount = payout / sum(payout_numerators[i] for i in index_sets)
    payout_sum = 0
    for i in 0..outcome_count:
        if (index_sets >> i) & 1:
            payout_sum += payout_numerators[i]
    
    if payout_sum > 0:
        redeemed = payout / payout_sum
    else:
        // 全输，从 positions 推断（所有涉及的 position 应该相等）
        redeemed = positions[first_set_bit(index_sets)]
    
    for i in 0..outcome_count:
        if not ((index_sets >> i) & 1): continue
        
        cost_removed = cost[i] * redeemed / positions[i]
        payout_price = payout_numerators[i]
        realized_pnl += redeemed * payout_price - cost_removed
        cost[i] -= cost_removed
        positions[i] -= redeemed

FPMMLPAdd:
    // LP 添加流动性: 投入 USDC → 获得 YES + NO
    // 实际 USDC 投入 = max(amount0, amount1)，因为 Split 是 1:1
    amount0 = amount   // YES 数量
    amount1 = price    // NO 数量
    usdc_spent = max(amount0, amount1)
    total = amount0 + amount1
    
    cost[0] += usdc_spent * amount0 / total
    cost[1] += usdc_spent * amount1 / total
    positions[0] += amount0
    positions[1] += amount1

FPMMLPRemove:
    // LP 移除流动性: 取回 YES + NO → 部分 Merge 成 USDC
    // 实际 USDC 取回 = min(amount0, amount1)
    amount0 = amount   // YES 数量
    amount1 = price    // NO 数量
    usdc_received = min(amount0, amount1)
    total = amount0 + amount1
    
    cost_removed0 = cost[0] * amount0 / positions[0]
    cost_removed1 = cost[1] * amount1 / positions[1]
    realized_pnl += usdc_received * amount0 / total - cost_removed0
    realized_pnl += usdc_received * amount1 / total - cost_removed1
    cost[0] -= cost_removed0
    cost[1] -= cost_removed1
    positions[0] -= amount0
    positions[1] -= amount1

Convert:
    // NegRisk: M 个 NO → (M-1) USDC
    // token_idx=1 (NO), amount 是数量, price 存 index_set
    index_set = price
    popcount = bitcount(index_set)

    // NO token 被销毁
    cost_removed = cost[1] * amount / positions[1]
    cost[1] -= cost_removed
    positions[1] -= amount

    // 收益分摊到每个 condition: (popcount - 1) / popcount * amount
    realized_pnl += (popcount - 1) * amount / popcount - cost_removed

TransferIn:
    // 0 成本获得 token
    positions[token_idx] += amount

TransferOut:
    // 转出 token，按比例移除成本，不产生 realized_pnl
    cost_removed = cost[token_idx] * amount / positions[token_idx]
    cost[token_idx] -= cost_removed
    positions[token_idx] -= amount
```
