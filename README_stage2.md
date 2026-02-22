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

### 设计原则与 Assert 验证

| 原则                                | 保证机制                  | Assert 验证                                |
| ----------------------------------- | ------------------------- | ------------------------------------------ |
| 1. 任何 token 流水都被包括          | Transfer 是唯一来源       | `amount > 0`, `cond_idx < conditions_.size()` |
| 2. 任何 token 流水不被 double count | 分类决策树互斥            | 每个分支 return，无 fallthrough            |
| 3. token 流水被精确还原             | 语义事件提供精确价格      | `price ∈ [0, 1e6]`, `tokens > 0`           |
| 4. 事件只记录给用户                 | 协议合约地址不记录        | `!is_protocol_contract(user)`              |
| 5. from/to 不同                     | Transfer 语义保证         | `from != to`                               |

## 执行流程

```
rebuild(target_block):
    Phase 0: rb_* → 内存映射 (增量模式, 首次跳过)

    固定 chunk 循环 (cursor → target_block):
        Phase 1: chunk 内 Stage1 表 → 更新内存映射 + rb_* 数据
                 ├─ 先处理 condition_preparation (含 question_id)
                 ├─ 并行: condition_resolution / token_map / fpmm
                 └─ neg_risk_question → cond_to_market_ 映射
        Phase 2: chunk 内 7 表 → 内存语义索引 (split/merge/redemption/convert 用 vector 存储多值)
        Phase 3: chunk 内 transfer → 分类 + 关联语义 → user_event 数据
        └─ 单事务: batch_insert(rb_* + user_event) + UPDATE cursor
```

单 chunk = Phase 1→2→3 完整流程 + 单事务写入，崩溃从 cursor 断点重做

## 数据结构

### 映射表 (持久化 rb\_\* + 内存)

| 映射                                                           | 来源                               | 用途                   |
| -------------------------------------------------------------- | ---------------------------------- | ---------------------- |
| `cond_map_[condition_id] → cond_idx`                           | condition_preparation              | 32字节→4字节压缩       |
| `conditions_[cond_idx] → {outcome_count, payout, question_id}` | condition_preparation + resolution | 结算状态 + NegRisk关联 |
| `token_map_[token_id] → {cond_idx, is_yes}`                    | token_map + fpmm计算               | Transfer 归属          |
| `fpmm_map_[fpmm_addr] → cond_idx`                              | fpmm                               | FPMM operator 识别     |
| `cond_to_market_[question_id] → market_id`                     | neg_risk_question                  | Convert 事件的市场查找 |

**关键洞察**：`token_map_` 有两个来源

1. TokenRegistered 事件直接提供 token0(YES) 和 token1(NO)
2. FPMMCreation 需要 keccak256 计算:
   - `collectionId = keccak256(parentCollectionId[32] + conditionId[32] + indexSet[32])`
   - `positionId = keccak256(collateralToken[20] + collectionId[32])`

### 语义索引 (仅内存, 不持久化)

```
tx_split_[(block, tx_hash, cond_id)]      → vector<{amount, stakeholder}>
tx_merge_[(block, tx_hash, cond_id)]      → vector<{amount, stakeholder}>
tx_redemption_[(block, tx_hash, cond_id)] → vector<{payout, redeemer}>
tx_convert_[(block, tx_hash, market_id)]  → vector<{index_set, amount, stakeholder}>
tx_order_[(block, tx_hash, token_id)]     → {maker, taker, side, amounts, fee}
tx_fpmm_trade_[(block, tx_hash)]          → {fpmm_addr, trader, side, outcome_index, amounts, fee}
tx_fpmm_funding_[(block, tx_hash)]        → {fpmm_addr, funder, side, amounts[]}
```

**关键设计**:

- Split/Merge/Redemption 用 `cond_id` 作 key，存储 vector 支持同一 tx 同一 condition 多次操作
- Convert 用 `market_id` 作 key，通过 condition 的 question_id 查找对应 market
- 遍历 vector 查找 stakeholder 匹配的事件，确保精确关联

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

### NegRisk vs 普通市场的关键区别

| 操作    | 普通市场                        | NegRisk 市场                                         |
| ------- | ------------------------------- | ---------------------------------------------------- |
| Split   | 用户直接 mint，stakeholder=用户 | Adapter mint 后 transfer 给用户，stakeholder=Adapter |
| Merge   | 用户直接 burn，stakeholder=用户 | 用户 transfer 给 Adapter 后 Adapter burn             |
| Convert | N/A                             | 用户 transfer 给 Adapter 后 Adapter burn             |

### 分类决策树

```
Transfer(operator, from, to, token_id, amount)
    │
    ├─ from == 0x0 (mint)
    │   │
    │   ├─ to == NEG_RISK_ADAPTER
    │   │   └─ 跳过 (NegRisk 内部 mint，用户通过后续 transfer 获取)
    │   │
    │   ├─ tx_split_ 存在 且 stakeholder == to
    │   │   └─ Split: user=to (普通市场用户直接 split)
    │   │
    │   ├─ tx_fpmm_funding_ 存在 且 side=Added
    │   │   └─ FPMMLPAdd: user=funder
    │   │
    │   └─ 否则 → 跳过 (FPMM 内部 mint)
    │
    ├─ to == 0x0 (burn)
    │   │
    │   ├─ from == NEG_RISK_ADAPTER
    │   │   └─ 跳过 (NegRisk 内部 burn，用户已通过之前的 transfer 记录)
    │   │
    │   ├─ tx_merge_ 存在 且 stakeholder == from
    │   │   └─ Merge: user=from (普通市场用户直接 merge)
    │   │
    │   ├─ tx_redemption_ 存在 且 redeemer == from
    │   │   └─ Redemption: user=from, price=payout_numerator
    │   │
    │   ├─ tx_fpmm_funding_ 存在 且 side=Removed
    │   │   └─ FPMMLPRemove: user=funder
    │   │
    │   └─ 否则 → 跳过 (FPMM 内部 burn)
    │
    ├─ operator == CTF_EXCHANGE / NEG_RISK_CTF_EXCHANGE
    │   │
    │   ├─ tx_order_ 存在
    │   │   └─ Buy(to) + Sell(from)，使用 transfer 的 from/to 而非 maker/taker
    │   │
    │   └─ 否则 → TransferIn(to) + TransferOut(from)
    │
    ├─ operator == NEG_RISK_ADAPTER
    │   │
    │   ├─ from == NEG_RISK_ADAPTER (Adapter → 用户)
    │   │   │
    │   │   ├─ tx_split_ 存在 且 stakeholder == NEG_RISK_ADAPTER
    │   │   │   └─ Split: user=to (NegRisk split，用户收到 token)
    │   │   │
    │   │   └─ 否则 → TransferIn: user=to
    │   │
    │   ├─ to == NEG_RISK_ADAPTER (用户 → Adapter)
    │   │   │
    │   │   ├─ tx_merge_ 存在 且 stakeholder == NEG_RISK_ADAPTER
    │   │   │   └─ Merge: user=from (NegRisk merge，用户转出 token)
    │   │   │
    │   │   ├─ tx_convert_ 存在 且 stakeholder == from
    │   │   │   └─ Convert: user=from, price=(M-1)/M
    │   │   │
    │   │   └─ 否则 → TransferOut: user=from
    │   │
    │   └─ 否则 → 跳过 (Adapter 内部 transfer)
    │
    ├─ operator in fpmm_map_
    │   │
    │   ├─ tx_fpmm_trade_ 存在 → FPMMBuy/FPMMSell: user=trader
    │   │
    │   ├─ from == fpmm → TransferIn: user=to (LP 返还多余 token)
    │   │
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

**跳过内部操作** (确保不会给协议合约地址记录事件):

| 跳过条件                         | 原因                                        |
| -------------------------------- | ------------------------------------------- |
| mint && to == NEG_RISK_ADAPTER   | NegRisk 内部 mint，用户通过 transfer 获取   |
| burn && from == NEG_RISK_ADAPTER | NegRisk 内部 burn，用户已通过 transfer 记录 |
| mint && stakeholder != to        | FPMM 内部 split (stakeholder=FPMM)          |
| burn && stakeholder != from      | FPMM 内部 merge (stakeholder=FPMM)          |
| operator=FPMM && 无语义事件      | FPMM 内部 transfer                          |

**用户识别**:

| 场景          | 用户来源                   | 说明                           |
| ------------- | -------------------------- | ------------------------------ |
| 普通 Split    | Transfer.to (mint)         | stakeholder == to              |
| NegRisk Split | Transfer.to (from=Adapter) | stakeholder == Adapter         |
| 普通 Merge    | Transfer.from (burn)       | stakeholder == from            |
| NegRisk Merge | Transfer.from (to=Adapter) | stakeholder == Adapter         |
| Convert       | Transfer.from (to=Adapter) | stakeholder == from (用户地址) |
| Redemption    | Transfer.from (burn)       | redeemer == from               |
| OrderFilled   | Transfer.from/to           | 不用 maker/taker，避免覆盖问题 |
| FPMMTrade     | tx*fpmm_trade*.trader      |                                |
| FPMMFunding   | tx*fpmm_funding*.funder    | 不是 Transfer.to/from          |

**价格计算**:

| 事件类型       | 价格公式                    | 单位       |
| -------------- | --------------------------- | ---------- |
| Split/Merge    | 1e6 / outcome_count         | 0.5 = 5e5  |
| Buy/Sell       | usdc_amount \* 1e6 / tokens | 1e6 = $1   |
| FPMMBuy/Sell   | usdc_amount \* 1e6 / tokens | 1e6 = $1   |
| Redemption     | payout_numerator \* 1e6     | 0/1e6      |
| Convert        | (M-1) \* 1e6 / M            | M=popcount |
| TransferIn/Out | 0                           | 无价格     |

## Stage 3 用户状态

```
ReplayState (回放中间态):
    positions[8]: i64   每个 outcome 的持仓
    cost[8]:      i64   每个 outcome 的成本
    realized_pnl: i64   已实现盈亏

Snapshot (快照):
    sort_key, delta, price, positions[8], cost_basis, realized_pnl, event_type, token_idx, outcome_count
```
