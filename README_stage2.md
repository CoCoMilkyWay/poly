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

## 运行模式

| 模式 | 条件                     | Phase 1A           | Phase 1B/2/3     |
| ---- | ------------------------ | ------------------ | ---------------- |
| 首次 | `rebuild_last_block` = 0 | 跳过 (rb\_\* 表空) | 全量处理         |
| 增量 | `rebuild_last_block` > 0 | 从 rb\_\* 加载映射 | 只处理 new_range |

## 执行流程

```
rebuild(target_block):
    last_block = sync_state['rebuild_last_block'] ?? 0
    new_range = (last_block, target_block]

    Phase 1A: rb_* → 内存映射 (首次跳过)
    Phase 1B: Stage1 表 + new_range → 更新内存映射 + 持久化到 rb_*
              ├─ 先处理 condition_preparation (其他都依赖 cond_map_)
              └─ 并行: condition_resolution / token_map / fpmm / neg_risk_question
    Phase 2:  并行扫 7 表 → 内存语义索引 (同 tx 关联, 无需持久化)
    Phase 3:  顺序扫 transfer → 分类 + 关联语义 → 批量写 user_event

    sync_state['rebuild_last_block'] = target_block
```

## 数据结构

### 映射表 (持久化 rb\_\* + 内存)

| 映射                                                     | 来源                               | 用途               |
| -------------------------------------------------------- | ---------------------------------- | ------------------ |
| `cond_map_[condition_id] → cond_idx`                     | condition_preparation              | 32字节→4字节压缩   |
| `conditions_[cond_idx] → {outcome_count, payout}`        | condition_preparation + resolution | 结算状态           |
| `token_map_[token_id] → {cond_idx, is_yes}`              | token_map + fpmm计算               | Transfer 归属      |
| `fpmm_map_[fpmm_addr] → {cond_idx, token_yes, token_no}` | fpmm                               | FPMM operator 识别 |
| `neg_risk_map_[(market_id, question_index)] → cond_idx`  | neg_risk_question                  | NegRisk Convert    |

**关键洞察**：`token_map_` 有两个来源

1. TokenRegistered 事件直接提供
2. FPMMCreation 需要 keccak256 计算: `token_id = keccak256(collateral, keccak256(cond_id, index_set))`

### 语义索引 (仅内存, 不持久化)

```
tx_split_[(block, tx_hash)]       → {condition_id, amount, stakeholder}
tx_merge_[(block, tx_hash)]       → {condition_id, amount, stakeholder}
tx_redemption_[(block, tx_hash)]  → {condition_id, index_sets, payout, redeemer}
tx_convert_[(block, tx_hash)]     → {market_id, index_set, amount, stakeholder}
tx_order_[(block, tx_hash, token_id)] → {maker, taker, side, amounts, fee}
tx_fpmm_trade_[(block, tx_hash)]  → {fpmm_addr, trader, side, outcome_index, amounts, fee}
tx_fpmm_funding_[(block, tx_hash)]→ {fpmm_addr, funder, side, amounts[]}
```

**为什么不持久化**：语义事件和 Transfer 在同一个 tx，增量只需索引 new_range 即可关联。

### user_event 表

```sql
CREATE TABLE user_event (
    user_addr  BLOB(20),
    sort_key   INTEGER,   -- block_number * 1e9 + log_index
    cond_idx   INTEGER,
    event_type INTEGER,   -- EventType enum
    token_idx  INTEGER,   -- 0=YES, 1=NO, 255=全部
    amount     INTEGER,   -- 6 decimals
    price      INTEGER,   -- 价格*1e6 或额外数据
    PRIMARY KEY (user_addr, sort_key, cond_idx, event_type)
);

EventType: Buy=0, Sell=1, Split=2, Merge=3, Redemption=4,
           FPMMBuy=5, FPMMSell=6, FPMMLPAdd=7, FPMMLPRemove=8,
           Convert=9, TransferIn=10, TransferOut=11
```

## Phase 3: Transfer 分类与处理

### 分类决策树

```
Transfer(operator, from, to, token_id, amount)
    │
    ├─ from == 0x0 (mint)
    │   ├─ tx_split_ 存在 → Split (price=0.5)
    │   ├─ tx_fpmm_funding_ 且 side=Added → FPMMLPAdd
    │   └─ 否则 → TransferIn (price=0, 未知来源)
    │
    ├─ to == 0x0 (burn)
    │   ├─ tx_merge_ 存在 → Merge (price=0.5)
    │   ├─ tx_redemption_ 存在 → Redemption (只处理 first index_set)
    │   ├─ tx_convert_ 存在 → Convert (只处理 NO token)
    │   ├─ tx_fpmm_funding_ 且 side=Removed → FPMMLPRemove
    │   └─ 否则 → TransferOut (price=0)
    │
    ├─ operator == CTF_EXCHANGE / NEG_RISK_CTF_EXCHANGE
    │   ├─ tx_order_ 存在 → Buy + Sell (price = usdc/token)
    │   └─ 否则 → direct transfer
    │
    ├─ operator in fpmm_map_
    │   ├─ tx_fpmm_trade_ 存在 → FPMMBuy / FPMMSell
    │   ├─ tx_fpmm_funding_ 存在
    │   │   ├─ from == fpmm → TransferIn (LP 返还多余)
    │   │   └─ to == fpmm → 跳过 (burn 时处理)
    │   └─ 否则 → direct transfer
    │
    └─ 其他 → TransferIn + TransferOut (用户间直接转账)
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

**避免 double count**：

- Split/Merge: YES/NO 两条 Transfer，两条都记录 (各自 token_idx)
- Redemption: 多条 burn Transfer，只在 first index_set 时记录一次
- Convert: 多个 NO burn，只在 token_idx=1 时记录一次
- FPMMLPAdd/Remove: 两条 Transfer，只在 token_idx=0 时记录一次

**价格计算**：

- Split/Merge: 固定 0.5 (1 YES + 1 NO = 1 USDC)
- Buy/Sell: `usdc_amount / token_amount`
- Convert: `price` 字段存 index_set，回放时用 popcount 计算收益

## Stage 3 用户状态

```
ReplayState (回放中间态):
    positions[8]: i64   每个 outcome 的持仓
    cost[8]:      i64   每个 outcome 的成本
    realized_pnl: i64   已实现盈亏

Snapshot (快照):
    sort_key, delta, price, positions[8], cost_basis, realized_pnl, event_type, token_idx, outcome_count
```
