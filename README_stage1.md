## Stage 1: eth_getLogs → 结构化表

Stage 1 保留原始事件的全部字段，不做过滤、不做计算字段。

**不入库的事件**: OrdersMatched (与OrderFilled冗余), OutcomeReported (与ConditionResolution冗余)

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
| amount       | BIGINT    | Transfer | 6 decimals                                  |

### condition_preparation (ConditionPreparation)

| column             | 类型        | 来源                 | 说明 |
| ------------------ | ----------- | -------------------- | ---- |
| block_number       | BIGINT      | log                  |      |
| tx_hash            | BLOB(32)    | log                  |      |
| log_index          | INTEGER     | log                  |      |
| condition_id       | BLOB(32) PK | ConditionPreparation |      |
| oracle             | BLOB(20)    | ConditionPreparation |      |
| question_id        | BLOB(32)    | ConditionPreparation |      |
| outcome_slot_count | INTEGER     | ConditionPreparation |      |

### condition_resolution (ConditionResolution)

| column             | 类型       | 来源                | 说明                       |
| ------------------ | ---------- | ------------------- | -------------------------- |
| block_number       | BIGINT PK  | log                 |                            |
| tx_hash            | BLOB(32)   | log                 |                            |
| log_index          | INTEGER PK | log                 |                            |
| condition_id       | BLOB(32)   | ConditionResolution |                            |
| oracle             | BLOB(20)   | ConditionResolution |                            |
| question_id        | BLOB(32)   | ConditionResolution |                            |
| outcome_slot_count | INTEGER    | ConditionResolution |                            |
| payout_numerators  | TEXT       | ConditionResolution | "[1,0]"=YES, "[0,1]"=NO 赢 |

### split (PositionSplit)

| column               | 类型       | 来源          | 说明                        |
| -------------------- | ---------- | ------------- | --------------------------- |
| block_number         | BIGINT PK  | log           |                             |
| tx_hash              | BLOB(32)   | log           |                             |
| log_index            | INTEGER PK | log           |                             |
| stakeholder          | BLOB(20)   | PositionSplit |                             |
| collateral_token     | BLOB(20)   | PositionSplit | USDC.e 或 Wrapped           |
| parent_collection_id | BLOB(32)   | PositionSplit | 几乎总是 0x0                |
| condition_id         | BLOB(32)   | PositionSplit |                             |
| partition            | TEXT       | PositionSplit | "[1,2]"                     |
| amount               | BIGINT     | PositionSplit | USDC消耗 = YES获得 = NO获得 |

### merge (PositionsMerge)

| column               | 类型       | 来源           | 说明                        |
| -------------------- | ---------- | -------------- | --------------------------- |
| block_number         | BIGINT PK  | log            |                             |
| tx_hash              | BLOB(32)   | log            |                             |
| log_index            | INTEGER PK | log            |                             |
| stakeholder          | BLOB(20)   | PositionsMerge |                             |
| collateral_token     | BLOB(20)   | PositionsMerge | USDC.e 或 Wrapped           |
| parent_collection_id | BLOB(32)   | PositionsMerge | 几乎总是 0x0                |
| condition_id         | BLOB(32)   | PositionsMerge |                             |
| partition            | TEXT       | PositionsMerge | "[1,2]"                     |
| amount               | BIGINT     | PositionsMerge | USDC获得 = YES消耗 = NO消耗 |

### redemption (PayoutRedemption)

| column               | 类型       | 来源             | 说明                    |
| -------------------- | ---------- | ---------------- | ----------------------- |
| block_number         | BIGINT PK  | log              |                         |
| tx_hash              | BLOB(32)   | log              |                         |
| log_index            | INTEGER PK | log              |                         |
| redeemer             | BLOB(20)   | PayoutRedemption |                         |
| collateral_token     | BLOB(20)   | PayoutRedemption | USDC.e 或 Wrapped       |
| parent_collection_id | BLOB(32)   | PayoutRedemption | 几乎总是 0x0            |
| condition_id         | BLOB(32)   | PayoutRedemption |                         |
| index_sets           | TEXT       | PayoutRedemption | "[1]", "[2]", "[1,2]"   |
| payout               | BIGINT     | PayoutRedemption | USDC获得 (可能为0=输家) |

### fpmm (FPMMCreation)

| column             | 类型        | 来源         | 说明                         |
| ------------------ | ----------- | ------------ | ---------------------------- |
| block_number       | BIGINT      | log          |                              |
| tx_hash            | BLOB(32)    | log          |                              |
| log_index          | INTEGER     | log          |                              |
| creator            | BLOB(20)    | FPMMCreation |                              |
| fpmm_addr          | BLOB(20) PK | FPMMCreation | fixedProductMarketMaker      |
| conditional_tokens | BLOB(20)    | FPMMCreation | ConditionalTokens合约地址    |
| collateral_token   | BLOB(20)    | FPMMCreation | USDC.e                       |
| condition_ids      | TEXT        | FPMMCreation | "[0x...]" 完整数组           |
| fee                | BIGINT      | FPMMCreation | 1e18 scale (1e18 = 100% fee) |

### fpmm_trade (FPMMBuy / FPMMSell)

AMM Taker 交易，单边操作。

| column        | 类型       | 来源         | 说明                                              |
| ------------- | ---------- | ------------ | ------------------------------------------------- |
| block_number  | BIGINT PK  | log          |                                                   |
| tx_hash       | BLOB(32)   | log          |                                                   |
| log_index     | INTEGER PK | log          |                                                   |
| fpmm_addr     | BLOB(20)   | log.address  |                                                   |
| trader        | BLOB(20)   | FPMMBuy/Sell | $.buyer 或 $.seller                               |
| side          | INTEGER    | 事件类型     | 1=Buy, 2=Sell                                     |
| outcome_index | INTEGER    | FPMMBuy/Sell | 0=YES, 1=NO                                       |
| usdc_amount   | BIGINT     | FPMMBuy/Sell | Buy: investmentAmount; Sell: returnAmount         |
| token_amount  | BIGINT     | FPMMBuy/Sell | Buy: outcomeTokensBought; Sell: outcomeTokensSold |
| fee           | BIGINT     | FPMMBuy/Sell | feeAmount                                         |

### fpmm_funding (FPMMFundingAdded / FPMMFundingRemoved)

AMM LP 操作。LP 按池子比例添加/取回 YES+NO。

| column                   | 类型       | 来源                 | 说明                                    |
| ------------------------ | ---------- | -------------------- | --------------------------------------- |
| block_number             | BIGINT PK  | log                  |                                         |
| tx_hash                  | BLOB(32)   | log                  |                                         |
| log_index                | INTEGER PK | log                  |                                         |
| fpmm_addr                | BLOB(20)   | log.address          |                                         |
| funder                   | BLOB(20)   | FundingAdded/Removed |                                         |
| side                     | INTEGER    | 事件类型             | 1=Added, 2=Removed                      |
| amounts                  | TEXT       | amountsAdded/Removed | "[yes, no]"                             |
| collateral_from_fee_pool | BIGINT     | FundingRemoved       | Removed时从手续费池取出的USDC (Added=0) |
| shares                   | BIGINT     | sharesMinted/Burnt   |                                         |

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
| maker_amount   | BIGINT     | OrderFilled | makerAmountFilled |
| taker_amount   | BIGINT     | OrderFilled | takerAmountFilled |
| fee            | BIGINT     | OrderFilled |                   |

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
| fee_bips     | INTEGER     | MarketPrepared | 万分比                          |
| data         | BLOB        | MarketPrepared | ABI编码: title, description, id |

### neg_risk_question (QuestionPrepared)

| column         | 类型        | 来源             | 说明                             |
| -------------- | ----------- | ---------------- | -------------------------------- |
| block_number   | BIGINT      | log              |                                  |
| tx_hash        | BLOB(32)    | log              |                                  |
| log_index      | INTEGER     | log              |                                  |
| market_id      | BLOB(32)    | QuestionPrepared |                                  |
| question_id    | BLOB(32) PK | QuestionPrepared | keccak256(marketId, questionIdx) |
| question_index | INTEGER     | QuestionPrepared |                                  |
| data           | BLOB        | QuestionPrepared | ABI编码: question, description   |

### convert (PositionsConverted)

| column       | 类型       | 来源               | 说明                 |
| ------------ | ---------- | ------------------ | -------------------- |
| block_number | BIGINT PK  | log                |                      |
| tx_hash      | BLOB(32)   | log                |                      |
| log_index    | INTEGER PK | log                |                      |
| stakeholder  | BLOB(20)   | PositionsConverted |                      |
| market_id    | BLOB(32)   | PositionsConverted |                      |
| index_set    | BIGINT     | PositionsConverted | bitmap: 哪些NO被转换 |
| amount       | BIGINT     | PositionsConverted | 每个position的数量   |

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
