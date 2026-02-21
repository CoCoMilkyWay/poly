# Stage 3: PnL 回放计算

从 user_event 表回放计算每个用户的 PnL。可全量预计算，也可按需查询。

## 用户状态

```
ReplayState (回放中间态):
    positions[8]: i64   每个 outcome 的持仓
    cost[8]:      i64   每个 outcome 的成本
    realized_pnl: i64   已实现盈亏

Snapshot (快照):
    sort_key, delta, price, positions[8], cost_basis, realized_pnl, event_type, token_idx, outcome_count
```

## 查询流程

```
def query_user(user_addr: bytes20) -> map<cond_idx, vec<Snapshot>>:
    // 1. 从 user_event 表拉取该用户的所有事件
    events = SELECT sort_key, cond_idx, event_type, token_idx, amount, price
             FROM user_event
             WHERE user_addr = :user_addr
             ORDER BY sort_key

    if events.empty(): return {}

    // 2. 回放事件
    states: map<cond_idx, ReplayState>
    snaps:  map<cond_idx, vec<Snapshot>>

    for evt in events:
        st = &states[evt.cond_idx]
        cond = &conditions_[evt.cond_idx]

        apply_event(evt, st, cond)

        snap = Snapshot {
            sort_key:     evt.sort_key,
            delta:        evt.amount,
            price:        evt.price,
            event_type:   evt.event_type,
            token_idx:    evt.token_idx,
            outcome_count: cond.outcome_count,
            positions:    copy(st.positions),
            cost_basis:   sum(st.cost),
            realized_pnl: st.realized_pnl,
        }
        snaps[evt.cond_idx].push(snap)

    return snaps
```

## 查询前需要加载的数据

```
// conditions_ 需要预加载（从 rb_condition 表）
// 或者按需加载：
def ensure_condition_loaded(cond_idx):
    if cond_idx not in conditions_:
        row = SELECT outcome_count, payout_numerators FROM rb_condition WHERE cond_idx = :cond_idx
        conditions_[cond_idx] = ConditionInfo { row.outcome_count, parse_or_none(row.payout_numerators) }
```

## apply_event

**核心原则**：所有操作只做简单的 +/- 仓位，不做任何持仓检查。负持仓说明有 bug。

---

### Buy / FPMMBuy

**步骤**:

1. 计算成本 = amount \* price / 1e6
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

### Sell / FPMMSell

**步骤**:

1. 计算按比例移除的成本 = cost[i] \* amount / positions[i]
2. 计算卖出收入 = amount \* price / 1e6
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

### Split

**步骤**:

1. 计算成本 = amount \* price / 1e6 (price = 500000 = $0.50)
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

### Merge

**步骤**:

1. 计算按比例移除的成本
2. 计算收入 = amount \* price / 1e6 (price = 500000 = $0.50)
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

### Redemption

**步骤**:

1. 解析 index_sets bitmap
2. 对每个涉及的 outcome：
   - realized_pnl += 持仓 \* payout_price - 成本
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

### FPMMLPAdd

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

### FPMMLPRemove

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

### Convert

**步骤**:

1. 解析 index_set，计算 popcount
2. 按比例移除 NO 成本
3. 减少 NO 持仓
4. 计算分摊收益 = (popcount-1)/popcount \* amount

**注意**:

- **仅限 NegRisk**：M 个 NO → (M-1) USDC
- 每个涉及的 condition 都会收到一个 Convert 事件
- **收益分摊**：总收益 (M-1)\*amount 平均分到 M 个 condition
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

### TransferIn

**步骤**:

1. 增加 positions

**注意**:

- **0 成本获得 token**：可能是赠与、空投、从其他账户转入
- 不增加 cost（成本为 0）

```
positions[token_idx] += amount
```

---

### TransferOut

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
