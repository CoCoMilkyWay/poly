## Stage 1: eth_getLogs → 结构化表

Stage 1 保留原始事件的全部字段，不做过滤、不做计算字段。

**不入库的事件**: OrdersMatched (与OrderFilled冗余), OutcomeReported (与ConditionResolution冗余)

数值字段约定:
- 所有链上 `uint256` 字段一律用 `BLOB(32)` 存储（大端，无损）。
- 所有链上 `uint256[]` / `bytes32[]` 字段一律用 `LIST<BLOB(32)>` 存储（无损），不再用 JSON 文本。

### transfer (TransferSingle / TransferBatch)

TransferBatch 拆分为多行，每个 token 一行，通过 sub_index 区分。

| column       | 类型      | 来源     | 说明                                        |
| ------------ | --------- | -------- | ------------------------------------------- |
| block_number | BIGINT PK | log      |                                             |
| tx_hash      | BLOB(32)  | log      |                                             |
| log_index    | BIGINT PK | 计算     | log_index \* 1000 + sub_index (Batch拆分用) |
| operator     | BLOB(20)  | Transfer | 执行操作的地址                              |
| from_addr    | BLOB(20)  | Transfer | 0x0 = mint                                  |
| to_addr      | BLOB(20)  | Transfer | 0x0 = burn                                  |
| token_id     | BLOB(32)  | Transfer | positionId                                  |
| amount       | BLOB(32)  | Transfer | uint256, 6 decimals（无损）                 |

### condition_preparation (ConditionPreparation)

| column             | 类型        | 来源                 | 说明            |
| ------------------ | ----------- | -------------------- | --------------- |
| block_number       | BIGINT      | log                  |                 |
| tx_hash            | BLOB(32)    | log                  |                 |
| log_index          | INTEGER     | log                  |                 |
| condition_id       | BLOB(32) PK | ConditionPreparation |                 |
| oracle             | BLOB(20)    | ConditionPreparation |                 |
| question_id        | BLOB(32)    | ConditionPreparation |                 |
| outcome_slot_count | BLOB(32)    | ConditionPreparation | uint256（无损） |

### condition_resolution (ConditionResolution)

| column             | 类型           | 来源                | 说明              |
| ------------------ | -------------- | ------------------- | ----------------- |
| block_number       | BIGINT PK      | log                 |                   |
| tx_hash            | BLOB(32)       | log                 |                   |
| log_index          | INTEGER PK     | log                 |                   |
| condition_id       | BLOB(32)       | ConditionResolution |                   |
| oracle             | BLOB(20)       | ConditionResolution |                   |
| question_id        | BLOB(32)       | ConditionResolution |                   |
| outcome_slot_count | BLOB(32)       | ConditionResolution | uint256（无损）   |
| payout_numerators  | LIST<BLOB(32)> | ConditionResolution | uint256[]（无损） |

### split (PositionSplit)

| column               | 类型           | 来源          | 说明              |
| -------------------- | -------------- | ------------- | ----------------- |
| block_number         | BIGINT PK      | log           |                   |
| tx_hash              | BLOB(32)       | log           |                   |
| log_index            | INTEGER PK     | log           |                   |
| stakeholder          | BLOB(20)       | PositionSplit |                   |
| collateral_token     | BLOB(20)       | PositionSplit | USDC.e 或 Wrapped |
| parent_collection_id | BLOB(32)       | PositionSplit | 几乎总是 0x0      |
| condition_id         | BLOB(32)       | PositionSplit |                   |
| partition            | LIST<BLOB(32)> | PositionSplit | uint256[]（无损） |
| amount               | BLOB(32)       | PositionSplit | uint256（无损）   |

### merge (PositionsMerge)

| column               | 类型           | 来源           | 说明              |
| -------------------- | -------------- | -------------- | ----------------- |
| block_number         | BIGINT PK      | log            |                   |
| tx_hash              | BLOB(32)       | log            |                   |
| log_index            | INTEGER PK     | log            |                   |
| stakeholder          | BLOB(20)       | PositionsMerge |                   |
| collateral_token     | BLOB(20)       | PositionsMerge | USDC.e 或 Wrapped |
| parent_collection_id | BLOB(32)       | PositionsMerge | 几乎总是 0x0      |
| condition_id         | BLOB(32)       | PositionsMerge |                   |
| partition            | LIST<BLOB(32)> | PositionsMerge | uint256[]（无损） |
| amount               | BLOB(32)       | PositionsMerge | uint256（无损）   |

### redemption (PayoutRedemption)

| column               | 类型           | 来源             | 说明              |
| -------------------- | -------------- | ---------------- | ----------------- |
| block_number         | BIGINT PK      | log              |                   |
| tx_hash              | BLOB(32)       | log              |                   |
| log_index            | INTEGER PK     | log              |                   |
| redeemer             | BLOB(20)       | PayoutRedemption |                   |
| collateral_token     | BLOB(20)       | PayoutRedemption | USDC.e 或 Wrapped |
| parent_collection_id | BLOB(32)       | PayoutRedemption | 几乎总是 0x0      |
| condition_id         | BLOB(32)       | PayoutRedemption |                   |
| index_sets           | LIST<BLOB(32)> | PayoutRedemption | uint256[]（无损） |
| payout               | BLOB(32)       | PayoutRedemption | uint256（无损）   |

### fpmm (FPMMCreation)

| column                | 类型           | 来源         | 说明                                                                      |
| --------------------- | -------------- | ------------ | ------------------------------------------------------------------------- |
| block_number          | BIGINT         | log          |                                                                           |
| tx_hash               | BLOB(32)       | log          |                                                                           |
| log_index             | INTEGER        | log          |                                                                           |
| factory               | BLOB(20)       | log.address  | 发出 `FixedProductMarketMakerCreation` 的工厂地址                         |
| creation_topics_count | BIGINT         | log.topics   | 创建事件 topics 长度（4=FixedProductMarketMakerFactory, 2=Deterministic） |
| creation_layout       | VARCHAR        | 解析标记     | `fixed_factory_v1` / `deterministic_factory_v1`                           |
| creator               | BLOB(20)       | FPMMCreation |                                                                           |
| fpmm_addr             | BLOB(20) PK    | FPMMCreation | fixedProductMarketMaker                                                   |
| conditional_tokens    | BLOB(20)       | FPMMCreation | ConditionalTokens 合约地址                                                |
| collateral_token      | BLOB(20)       | FPMMCreation | USDC.e                                                                    |
| condition_ids         | LIST<BLOB(32)> | FPMMCreation | bytes32[]（无损）                                                         |
| fee                   | BLOB(32)       | FPMMCreation | uint256（无损）                                                           |

### fpmm_trade (FPMMBuy / FPMMSell)

AMM Taker 交易，单边操作。

| column        | 类型       | 来源         | 说明                |
| ------------- | ---------- | ------------ | ------------------- |
| block_number  | BIGINT PK  | log          |                     |
| tx_hash       | BLOB(32)   | log          |                     |
| log_index     | INTEGER PK | log          |                     |
| fpmm_addr     | BLOB(20)   | log.address  |                     |
| trader        | BLOB(20)   | FPMMBuy/Sell | $.buyer 或 $.seller |
| side          | INTEGER    | 事件类型     | 1=Buy, 2=Sell       |
| outcome_index | BLOB(32)   | FPMMBuy/Sell | uint256（无损）     |
| usdc_amount   | BLOB(32)   | FPMMBuy/Sell | uint256（无损）     |
| token_amount  | BLOB(32)   | FPMMBuy/Sell | uint256（无损）     |
| fee           | BLOB(32)   | FPMMBuy/Sell | uint256（无损）     |

### fpmm_funding (FPMMFundingAdded / FPMMFundingRemoved)

AMM LP 操作。LP 按池子比例添加/取回 YES+NO。

| column                   | 类型           | 来源                 | 说明               |
| ------------------------ | -------------- | -------------------- | ------------------ |
| block_number             | BIGINT PK      | log                  |                    |
| tx_hash                  | BLOB(32)       | log                  |                    |
| log_index                | INTEGER PK     | log                  |                    |
| fpmm_addr                | BLOB(20)       | log.address          |                    |
| funder                   | BLOB(20)       | FundingAdded/Removed |                    |
| side                     | INTEGER        | 事件类型             | 1=Added, 2=Removed |
| amounts                  | LIST<BLOB(32)> | amountsAdded/Removed | uint256[]（无损）  |
| collateral_from_fee_pool | BLOB(32)       | FundingRemoved       | uint256（无损）    |
| shares                   | BLOB(32)       | sharesMinted/Burnt   | uint256（无损）    |

### order_filled (OrderFilled)

| column         | 类型       | 来源        | 说明              |
| -------------- | ---------- | ----------- | ----------------- |
| block_number   | BIGINT PK  | log         |                   |
| tx_hash        | BLOB(32)   | log         |                   |
| log_index      | INTEGER PK | log         |                   |
| exchange       | TEXT       | log.address | "CTF" / "NegRisk" |
| order_hash     | BLOB(32)   | OrderFilled |                   |
| maker          | BLOB(20)   | OrderFilled |                   |
| taker          | BLOB(20)   | OrderFilled |                   |
| maker_asset_id | BLOB(32)   | OrderFilled | 0 = collateral    |
| taker_asset_id | BLOB(32)   | OrderFilled | 0 = collateral    |
| maker_amount   | BLOB(32)   | OrderFilled | uint256（无损）   |
| taker_amount   | BLOB(32)   | OrderFilled | uint256（无损）   |
| fee            | BLOB(32)   | OrderFilled | uint256（无损）   |

### token_map (TokenRegistered)

| column       | 类型       | 来源            | 说明              |
| ------------ | ---------- | --------------- | ----------------- |
| block_number | BIGINT PK  | log             |                   |
| tx_hash      | BLOB(32)   | log             |                   |
| log_index    | INTEGER PK | log             |                   |
| exchange     | TEXT       | log.address     | "CTF" / "NegRisk" |
| token0       | BLOB(32)   | TokenRegistered | YES tokenId       |
| token1       | BLOB(32)   | TokenRegistered | NO tokenId        |
| condition_id | BLOB(32)   | TokenRegistered |                   |

### neg_risk_market (MarketPrepared)

| column       | 类型        | 来源           | 说明                            |
| ------------ | ----------- | -------------- | ------------------------------- |
| block_number | BIGINT      | log            |                                 |
| tx_hash      | BLOB(32)    | log            |                                 |
| log_index    | INTEGER     | log            |                                 |
| market_id    | BLOB(32) PK | MarketPrepared |                                 |
| oracle       | BLOB(20)    | MarketPrepared |                                 |
| fee_bips     | BLOB(32)    | MarketPrepared | uint256（无损）                 |
| data         | BLOB        | MarketPrepared | ABI编码: title, description, id |

### neg_risk_question (QuestionPrepared)

| column         | 类型        | 来源             | 说明                             |
| -------------- | ----------- | ---------------- | -------------------------------- |
| block_number   | BIGINT      | log              |                                  |
| tx_hash        | BLOB(32)    | log              |                                  |
| log_index      | INTEGER     | log              |                                  |
| market_id      | BLOB(32)    | QuestionPrepared |                                  |
| question_id    | BLOB(32) PK | QuestionPrepared | keccak256(marketId, questionIdx) |
| question_index | BLOB(32)    | QuestionPrepared | uint256（无损）                  |
| data           | BLOB        | QuestionPrepared | ABI编码: question, description   |

### convert (PositionsConverted)

| column       | 类型       | 来源               | 说明                   |
| ------------ | ---------- | ------------------ | ---------------------- |
| block_number | BIGINT PK  | log                |                        |
| tx_hash      | BLOB(32)   | log                |                        |
| log_index    | INTEGER PK | log                |                        |
| stakeholder  | BLOB(20)   | PositionsConverted |                        |
| market_id    | BLOB(32)   | PositionsConverted |                        |
| index_set    | BLOB(32)   | PositionsConverted | uint256 bitmap（无损） |
| amount       | BLOB(32)   | PositionsConverted | uint256（无损）        |

### sync_state

| key        | 含义           |
| ---------- | -------------- |
| last_block | 已同步到的区块 |

## PnL 计算

```
PnL = Σ(Sell) + Σ(Merge) + Σ(Redemption) + Σ(Convert收益)
    - Σ(Buy) - Σ(Split) - Σ(Fee)
```

| 来源              | 加减 | 说明                            |
| ----------------- | ---- | ------------------------------- |
| split.amount      | -    | 铸造消耗 USDC                   |
| merge.amount      | +    | 销毁获得 USDC                   |
| redemption.payout | +    | 结算赎回                        |
| fpmm_trade Buy    | -    | FPMM买入花费 USDC               |
| fpmm_trade Sell   | +    | FPMM卖出获得 USDC               |
| fpmm_trade.fee    | -    | FPMM手续费                      |
| order_filled Buy  | -    | 买入花费 USDC                   |
| order_filled Sell | +    | 卖出获得 USDC                   |
| order_filled.fee  | -    | 手续费                          |
| convert           | +    | (popcount(index_set)-1)\*amount |
