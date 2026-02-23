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
| FPMM.removeFunding       | safeBatchTransferFrom (NOT burn!)         | FPMMFundingRemoved | 同一函数 |
| NegRisk.convertPositions | splitPosition + safeBatchTransferFrom × 3 | PositionsConverted | 同一函数 |

**边界情况**：用户直接调用 `safeTransferFrom` 只有 Transfer、无语义事件 → 分类为 TransferIn/Out

### 设计原则与 Assert 验证

| 原则                                | 保证机制             | Assert 验证                                    |
| ----------------------------------- | -------------------- | ---------------------------------------------- |
| 1. 任何 token 流水都被包括          | Transfer 是唯一来源  | `amount >= 0`, `cond_idx < conditions_.size()` |
| 2. 任何 token 流水不被 double count | 分类决策树互斥       | 每个分支 return，无 fallthrough                |
| 3. token 流水被精确还原             | 语义事件提供精确价格 | `price ∈ [0, 1e6]`, `tokens > 0`               |
| 4. 事件只记录给用户                 | 协议合约地址不记录   | `!is_protocol_contract(user)`                  |
| 5. from/to 不同                     | Transfer 语义保证    | `from != to`                                   |

### 关键 Assert 验证 (基于合约代码分析)

| 操作          | Assert 验证                                | 合约依据                                      |
| ------------- | ------------------------------------------ | --------------------------------------------- |
| LP Add mint   | `amount == max(amount0, amount1)`          | split 生成 addedFunds 份 YES+NO               |
| LP Remove     | `amount == amountsRemoved[token_idx]`      | FPMMFundingRemoved.amounts = transfer 量      |
| NegRisk Split | `amount == info.amount`                    | Adapter 转给用户的量 = PositionSplit.amount   |
| NegRisk Merge | `amount == info.amount`                    | 用户转给 Adapter 的量 = PositionsMerge.amount |
| Convert NO    | `token_idx == 1`, `amount == info.amount`  | 只有 NO token 发到 BurnAddr                   |
| Convert YES   | `amount <= info.amount`                    | 扣手续费后可能小于 split 量                   |
| Order         | `amount == tokens`                         | 1 个 OrderFilled = 1 次 transfer              |
| FPMM Trade    | `amount == tokens`, `outcome == token_idx` | FPMMBuy/Sell 精确记录                         |

## 执行流程

```
rebuild(target_block):
    Phase 0: rb_* → 内存映射 (增量恢复，首次跳过)

    固定 chunk 循环 (cursor → target_block):
        Phase 1: 更新映射表 (依赖顺序: 先注册 condition/token，后建立关联)
                 ① condition_preparation → cond_map_, conditions_
                 ② condition_resolution → conditions_.payout
                 ③ token_map           → token_map_ (需 condition 存在)
                 ④ fpmm                → fpmm_map_, token_map_ (需 condition 存在，计算 token_id)
                 ⑤ neg_risk_question   → cond_to_market_ (建立 question_id → market_id)

        Phase 2: 构建语义索引 (7 表，仅内存)
                 split/merge/redemption/convert/order_filled/fpmm_trade/fpmm_funding
                 → tx_split_, tx_merge_, tx_redemption_, tx_convert_, tx_order_, tx_fpmm_trade_, tx_fpmm_funding_

        Phase 3: 处理 Transfer (按 block + log_index 排序)
                 → 分类决策树 + 关联语义索引 → user_event

        Commit: 单事务写入 (rb_* + user_event) + UPDATE cursor

崩溃恢复: 从 cursor 断点重做，语义索引不持久化（同 tx 保证）
```

**依赖关系保证**:

- Stage1 扫链保证: condition_preparation < 使用该 condition 的任何事件
- Phase 1 顺序保证: ①②③④⑤ 严格顺序，后续步骤可依赖前序结果
- 同 tx 保证: 语义事件和 Transfer 在同一 tx，必在同一 chunk

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
tx_split_[(block, tx_hash, cond_id)]       → vector<{amount, stakeholder}>
tx_merge_[(block, tx_hash, cond_id)]       → vector<{amount, stakeholder}>
tx_redemption_[(block, tx_hash, cond_id)]  → vector<{payout, redeemer}>
tx_convert_[(block, tx_hash, market_id)]   → vector<{index_set, amount, stakeholder}>
tx_order_[(block, tx_hash, token_id)]      → {maker, taker, side, amounts, fee}
tx_fpmm_trade_[(block, tx_hash, fpmm)]     → {trader, side, outcome_index, amounts}
tx_fpmm_funding_[(block, tx_hash, fpmm)]   → {funder, side, amounts[]}
```

**关键设计**:

- Split/Merge/Redemption 用 `cond_id` 作 key，vector 支持同 tx 同 condition 多次操作
- Convert 用 `market_id` 作 key，通过 condition 的 question_id 查找对应 market
- Order 用 `token_id` 作 key，同 tx 可能有多个 order 针对不同 token
- FPMM trade/funding 用 `fpmm_addr` 作 key，同 tx 可能和多个 FPMM 交互

**为什么不持久化**: 语义事件和 Transfer 在同一个 tx，增量只需索引 chunk 范围即可关联。

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

| 操作    | 普通市场                        | NegRisk 市场                                          |
| ------- | ------------------------------- | ----------------------------------------------------- |
| Split   | 用户直接 mint，stakeholder=用户 | Adapter mint 后 transfer 给用户，stakeholder=Adapter  |
| Merge   | 用户直接 burn，stakeholder=用户 | 用户 transfer 给 Adapter 后 Adapter burn              |
| Convert | N/A                             | 用户 NO → BurnAddr，Adapter split YES+NO → 用户收 YES |

### FPMM LP 操作的关键区别

| 操作      | Transfer 类型                           | 说明                                |
| --------- | --------------------------------------- | ----------------------------------- |
| LP Add    | mint (from=0x0, to=FPMM) + transfer返还 | splitPosition 后返还多余 token      |
| LP Remove | transfer (from=FPMM, to=user)           | **不是 burn！** 直接转给用户        |
| Buy       | mint + transfer                         | splitPosition 后转 outcome 给用户   |
| Sell      | transfer + burn                         | 用户转入后 FPMM 内部 merge (有burn) |

### 分类决策树

```
Transfer(operator, from, to, token_id, amount)
    │
    ├─ from == 0x0 (mint)
    │   │
    │   ├─ to == NEG_RISK_ADAPTER → 跳过 (NegRisk 内部 mint)
    │   │
    │   ├─ to in fpmm_map_ (FPMM 内部 mint，必须先检查！)
    │   │   ├─ tx_fpmm_funding_ 存在 且 side=Added → FPMMLPAdd: user=funder
    │   │   └─ 否则 → 跳过 (FPMM 内部 split for buy)
    │   │
    │   ├─ tx_split_ 存在 且 stakeholder == to
    │   │   └─ Split: user=to (普通市场用户直接 split)
    │   │
    │   └─ 否则 → 跳过
    │
    ├─ to == 0x0 (burn)
    │   │
    │   ├─ from == NEG_RISK_ADAPTER → 跳过 (NegRisk 内部 burn)
    │   │
    │   ├─ from in fpmm_map_ → 跳过 (FPMM 内部 merge for sell)
    │   │   # 注意: LP Remove 不是 burn！是 FPMM→user 的 transfer
    │   │
    │   ├─ tx_merge_ 存在 且 stakeholder == from
    │   │   └─ Merge: user=from (普通市场用户直接 merge)
    │   │
    │   ├─ tx_redemption_ 存在 且 redeemer == from
    │   │   └─ Redemption: user=from, price=payout_numerator
    │   │
    │   └─ 否则 → 跳过
    │
    ├─ operator == CTF_EXCHANGE / NEG_RISK_CTF_EXCHANGE
    │   ├─ tx_order_ 存在 → Buy(to) + Sell(from)
    │   └─ 否则 → TransferIn(to) + TransferOut(from)
    │
    ├─ operator == NEG_RISK_ADAPTER
    │   │
    │   ├─ from == NEG_RISK_ADAPTER (Adapter → 用户)
    │   │   ├─ tx_split_ 存在 且 stakeholder == Adapter → Split: user=to
    │   │   │   # 包括 Convert 时输出的 YES token (可能有手续费扣减)
    │   │   └─ 否则 → TransferIn: user=to
    │   │
    │   ├─ to == NEG_RISK_ADAPTER (用户 → Adapter)
    │   │   ├─ tx_merge_ 存在 且 stakeholder == Adapter → Merge: user=from
    │   │   └─ 否则 → TransferOut: user=from
    │   │
    │   ├─ to == NO_TOKEN_BURN_ADDRESS (Convert 专用销毁)
    │   │   ├─ from == NEG_RISK_ADAPTER → 跳过 (Adapter 内部 NO burn)
    │   │   ├─ tx_convert_ 存在 且 stakeholder == from → Convert: user=from
    │   │   └─ 否则 → 错误 (不应发生)
    │   │
    │   └─ 否则 → 跳过 (Adapter 内部)
    │
    ├─ operator in fpmm_map_
    │   │
    │   ├─ tx_fpmm_trade_ 存在 → FPMMBuy/FPMMSell: user=trader
    │   │
    │   ├─ tx_fpmm_funding_ 存在 且 from == fpmm (FPMM → 用户)
    │   │   ├─ side == Removed → FPMMLPRemove: user=to (LP 撤出！)
    │   │   └─ side == Added   → 跳过 (LP Add 时返还多余 token)
    │   │
    │   └─ 否则 → 跳过 (FPMM 内部)
    │
    └─ 其他 → TransferIn(to) + TransferOut(from)
```

### 已知合约地址

```
ZERO_ADDR            = 0x0000000000000000000000000000000000000000
CTF_EXCHANGE         = 0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e
NEG_RISK_CTF_EXCHANGE= 0xc5d563a36ae78145c45a50134d48a1215220f80a
NEG_RISK_ADAPTER     = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296
CONDITIONAL_TOKENS   = 0x4d97dcd97ec945f40cf65f87097ace5ea0476045
NO_TOKEN_BURN_ADDRESS= 0x36a0e974a7083ea0ad4dea6a27b90fab22e93a32  # Convert 专用销毁地址
```

**注意**: `NO_TOKEN_BURN_ADDRESS` 是 NegRisk Convert 操作中销毁 NO token 的专用地址，
由 `keccak256("NO_TOKEN_BURN_ADDRESS")` 的前 20 字节确定，不是零地址！

### 关键处理逻辑

**避免 double count**:

一个事件一个 token，每条 Transfer 自然对应一个 user_event，无需额外去重。

**跳过内部操作** (确保不会给协议合约地址记录事件):

| 跳过条件                          | 原因                                        |
| --------------------------------- | ------------------------------------------- |
| mint && to == NEG_RISK_ADAPTER    | NegRisk 内部 mint，用户通过 transfer 获取   |
| burn && from == NEG_RISK_ADAPTER  | NegRisk 内部 burn，用户已通过 transfer 记录 |
| mint && stakeholder != to (FPMM)  | FPMM 内部 split (stakeholder=FPMM)          |
| burn && from in fpmm*map*         | FPMM sell 时的内部 merge                    |
| to == BurnAddr && from == Adapter | Convert 时 Adapter 的内部 NO burn           |
| operator=FPMM && LP Add 返还      | LP Add 返还多余 token，不影响 LP 头寸       |

**用户识别**:

| 场景          | 用户来源                    | 说明                           |
| ------------- | --------------------------- | ------------------------------ |
| 普通 Split    | Transfer.to (mint)          | stakeholder == to              |
| NegRisk Split | Transfer.to (from=Adapter)  | stakeholder == Adapter         |
| 普通 Merge    | Transfer.from (burn)        | stakeholder == from            |
| NegRisk Merge | Transfer.from (to=Adapter)  | stakeholder == Adapter         |
| Convert NO    | Transfer.from (to=BurnAddr) | stakeholder == from (用户地址) |
| Convert YES   | Transfer.to (from=Adapter)  | 通过 Split 记录 (可能扣手续费) |
| Redemption    | Transfer.from (burn)        | redeemer == from               |
| OrderFilled   | Transfer.from/to            | 不用 maker/taker，避免覆盖问题 |
| FPMMTrade     | tx*fpmm_trade*.trader       |                                |
| FPMMLPAdd     | tx*fpmm_funding*.funder     | mint 时 to=FPMM，不是用户      |
| FPMMLPRemove  | Transfer.to (from=FPMM)     | transfer 不是 burn！user=to    |

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
