# Polymarket PnL Replayer

```
项目目标:  可以replay polymarket 历史上任何用户的历史Token净值/PnL
polygon链上polymarket协议合约节点本身的实现: /home/chuyin/work/poly/doc/smart-contracts

老架构:基于the Graph协议的indexer实现
新架构:直接query RPC节点获得历史信息(本地Erigon/远程Alchemy节点, 行为一致)

老架构:
    思路: doc/indexer/README.md
    官方推荐的the Graph实现: /home/chuyin/work/poly/doc/indexer/the graph/official (但真正的实现和他其实差异很大)
    商业graph indexer实现:
        IPFS manifest: /home/chuyin/work/poly/doc/indexer/the graph/implemented/IPFS
        graphql schema: /home/chuyin/work/poly/doc/indexer/the graph/implemented/schema

```

## 新架构

| 事件                 |               历史Token持仓                |                  历史Token交易                   |                   历史Token净值/PnL                   | 辅助映射表 |
| -------------------- | :----------------------------------------: | :----------------------------------------------: | :---------------------------------------------------: | :--------: |
| **能知道**           | Token协议流水; Token账户流水; USDC协议流水 |       Token协议流水; USDC协议流水; 成交价        | Token协议流水; Token账户流水; USDC协议流水; Token PnL |            |
| **不能知道**         | USDC钱包流水; 成交价; Token PnL; 实时市价  | Token账户流水; USDC钱包流水; Token PnL; 实时市价 |                USDC钱包流水; 实时市价                 |            |
| TransferSingle       |                     ✓                      |                                                  |                           ✓                           |            |
| TransferBatch        |                     ✓                      |                                                  |                           ✓                           |            |
| OrderFilled          |                                            |                        ✓                         |                           ✓                           |            |
| OrdersMatched        |                                            |                                                  |                                                       |            |
| TokenRegistered      |                                            |                                                  |                                                       | token_map  |
| PositionSplit        |                                            |                        ✓                         |                           ✓                           |            |
| PositionsMerge       |                                            |                        ✓                         |                           ✓                           |            |
| PayoutRedemption     |                                            |                        ✓                         |                           ✓                           |            |
| PositionsConverted   |                                            |                        ✓                         |                           ✓                           |            |
| ConditionPreparation |                                            |                                                  |                                                       | condition  |
| ConditionResolution  |                                            |                                                  |                                                       | condition  |
| MarketPrepared       |                                            |                                                  |                                                       |  neg_risk  |
| QuestionPrepared     |                                            |                                                  |                                                       |  neg_risk  |
| OutcomeReported      |                                            |                                                  |                                                       |  neg_risk  |

- **Token 持仓** = Token 协议流水(可追踪) + Token 账户流水(可追踪)
- **USDC 持仓** = USDC 协议流水(可追踪) + USDC 钱包流水（USDC ERC20 Transfer）(此项目未追踪)
- **Token 协议流水**: Split/Merge/Redemption/Convert 中的 token 变动
- **Token 账户流水**: ERC1155 Transfer（含用户间直接转账）
- **USDC 协议流水**: OrderFilled/Split/Merge/Redemption 中的 USDC 变动
- **USDC 钱包流水**: USDC ERC20 Transfer（充值/提现，未追踪）
- **成交价 ≠ 市价**: 只有历史成交价，没有实时 bid/ask 报价

**Split/Merge/Convert/Redemption 使用场景**:

1. **Split(铸造)— 市场进行中**
   - 操作: USDC → YES + NO (固定 $0.50/$0.50)
   - 做市: 铸造后挂单卖双边, 提供流动性
   - 套利: 当 YES + NO 市场价之和 > $1 时, 铸造后卖双边获利
   - 方向性建仓: 当看空方流动性好时, 铸造后卖看空方, 建仓看多方

2. **Merge(销毁)— 市场进行中**
   - 操作: YES + NO → USDC (固定 $0.50/$0.50)
   - 做市: 买入双边后销毁, 退出流动性
   - 套利: 当 YES + NO 市场价之和 < $1 时, 买双边后销毁获利
   - 方向性平仓: 当看多方流动性差时, 买看空方后销毁双边, 平仓看多方

3. **Convert(转换)— NegRisk市场进行中**
   - 操作: M 个选项的 NO tokens → (M-1) USDC（仅限 NegRisk 多选项互斥市场）
   - 原理: N 个互斥选项的 NO 组合 = "所有选项都不赢" = 不可能, 所以 M 个 NO 的价值是 M-1
   - 套利: 若 M 个 NO tokens 总成本 < (M-1) USDC, 买入后 convert 获利

4. **Redemption(赎回)— 市场结算后**
   - 操作: tokens → USDC (只能在市场结算后操作)
   - 用途: 赎回 winning tokens 获得收益, losing tokens 归零 (价格由 payoutNumerators/payoutDenominator 决定)

## Flow

```
Stage 1: eth_getLogs → raw_log (暂存JSON, ~15min)
Stage 2: raw_log → 最终表 (纯SQL转换, ~2min)
```

## 合约

| 合约               | 地址                                       | 起始区块   |
| ------------------ | ------------------------------------------ | ---------- |
| ConditionalTokens  | 0x4D97DCd97eC945f40cF65F87097ACe5EA0476045 | 4,027,499  |
| CTFExchange        | 0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E | 35,887,522 |
| NegRiskCTFExchange | 0xC5d563A36AE78145C45a50134d48A1215220f80a | 51,405,773 |
| NegRiskAdapter     | 0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296 | 50,748,168 |

**Collateral**:

- FiatToken V2.2 (Circle): Native USDC: **polymarket协议不使用**: `0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359` (2023年10月推出, Circle 原生发行)
- Polygon PoS Bridge: 普通市场 (CTFExchange): USDC.e(Bridged) (`0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174`) (2020年9月推出, 从以太坊桥接)
- NegRiskAdapter (Polymarket): Wrapped Collateral (`0x3a3bd7bb9528e159577f7c2e685cc81a765002e2`) - 1:1 包装的 USDC.e (为了支持Convert操作, 原生 CTF 里做不到, 需要wrap一下)

```
polymarket-indexer/service$ /bin/python3 /home/chuyin/work/poly/scripts/scan_events.py

  Polymarket 事件扫描  (head=83,000,000)

  块进度:   65,467,999 / 83,000,000  (78.9%)  -  速度: 535 blk/s  ETA: 546.4 min

  合约                事件                          总计       首block       末block      2020       2021       2022       2023       2024       2025       2026
  --------------------------------------------------------------------------------------------------------------------------------------------------------------
  ConditionalTokens   TransferSingle          47,348,105     4,028,711    65,467,999         0  1,322,645  1,455,391    493,401 39,332,437  4,744,231          0
  ConditionalTokens   TransferBatch           30,683,111     4,028,608    65,467,999         0  1,201,193  1,831,804    378,200 25,262,411  2,009,503          0
  ConditionalTokens   ConditionPreparation        28,936     4,027,499    65,452,567         0      1,026      7,388      4,225     15,363        934          0
  ConditionalTokens   ConditionResolution         22,696     6,205,069    65,466,968         0        749      3,730      3,805     13,545        867          0
  ConditionalTokens   PositionSplit           12,798,752     4,028,608    65,467,999         0    616,869    968,444    230,975 10,124,784    857,680          0
  ConditionalTokens   PositionsMerge           5,773,935     4,028,724    65,467,998         0    489,725    657,668    113,704  4,149,360    363,478          0
  ConditionalTokens   PayoutRedemption         2,602,647     6,233,711    65,467,995         0    332,502    246,240     70,204  1,729,971    223,730          0
  CTFExchange         OrderFilled              9,552,442    35,896,869    65,467,999         0          0          0    246,898  7,504,812  1,800,732          0
  CTFExchange         OrdersMatched            4,063,600    35,896,869    65,467,999         0          0          0    101,015  3,176,077    786,508          0
  CTFExchange         TokenRegistered             22,230    35,887,522    65,452,605         0          0          0      6,744     14,524        962          0
  NegRiskCTFExchange  OrderFilled             32,890,431    51,408,357    65,467,999         0          0          0          0 30,171,094  2,719,337          0
  NegRiskCTFExchange  OrdersMatched           15,252,068    51,408,357    65,467,999         0          0          0          0 14,004,104  1,247,964          0
  NegRiskCTFExchange  TokenRegistered             16,946    51,405,773    65,451,107         0          0          0          0     16,042        904          0
  NegRiskAdapter      MarketPrepared               1,418    50,748,168    65,450,786         0          0          0          0      1,337         81          0
  NegRiskAdapter      QuestionPrepared             8,458    50,750,368    65,451,080         0          0          0          0      8,005        453          0
  NegRiskAdapter      PositionsConverted         276,724    50,861,311    65,467,758         0          0          0          0    261,040     15,684          0
  NegRiskAdapter      OutcomeReported              7,006    51,868,332    65,456,813         0          0          0          0      6,544        462          0
```

## 链上事件结构

### ConditionalTokens

| 事件                 | topic0                                                             |
| -------------------- | ------------------------------------------------------------------ |
| ConditionPreparation | 0xab3760c3bd2bb38b5bcf54dc79802ed67338b4cf29f3054ded67ed24661e4177 |
| PositionSplit        | 0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298 |
| PositionsMerge       | 0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca |
| TransferSingle       | 0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62 |
| TransferBatch        | 0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb |
| ConditionResolution  | 0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894 |
| PayoutRedemption     | 0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d |

**ConditionPreparation**

| 字段             | 类型    | indexed | 说明                |
| ---------------- | ------- | ------- | ------------------- |
| conditionId      | bytes32 | yes     | 条件 ID             |
| oracle           | address | yes     | oracle 地址         |
| questionId       | bytes32 | yes     | 问题 ID             |
| outcomeSlotCount | uint256 | no      | 固定为 2            |
| tx_hash          | bytes32 | meta    | log.transactionHash |
| block_number     | uint64  | meta    | log.blockNumber     |
| log_index        | uint32  | meta    | log.logIndex        |

**PositionSplit**

collateral → YES + NO (铸造)

| 字段               | 类型      | indexed | 说明                                                                                                                                        |
| ------------------ | --------- | ------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| stakeholder        | address   | yes     | 操作者                                                                                                                                      |
| collateralToken    | address   | no      | 普通市场: USDC.e (0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174); NegRisk市场: Wrapped Collateral (0x3a3bd7bb9528e159577f7c2e685cc81a765002e2) |
| parentCollectionId | bytes32   | yes     | 几乎总是 0x0                                                                                                                                |
| conditionId        | bytes32   | yes     | 条件 ID                                                                                                                                     |
| partition          | uint256[] | no      | [1,2] = YES+NO                                                                                                                              |
| amount             | uint256   | no      | 消耗的collateral数量 (6 decimals), 同时获得等量YES和NO token                                                                                |
| tx_hash            | bytes32   | meta    | log.transactionHash                                                                                                                         |
| block_number       | uint64    | meta    | log.blockNumber                                                                                                                             |
| log_index          | uint32    | meta    | log.logIndex                                                                                                                                |

**PositionsMerge**

YES + NO → Collateral (销毁)

| 字段               | 类型      | indexed | 说明                                                                                                                                        |
| ------------------ | --------- | ------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| stakeholder        | address   | yes     | 操作者                                                                                                                                      |
| collateralToken    | address   | no      | 普通市场: USDC.e (0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174); NegRisk市场: Wrapped Collateral (0x3a3bd7bb9528e159577f7c2e685cc81a765002e2) |
| parentCollectionId | bytes32   | yes     | 几乎总是 0x0                                                                                                                                |
| conditionId        | bytes32   | yes     | 条件 ID                                                                                                                                     |
| partition          | uint256[] | no      | [1,2] = YES+NO                                                                                                                              |
| amount             | uint256   | no      | 销毁的YES+NO数量 (6 decimals), 同时获得等量collateral                                                                                       |
| tx_hash            | bytes32   | meta    | log.transactionHash                                                                                                                         |
| block_number       | uint64    | meta    | log.blockNumber                                                                                                                             |
| log_index          | uint32    | meta    | log.logIndex                                                                                                                                |

**TransferSingle / TransferBatch**

TransferSingle + TransferBatch 覆盖**所有**持仓变化, 不会重合:

| 场景         | 事件   | 原因                                    |
| ------------ | ------ | --------------------------------------- |
| Split铸造    | Batch  | 同时mint YES+NO 两个token               |
| Merge销毁    | Batch  | 同时burn YES+NO 两个token               |
| 交易撮合     | Single | 只转移一个token (买YES或买NO)           |
| 赎回(双边)   | Batch  | 同时burn YES+NO                         |
| 赎回(单边)   | Single | 只burn一个position                      |
| NegRisk转换  | Batch  | 多个NO tokens burn → Wrapped Collateral |
| 用户直接转账 | 取决于 | 转几种token就用哪个                     |

**TransferSingle 字段**

| 字段         | 类型    | indexed | 说明                                                                                                                                                        |
| ------------ | ------- | ------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| operator     | address | yes     | 执行操作的地址。**合约**: CTFExchange(交易撮合)/NegRiskAdapter(Split/Merge/Convert)/NegRiskCTFExchange(NegRisk交易); **用户**: 直接调用safeTransferFrom转账 |
| from         | address | yes     | 发送方。**0x0=mint**                                                                                                                                        |
| to           | address | yes     | 接收方。**0x0=burn**                                                                                                                                        |
| id           | uint256 | no      | positionId (ERC1155 tokenId), 256位整数, 由 keccak256(collateralToken, collectionId) 计算                                                                   |
| value        | uint256 | no      | token数量, 6 decimals (1 token = 1,000,000 units)                                                                                                           |
| tx_hash      | bytes32 | meta    | log.transactionHash                                                                                                                                         |
| block_number | uint64  | meta    | log.blockNumber                                                                                                                                             |
| log_index    | uint32  | meta    | log.logIndex                                                                                                                                                |

**TransferBatch 字段**

| 字段         | 类型      | indexed | 说明                 |
| ------------ | --------- | ------- | -------------------- |
| operator     | address   | yes     | 同TransferSingle     |
| from         | address   | yes     | 发送方。**0x0=mint** |
| to           | address   | yes     | 接收方。**0x0=burn** |
| ids          | uint256[] | no      | positionId 数组      |
| values       | uint256[] | no      | 数量数组, 6 decimals |
| tx_hash      | bytes32   | meta    | log.transactionHash  |
| block_number | uint64    | meta    | log.blockNumber      |
| log_index    | uint32    | meta    | log.logIndex         |

**ConditionResolution**

| 字段             | 类型      | indexed | 说明                                |
| ---------------- | --------- | ------- | ----------------------------------- |
| conditionId      | bytes32   | yes     | 条件 ID                             |
| oracle           | address   | yes     | UmaCtfAdapter 地址                  |
| questionId       | bytes32   | yes     | 问题 ID                             |
| outcomeSlotCount | uint256   | no      | 固定为 2                            |
| payoutNumerators | uint256[] | no      | [1,0]=YES赢, [0,1]=NO赢, [1,1]=平局 |
| tx_hash          | bytes32   | meta    | log.transactionHash                 |
| block_number     | uint64    | meta    | log.blockNumber                     |
| log_index        | uint32    | meta    | log.logIndex                        |

**PayoutRedemption**

| 字段               | 类型      | indexed | 说明                                                         |
| ------------------ | --------- | ------- | ------------------------------------------------------------ |
| redeemer           | address   | yes     | 操作者                                                       |
| collateralToken    | address   | yes     | 普通市场: USDC.e; NegRisk市场: Wrapped Collateral (地址同上) |
| parentCollectionId | bytes32   | yes     | 几乎总是 0x0                                                 |
| conditionId        | bytes32   | no      | 条件 ID                                                      |
| indexSets          | uint256[] | no      | 赎回的position组合: [1]=仅YES, [2]=仅NO, [1,2]=两者都赎回    |
| payout             | uint256   | no      | 获得的collateral (6 decimals), 可能为0(输家token赎回值为0)   |
| tx_hash            | bytes32   | meta    | log.transactionHash                                          |
| block_number       | uint64    | meta    | log.blockNumber                                              |
| log_index          | uint32    | meta    | log.logIndex                                                 |

### CTFExchange / NegRiskCTFExchange

| 事件            | topic0                                                             |
| --------------- | ------------------------------------------------------------------ |
| TokenRegistered | 0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d |
| OrderFilled     | 0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6 |
| OrdersMatched   | 0x63bf4d16b7fa898ef4c4b2b6d90fd201e9c56313b65638af6088d149d2ce956c |

**TokenRegistered**

| 字段         | 类型    | indexed | 说明                              |
| ------------ | ------- | ------- | --------------------------------- |
| token0       | uint256 | yes     | YES tokenId                       |
| token1       | uint256 | yes     | NO tokenId                        |
| conditionId  | bytes32 | yes     | 条件 ID                           |
| tx_hash      | bytes32 | meta    | log.transactionHash               |
| block_number | uint64  | meta    | log.blockNumber                   |
| log_index    | uint32  | meta    | log.logIndex                      |
| exchange     | TEXT    | meta    | CTFExchange 或 NegRiskCTFExchange |

**OrderFilled**

买卖方向: makerAssetId=0 → maker出collateral换token → maker是买方。价格=makerAmountFilled/takerAmountFilled

| 字段              | 类型    | indexed | 说明                                           |
| ----------------- | ------- | ------- | ---------------------------------------------- |
| orderHash         | bytes32 | yes     | 订单哈希                                       |
| maker             | address | yes     | 挂单方                                         |
| taker             | address | yes     | 吃单方                                         |
| makerAssetId      | uint256 | no      | maker给出的资产。**0=collateral**, 非0=tokenId |
| takerAssetId      | uint256 | no      | taker给出的资产。**0=collateral**, 非0=tokenId |
| makerAmountFilled | uint256 | no      | maker给出的数量 (6 decimals)                   |
| takerAmountFilled | uint256 | no      | taker给出的数量 (6 decimals)                   |
| fee               | uint256 | no      | 手续费 (collateral, 6 decimals)                |
| tx_hash           | bytes32 | meta    | log.transactionHash                            |
| block_number      | uint64  | meta    | log.blockNumber                                |
| log_index         | uint32  | meta    | log.logIndex                                   |
| exchange          | TEXT    | meta    | CTFExchange 或 NegRiskCTFExchange              |

**OrdersMatched**

| 字段              | 类型    | indexed | 说明                                                 |
| ----------------- | ------- | ------- | ---------------------------------------------------- |
| takerOrderHash    | bytes32 | yes     | taker 订单哈希                                       |
| takerOrderMaker   | address | yes     | **名字易混淆**: "taker订单的maker", 即发起吃单的用户 |
| makerAssetId      | uint256 | no      | maker给出的资产。**0=collateral**, 非0=tokenId       |
| takerAssetId      | uint256 | no      | taker给出的资产。**0=collateral**, 非0=tokenId       |
| makerAmountFilled | uint256 | no      | maker给出的数量 (6 decimals)                         |
| takerAmountFilled | uint256 | no      | taker给出的数量 (6 decimals)                         |
| tx_hash           | bytes32 | meta    | log.transactionHash                                  |
| block_number      | uint64  | meta    | log.blockNumber                                      |
| log_index         | uint32  | meta    | log.logIndex                                         |
| exchange          | TEXT    | meta    | CTFExchange 或 NegRiskCTFExchange                    |

### NegRiskAdapter

| 事件               | topic0                                                             |
| ------------------ | ------------------------------------------------------------------ |
| MarketPrepared     | 0xf059ab16d1ca60e123eab60e3c02b68faf060347c701a5d14885a8e1def7b3a8 |
| QuestionPrepared   | 0xaac410f87d423a922a7b226ac68f0c2eaf5bf6d15e644ac0758c7f96e2c253f7 |
| OutcomeReported    | 0x9e9fa7fd355160bd4cd3f22d4333519354beff1f5689bde87f2c5e63d8d484b2 |
| PositionsConverted | 0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e |

**MarketPrepared**

| 字段         | 类型    | indexed | 说明                 |
| ------------ | ------- | ------- | -------------------- |
| marketId     | bytes32 | yes     | 市场 ID              |
| oracle       | address | yes     | NegRiskOperator 地址 |
| feeBips      | uint256 | no      | 手续费率 (万分比)    |
| data         | bytes   | no      | 市场元数据           |
| tx_hash      | bytes32 | meta    | log.transactionHash  |
| block_number | uint64  | meta    | log.blockNumber      |
| log_index    | uint32  | meta    | log.logIndex         |

**QuestionPrepared**

| 字段          | 类型    | indexed | 说明                               |
| ------------- | ------- | ------- | ---------------------------------- |
| marketId      | bytes32 | yes     | 市场 ID                            |
| questionId    | bytes32 | yes     | keccak256(marketId, questionIndex) |
| questionIndex | uint256 | no      | 问题索引                           |
| data          | bytes   | no      | 问题元数据                         |
| tx_hash       | bytes32 | meta    | log.transactionHash                |
| block_number  | uint64  | meta    | log.blockNumber                    |
| log_index     | uint32  | meta    | log.logIndex                       |

**OutcomeReported**

| 字段         | 类型    | indexed | 说明                |
| ------------ | ------- | ------- | ------------------- |
| marketId     | bytes32 | yes     | 市场 ID             |
| questionId   | bytes32 | yes     | 问题 ID             |
| outcome      | bool    | no      | 结果                |
| tx_hash      | bytes32 | meta    | log.transactionHash |
| block_number | uint64  | meta    | log.blockNumber     |
| log_index    | uint32  | meta    | log.logIndex        |

**PositionsConverted**

NegRisk转换: M 个 NO tokens burn → (M-1) Wrapped Collateral (利用互斥选项的逻辑冗余套利)

| 字段         | 类型    | indexed | 说明                                                                  |
| ------------ | ------- | ------- | --------------------------------------------------------------------- |
| stakeholder  | address | yes     | 操作者                                                                |
| marketId     | bytes32 | yes     | NegRisk市场ID, 一个市场包含多个互斥问题(如"谁会赢得选举": A/B/C/其他) |
| indexSet     | uint256 | yes     | **bitmap**: 转换了哪些NO。如6=0b110表示第2和第3个NO token             |
| amount       | uint256 | no      | 每个被转换position的数量 (6 decimals)                                 |
| tx_hash      | bytes32 | meta    | log.transactionHash                                                   |
| block_number | uint64  | meta    | log.blockNumber                                                       |
| log_index    | uint32  | meta    | log.logIndex                                                          |

### ID 计算

| ID                                                | 计算方式                                                   |
| ------------------------------------------------- | ---------------------------------------------------------- |
| conditionId                                       | keccak256(oracle, questionId, outcomeSlotCount)            |
| collectionId (parentCollectionId=0x0, indexSet=1) | keccak256(conditionId, 1) → YES                            |
| collectionId (parentCollectionId=0x0, indexSet=2) | keccak256(conditionId, 2) → NO                             |
| positionId                                        | keccak256(collateralToken, collectionId) → ERC1155 tokenId |

## Stage 1: eth_getLogs → 结构化表

### order_filled

| column       | 类型       | 来源        | 处理                                          |
| ------------ | ---------- | ----------- | --------------------------------------------- |
| block_number | BIGINT PK  | log         |                                               |
| log_index    | INTEGER PK | log         |                                               |
| exchange     | TEXT       | log.address | "CTF" \| "NegRisk"                            |
| maker        | BLOB(20)   | OrderFilled | $.maker                                       |
| taker        | BLOB(20)   | OrderFilled | $.taker                                       |
| token_id     | BLOB(32)   | 计算        | makerAssetId==0 ? takerAssetId : makerAssetId |
| side         | INTEGER    | 计算        | makerAssetId==0 ? 1(Buy) : 2(Sell)            |
| usdc_amount  | BIGINT     | 计算        | collateral数量 (6 decimals)                   |
| token_amount | BIGINT     | 计算        | token数量 (6 decimals)                        |
| fee          | BIGINT     | OrderFilled | $.fee (6 decimals)                            |

### split

| column       | 类型       | 来源          | 处理                        |
| ------------ | ---------- | ------------- | --------------------------- |
| block_number | BIGINT PK  | log           |                             |
| log_index    | INTEGER PK | log           |                             |
| stakeholder  | BLOB(20)   | PositionSplit | $.stakeholder               |
| condition_id | BLOB(32)   | PositionSplit | $.conditionId               |
| amount       | BIGINT     | PositionSplit | USDC消耗 = YES获得 = NO获得 |

### merge

| column       | 类型       | 来源           | 处理                        |
| ------------ | ---------- | -------------- | --------------------------- |
| block_number | BIGINT PK  | log            |                             |
| log_index    | INTEGER PK | log            |                             |
| stakeholder  | BLOB(20)   | PositionsMerge | $.stakeholder               |
| condition_id | BLOB(32)   | PositionsMerge | $.conditionId               |
| amount       | BIGINT     | PositionsMerge | USDC获得 = YES消耗 = NO消耗 |

### redemption

| column       | 类型       | 来源             | 处理                        |
| ------------ | ---------- | ---------------- | --------------------------- |
| block_number | BIGINT PK  | log              |                             |
| log_index    | INTEGER PK | log              |                             |
| redeemer     | BLOB(20)   | PayoutRedemption | $.redeemer                  |
| condition_id | BLOB(32)   | PayoutRedemption | $.conditionId               |
| index_sets   | INTEGER    | PayoutRedemption | bitmap: 1=YES, 2=NO, 3=both |
| payout       | BIGINT     | PayoutRedemption | USDC获得 (6 decimals)       |

### convert

| column       | 类型       | 来源               | 处理                 |
| ------------ | ---------- | ------------------ | -------------------- |
| block_number | BIGINT PK  | log                |                      |
| log_index    | INTEGER PK | log                |                      |
| stakeholder  | BLOB(20)   | PositionsConverted | $.stakeholder        |
| market_id    | BLOB(32)   | PositionsConverted | $.marketId           |
| index_set    | BIGINT     | PositionsConverted | bitmap: 哪些NO被转换 |
| amount       | BIGINT     | PositionsConverted | 每个position的数量   |

### transfer

**过滤** (只保留用户直接转账):

- `from != 0x0 AND to != 0x0` (跳过mint/burn)
- `operator NOT IN (CTFExchange, NegRiskCTFExchange, NegRiskAdapter)` (跳过合约操作，已被order_filled/split/merge/convert覆盖)

| column       | 类型      | 来源     | 处理                                        |
| ------------ | --------- | -------- | ------------------------------------------- |
| block_number | BIGINT PK | log      |                                             |
| log_index    | BIGINT PK | 计算     | log_index \* 1000 + sub_index (Batch拆分用) |
| from_addr    | BLOB(20)  | Transfer | $.from (≠0x0)                               |
| to_addr      | BLOB(20)  | Transfer | $.to (≠0x0)                                 |
| token_id     | BLOB(32)  | Transfer | $.id                                        |
| amount       | BIGINT    | Transfer | $.value                                     |

### token_map

| column       | 类型        | 来源            | 处理                                                      |
| ------------ | ----------- | --------------- | --------------------------------------------------------- |
| token_id     | BLOB(32) PK | TokenRegistered | token0 或 token1                                          |
| condition_id | BLOB(32)    | TokenRegistered | $.conditionId                                             |
| exchange     | TEXT        | log.address     | "CTF" \| "NegRisk"                                        |
| is_yes       | INTEGER     | 计算            | 仅处理 token0 < token1 的行 → token0=YES(1), token1=NO(0) |

### condition

| column            | 类型        | 来源                 | 处理                                                 |
| ----------------- | ----------- | -------------------- | ---------------------------------------------------- |
| condition_id      | BLOB(32) PK | ConditionPreparation | $.conditionId                                        |
| oracle            | BLOB(20)    | ConditionPreparation | $.oracle                                             |
| question_id       | BLOB(32)    | ConditionPreparation | $.questionId                                         |
| payout_numerators | TEXT        | ConditionResolution  | NULL=未结算, "[1,0]"=YES赢, "[0,1]"=NO赢, "[1,1]"=平 |
| resolution_block  | BIGINT      | ConditionResolution  | NULL=未结算                                          |

### neg_risk_market

| column    | 类型        | 来源           | 处理                                     |
| --------- | ----------- | -------------- | ---------------------------------------- |
| market_id | BLOB(32) PK | MarketPrepared | $.marketId                               |
| oracle    | BLOB(20)    | MarketPrepared | $.oracle                                 |
| fee_bips  | INTEGER     | MarketPrepared | $.feeBips                                |
| data      | BLOB        | MarketPrepared | $.data (ABI编码: title, description, id) |

### neg_risk_question

| column         | 类型        | 来源             | 处理                                        |
| -------------- | ----------- | ---------------- | ------------------------------------------- |
| question_id    | BLOB(32) PK | QuestionPrepared | $.questionId                                |
| market_id      | BLOB(32)    | QuestionPrepared | $.marketId                                  |
| question_index | INTEGER     | QuestionPrepared | $.questionIndex                             |
| data           | BLOB        | QuestionPrepared | $.data (ABI编码: question, description, id) |

**关键连接**: `neg_risk_question.question_id` = `condition.question_id`

### sync_state

| key        | 含义           |
| ---------- | -------------- |
| last_block | 已同步到的区块 |

## 索引

| 表                | 索引        | 用途         |
| ----------------- | ----------- | ------------ |
| order_filled      | maker       | 按用户查交易 |
| order_filled      | taker       | 按用户查交易 |
| order_filled      | token_id    | 按市场查交易 |
| split             | stakeholder | 按用户查     |
| merge             | stakeholder | 按用户查     |
| redemption        | redeemer    | 按用户查     |
| convert           | stakeholder | 按用户查     |
| transfer          | from_addr   | 按用户查     |
| transfer          | to_addr     | 按用户查     |
| neg_risk_question | market_id   | 按市场查问题 |

## PnL 计算

```
PnL = Σ(Sell) + Σ(Merge) + Σ(Redemption) + Σ(Convert收益)
    - Σ(Buy) - Σ(Split) - Σ(Fee)
```

| 来源              | 加减 | 说明                            |
| ----------------- | ---- | ------------------------------- |
| order_filled Buy  | -    | 买入花费 USDC                   |
| order_filled Sell | +    | 卖出获得 USDC                   |
| order_filled.fee  | -    | 手续费                          |
| split.amount      | -    | 铸造消耗 USDC                   |
| merge.amount      | +    | 销毁获得 USDC                   |
| redemption.payout | +    | 结算赎回                        |
| convert           | +    | (popcount(index_set)-1)\*amount |
