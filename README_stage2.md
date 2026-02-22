# Stage 2: 构建映射 + 写 user_event

## 关键假设 (已验证)

### 假设 1: Transfer 是持仓变化的唯一来源 ✅

ERC1155 规范强制要求：**任何 `_balances` 修改都必须 emit Transfer**

```
ERC1155.sol 中所有修改余额的地方：
  safeTransferFrom   → emit TransferSingle
  safeBatchTransferFrom → emit TransferBatch
  _mint / _batchMint → emit TransferSingle / TransferBatch (from=0x0)
  _burn / _batchBurn → emit TransferSingle / TransferBatch (to=0x0)
```

### 假设 2: 语义事件和 Transfer 在同一个 tx ✅

所有协议操作都是**原子的**，在同一个函数调用中完成 Transfer 和 emit 语义事件：

| 操作                     | Transfer                                  | 语义事件           | 来源     |
| ------------------------ | ----------------------------------------- | ------------------ | -------- |
| CTF.splitPosition        | `_batchMint` → TransferBatch              | PositionSplit      | 同一函数 |
| CTF.mergePositions       | `_batchBurn` → TransferBatch              | PositionsMerge     | 同一函数 |
| CTF.redeemPositions      | `_burn` → TransferSingle                  | PayoutRedemption   | 同一函数 |
| Exchange.fillOrder       | `safeTransferFrom` × 2                    | OrderFilled        | 同一函数 |
| FPMM.buy                 | splitPosition + safeTransferFrom          | FPMMBuy            | 同一函数 |
| FPMM.sell                | safeTransferFrom + mergePositions         | FPMMSell           | 同一函数 |
| FPMM.addFunding          | splitPosition + safeBatchTransferFrom     | FPMMFundingAdded   | 同一函数 |
| NegRisk.convertPositions | splitPosition + safeBatchTransferFrom × 3 | PositionsConverted | 同一函数 |

**边界情况**：用户直接调用 `safeTransferFrom` 只有 Transfer、无语义事件 → 分类为 TransferIn/Out

### 设计原则

```
1. 任何 token 流水都被包括 (Transfer 是唯一来源保证)
2. 任何 token 流水不被 double count (分类决策树保证)
3. token 流水被精确还原，不含近似假设 (语义事件提供精确价格)
```

## 执行流程

```
rebuild(target_block):
    Phase 0: rb_* → 内存映射 (增量模式, 首次跳过)

    固定 chunk 循环 (cursor → target_block):
        Phase 1: chunk 内 Stage1 表 → 更新内存映射 + rb_* 数据
                 ├─ 先处理 condition_preparation
                 └─ 并行: condition_resolution / token_map / fpmm
        Phase 2: chunk 内 7 表 → 内存语义索引
        Phase 3: chunk 内 transfer → 分类 + 关联语义 → user_event 数据
        └─ 单事务: batch_insert(rb_* + user_event) + UPDATE cursor
```

单 chunk = Phase 1→2→3 完整流程 + 单事务写入，崩溃从 cursor 断点重做

## 数据结构

### 映射表 (持久化 rb\_\* + 内存)

| 映射                                                     | 来源                               | 用途               |
| -------------------------------------------------------- | ---------------------------------- | ------------------ |
| `cond_map_[condition_id] → cond_idx`                     | condition_preparation              | 32字节→4字节压缩   |
| `conditions_[cond_idx] → {outcome_count, payout}`        | condition_preparation + resolution | 结算状态           |
| `token_map_[token_id] → {cond_idx, is_yes}`              | token_map + fpmm计算               | Transfer 归属      |
| `fpmm_map_[fpmm_addr] → {cond_idx, token_yes, token_no}` | fpmm                               | FPMM operator 识别 |
**关键洞察**：`token_map_` 有两个来源

1. TokenRegistered 事件直接提供
2. FPMMCreation 需要 keccak256 计算: `token_id = keccak256(collateral, keccak256(cond_id, index_set))`

### 语义索引 (仅内存, 不持久化)

```
tx_split_[(block, tx_hash, cond_id)]  → {amount, stakeholder}
tx_merge_[(block, tx_hash, cond_id)]  → {amount, stakeholder}
tx_redemption_[(block, tx_hash, cond_id)] → {index_sets, payout, redeemer}
tx_convert_[(block, tx_hash)]         → {market_id, index_set, amount, stakeholder}
tx_order_[(block, tx_hash, token_id)] → {maker, taker, side, amounts, fee}
tx_fpmm_trade_[(block, tx_hash)]      → {fpmm_addr, trader, side, outcome_index, amounts, fee}
tx_fpmm_funding_[(block, tx_hash)]    → {fpmm_addr, funder, side, amounts[]}
```

**注意**: Split/Merge/Redemption 用 `cond_id` 作 key，因为同一 tx 可能对多个 condition 操作 (套利机器人)

**为什么不持久化**: 语义事件和 Transfer 在同一个 tx，增量只需索引 new_range 即可关联。

### user_event 表

```sql
CREATE TABLE user_event (
    user_addr  BLOB(20),
    sort_key   INTEGER,   -- block_number * 1e9 + log_index
    cond_idx   INTEGER,
    event_type INTEGER,   -- EventType enum
    token_idx  INTEGER,   -- 见下方说明
    amount     INTEGER,   -- 6 decimals
    price      INTEGER,   -- 价格*1e6 或额外数据
    PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
);

EventType: Buy=0, Sell=1, Split=2, Merge=3, Redemption=4,
           FPMMBuy=5, FPMMSell=6, FPMMLPAdd=7, FPMMLPRemove=8,
           Convert=9, TransferIn=10, TransferOut=11
```

**设计原则: 一个事件一个 token**

所有事件都拆分为单 token，Stage3 统一处理：

```
apply_event(token_idx, amount, price):
    positions[token_idx] += delta
    cost[token_idx] += amount * price
```

| EventType        | token_idx | 说明                                            |
| ---------------- | --------- | ----------------------------------------------- |
| Buy/Sell         | 0..N      | 单 token 交易                                   |
| FPMMBuy/FPMMSell | 0..N      | 单 token 交易                                   |
| Split/Merge      | 0..N      | 每个 token 一条，price=1/outcome_count          |
| Redemption       | 0..N      | 每个被赎回的 token 一条，price=payout_numerator |
| FPMMLPAdd/Remove | 0..N      | 每个 token 一条，price=implied (按池子比例)     |
| Convert          | 0..N      | 每个被 burn 的 NO 一条，price=(M-1)/M           |
| TransferIn/Out   | 0..N      | 单 token 转账，price=0                          |

## Phase 3: Transfer 分类与处理

### 分类决策树

```
Transfer(operator, from, to, token_id, amount)
    │
    ├─ from == 0x0 (mint)
    │   │
    │   ├─ tx_split_ 存在 且 stakeholder == to
    │   │   └─ Split: user=to, 每条 Transfer 一个事件 (token_idx)
    │   │
    │   ├─ tx_fpmm_funding_ 存在 且 side=Added
    │   │   └─ FPMMLPAdd: user=funder, 每条 Transfer 一个事件
    │   │
    │   └─ 否则 → 跳过 (FPMM/NegRisk 内部 mint，不是用户操作)
    │
    ├─ to == 0x0 (burn)
    │   │
    │   ├─ tx_merge_ 存在 且 stakeholder == from
    │   │   └─ Merge: user=from, 每条 Transfer 一个事件 (token_idx)
    │   │
    │   ├─ tx_redemption_ 存在 且 redeemer == from
    │   │   └─ Redemption: user=from, 每条 burn 一个事件, price=payout_numerator
    │   │
    │   ├─ tx_convert_ 存在 且 stakeholder == from
    │   │   └─ Convert: user=from, 每条 NO burn 一个事件, price=(M-1)/M
    │   │
    │   ├─ tx_fpmm_funding_ 存在 且 side=Removed
    │   │   └─ FPMMLPRemove: user=funder, 每条 Transfer 一个事件
    │   │
    │   └─ 否则 → 跳过 (FPMM/NegRisk 内部 burn)
    │
    ├─ operator == CTF_EXCHANGE / NEG_RISK_CTF_EXCHANGE
    │   ├─ tx_order_ 存在
    │   │   └─ 生成两个事件: maker 的 Buy/Sell + taker 的 Sell/Buy
    │   └─ 否则 → TransferIn(to) + TransferOut(from)
    │
    ├─ operator == NEG_RISK_ADAPTER
    │   └─ 跳过 (Split/Merge/Convert 内部 transfer，已在 mint/burn 分支处理)
    │
    ├─ operator in fpmm_map_
    │   ├─ tx_fpmm_trade_ 存在 → FPMMBuy/FPMMSell: user=trader
    │   ├─ from == fpmm → TransferIn: user=to (LP 返还多余 token)
    │   └─ 否则 → 跳过 (FPMM 内部 transfer)
    │
    └─ 其他 → TransferIn(to) + TransferOut(from) (用户间直接转账)
```

### 已知合约地址

```
ZERO_ADDR            = 0x0000000000000000000000000000000000000000
CTF_EXCHANGE         = 0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e
NEG_RISK_CTF_EXCHANGE= 0xc5d563a36ae78145c45a50134d48a1215220f80a
NEG_RISK_ADAPTER     = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296
CONDITIONAL_TOKENS   = 0x4d97dcd97ec945f40cf65f87097ace5ea0476045
```

### 关键处理逻辑

**避免 double count**:

一个事件一个 token，每条 Transfer 自然对应一个 user_event，无需额外去重。

**跳过内部操作**:

- FPMM.buy/sell 内部的 Split/Merge: stakeholder=FPMM，不等于 Transfer.to/from
- NegRisk 内部的 Split: operator=NEG_RISK_ADAPTER 且 stakeholder != 用户
- FPMM 内部的 Transfer: operator=fpmm 且没有对应语义事件

**用户识别**:

- Split/Merge/Redemption: 从 Transfer.to (mint) 或 Transfer.from (burn)
- OrderFilled: 从 tx*order*.maker/taker
- FPMMTrade: 从 tx*fpmm_trade*.trader
- FPMMFunding: 从 tx*fpmm_funding*.funder (**不是 Transfer.to/from!**)
- Convert: 从 tx*convert*.stakeholder

**价格计算**:

- Split/Merge: `1 / outcome_count` (二元市场 = 0.5)
- Buy/Sell/FPMMBuy/FPMMSell: `usdc_amount / token_amount`
- Redemption: `payout_numerator` (赢家=1, 输家=0, 平局=0.5)
- FPMMLPAdd/Remove: 按池子当时的隐含价格
- Convert: `(M-1) / M`，M = popcount(index_set)
- TransferIn/Out: 0 (无价格信息)

## Stage 3 用户状态

```
ReplayState (回放中间态):
    positions[8]: i64   每个 outcome 的持仓
    cost[8]:      i64   每个 outcome 的成本
    realized_pnl: i64   已实现盈亏

Snapshot (快照):
    sort_key, delta, price, positions[8], cost_basis, realized_pnl, event_type, token_idx, outcome_count
```
