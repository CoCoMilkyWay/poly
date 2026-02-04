## 9个在线Subgraph节点对比

| #   | 名称                           | Subgraph ID                                  |
| --- | ------------------------------ | -------------------------------------------- |
| 1   | Polymarket                     | 81Dm16JjuFSrqz813HysXoUPvzTwE7fsfPk2RTf66nyC |
| 2   | Polymarket PnL                 | EtXYMmR6ZVMkR1ndhfb5kZSee2T8hHg72FzPH5CPkypa |
| 3   | Polymarket Profit and Loss     | 6c58N5U4MtQE2Y8njfVrrAfRykzfqajMGeTMEvMmskVz |
| 4   | Polymarket Names               | 22CoTbEtpv6fURB6moTNfJPWNUPXtiFGRA8h1zajMha3 |
| 5   | Polymarket Orderbook           | 7fu2DWYK93ePfzB24c2wrP94S3x4LGHUrQxphhoEypyY |
| 6   | Polymarket Open Interest       | ELaW6RtkbmYNmMMU6hEPsghG9Ko3EXSmiRkH855M4qfF |
| 7   | Polymarket Activity Polygon    | Bx1W4S7kDVxs9gC3s2G6DS8kdNBJNVhMviCtin2DiBp  |
| 8   | polymarket-order-filled-events | EZCTgSzLPuBSqQcuR3ifeiKHKBnpjHSNbYpty8Mnjm9D |
| 9   | olas-predict-polymarket        | GsVaaS7CTPEMdBHshwxPJFcFFhsMq3Mivt27vvCZdyCp |

---

## 完整事件覆盖总表

|                                              | 1        | 2        | 3        | 4        | 5        | 6        | 7        | 8        | 9        |
| -------------------------------------------- | -------- | -------- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |
| **ConditionalTokens**                        |          |          |          |          |          |          |          |          |          |
| `0x4D97DCd97eC945f40cF65F87097ACe5EA0476045` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:4023686)                    | 4023686  | 4023686  | 4023686  | -        | -        | 4023686  | 4023686  | 4023686  | 78425180 |
| ConditionPreparation                         | ✅        | ✅        | ✅        |          |          | ✅        | ✅        |          | ✅        |
| ConditionResolution                          | ✅        | ✅        | ✅        |          |          |          |          |          |          |
| PositionSplit                                | ✅        | ✅        | ✅        |          |          | ✅        | ✅        |          |          |
| PositionsMerge                               | ✅        | ✅        | ✅        |          |          | ✅        | ✅        |          |          |
| PayoutRedemption                             | ✅        | ✅        | ✅        |          |          | ✅        | ✅        | ✅        | ✅        |
| TransferSingle                               | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| TransferBatch                                | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| ApprovalForAll                               | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **CTFExchange**                              |          |          |          |          |          |          |          |          |          |
| `0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:33605403)                   | 33605403 | 33605403 | 33605403 | -        | 70000000 | -        | -        | 33605403 | 78425180 |
| OrderFilled                                  | ✅        | ✅        | ✅        |          | ✅        |          |          | ✅        | ✅        |
| OrdersMatched                                | ✅        |          |          |          | ✅        |          |          |          |          |
| TokenRegistered                              | ✅        |          |          |          | ✅        |          |          |          | ✅        |
| OrderCancelled                               | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| FeeCharged                                   | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **NegRiskCTFExchange**                       |          |          |          |          |          |          |          |          |          |
| `0xC5d563A36AE78145C45a50134d48A1215220f80a` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:50505492)                   | 50505492 | 50505492 | 50505492 | -        | 70000000 | -        | -        | 50505492 | 78425180 |
| OrderFilled                                  | ✅        | ✅        | ✅        |          | ✅        |          |          | ✅        | ✅        |
| OrdersMatched                                | ✅        |          |          |          | ✅        |          |          |          |          |
| TokenRegistered                              | ✅        |          |          |          | ✅        |          |          |          | ✅        |
| OrderCancelled                               | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| FeeCharged                                   | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **NegRiskAdapter**                           |          |          |          |          |          |          |          |          |          |
| `0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:50505403)                   | -        | 50505403 | 50505403 | -        | -        | 50505403 | 50505403 | 50505403 | 78425180 |
| MarketPrepared                               |          | ✅        | ✅        |          |          | ✅        | ✅        |          |          |
| QuestionPrepared                             |          | ✅        | ✅        |          |          | ✅        | ✅        |          | ✅        |
| PositionSplit                                |          | ✅        | ✅        |          |          | ✅        | ✅        |          |          |
| PositionsMerge                               |          | ✅        | ✅        |          |          | ✅        | ✅        |          |          |
| PositionsConverted                           |          | ✅        | ✅        |          |          | ✅        | ✅        |          |          |
| PayoutRedemption                             |          | ✅        | ✅        |          |          | ✅        | ✅        | ✅        | ✅        |
| OutcomeReported                              |          |          |          |          |          |          |          |          | ✅        |
|                                              |          |          |          |          |          |          |          |          |          |
| **FPMMFactory**                              |          |          |          |          |          |          |          |          |          |
| `0x8B9805A2f595B6705e74F7310829f2d299D21522` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:4023693)                    | 4023693  | 4023693  | 4023693  | -        | -        | -        | 4023693  | -        | -        |
| FixedProductMarketMakerCreation              | ✅        | ✅        | ✅        |          |          |          | ✅        |          |          |
|                                              |          |          |          |          |          |          |          |          |          |
| **FPMM (动态Template)**                      |          |          |          |          |          |          |          |          |          |
| FPMMBuy                                      | ✅        | ✅        | ✅        |          |          |          |          |          |          |
| FPMMSell                                     | ✅        | ✅        | ✅        |          |          |          |          |          |          |
| FPMMFundingAdded                             | ✅        | ✅        | ✅        |          |          |          |          |          |          |
| FPMMFundingRemoved                           | ✅        | ✅        | ✅        |          |          |          |          |          |          |
| Transfer (LP share)                          | ✅        |          |          |          |          |          |          |          |          |
|                                              |          |          |          |          |          |          |          |          |          |
| **UmaCtfAdapter Old**                        |          |          |          |          |          |          |          |          |          |
| `0xCB1822859cEF82Cd2Eb4E6276C7916e692995130` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:23569780)                   | -        | -        | -        | 72000000 | -        | -        | -        | -        | -        |
| QuestionInitialized                          |          |          |          | ✅        |          |          |          |          |          |
| QuestionReset                                | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionResolved                             | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionSettled                              | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **UmaCtfAdapter V2**                         |          |          |          |          |          |          |          |          |          |
| `0x6A9D222616C90FcA5754cd1333cFD9b7fb6a4F74` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:35203539)                   | -        | -        | -        | 72000000 | -        | -        | -        | -        | -        |
| QuestionInitialized                          |          |          |          | ✅        |          |          |          |          |          |
| QuestionReset                                | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionResolved                             | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **UmaCtfAdapter Legacy**                     |          |          |          |          |          |          |          |          |          |
| `0x71392E133063CC0D16F40E1F9B60227404Bc03f7` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:~72M)                       | -        | -        | -        | 72000000 | -        | -        | -        | -        | -        |
| QuestionInitialized                          |          |          |          | ✅        |          |          |          |          |          |
| QuestionReset                                | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionResolved                             | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **UmaCtfAdapter V3**                         |          |          |          |          |          |          |          |          |          |
| `0x157Ce2d672854c848c9b79C49a8Cc6cc89176a49` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:46755254)                   | -        | -        | -        | -        | -        | -        | -        | -        | 78425180 |
| QuestionInitialized                          |          |          |          |          |          |          |          |          | ✅        |
| QuestionReset                                | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionResolved                             |          |          |          |          |          |          |          |          | ✅        |
|                                              |          |          |          |          |          |          |          |          |          |
| **UmaCtfAdapter V4**                         |          |          |          |          |          |          |          |          |          |
| `0x65070BE91477460D8A7AeEb94ef92fe056C2f2A7` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:74797879)                   | -        | -        | -        | -        | -        | -        | -        | -        | 78425180 |
| QuestionInitialized                          |          |          |          |          |          |          |          |          | ✅        |
| QuestionReset                                | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionResolved                             |          |          |          |          |          |          |          |          | ✅        |
|                                              |          |          |          |          |          |          |          |          |          |
| **NegRiskUmaCtfAdapter**                     |          |          |          |          |          |          |          |          |          |
| `0x2F5e3684cb1F318ec51b00Edba38d79Ac2c0aA9d` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:50505488)                   | -        | -        | -        | -        | -        | -        | -        | -        | -        |
| QuestionInitialized                          | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionReset                                | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| QuestionResolved                             | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **OptimisticOracleV2**                       |          |          |          |          |          |          |          |          |          |
| `0xeE3Afe347D5C74317041E2618C49534dAf887c24` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:35203539)                   | -        | -        | -        | -        | -        | -        | -        | -        | -        |
| RequestPrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| ProposePrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| DisputePrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| Settle                                       | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **OptimisticOracleOld**                      |          |          |          |          |          |          |          |          |          |
| `0xBb1A8db2D4350976a11cdfA60A1d43f97710Da49` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:23569780)                   | -        | -        | -        | -        | -        | -        | -        | -        | -        |
| RequestPrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| ProposePrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| DisputePrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **ManagedOptimisticOracleV2**                |          |          |          |          |          |          |          |          |          |
| `0x2C0367a9DB231dDeBd88a94b4f6461a6e47C58B1` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:74677419)                   | -        | -        | -        | -        | -        | -        | -        | -        | -        |
| RequestPrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| ProposePrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| DisputePrice                                 | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **ModRegistry**                              |          |          |          |          |          |          |          |          |          |
| `0xe1c9271516930B9e1355b87232556a0f39D3aBD3` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:52699785)                   | -        | -        | -        | -        | -        | -        | -        | -        | -        |
| ModAdded                                     | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
| ModRemoved                                   | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        | ❌        |
|                                              |          |          |          |          |          |          |          |          |          |
| **ServiceRegistryL2 (Olas)**                 |          |          |          |          |          |          |          |          |          |
| `0xE3607b00E75f6405248323A9417ff6b39B244b50` |          |          |          |          |          |          |          |          |          |
| startBlock (部署:80360433)                   | -        | -        | -        | -        | -        | -        | -        | -        | 80360433 |
| RegisterInstance                             |          |          |          |          |          |          |          |          | ✅        |
| CreateMultisigWithAgents                     |          |          |          |          |          |          |          |          | ✅        |

---

### 推荐组合
| 目标             | 节点组合                         |
| ---------------- | -------------------------------- |
| 最完整历史       | #1 + #2 (覆盖核心合约从部署开始) |
| +Market元数据    | +#4 (但丢失72M前的market title)  |
| +OutcomeReported | +#9 (但仅78M后数据)              |

---

## 合约归属总结

| 合约                      | 地址                                         | 区块链  | 协议       | 运营方                | 索引 |
| ------------------------- | -------------------------------------------- | ------- | ---------- | --------------------- | ---- |
| ConditionalTokens         | `0x4D97DCd97eC945f40cF65F87097ACe5EA0476045` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| CTFExchange               | `0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| NegRiskCTFExchange        | `0xC5d563A36AE78145C45a50134d48A1215220f80a` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| NegRiskAdapter            | `0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| FPMMFactory               | `0x8B9805A2f595B6705e74F7310829f2d299D21522` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| UmaCtfAdapter Old         | `0xCB1822859cEF82Cd2Eb4E6276C7916e692995130` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| UmaCtfAdapter V2          | `0x6A9D222616C90FcA5754cd1333cFD9b7fb6a4F74` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| UmaCtfAdapter Legacy      | `0x71392E133063CC0D16F40E1F9B60227404Bc03f7` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| UmaCtfAdapter V3          | `0x157Ce2d672854c848c9b79C49a8Cc6cc89176a49` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| UmaCtfAdapter V4          | `0x65070BE91477460D8A7AeEb94ef92fe056C2f2A7` | Polygon | Polymarket | Polymarket Inc.       | ✅    |
| NegRiskUmaCtfAdapter      | `0x2F5e3684cb1F318ec51b00Edba38d79Ac2c0aA9d` | Polygon | Polymarket | Polymarket Inc.       | ❌    |
| ModRegistry               | `0xe1c9271516930B9e1355b87232556a0f39D3aBD3` | Polygon | Polymarket | Polymarket Inc.       | ❌    |
| OptimisticOracleV2        | `0xeE3Afe347D5C74317041E2618C49534dAf887c24` | Polygon | UMA        | Risk Labs Foundation  | ❌    |
| OptimisticOracleOld       | `0xBb1A8db2D4350976a11cdfA60A1d43f97710Da49` | Polygon | UMA        | Risk Labs Foundation  | ❌    |
| ManagedOptimisticOracleV2 | `0x2C0367a9DB231dDeBd88a94b4f6461a6e47C58B1` | Polygon | UMA        | Risk Labs Foundation  | ❌    |
| ServiceRegistryL2         | `0xE3607b00E75f6405248323A9417ff6b39B244b50` | Polygon | Olas       | Valory AG (Autonolas) | ✅    |
