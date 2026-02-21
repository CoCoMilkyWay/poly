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
│                                           ▲                     │
│  fpmm ──────────► (join cond_map_) ─► fpmm_map_                 │
│       └──────────► (keccak256)  ──────────┘ (补全FPMM token)    │
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

**步骤**:
1. 查 token_map_ 获取 cond_idx 和 is_yes
2. 计算 price = usdc_amount * 1e6 / token_amount
3. 根据 side 判断 maker/taker 的买卖方向
4. 为 maker 和 taker 各生成一个事件

**注意**:
- side=1 表示 maker 买入（maker 出 USDC 换 token）
- side=2 表示 maker 卖出（maker 出 token 换 USDC）
- taker 方向与 maker 相反
- price 单位是 1e6 = $1（与 token 数量单位一致）

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

**步骤**:
1. 过滤 NegRiskAdapter 地址
2. 查 cond_map_ 获取 cond_idx
3. 生成 Split 事件，token_idx=0xFF 表示双边

**注意**:
- **必须过滤 NegRiskAdapter**：NegRisk 用户通过 Adapter 操作，Split 的 stakeholder 是 Adapter 而非用户。用户的实际 token 流水已在 order_filled 中记录
- amount 是消耗的 USDC 数量，同时获得等量 YES 和 NO
- token_idx=0xFF 表示操作涉及所有 outcome（YES+NO）

```sql
SELECT block_number, log_index, stakeholder, condition_id, amount
FROM split ORDER BY block_number, log_index
```

```
NEGRISK_ADAPTER = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296

for row in results:
    if stakeholder == NEGRISK_ADAPTER: continue

    cond_id = lowercase(condition_id)
    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(stakeholder)

    push(uid, { sort_key, cond_idx, Split, 0xFF, amount, 0 })
```

### scan_merge

**步骤**:
1. 过滤 NegRiskAdapter 地址
2. 查 cond_map_ 获取 cond_idx
3. 生成 Merge 事件，token_idx=0xFF 表示双边

**注意**:
- **必须过滤 NegRiskAdapter**：同 scan_split
- amount 是销毁的 YES/NO 数量（各 amount 个），获得等量 USDC
- Merge 是 Split 的逆操作

```sql
SELECT block_number, log_index, stakeholder, condition_id, amount
FROM merge ORDER BY block_number, log_index
```

```
NEGRISK_ADAPTER = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296

for row in results:
    if stakeholder == NEGRISK_ADAPTER: continue

    cond_id = lowercase(condition_id)
    if cond_id not in cond_map_: continue

    cond_idx = cond_map_[cond_id]
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(stakeholder)

    push(uid, { sort_key, cond_idx, Merge, 0xFF, amount, 0 })
```

### scan_redemption

**步骤**:
1. 查 cond_map_ 获取 cond_idx
2. 生成 Redemption 事件，token_idx 存 index_sets，amount 存 payout

**注意**:
- index_sets 是 bitmap：1=YES, 2=NO, 3=两者都赎回
- payout 是获得的 USDC 总额（可能为 0，表示赎回的是输家 token）
- **apply_event 中假设全量赎回**：清空涉及的 positions
- 市场必须已结算（payout_numerators 非空）才能赎回

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

**步骤**:
1. 查 fpmm_map_ 获取 cond_idx
2. 计算 price = usdc_amount * 1e6 / token_amount
3. 根据 side 判断 Buy/Sell，生成 FPMMBuy 或 FPMMSell 事件

**注意**:
- FPMM 是 AMM 时代的交易（已废弃，但历史数据仍需处理）
- side=1 表示 Buy（用户投入 USDC 获得 token）
- side=2 表示 Sell（用户卖出 token 获得 USDC）
- outcome_index: 0=YES, 1=NO
- 与 order_filled 不同，FPMM 只有 taker（用户），没有 maker

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

**步骤**:
1. 查 fpmm_map_ 获取 cond_idx
2. 根据 side 判断 Add/Remove，生成 FPMMLPAdd 或 FPMMLPRemove 事件
3. amount 存 YES 数量，price 存 NO 数量

**注意**:
- LP 添加/移除流动性，涉及双边 token（YES+NO）
- side=1 表示 FundingAdded（LP 投入 USDC，获得 YES+NO）
- side=2 表示 FundingRemoved（LP 取回 YES+NO）
- amount0/amount1 是按池子当前比例添加/取回的 YES/NO 数量
- **成本计算**：Add 时 usdc_spent = max(amount0, amount1)；Remove 时 usdc_received = min(amount0, amount1)

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

**步骤**:
1. 遍历 index_set 的每个 bit，找到涉及的 question_index
2. 查 neg_risk_map_ 获取每个 question 对应的 cond_idx
3. 为每个涉及的 condition 生成一个 Convert 事件

**注意**:
- **仅限 NegRisk 市场**：Convert 操作是 NegRisk 多选项互斥市场的套利机制
- **原理**：M 个互斥选项的 NO 组合 = "所有选项都不赢" = 不可能，所以 M 个 NO 可以换 (M-1) USDC
- index_set 是 bitmap：bit N = 1 表示 question_index=N 的 NO 参与转换
- amount 是每个 NO 的数量（各 amount 个）
- **收益分摊**：每个 condition 记录 (popcount-1)/popcount * amount 的收益
- token_idx=1 表示 NO token，price 存 index_set 供 apply_event 计算 popcount

```sql
SELECT block_number, log_index, stakeholder, market_id, index_set, amount
FROM convert ORDER BY block_number, log_index
```

```
for row in results:
    market_id = lowercase(market_id)
    sort_key = block_number * 1e9 + log_index
    uid = intern_user(stakeholder)

    // 遍历 index_set 的每个 bit，为每个涉及的 condition 生成事件
    for bit_idx in 0..64:
        if not ((index_set >> bit_idx) & 1): continue

        key = (market_id, bit_idx)
        if key not in neg_risk_map_: continue

        cond_idx = neg_risk_map_[key]
        push(uid, { sort_key, cond_idx, Convert, 1, amount, index_set })
```

### scan_transfer

**步骤**:
1. 查 token_map_ 获取 cond_idx 和 is_yes
2. 为 from_addr 生成 TransferOut 事件
3. 为 to_addr 生成 TransferIn 事件

**注意**:
- transfer 表已在 Stage1 过滤掉：
  - mint (from=0x0) 和 burn (to=0x0)
  - operator 是 CTFExchange/NegRiskCTFExchange/NegRiskAdapter 的记录
  - from/to 是 FPMM 地址的记录
- 剩下的主要是用户间直接转账
- **TransferIn 是 0 成本获得 token**（可能是赠与、空投等）
- **TransferOut 不产生 realized_pnl**（只是成本转移）
- 如果 token_id 不在 token_map_ 中，会被 skip（FPMM-only 市场已在 Phase 1.3 补全）

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
1. 为每个 outcome 增加成本 = amount / outcome_count
2. 为每个 outcome 增加持仓 = amount

**注意**:
- 1 USDC → 1 YES + 1 NO（binary market）
- 成本平均分配到 YES 和 NO（各 0.5 USDC）
- amount 是消耗的 USDC，也是获得的每种 token 数量

```
for i in 0..outcome_count:
    cost[i] += amount / outcome_count
    positions[i] += amount
```

---

#### Merge

**步骤**:
1. 为每个 outcome 计算按比例移除的成本
2. 计算每个 outcome 的收入 = amount / outcome_count
3. realized_pnl += 收入 - 成本
4. 减少 cost 和 positions

**注意**:
- 1 YES + 1 NO → 1 USDC（binary market）
- 是 Split 的逆操作
- **假设各 outcome 持仓相等**：Merge 需要等量的 YES 和 NO

```
for i in 0..outcome_count:
    cost_removed = cost[i] * amount / positions[i]
    realized_pnl += amount / outcome_count - cost_removed
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
2. 按 token 比例分配成本
3. 增加 positions

**注意**:
- LP 投入 USDC → Split 成 YES+NO → 按池子比例添加 → 多余 token 返还
- **usdc_spent = max(amount0, amount1)**：因为 Split 是 1:1，需要 Split 足够的 USDC 来获得较多的那一边
- 成本按实际获得的 token 比例分配

```
amount0 = amount   // YES 数量
amount1 = price    // NO 数量
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
1. 计算实际 USDC 取回 = min(amount0, amount1)
2. 计算按比例移除的成本
3. 计算按比例分配的收入
4. realized_pnl += 收入 - 成本
5. 减少 cost 和 positions

**注意**:
- LP 取回 YES+NO → 较少的那边可以和对方 Merge 成 USDC
- **usdc_received = min(amount0, amount1)**：只有 min 部分可以 Merge
- 多余的 token 留在用户手中（已计入 positions）

```
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
