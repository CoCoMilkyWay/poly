# Polymarket PnL Replayer

```
项目目标： 可以replay polymarket 历史上任何用户的历史上每一笔交易记录
polygon链上polymarket协议合约节点本身的实现: /home/chuyin/work/poly/doc/smart-contracts

老架构:基于the Graph协议的indexer实现
新架构:直接query RPC节点获得历史信息(本地Erigon/远程Alchemy节点, 行为一致)

老架构:
    思路: doc/indexer/README.md
    官方推荐的the Graph实现: /home/chuyin/work/poly/doc/indexer/the graph/official (但真正的实现和他其实差异很大)
    商业graph indexer实现:
        IPFS manifest: /home/chuyin/work/poly/doc/indexer/the graph/implemented/IPFS
        graphql schema: /home/chuyin/work/poly/doc/indexer/the graph/implemented/schema

新架构: 如下
```

**Split/Merge/Redemption 使用场景**:

1. **Split(铸造)— 市场进行中**
   - 操作: USDC → YES + NO (固定 $0.50/$0.50)
   - 做市: 铸造后挂单卖双边，提供流动性
   - 套利: 当 YES + NO 市场价之和 > $1 时，铸造后卖双边获利
   - 方向性建仓: 当看空方流动性好时，铸造后卖看空方，建仓看多方

2. **Merge(销毁)— 市场进行中**
   - 操作: YES + NO → USDC (固定 $0.50/$0.50)
   - 做市: 买入双边后销毁，退出流动性
   - 套利: 当 YES + NO 市场价之和 < $1 时，买双边后销毁获利
   - 方向性平仓: 当看多方流动性差时，买看空方后销毁双边，平仓看多方

3. **Redemption(赎回)— 市场结算后**
   - 操作: tokens → USDC (只能在市场结算后操作)
   - 用途: 赎回 winning tokens 获得收益，losing tokens 归零 (价格由 payoutNumerators/payoutDenominator 决定)

## Flow

```
Round 1: eth_getLogs → raw_log (暂存JSON，~15min)
Round 2: raw_log → 最终表 (纯SQL转换，~2min)
```

## 合约

| 合约               | 地址                                       | 起始区块 |
| ------------------ | ------------------------------------------ | -------- |
| ConditionalTokens  | 0x4D97DCd97eC945f40cF65F87097ACe5EA0476045 | 4023686  |
| CTFExchange        | 0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E | 33605403 |
| NegRiskCTFExchange | 0xC5d563A36AE78145C45a50134d48A1215220f80a | 50505492 |
| NegRiskAdapter     | 0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296 | 50505403 |

```
chuyin@chuyin:~/work/polymarket-indexer/service$ /bin/python3 /home/chuyin/work/poly/scripts/scan_events.py

  Polymarket 事件扫描  (head=83,000,000)

  块进度:   74,369,999 / 83,000,000  (89.6%)  -  速度: 318 blk/s  ETA: 451.7 min

  合约                  事件                        总计       首block       末block      2020       2021       2022       2023       2024       2025      2026
  -------------------------------------------------------------------------------------------------------------------------------------------------------------
  ConditionalTokens   TransferSingle         132,416,088     4,028,711    74,369,999         0  1,323,929  1,455,369    492,774 39,667,704 89,476,312         0
  ConditionalTokens   TransferBatch           85,890,764     4,028,608    74,369,999         0  1,202,775  1,832,003    376,675 25,391,336 57,087,975         0
  ConditionalTokens   ConditionPreparation        77,416     4,027,499    74,369,693         0      1,029      7,392      4,218     15,364     49,413         0
  ConditionalTokens   ConditionResolution         64,718     6,205,069    74,369,952         0        750      3,733      3,805     13,606     42,824         0
  ConditionalTokens   PositionSplit           38,000,136     4,028,608    74,369,999         0    617,762    968,524    230,227 10,177,708 26,005,915         0
  ConditionalTokens   PositionsMerge          13,957,678     4,028,724    74,369,999         0    490,210    657,656    113,262  4,172,673  8,523,877         0
  ConditionalTokens   PayoutRedemption        10,249,718     6,233,711    74,369,995         0    332,902    246,192     69,917  1,747,897  7,852,810         0
  CTFExchange         OrderFilled             39,573,179    35,896,869    74,369,999         0          0          0    247,475  7,633,619 31,692,085         0
  CTFExchange         OrdersMatched           16,494,141    35,896,869    74,369,999         0          0          0    101,248  3,233,641 13,159,252         0
  CTFExchange         TokenRegistered             73,224    35,887,522    74,369,705         0          0          0      6,744     14,526     51,954         0
  NegRiskCTFExchange  OrderFilled             80,419,827    51,408,357    74,369,999         0          0          0          0 30,359,912 50,059,915         0
  NegRiskCTFExchange  OrdersMatched           36,356,327    51,408,357    74,369,999         0          0          0          0 14,092,052 22,264,275         0
  NegRiskCTFExchange  TokenRegistered             60,990    51,405,773    74,367,455         0          0          0          0     16,042     44,948         0
  NegRiskAdapter      MarketPrepared                   0       -             -               0          0          0          0          0          0         0
  NegRiskAdapter      QuestionPrepared                 0       -             -               0          0          0          0          0          0         0
  NegRiskAdapter      PositionsConverted               0       -             -               0          0          0          0          0          0         0
  NegRiskAdapter      OutcomeReported                  0       -             -               0          0          0          0          0          0         0


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

USDC → YES + NO (铸造)

| 字段               | 类型      | indexed | 说明                                                   |
| ------------------ | --------- | ------- | ------------------------------------------------------ |
| stakeholder        | address   | yes     | 操作者                                                 |
| collateralToken    | address   | no      | 固定为USDC: 0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174 |
| parentCollectionId | bytes32   | yes     | 几乎总是 0x0                                           |
| conditionId        | bytes32   | yes     | 条件 ID                                                |
| partition          | uint256[] | no      | [1,2] = YES+NO                                         |
| amount             | uint256   | no      | 消耗的USDC数量 (6 decimals)，同时获得等量YES和NO token |
| tx_hash            | bytes32   | meta    | log.transactionHash                                    |
| block_number       | uint64    | meta    | log.blockNumber                                        |
| log_index          | uint32    | meta    | log.logIndex                                           |

**PositionsMerge**

YES + NO → USDC (销毁)

| 字段               | 类型      | indexed | 说明                                                   |
| ------------------ | --------- | ------- | ------------------------------------------------------ |
| stakeholder        | address   | yes     | 操作者                                                 |
| collateralToken    | address   | no      | 固定为USDC: 0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174 |
| parentCollectionId | bytes32   | yes     | 几乎总是 0x0                                           |
| conditionId        | bytes32   | yes     | 条件 ID                                                |
| partition          | uint256[] | no      | [1,2] = YES+NO                                         |
| amount             | uint256   | no      | 销毁的YES+NO数量 (6 decimals)，同时获得等量USDC        |
| tx_hash            | bytes32   | meta    | log.transactionHash                                    |
| block_number       | uint64    | meta    | log.blockNumber                                        |
| log_index          | uint32    | meta    | log.logIndex                                           |

**TransferSingle**

覆盖**所有**持仓变化: Split铸造、Merge销毁、交易所撮合、赎回、用户间直接转账

| 字段         | 类型    | indexed | 说明                                                                                                          |
| ------------ | ------- | ------- | ------------------------------------------------------------------------------------------------------------- |
| operator     | address | yes     | 执行操作的**合约**，非用户。CTFExchange=0x4bFb41.../NegRiskAdapter=0xd91E80.../NegRiskCTFExchange=0xC5d563... |
| from         | address | yes     | 发送方。**0x0=mint**(Split铸造)                                                                               |
| to           | address | yes     | 接收方。**0x0=burn**(Merge销毁/Redemption赎回)                                                                |
| id           | uint256 | no      | positionId (ERC1155 tokenId)，256位整数，由 keccak256(USDC地址, collectionId) 计算                            |
| value        | uint256 | no      | token数量，6 decimals (1 token = 1,000,000 units)                                                             |
| tx_hash      | bytes32 | meta    | log.transactionHash                                                                                           |
| block_number | uint64  | meta    | log.blockNumber                                                                                               |
| log_index    | uint32  | meta    | log.logIndex                                                                                                  |

**TransferBatch**

同 TransferSingle，批量版本

| 字段         | 类型      | indexed | 说明                       |
| ------------ | --------- | ------- | -------------------------- |
| operator     | address   | yes     | 执行操作的**合约**，非用户 |
| from         | address   | yes     | 发送方。**0x0=mint**       |
| to           | address   | yes     | 接收方。**0x0=burn**       |
| ids          | uint256[] | no      | positionId 数组            |
| values       | uint256[] | no      | 数量数组，6 decimals       |
| tx_hash      | bytes32   | meta    | log.transactionHash        |
| block_number | uint64    | meta    | log.blockNumber            |
| log_index    | uint32    | meta    | log.logIndex               |

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

| 字段               | 类型      | indexed | 说明                                                      |
| ------------------ | --------- | ------- | --------------------------------------------------------- |
| redeemer           | address   | yes     | 操作者                                                    |
| collateralToken    | address   | yes     | 固定为USDC: 0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174    |
| parentCollectionId | bytes32   | yes     | 几乎总是 0x0                                              |
| conditionId        | bytes32   | no      | 条件 ID                                                   |
| indexSets          | uint256[] | no      | 赎回的position组合: [1]=仅YES, [2]=仅NO, [1,2]=两者都赎回 |
| payout             | uint256   | no      | 获得的USDC (6 decimals)，可能为0(输家token赎回值为0)      |
| tx_hash            | bytes32   | meta    | log.transactionHash                                       |
| block_number       | uint64    | meta    | log.blockNumber                                           |
| log_index          | uint32    | meta    | log.logIndex                                              |

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

买卖方向: makerAssetId=0 → maker出USDC换token → maker是买方。价格=makerAmountFilled/takerAmountFilled

| 字段              | 类型    | indexed | 说明                                     |
| ----------------- | ------- | ------- | ---------------------------------------- |
| orderHash         | bytes32 | yes     | 订单哈希                                 |
| maker             | address | yes     | 挂单方                                   |
| taker             | address | yes     | 吃单方                                   |
| makerAssetId      | uint256 | no      | maker给出的资产。**0=USDC**，非0=tokenId |
| takerAssetId      | uint256 | no      | taker给出的资产。**0=USDC**，非0=tokenId |
| makerAmountFilled | uint256 | no      | maker给出的数量 (6 decimals)             |
| takerAmountFilled | uint256 | no      | taker给出的数量 (6 decimals)             |
| fee               | uint256 | no      | 手续费 (USDC, 6 decimals)                |
| tx_hash           | bytes32 | meta    | log.transactionHash                      |
| block_number      | uint64  | meta    | log.blockNumber                          |
| log_index         | uint32  | meta    | log.logIndex                             |
| exchange          | TEXT    | meta    | CTFExchange 或 NegRiskCTFExchange        |

**OrdersMatched**

| 字段              | 类型    | indexed | 说明                                                 |
| ----------------- | ------- | ------- | ---------------------------------------------------- |
| takerOrderHash    | bytes32 | yes     | taker 订单哈希                                       |
| takerOrderMaker   | address | yes     | **名字易混淆**: "taker订单的maker"，即发起吃单的用户 |
| makerAssetId      | uint256 | no      | maker给出的资产。**0=USDC**，非0=tokenId             |
| takerAssetId      | uint256 | no      | taker给出的资产。**0=USDC**，非0=tokenId             |
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

NegRisk转换: 多个NO tokens → 单个YES token (当你确信某个选项会赢时)

| 字段         | 类型    | indexed | 说明                                                                  |
| ------------ | ------- | ------- | --------------------------------------------------------------------- |
| stakeholder  | address | yes     | 操作者                                                                |
| marketId     | bytes32 | yes     | NegRisk市场ID，一个市场包含多个互斥问题(如"谁会赢得选举": A/B/C/其他) |
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

## Round 1: raw_log 暂存表

| column       | 类型       | 来源                              |
| ------------ | ---------- | --------------------------------- |
| id           | INTEGER PK | block_number \* 10000 + log_index |
| block_number | INTEGER    | log.blockNumber                   |
| log_index    | INTEGER    | log.logIndex                      |
| event_type   | TEXT       | topic0 解码                       |
| contract     | TEXT       | log.address                       |
| data         | TEXT       | JSON: 所有字段原始解析            |

## Round 2: SQL 转换

### token_map

| column       | 类型    | 来源            | 处理                                            |
| ------------ | ------- | --------------- | ----------------------------------------------- |
| token_id     | TEXT PK | raw_log.data    | TokenRegistered: token0/token1                  |
| condition_id | TEXT    | raw_log.data    | $.conditionId                                   |
| exchange     | TEXT    | TokenRegistered | CTFExchange 或 NegRiskCTFExchange               |
| is_yes       | INTEGER | 计算            | token0 < token1 时: token0=YES(1), token1=NO(0) |

### condition

| column             | 类型    | 来源事件             | 处理                                                |
| ------------------ | ------- | -------------------- | --------------------------------------------------- |
| condition_id       | TEXT PK | ConditionPreparation | $.conditionId                                       |
| oracle             | TEXT    | ConditionPreparation | $.oracle                                            |
| question_id        | TEXT    | ConditionPreparation | $.questionId                                        |
| outcome_slot_count | INTEGER | ConditionPreparation | $.outcomeSlotCount (固定2)                          |
| payout_numerators  | TEXT    | ConditionResolution  | $.payoutNumerators, [1,0]=YES赢 [0,1]=NO赢 [1,1]=平 |
| resolution_block   | INTEGER | ConditionResolution  | block_number, NULL=未结算                           |

### order_filled

| column       | 类型       | 来源        | 处理                                                   |
| ------------ | ---------- | ----------- | ------------------------------------------------------ |
| id           | INTEGER PK | raw_log     | block\*10000+log_idx                                   |
| block_number | INTEGER    | raw_log     | 直接取                                                 |
| log_index    | INTEGER    | raw_log     | 直接取                                                 |
| exchange     | TEXT       | OrderFilled | CTFExchange 或 NegRiskCTFExchange                      |
| maker        | TEXT       | OrderFilled | $.maker                                                |
| taker        | TEXT       | OrderFilled | $.taker                                                |
| market       | TEXT       | 计算        | makerAssetId=0 ? takerAssetId : makerAssetId           |
| side         | TEXT       | 计算        | makerAssetId=0 ? 'Buy' : 'Sell'                        |
| size         | INTEGER    | 计算        | makerAssetId=0 ? makerAmountFilled : takerAmountFilled |
| price_num    | INTEGER    | OrderFilled | makerAmountFilled                                      |
| price_den    | INTEGER    | OrderFilled | takerAmountFilled                                      |
| fee          | INTEGER    | OrderFilled | $.fee (USDC 6 decimals)                                |

**side/market 判断**: makerAssetId=0 表示 maker 出 USDC 买 token → Buy; 否则 maker 出 token 换 USDC → Sell

### split

| column       | 类型       | 来源          | 处理                       |
| ------------ | ---------- | ------------- | -------------------------- |
| id           | INTEGER PK | raw_log       | block\*10000+log_idx       |
| block_number | INTEGER    | raw_log       | 直接取                     |
| log_index    | INTEGER    | raw_log       | 直接取                     |
| stakeholder  | TEXT       | PositionSplit | $.stakeholder              |
| condition_id | TEXT       | PositionSplit | $.conditionId              |
| amount       | INTEGER    | PositionSplit | $.amount (USDC 6 decimals) |

### merge

| column       | 类型       | 来源           | 处理                 |
| ------------ | ---------- | -------------- | -------------------- |
| id           | INTEGER PK | raw_log        | block\*10000+log_idx |
| block_number | INTEGER    | raw_log        | 直接取               |
| log_index    | INTEGER    | raw_log        | 直接取               |
| stakeholder  | TEXT       | PositionsMerge | $.stakeholder        |
| condition_id | TEXT       | PositionsMerge | $.conditionId        |
| amount       | INTEGER    | PositionsMerge | $.amount             |

### redemption

| column       | 类型       | 来源             | 处理                     |
| ------------ | ---------- | ---------------- | ------------------------ |
| id           | INTEGER PK | raw_log          | block\*10000+log_idx     |
| block_number | INTEGER    | raw_log          | 直接取                   |
| log_index    | INTEGER    | raw_log          | 直接取                   |
| redeemer     | TEXT       | PayoutRedemption | $.redeemer               |
| condition_id | TEXT       | PayoutRedemption | $.conditionId            |
| index_sets   | TEXT       | PayoutRedemption | $.indexSets (JSON array) |
| payout       | INTEGER    | PayoutRedemption | $.payout (USDC)          |

### sync_state

| key         | 含义                  |
| ----------- | --------------------- |
| last_block  | Round1 已同步到的区块 |
| round1_done | Round1 完成标记       |
| round2_done | Round2 完成标记       |

## 索引

| 表           | 索引        | 用途           |
| ------------ | ----------- | -------------- |
| order_filled | maker       | 按用户查交易   |
| order_filled | taker       | 按用户查交易   |
| order_filled | market      | 按市场查交易   |
| split        | stakeholder | 按用户查 split |
| merge        | stakeholder | 按用户查 merge |
| redemption   | redeemer    | 按用户查赎回   |

## PnL 计算

```
PnL = totalProceeds + totalRedemption - totalCostBasis - totalFees
```

| 来源                | 加减         | 说明          |
| ------------------- | ------------ | ------------- |
| order_filled (Buy)  | - costBasis  | 买入花费 USDC |
| order_filled (Sell) | + proceeds   | 卖出获得 USDC |
| order_filled.fee    | - fees       | 手续费        |
| redemption.payout   | + redemption | 结算赎回      |
