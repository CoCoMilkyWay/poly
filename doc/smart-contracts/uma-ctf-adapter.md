```
uma-ctf-adapter/
└── src/
    ├── UmaCtfAdapter.sol              # ===== 主合约: 连接 UMA Oracle 和 CTF =====
    │   │
    │   │  --- 不可变量 ---
    │   ├── ctf                        # ConditionalTokens 地址
    │   ├── optimisticOracle           # UMA Optimistic Oracle V2 地址
    │   ├── collateralWhitelist        # UMA 允许的奖励代币白名单
    │   ├── YES_OR_NO_IDENTIFIER       # UMA 查询类型标识 (UMIP-107)
    │   ├── SAFETY_PERIOD              # 手动解析前的安全等待期 (1小时)
    │   │
    │   │  --- 核心流程: 初始化 → 等待 → 解析 ---
    │   ├── initialize(ancillaryData, rewardToken, reward, proposalBond, liveness)
    │   │       # 创建问题, 同时:
    │   │       #   1. 存储 QuestionData
    │   │       #   2. 调 ctf.prepareCondition() 创建条件
    │   │       #   3. 向 UMA OO 发起价格请求
    │   │       # questionID = keccak256(ancillaryData + creator)
    │   │
    │   ├── resolve(questionID)
    │   │       # 任何人可调, 从 OO 拉取结果并解析市场
    │   │       # 调 ctf.reportPayouts() 设置结果
    │   │       # OO 返回: 1e18=YES赢, 0=NO赢, 0.5e18=平局
    │   │
    │   │  --- 查询 ---
    │   ├── ready(questionID)          # OO 结果是否就绪可解析
    │   ├── getExpectedPayouts(questionID)  # 预览 payout 数组
    │   ├── getQuestion(questionID)    # 返回完整 QuestionData
    │   ├── isInitialized(questionID)
    │   ├── isFlagged(questionID)      # 是否被标记需手动解析
    │   │
    │   │  --- OO 回调 ---
    │   ├── priceDisputed(...)         # OO 争议回调, 重置问题并重新请求价格
    │   │
    │   │  --- Admin 管理 ---
    │   ├── flag(questionID)           # 标记问题, 暂停自动解析, 启动 SAFETY_PERIOD
    │   ├── unflag(questionID)         # 取消标记 (SAFETY_PERIOD 内)
    │   ├── resolveManually(questionID, payouts[])
    │   │       # 手动解析 (需先 flag 且过了 SAFETY_PERIOD)
    │   ├── reset(questionID)          # 重置问题, 重新向 OO 请求价格
    │   ├── pause(questionID)          # 暂停解析
    │   └── unpause(questionID)
    │
    ├── libraries/
    │   ├── AncillaryDataLib.sol       # 拼接 ancillaryData (加上 creator 地址)
    │   ├── PayoutHelperLib.sol        # 校验 payout 数组有效性
    │   └── TransferHelper.sol         # ERC20 转账
    │
    └── mixins/
        ├── Auth.sol                   # Admin 角色管理
        │   ├── addAdmin(addr)
        │   ├── removeAdmin(addr)
        │   ├── renounceAdmin()
        │   └── isAdmin(addr)
        │
        └── BulletinBoard.sol          # 问题补充信息公告板
            ├── postUpdate(questionID, data)   # 任何人可发布更新
            ├── getUpdates(questionID, owner)  # 获取某 owner 的所有更新
            └── getLatestUpdate(questionID, owner)
```
