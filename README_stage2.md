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

## 架构

```
Stage2Sync (timer驱动, boost::asio)
  ├─ 检查 stage1_last_block vs stage2_cursor
  ├─ behind > 0 → build_chunk(cursor + chunk_size)
  │   ├─ Phase 1: phase1_update_mappings
  │   ├─ Phase 2: phase2_build_semantic_index
  │   ├─ Phase 3: phase3_process_transfers
  │   └─ commit_chunk (单事务: rb_* + user_event + cursor)
  └─ behind == 0 → sleep(base_interval)
```

**崩溃恢复**: 从 stage2_cursor 断点重做，语义索引不持久化

## 枚举类型

```cpp
EventType: Buy=0, Sell=1, Split=2, Merge=3, Redemption=4,
           FPMMBuy=5, FPMMSell=6, FPMMLPAdd=7, FPMMLPRemove=8,
           Convert=9, TransferIn=10, TransferOut=11

ConditionSource: ConditionPrep=0, PolymarketTokenReg=1, PolymarketFPMM=2,
                 OtherFPMM=3, SplitEvent=4, TransferInferred=5

TokenSource: PolymarketTokenReg=0, PolymarketFPMM=1, OtherFPMM=2,
             SplitEvent=3, TransferInferred=4

Collateral: Unknown=0, USDC=1 (bridged), USDCe=2 (native), WETH=3, DAI=4, WMATIC=5, USDT=6
```

`UNKNOWN_COND_IDX = UINT32_MAX` — TransferInferred token 无 condition 信息

## 数据结构

### 映射表 (持久化 rb\_\* + 内存)

| 映射                                                                   | 来源                                             | 用途                      |
| ---------------------------------------------------------------------- | ------------------------------------------------ | ------------------------- |
| `cond_map_[cond_id] → cond_idx`                                        | condition_preparation / token_map / fpmm / split | 32字节→4字节压缩          |
| `conditions_[cond_idx] → {outcome_count, payout, question_id, source}` | condition_preparation + resolution               | 结算状态 + NegRisk关联    |
| `token_map_[token_id] → {cond_idx, is_yes, source}`                    | token_map / fpmm计算 / split计算 / transfer推断  | Transfer 归属             |
| `fpmm_map_[fpmm_addr] → {cond_idx, collateral}`                        | fpmm                                             | FPMM operator 识别        |
| `cond_to_market_[question_id] → market_id`                             | neg_risk_question                                | Convert 事件的市场查找    |
| `fpmm_cond_idxs_` (set)                                                | fpmm                                             | 判断 Polymarket condition |
| `negrisk_cond_idxs_` (set)                                             | neg_risk_question + keccak256计算                | 判断 NegRisk condition    |
| `cond_collateral_[cond_idx] → Collateral`                              | fpmm / split                                     | 价格计算时判断是否 USDC   |

**Condition 来源优先级**: intern_condition 不覆盖已存在条目，但会补充 question_id

**token*map* 有四个来源** (按优先级):

1. TokenRegistered 事件直接提供 token0(YES) 和 token1(NO) — `PolymarketTokenReg`
2. FPMMCreation 需要 BN128 CTF position ID 计算 — `PolymarketFPMM`
3. Split 事件中的 condition_id + collateral_token 计算 — `SplitEvent`
4. Transfer 中发现未知 token_id → `TransferInferred` (cond_idx=UNKNOWN_COND_IDX, is_yes=0xFF)

### 语义索引 (仅内存, 不持久化)

```
tx_split_[TxKey{block, tx_hash}]             → vector<{amount, stakeholder, cond_id}>
tx_merge_[TxKey{block, tx_hash}]             → vector<{amount, stakeholder, cond_id}>
tx_redemption_[TxKey{block, tx_hash}]        → vector<{payout, redeemer, cond_id}>
tx_convert_[TxMarketKey{block, tx_hash, market_id}]  → vector<{market_id, index_set, amount, stakeholder}>
tx_order_[TxTokenKey{block, tx_hash, token_id}]      → {maker, taker, maker_side, usdc, tokens, fee}
tx_fpmm_trade_[TxFPMMKey{block, tx_hash, fpmm_addr}] → {fpmm_addr, trader, side, outcome_idx, usdc, tokens}
tx_fpmm_funding_[TxFPMMKey{block, tx_hash, fpmm_addr}] → {fpmm_addr, funder, side, amount0, amount1}
```

Split/Merge/Redemption 用 `TxKey` (无 cond_id)，同一 tx 的多个 split/merge/redemption 放在同一 vector，分类时通过 `cond_matches(info.cond_id)` 匹配

### user_event 表

```sql
CREATE TABLE user_event (
    user_addr   BLOB NOT NULL,
    sort_key    BIGINT NOT NULL,   -- block_number * 1e9 + log_index
    cond_idx    INTEGER NOT NULL,
    event_type  INTEGER NOT NULL,  -- EventType enum
    token_idx   INTEGER NOT NULL,  -- is_yes ? 0 : 1
    collateral  INTEGER NOT NULL DEFAULT 1, -- Collateral enum
    amount      BIGINT NOT NULL,   -- raw units, 负数=流出
    price       BIGINT NOT NULL,   -- price*1e6, 非USDC时为0
    PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
);
```

### RawEvent (内存, 32字节)

```
sort_key:   i64   block_number * 1e9 + log_index
cond_idx:   u32
type:       u8    EventType
token_idx:  u8    0=YES, 1=NO
collateral: u8    Collateral enum
_pad:       u8
amount:     i64   raw units (1e6 = $1), 负数=流出
price:      i64   price * 1e6, 非USDC时为0
```

## Phase 1: 更新映射 (phase1_update_mappings)

顺序处理 (依赖关系保证):

```
① condition_preparation → intern_condition(cid, cnt, ConditionPrep, question_id)
② condition_resolution  → update_condition_payout(idx, payouts)
③ token_map             → intern_condition(cid, 2, PolymarketTokenReg) + intern_token(YES/NO)
④ fpmm                  → intern_condition(cid, 2, PolymarketFPMM) + intern_fpmm + intern_condition_tokens(BN128)
⑤ split (DISTINCT)      → intern_condition(cid, 2, SplitEvent) + intern_condition_tokens(BN128)
⑥ neg_risk_question     → cond_to_market_ + negrisk_cond_idxs_ (keccak256反推conditionId)
→ update_cond_type_stats() 更新 ConditionTree / TokenTree
```

## Phase 2: 构建语义索引 (phase2_build_semantic_index)

从 stage1 读取 7 张表 → 填充 tx*split*/tx*merge*/tx*redemption*/tx*convert*/tx*order*/tx*fpmm_trade*/tx*fpmm_funding*

OrderFilled 的 maker_side 判断: `maker_asset == 0x00...00` → maker 出 USDC → maker_side=1(买)

## Phase 3: Transfer 分类 (classify_and_emit)

### TransferClass 完整分类 (33类)

```
Split(3):       SplitNormal, SplitNegRisk, SplitNonPoly
Merge(3):       MergeNormal, MergeNegRisk, MergeNonPoly
Redemption(2):  Redemption, RedemptionNonPoly
Poly专属(8):    Convert, OrderBuy, OrderSell, FPMMBuy, FPMMSell, FPMMLPAdd, FPMMLPRemove, FPMMLPReturn
Transfer(6):    TransferIn{NegRisk,Other,NonPoly}, TransferOut{NegRisk,Other,NonPoly}
InternalMint(2):  InternalMintNegRisk, InternalMintFPMM
InternalBurn(3):  InternalBurnNegRisk, InternalBurnFPMM, InternalBurnConvert
InternalXfer(5):  InternalTransferZero, InternalTransferOrder, InternalTransferNegRisk, InternalTransferFPMM, InternalTransferOther
Error(1):       Unclassified (必须=0, assert验证)
```

`verify()`: `total == user_events + internal + unclassified`, `unclassified == 0`

### 分类决策树

```
classify_and_emit(sort_key, tx_hash, block, op, from, to, token_id, amount, cond_idx, token_idx, collateral):
    assert(amount >= 0)
    if amount == 0 → InternalTransferZero
    known_token = (cond_idx != UNKNOWN_COND_IDX)
    assert(from != to)
    │
    ├─ from == 0x0 (mint)
    │   ├─ to == NEG_RISK_ADAPTER → InternalMintNegRisk
    │   ├─ to in fpmm_map_
    │   │   ├─ tx_fpmm_funding_ 有 且 side==Added
    │   │   │   assert(amount == max(amount0, amount1))
    │   │   │   → emit FPMMLPAdd(funder) + return FPMMLPAdd
    │   │   └─ → InternalMintFPMM
    │   ├─ tx_split_ 有 且 stakeholder==to && amount==info.amount && cond_matches
    │   │   ├─ known_token → emit Split(to) + return SplitNormal
    │   │   └─ !known_token → SplitNonPoly
    │   └─ assert(false) → Unclassified
    │
    ├─ to == 0x0 (burn)
    │   ├─ from == NEG_RISK_ADAPTER → InternalBurnNegRisk
    │   ├─ from in fpmm_map_ → InternalBurnFPMM
    │   ├─ tx_merge_ 有 且 stakeholder==from && amount==info.amount && cond_matches
    │   │   ├─ known_token → emit Merge(from) + return MergeNormal
    │   │   └─ !known_token → MergeNonPoly
    │   ├─ tx_redemption_ 有 且 redeemer==from && cond_matches
    │   │   ├─ known_token → emit Redemption(from, price=payout) + return Redemption
    │   │   └─ !known_token → RedemptionNonPoly
    │   └─ assert(false) → Unclassified
    │
    ├─ op == CTF_EXCHANGE / NEG_RISK_CTF_EXCHANGE
    │   ├─ tx_order_ 有
    │   │   assert(amount==tokens, maker/taker方向)
    │   │   emit Buy(to) + Sell(from)
    │   │   → OrderBuy / OrderSell / InternalTransferOrder
    │   ├─ !known_token → Unclassified
    │   └─ assert(false) → Unclassified
    │
    ├─ op == NEG_RISK_ADAPTER
    │   ├─ from == NEG_RISK_ADAPTER (Adapter→用户)
    │   │   ├─ tx_split_ 有 且 stakeholder==Adapter && cond_matches
    │   │   │   检测是否为Convert输出(YES token + 同tx有convert事件)
    │   │   │   → emit Split(to) + return SplitNegRisk
    │   │   └─ → emit TransferIn(to) + return TransferInNegRisk
    │   ├─ to == NEG_RISK_ADAPTER (用户→Adapter)
    │   │   ├─ tx_merge_ 有 且 stakeholder==Adapter && cond_matches
    │   │   │   → emit Merge(from) + return MergeNegRisk
    │   │   └─ → emit TransferOut(from) + return TransferOutNegRisk
    │   └─ to == NO_TOKEN_BURN_ADDRESS
    │       assert(known_token && token_idx==1, "只有NO token")
    │       通过 question_id → cond_to_market_ → tx_convert_ 查找
    │       → emit Convert(from) + return Convert
    │
    ├─ op in fpmm_map_
    │   ├─ from == op (FPMM→用户)
    │   │   ├─ tx_fpmm_funding_ 有 且 side==Added
    │   │   │   assert(amount0 != amount1, amount == |diff|, token_idx == 少的那个)
    │   │   │   → emit FPMMLPAdd(to, -amount) + return FPMMLPReturn
    │   │   ├─ tx_fpmm_trade_ 有 且 side==Buy
    │   │   │   → emit FPMMBuy(to) + return FPMMBuy
    │   │   └─ 默认 → emit FPMMLPRemove(to) + return FPMMLPRemove
    │   └─ to == op (用户→FPMM)
    │       └─ tx_fpmm_trade_ 有 且 side==Sell
    │           → emit FPMMSell(from) + return FPMMSell
    │
    └─ 其他
        ├─ known_token
        │   emit TransferIn(to) + TransferOut(from)
        │   → TransferInOther / TransferOutOther / InternalTransferOther
        └─ !known_token (NonPoly, 不写user_event)
            → TransferInNonPoly / TransferOutNonPoly / InternalTransferOther
```

### known_token 与 NonPoly 处理

- Phase 3 遇到 token*map* 中不存在的 token_id → 注册为 `TransferInferred` (cond_idx=UNKNOWN_COND_IDX)
- `cond_matches(info_cond_id)` 对 unknown token 总是返回 true (允许匹配任何 Split/Merge/Redemption)
- NonPoly 分类的 transfer **不写 user_event**，只计入 TransferStats
- `is_protocol_contract` 检查: ZERO*ADDR, CTF_EXCHANGE, NEG_RISK_CTF_EXCHANGE, NEG_RISK_ADAPTER, CONDITIONAL_TOKENS, NO_TOKEN_BURN_ADDRESS, fpmm_map* 中的地址

### 价格计算

| 事件类型       | 条件            | price 公式                      |
| -------------- | --------------- | ------------------------------- |
| Split/Merge    | USDC collateral | 1e6 / outcome_count             |
| Split/Merge    | 非USDC          | 0                               |
| Buy/Sell       | USDC            | usdc \* 1e6 / tokens            |
| Buy/Sell       | 非USDC          | 0                               |
| FPMMBuy/Sell   | USDC            | usdc \* 1e6 / tokens            |
| FPMMBuy/Sell   | 非USDC          | 0                               |
| FPMMLPAdd/Rem  | USDC            | 1e6 / outcome_count             |
| Redemption     | USDC            | payout_numerator (from条件结算) |
| Redemption     | 非USDC          | 0                               |
| Convert        | -               | 0 (当前实现)                    |
| TransferIn/Out | -               | 0                               |

### LP 特殊处理

FPMMLPAdd 产生 **两条** user_event:

1. **mint→FPMM** (FPMMLPAdd): amount = `max(amount0, amount1)` (每个 token 的池子投入量)
2. **FPMM→user** (FPMMLPReturn): amount = `-|amount0 - amount1|` (退还多余 token, 负值抵消)

FPMMLPRemove: FPMM→user 是 transfer 不是 burn，默认归类 (from==FPMM 且无 trade/funding-Added)

### 已知合约地址

```
ZERO_ADDR              = 0x0000000000000000000000000000000000000000
CTF_EXCHANGE           = 0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e
NEG_RISK_CTF_EXCHANGE  = 0xc5d563a36ae78145c45a50134d48a1215220f80a
NEG_RISK_ADAPTER       = 0xd91e80cf2e7be2e162c6513ced06f1dd0da35296
CONDITIONAL_TOKENS     = 0x4d97dcd97ec945f40cf65f87097ace5ea0476045
NO_TOKEN_BURN_ADDRESS  = 0x36a0e974a7083ea0ad4dea6a27b90fab22e93a32
USDC_E (bridged)       = 0x2791bca1f2de4661ed88a30c99a7a9449aa84174
USDC_NATIVE            = 0x3c499c542cef5e3811e1192ce70d8cc03d5c3359
WETH                   = 0x7ceb23fd6bc0add59e62ac25578270cff1b9f619
DAI                    = 0x8f3cf7ad23cd3cadbd9735aff958023239c6a063
WMATIC                 = 0x0d500b1d8e8ef31e21c99d1db9a6444d3adf1270
USDT                   = 0xc2132d05d31c914a87c6611c10748aeb04b58e8f
```

## Commit & 持久化

```
commit_chunk(new_cursor):
  BEGIN TRANSACTION
    批量写 rb_condition       (temp table + Appender → INSERT OR REPLACE)
    批量写 rb_token           (temp table + Appender → INSERT OR IGNORE)
    批量写 rb_fpmm            (temp table + Appender → INSERT OR IGNORE)
    批量写 rb_neg_risk_market (temp table + Appender → INSERT OR IGNORE)
    批量写 user_event         (temp table + Appender → INSERT OR IGNORE)
    UPDATE stage2_cursor (last_block + 各计数器)
  COMMIT
```

## 统计 (BuildProgress)

### ConditionTree (条件分区)

```
total
├─ polymarket.total
│   ├─ token_reg.total (source=PolymarketTokenReg)
│   │   ├─ amm     (后来创建了FPMM)
│   │   ├─ negrisk (在negrisk_cond_idxs_中)
│   │   └─ normal  (无FPMM，非NegRisk)
│   └─ fpmm_only (source=PolymarketFPMM, 无TokenReg)
└─ other.total
    ├─ prep       (source=ConditionPrep)
    ├─ other_fpmm (source=OtherFPMM, 预期=0)
    └─ split      (source=SplitEvent)
```

### TokenTree (代币分区)

```
total
├─ polymarket.total
│   ├─ token_reg.total (source=PolymarketTokenReg)
│   │   ├─ amm / negrisk / normal
│   └─ fpmm_only.total (source=PolymarketFPMM, 无TokenReg)
│       ├─ usdc     (USDC抵押品)
│       └─ non_usdc (WETH等)
└─ other.total
    ├─ other_fpmm        (source=OtherFPMM, 预期=0)
    ├─ split             (source=SplitEvent)
    └─ transfer_inferred (cond_idx=UNKNOWN, 从Transfer中发现)
```

### event_by_collateral

按 `(EventType, Collateral)` 分组计数: key = `EventType * 16 + Collateral`

### ChunkLog

只在有 NonPolymarket transfer 时写文件: `data/stage2/log/chunk_{start}_{NP数量}NP.log`
