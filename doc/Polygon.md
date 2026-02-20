# Polygon Node: Full vs Archive

**结论: Full node 足够。所有字段均来自 event log 或本地纯计算, 无需 `eth_call` / `eth_getStorageAt` 读历史状态。**

## 逐 Entity 审计

### Condition (ConditionalTokens.sol)

| 字段                | 来源                                   | 说明                                                                       |
| ------------------- | -------------------------------------- | -------------------------------------------------------------------------- |
| id (conditionId)    | `ConditionPreparation` event           | `keccak256(oracle, questionId, outcomeSlotCount)`                          |
| questionId          | `ConditionPreparation` event (indexed) |                                                                            |
| oracle              | `ConditionPreparation` event (indexed) |                                                                            |
| outcomeSlotCount    | `ConditionPreparation` event           |                                                                            |
| payoutNumerators    | `ConditionResolution` event            |                                                                            |
| payoutDenominator   | 本地计算                               | `sum(payoutNumerators)`, pnl-subgraph 就是这么做的                         |
| resolutionTimestamp | `ConditionResolution` event            | `event.block.timestamp`                                                    |
| positionIds         | 本地计算                               | `keccak256(collateral, getCollectionId(0, conditionId, indexSet))`, 纯哈希 |

### PnlCondition (ConditionalTokens.sol)

| 字段        | 来源                         | 说明                                                             |
| ----------- | ---------------------------- | ---------------------------------------------------------------- |
| id          | `ConditionPreparation` event | 同上                                                             |
| positionIds | 本地计算                     | 同上, 需知道 negRisk 用 wrappedCollateral 地址、普通用 USDC 地址 |

### EnrichedOrderFilled (CTFExchange Trading.sol)

源 event: `OrderFilled(bytes32 indexed orderHash, address indexed maker, address indexed taker, uint256 makerAssetId, uint256 takerAssetId, uint256 makerAmountFilled, uint256 takerAmountFilled, uint256 fee)`

| 字段             | 来源            | 说明                                                |
| ---------------- | --------------- | --------------------------------------------------- |
| timestamp        | event           | `block.timestamp`                                   |
| maker            | event (indexed) |                                                     |
| taker            | event (indexed) |                                                     |
| market (tokenID) | event 派生      | `makerAssetId == 0 ? takerAssetId : makerAssetId`   |
| side             | event 派生      | `makerAssetId == 0 → BUY, else SELL`                |
| size             | event 派生      | `side==BUY ? takerAmountFilled : makerAmountFilled` |
| price            | event 派生      | `quoteAmount * 1e6 / baseAmount`                    |

### Split (ConditionalTokens.sol)

源 event: `PositionSplit(address indexed stakeholder, IERC20 collateralToken, bytes32 indexed parentCollectionId, bytes32 indexed conditionId, uint[] partition, uint amount)`

| 字段        | 来源                          |
| ----------- | ----------------------------- |
| timestamp   | `block.timestamp`             |
| stakeholder | event (indexed)               |
| condition   | event `conditionId` (indexed) |
| amount      | event                         |

NegRiskAdapter 也 emit 自己的 `PositionSplit(stakeholder, conditionId, amount)`, 同样全在 event 里。

### Merge (ConditionalTokens.sol)

源 event: `PositionsMerge(address indexed stakeholder, IERC20 collateralToken, bytes32 indexed parentCollectionId, bytes32 indexed conditionId, uint[] partition, uint amount)`

与 Split 完全对称, 全部来自 event。

### Redemption (ConditionalTokens.sol)

源 event: `PayoutRedemption(address indexed redeemer, IERC20 indexed collateralToken, bytes32 indexed parentCollectionId, bytes32 conditionId, uint[] indexSets, uint payout)`

| 字段      | 来源                |
| --------- | ------------------- |
| timestamp | `block.timestamp`   |
| redeemer  | event (indexed)     |
| condition | event `conditionId` |
| indexSets | event               |
| payout    | event               |

### 额外: TokenRegistered (CTFExchange Registry.sol)

`TokenRegistered(uint256 indexed token0, uint256 indexed token1, bytes32 indexed conditionId)` — 建 token→condition 映射用, 也是 event。

## 自建 indexer 需监听的合约 & event 汇总

| 合约              | Event                  | 用途                                                    |
| ----------------- | ---------------------- | ------------------------------------------------------- |
| ConditionalTokens | `ConditionPreparation` | 构建 Condition/PnlCondition                             |
| ConditionalTokens | `ConditionResolution`  | 更新 payout + resolutionTimestamp                       |
| ConditionalTokens | `PositionSplit`        | Split 事件 (过滤掉 stakeholder=Exchange/NegRiskAdapter) |
| ConditionalTokens | `PositionsMerge`       | Merge 事件 (同上过滤)                                   |
| ConditionalTokens | `PayoutRedemption`     | Redemption 事件 (过滤掉 redeemer=NegRiskAdapter)        |
| CTFExchange       | `OrderFilled`          | EnrichedOrderFilled                                     |
| CTFExchange       | `TokenRegistered`      | token→condition 映射                                    |
| NegRiskAdapter    | `PositionSplit`        | NegRisk Split                                           |
| NegRiskAdapter    | `PositionsMerge`       | NegRisk Merge                                           |
| NegRiskAdapter    | `PayoutRedemption`     | NegRisk Redemption                                      |
| NegRiskAdapter    | `QuestionPrepared`     | NegRisk condition 创建                                  |
