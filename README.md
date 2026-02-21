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
| ConditionPreparation |                                            |                                                  |                                                       | condition  |
| ConditionResolution  |                                            |                                                  |                                                       | condition  |
| PositionSplit        |                                            |                        ✓                         |                           ✓                           |            |
| PositionsMerge       |                                            |                        ✓                         |                           ✓                           |            |
| PayoutRedemption     |                                            |                        ✓                         |                           ✓                           |            |
| FPMMCreation         |                                            |                                                  |                                                       |    fpmm    |
| FPMMBuy              |                                            |                        ✓                         |                           ✓                           |            |
| FPMMSell             |                                            |                        ✓                         |                           ✓                           |            |
| FPMMFundingAdded     |                                            |                        ✓                         |                           ✓                           |            |
| FPMMFundingRemoved   |                                            |                        ✓                         |                           ✓                           |            |
| OrderFilled          |                                            |                        ✓                         |                           ✓                           |            |
| OrdersMatched        |                                            |                                                  |                                                       |            |
| TokenRegistered      |                                            |                                                  |                                                       | token_map  |
| MarketPrepared       |                                            |                                                  |                                                       |  neg_risk  |
| QuestionPrepared     |                                            |                                                  |                                                       |  neg_risk  |
| PositionsConverted   |                                            |                        ✓                         |                           ✓                           |            |
| OutcomeReported      |                                            |                                                  |                                                       |  neg_risk  |

- **Token 持仓** = Token 协议流水(可追踪) + Token 账户流水(可追踪)
- **USDC 持仓** = USDC 协议流水(可追踪) + USDC 钱包流水（USDC ERC20 Transfer）(此项目未追踪)
- **Token 协议流水** = 特殊操作 + 与市场交互
  - 特殊操作 (两个时代通用): Split/Merge/Redemption/Convert (YES+NO双边变动)
  - 订单簿时代: OrderFilled (Taker+Maker, 单边)
  - AMM时代: FPMMBuy/FPMMSell (Taker, 单边) + FPMMFundingAdded/Removed (LP池 Maker, 双边按池子比例)
- **Token 账户流水**: ERC1155 Transfer（含用户间直接转账）(TransferSingle/TransferBatch)
- **USDC 协议流水**:
  - 特殊操作 (两个时代通用): Split/Merge/Redemption/Convert (USDC双边变动)
  - 订单簿时代: OrderFilled 中的 USDC 变动
  - AMM时代: FPMMBuy/FPMMSell/FPMMFundingAdded/FPMMFundingRemoved 中的 USDC 变动
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

| 合约               | 地址                                       | 起始区块   | 备注                       |
| ------------------ | ------------------------------------------ | ---------- | -------------------------- |
| ConditionalTokens  | 0x4D97DCd97eC945f40cF65F87097ACe5EA0476045 | 4,027,499  | 核心token合约              |
| FPMMFactory        | 0x8B9805A2f595B6705e74F7310829f2d299D21522 | 4,023,693  | 早期AMM, CTFExchange上线前 |
| CTFExchange        | 0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E | 35,887,522 | 订单簿交易所               |
| NegRiskCTFExchange | 0xC5d563A36AE78145C45a50134d48A1215220f80a | 51,405,773 | NegRisk订单簿              |
| NegRiskAdapter     | 0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296 | 50,748,168 | NegRisk市场管理            |

**权限与中心化**:

- 链上协议完全开放，任何人都可以创建prepareCondition和FPMMCreation(协议仍可使用，只是已经被官方自己废弃)
- 但Polymarket网站只显示官方创建的市场，第三方创建的市场无流动性, 而且不被平台显示
- 结算权在oracle手中 (conditionId = keccak256(oracle, questionId, outcomeSlotCount))
- 本质是"协议开放 + 运营中心化"模式

**Collateral**:

- FiatToken V2.2 (Circle): Native USDC: **polymarket协议不使用**: `0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359` (2023年10月推出, Circle 原生发行)
- Polygon PoS Bridge: 普通市场 (CTFExchange): USDC.e(Bridged) (`0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174`) (2020年9月推出, 从以太坊桥接)
- NegRiskAdapter (Polymarket): Wrapped Collateral (`0x3a3bd7bb9528e159577f7c2e685cc81a765002e2`) - 1:1 包装的 USDC.e (为了支持Convert操作, 原生 CTF 里做不到, 需要wrap一下)

```
  Polymarket 事件扫描  (head=83,000,000)

  块进度:   62,889,999 / 83,000,000  (75.8%)  -  速度: 458 blk/s  ETA: 731.8 min

  合约                事件                          总计       首block       末block       2020       2021       2022       2023       2024       2025       2026
  ---------------------------------------------------------------------------------------------------------------------------------------------------------------
  ConditionalTokens   TransferSingle          15,922,185     4,028,711    62,889,999          0  1,319,462  1,457,584    493,197 12,651,942          0          0
  ConditionalTokens   TransferBatch           12,080,571     4,028,608    62,889,999          0  1,197,332  1,834,233    379,225  8,669,781          0          0
  ConditionalTokens   ConditionPreparation        23,270     4,027,499    62,887,016          0      1,012      7,388      4,236     10,634          0          0
  ConditionalTokens   ConditionResolution         17,159     6,205,069    62,889,281          0        743      3,734      3,797      8,885          0          0
  ConditionalTokens   PositionSplit            5,692,159     4,028,608    62,889,999          0    614,470    970,174    231,322  3,876,193          0          0
  ConditionalTokens   PositionsMerge           2,500,809     4,028,724    62,889,998          0    488,700    658,156    114,156  1,239,797          0          0
  ConditionalTokens   PayoutRedemption         1,284,556     6,233,711    62,889,985          0    331,252    247,416     70,202    635,686          0          0
  FPMMFactory         FPMMCreation                13,642     4,027,846    62,760,657          0        981      7,230      4,240      1,191          0          0
  FPMM                FPMMBuy                  1,068,771     4,028,711    62,889,297          0    264,187    640,882    161,829      1,873          0          0
  FPMM                FPMMSell                 1,064,706     4,028,724    62,876,506          0    448,241    540,392     75,316        757          0          0
  FPMM                FPMMFundingAdded           186,350     4,028,608    61,759,915          0     49,219    108,232     28,689        210          0          0
  FPMM                FPMMFundingRemoved         168,320     4,350,124    62,752,190          0     44,937     97,533     25,691        159          0          0
  CTFExchange         OrderFilled              3,215,134    35,896,869    62,889,996          0          0          0    245,770  2,969,364          0          0
  CTFExchange         OrdersMatched            1,324,672    35,896,869    62,889,996          0          0          0    100,553  1,224,119          0          0
  CTFExchange         TokenRegistered             16,372    35,887,522    62,887,050          0          0          0      6,738      9,634          0          0
  NegRiskCTFExchange  OrderFilled              9,044,559    51,408,357    62,889,999          0          0          0          0  9,044,559          0          0
  NegRiskCTFExchange  OrdersMatched            4,105,482    51,408,357    62,889,999          0          0          0          0  4,105,482          0          0
  NegRiskCTFExchange  TokenRegistered             11,444    51,405,773    62,881,562          0          0          0          0     11,444          0          0
  NegRiskAdapter      MarketPrepared                 947    50,748,168    62,881,479          0          0          0          0        947          0          0
  NegRiskAdapter      QuestionPrepared             5,732    50,750,368    62,881,531          0          0          0          0      5,732          0          0
  NegRiskAdapter      PositionsConverted         119,043    50,861,311    62,889,978          0          0          0          0    119,043          0          0
  NegRiskAdapter      OutcomeReported              4,239    51,868,332    62,889,281          0          0          0          0      4,239          0          0
```

## 链上事件结构

### ConditionalTokens

| 事件                 | topic0                                                             |
| -------------------- | ------------------------------------------------------------------ |
| TransferSingle       | 0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62 |
| TransferBatch        | 0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb |
| ConditionPreparation | 0xab3760c3bd2bb38b5bcf54dc79802ed67338b4cf29f3054ded67ed24661e4177 |
| ConditionResolution  | 0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894 |
| PositionSplit        | 0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298 |
| PositionsMerge       | 0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca |
| PayoutRedemption     | 0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d |

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
| OrderFilled     | 0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6 |
| OrdersMatched   | 0x63bf4d16b7fa898ef4c4b2b6d90fd201e9c56313b65638af6088d149d2ce956c |
| TokenRegistered | 0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d |

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

### NegRiskAdapter

| 事件               | topic0                                                             |
| ------------------ | ------------------------------------------------------------------ |
| MarketPrepared     | 0xf059ab16d1ca60e123eab60e3c02b68faf060347c701a5d14885a8e1def7b3a8 |
| QuestionPrepared   | 0xaac410f87d423a922a7b226ac68f0c2eaf5bf6d15e644ac0758c7f96e2c253f7 |
| PositionsConverted | 0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e |
| OutcomeReported    | 0x9e9fa7fd355160bd4cd3f22d4333519354beff1f5689bde87f2c5e63d8d484b2 |

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

**OutcomeReported**

| 字段         | 类型    | indexed | 说明                |
| ------------ | ------- | ------- | ------------------- |
| marketId     | bytes32 | yes     | 市场 ID             |
| questionId   | bytes32 | yes     | 问题 ID             |
| outcome      | bool    | no      | 结果                |
| tx_hash      | bytes32 | meta    | log.transactionHash |
| block_number | uint64  | meta    | log.blockNumber     |
| log_index    | uint32  | meta    | log.logIndex        |

### FPMMFactory (早期AMM)

| 事件                            | topic0                                        |
| ------------------------------- | --------------------------------------------- |
| FixedProductMarketMakerCreation | (从Factory合约发出, 每个市场部署一个FPMM合约) |

**FixedProductMarketMakerCreation** (FPMMFactory发出)

| 字段                    | 类型      | indexed | 说明                      |
| ----------------------- | --------- | ------- | ------------------------- |
| creator                 | address   | yes     | FPMM创建者                |
| fixedProductMarketMaker | address   | no      | **新部署的FPMM合约地址**  |
| conditionalTokens       | address   | yes     | ConditionalTokens合约地址 |
| collateralToken         | address   | yes     | collateral地址 (USDC.e)   |
| conditionIds            | bytes32[] | no      | 关联的conditionId数组     |
| fee                     | uint256   | no      | 手续费率 (1e18 = 100%)    |

### FPMM合约 (动态部署, 每市场一个)

| 事件               | 角色  | 说明                              | 用户Token变动         |
| ------------------ | ----- | --------------------------------- | --------------------- |
| FPMMBuy            | Taker | 用户投入USDC，获得token           | 单边 (获得YES或NO)    |
| FPMMSell           | Taker | 用户卖出token，获得USDC           | 单边 (卖出YES或NO)    |
| FPMMFundingAdded   | LP    | LP投入USDC，按池子比例添加YES+NO  | 双边 (按池子比例)     |
| FPMMFundingRemoved | LP    | LP销毁shares，取回YES+NO + 手续费 | 双边 (按池子比例取回) |

fpmm_trade (Taker) + fpmm_funding (LP Maker结算) = 订单簿时代的 order_filled。

**FPMMBuy** (FPMM合约发出)

| 字段                | 类型    | indexed | 说明                                   |
| ------------------- | ------- | ------- | -------------------------------------- |
| fpmm                | address | meta    | log.address (动态部署, 需记录合约地址) |
| buyer               | address | yes     | 买家地址                               |
| investmentAmount    | uint256 | no      | 投入的USDC数量 (含手续费, 6 decimals)  |
| feeAmount           | uint256 | no      | 手续费 (6 decimals)                    |
| outcomeIndex        | uint256 | yes     | 0=YES, 1=NO                            |
| outcomeTokensBought | uint256 | no      | 获得的token数量 (6 decimals)           |

**FPMMSell** (FPMM合约发出)

| 字段              | 类型    | indexed | 说明                                    |
| ----------------- | ------- | ------- | --------------------------------------- |
| fpmm              | address | meta    | log.address (动态部署, 需记录合约地址)  |
| seller            | address | yes     | 卖家地址                                |
| returnAmount      | uint256 | no      | 获得的USDC数量 (不含手续费, 6 decimals) |
| feeAmount         | uint256 | no      | 手续费 (6 decimals)                     |
| outcomeIndex      | uint256 | yes     | 0=YES, 1=NO                             |
| outcomeTokensSold | uint256 | no      | 卖出的token数量 (6 decimals)            |

**FPMMFundingAdded** (FPMM合约发出)

| 字段         | 类型      | indexed | 说明                                   |
| ------------ | --------- | ------- | -------------------------------------- |
| fpmm         | address   | meta    | log.address (动态部署, 需记录合约地址) |
| funder       | address   | yes     | LP地址                                 |
| amountsAdded | uint256[] | no      | LP添加的YES/NO数量 (按池子当前比例)    |
| sharesMinted | uint256   | no      | 铸造的LP份额                           |

**FPMMFundingRemoved** (FPMM合约发出)

| 字段                         | 类型      | indexed | 说明                                   |
| ---------------------------- | --------- | ------- | -------------------------------------- |
| fpmm                         | address   | meta    | log.address (动态部署, 需记录合约地址) |
| funder                       | address   | yes     | LP地址                                 |
| amountsRemoved               | uint256[] | no      | LP取回的YES/NO token数量               |
| collateralRemovedFromFeePool | uint256   | no      | 从手续费池取出的USDC                   |
| sharesBurnt                  | uint256   | no      | 销毁的LP份额                           |

**FPMM内部机制**:

- FPMMBuy: 用户USDC→FPMM→`safeTransferFrom(FPMM, user, tokenId)`转单边token
- FPMMSell: 用户`safeTransferFrom(user, FPMM, tokenId)`→FPMM→转USDC给用户
- FPMMFundingAdded: 用户USDC→FPMM split成YES+NO→按池子比例添加→LP获得shares (多余tokens返还)
- FPMMFundingRemoved: LP销毁shares→按池子比例取回YES+NO→`safeBatchTransferFrom(FPMM, user, ids)`
- 这就是为什么Transfer里operator=FPMM地址的记录需要被过滤

### ID 计算

| ID                                                | 计算方式                                                   |
| ------------------------------------------------- | ---------------------------------------------------------- |
| conditionId                                       | keccak256(oracle, questionId, outcomeSlotCount)            |
| collectionId (parentCollectionId=0x0, indexSet=1) | keccak256(conditionId, 1) → YES                            |
| collectionId (parentCollectionId=0x0, indexSet=2) | keccak256(conditionId, 2) → NO                             |
| positionId                                        | keccak256(collateralToken, collectionId) → ERC1155 tokenId |

## Stage 1: eth_getLogs → 结构化表

### transfer

**过滤**

- `from != 0x0 AND to != 0x0` (跳过mint/burn)
- `operator NOT IN (CTFExchange, NegRiskCTFExchange, NegRiskAdapter)` (跳过合约操作，已被order_filled/split/merge/convert覆盖)
- `from NOT IN fpmm_addrs AND to NOT IN fpmm_addrs` (跳过FPMM相关transfer，已被fpmm_trade/fpmm_funding覆盖；fpmm_addrs = 数据库已有 + 当前batch新增)

| column       | 类型      | 来源     | 处理                                        |
| ------------ | --------- | -------- | ------------------------------------------- |
| block_number | BIGINT PK | log      |                                             |
| log_index    | BIGINT PK | 计算     | log_index \* 1000 + sub_index (Batch拆分用) |
| from_addr    | BLOB(20)  | Transfer | $.from (≠0x0)                               |
| to_addr      | BLOB(20)  | Transfer | $.to (≠0x0)                                 |
| token_id     | BLOB(32)  | Transfer | $.id                                        |
| amount       | BIGINT    | Transfer | $.value                                     |

### condition

ConditionPreparation 时 INSERT，ConditionResolution 时 UPDATE 同一行。

| column            | 类型        | 来源                 | 处理                                                         |
| ----------------- | ----------- | -------------------- | ------------------------------------------------------------ |
| condition_id      | BLOB(32) PK | ConditionPreparation | $.conditionId                                                |
| oracle            | BLOB(20)    | ConditionPreparation | $.oracle                                                     |
| question_id       | BLOB(32)    | ConditionPreparation | $.questionId                                                 |
| payout_numerators | TEXT        | ConditionResolution  | UPDATE, NULL=未结算, "[1,0]"=YES赢, "[0,1]"=NO赢, "[1,1]"=平 |
| resolution_block  | BIGINT      | ConditionResolution  | UPDATE, NULL=未结算                                          |

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

### fpmm (FPMM合约映射表)

记录动态部署的 FPMM 合约地址 → conditionId 映射，用于关联 fpmm_trade/fpmm_funding 和过滤 transfer。

| column       | 类型        | 来源                            | 处理                      |
| ------------ | ----------- | ------------------------------- | ------------------------- |
| fpmm_addr    | BLOB(20) PK | FixedProductMarketMakerCreation | $.fixedProductMarketMaker |
| condition_id | BLOB(32)    | FixedProductMarketMakerCreation | $.conditionIds[0]         |
| fee          | BIGINT      | FixedProductMarketMakerCreation | $.fee (1e18 scale)        |
| block_number | BIGINT      | log                             |                           |

### fpmm_trade (AMM Taker交易)

AMM 的 Taker 交易记录 (FPMMBuy/FPMMSell)，单边操作。

| column        | 类型       | 来源         | 处理                                              |
| ------------- | ---------- | ------------ | ------------------------------------------------- |
| block_number  | BIGINT PK  | log          |                                                   |
| log_index     | INTEGER PK | log          |                                                   |
| fpmm_addr     | BLOB(20)   | log.address  | 发出事件的FPMM合约地址                            |
| trader        | BLOB(20)   | FPMMBuy/Sell | $.buyer 或 $.seller                               |
| side          | INTEGER    | 事件类型     | 1=Buy, 2=Sell                                     |
| outcome_index | INTEGER    | FPMMBuy/Sell | 0=YES, 1=NO                                       |
| usdc_amount   | BIGINT     | FPMMBuy/Sell | Buy: investmentAmount; Sell: returnAmount         |
| token_amount  | BIGINT     | FPMMBuy/Sell | Buy: outcomeTokensBought; Sell: outcomeTokensSold |
| fee           | BIGINT     | FPMMBuy/Sell | $.feeAmount (6 decimals)                          |

### fpmm_funding (LP操作)

AMM 的 LP Maker 操作记录。LP 按池子当前比例添加/取回 YES+NO，shares 是整个池子的份额（不分 YES/NO）, 相当于强行做多流动性冲裕方。

| column       | 类型       | 来源                 | 处理                                  |
| ------------ | ---------- | -------------------- | ------------------------------------- |
| block_number | BIGINT PK  | log                  |                                       |
| log_index    | INTEGER PK | log                  |                                       |
| fpmm_addr    | BLOB(20)   | log.address          | 发出事件的FPMM合约地址                |
| funder       | BLOB(20)   | FundingAdded/Removed | LP地址                                |
| side         | INTEGER    | 事件类型             | 1=Added, 2=Removed                    |
| amount0      | BIGINT     | amountsAdded/Removed | LP添加/取回的YES数量 (按池子当前比例) |
| amount1      | BIGINT     | amountsAdded/Removed | LP添加/取回的NO数量 (按池子当前比例)  |
| shares       | BIGINT     | sharesMinted/Burnt   | LP份额变化                            |

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

### token_map

| column       | 类型        | 来源            | 处理                                                     |
| ------------ | ----------- | --------------- | -------------------------------------------------------- |
| token_id     | BLOB(32) PK | TokenRegistered | token0 或 token1                                         |
| condition_id | BLOB(32)    | TokenRegistered | $.conditionId                                            |
| exchange     | TEXT        | log.address     | "CTF" \| "NegRisk"                                       |
| is_yes       | INTEGER     | 计算            | 先swap确保token0<token1，然后token0=YES(1), token1=NO(0) |

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

### convert

| column       | 类型       | 来源               | 处理                 |
| ------------ | ---------- | ------------------ | -------------------- |
| block_number | BIGINT PK  | log                |                      |
| log_index    | INTEGER PK | log                |                      |
| stakeholder  | BLOB(20)   | PositionsConverted | $.stakeholder        |
| market_id    | BLOB(32)   | PositionsConverted | $.marketId           |
| index_set    | BIGINT     | PositionsConverted | bitmap: 哪些NO被转换 |
| amount       | BIGINT     | PositionsConverted | 每个position的数量   |

**关键连接**: `neg_risk_question.question_id` = `condition.question_id`

### sync_state

| key        | 含义           |
| ---------- | -------------- |
| last_block | 已同步到的区块 |

## 索引

| 表                | 索引         | 用途              |
| ----------------- | ------------ | ----------------- |
| transfer          | from_addr    | 按用户查          |
| transfer          | to_addr      | 按用户查          |
| split             | stakeholder  | 按用户查          |
| merge             | stakeholder  | 按用户查          |
| redemption        | redeemer     | 按用户查          |
| fpmm              | condition_id | FPMM按condition查 |
| fpmm_trade        | trader       | 按用户查FPMM交易  |
| fpmm_trade        | fpmm_addr    | 按FPMM合约查交易  |
| order_filled      | maker        | 按用户查交易      |
| order_filled      | taker        | 按用户查交易      |
| order_filled      | token_id     | 按市场查交易      |
| neg_risk_question | market_id    | 按市场查问题      |
| convert           | stakeholder  | 按用户查          |

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
