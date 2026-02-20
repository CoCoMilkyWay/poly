```
conditional-tokens-contracts/
└── contracts/
    ├── ConditionalTokens.sol          # ===== 主合约, 继承 ERC1155 =====
    │   │
    │   │  --- 状态变量 ---
    │   ├── payoutNumerators[conditionId]  # 结果向量, 如 [1,0]=YES赢, [0,1]=NO赢
    │   ├── payoutDenominator[conditionId] # 非零表示已结算
    │   │
    │   │  --- 生命周期 ---
    │   ├── prepareCondition(oracle, questionId, outcomeSlotCount)
    │   │       # 创建新条件
    │   │       # conditionId = keccak256(oracle, questionId, outcomeSlotCount)
    │   │       # PM 的 oracle 地址是 UmaCtfAdapter
    │   │
    │   ├── reportPayouts(questionId, payouts[])
    │   │       # 只有 oracle (msg.sender) 能调
    │   │       # 设置 payoutNumerators 和 payoutDenominator
    │   │
    │   │  --- 核心操作 ---
    │   ├── splitPosition(collateral, parentCollectionId, conditionId, partition, amount)
    │   │       # 抵押品 → 条件代币
    │   │       # partition=[1,2] 即 [0b01, 0b10] 拆出 YES(index0) + NO(index1)
    │   │       # 100 USDC → 100 YES + 100 NO
    │   │
    │   ├── mergePositions(collateral, parentCollectionId, conditionId, partition, amount)
    │   │       # 条件代币 → 抵押品 (反向操作)
    │   │       # 100 YES + 100 NO → 100 USDC
    │   │
    │   ├── redeemPositions(collateral, parentCollectionId, conditionId, indexSets[])
    │   │       # 结算后赎回
    │   │       # 按 payoutNumerators/payoutDenominator 比例拿回抵押品
    │   │       # 赢家: 100 YES → 100 USDC
    │   │       # 输家: 100 NO → 0 USDC
    │   │
    │   │  --- 查询 ---
    │   ├── getOutcomeSlotCount(conditionId)   # outcome 数量 (PM 固定为 2)
    │   ├── getConditionId(oracle, questionId, count)
    │   ├── getCollectionId(parentId, conditionId, indexSet)
    │   └── getPositionId(collateral, collectionId)  # = ERC1155 tokenId
    │
    ├── CTHelpers.sol                  # ID 计算库
    │   ├── getConditionId()           # keccak256(oracle, questionId, outcomeCount)
    │   ├── getCollectionId()          # 椭圆曲线点加法, 支持嵌套条件组合 (PM 未用)
    │   └── getPositionId()            # keccak256(collateral, collectionId)
    │
    └── ERC1155/
        ├── ERC1155.sol                # 标准 ERC1155 实现
        ├── ERC1155TokenReceiver.sol   # onERC1155Received 回调
        ├── IERC1155.sol
        └── IERC1155TokenReceiver.sol
```
