```
neg-risk-ctf-adapter/
└── src/
    ├── NegRiskAdapter.sol             # ===== 核心: 多选一市场的 CTF 适配器 =====
    │   │
    │   │  --- 不可变量 ---
    │   ├── ctf                        # ConditionalTokens 地址
    │   ├── col                        # 抵押品 (USDC)
    │   ├── wcol                       # 包装抵押品 (WrappedCollateral)
    │   ├── vault                      # 手续费收款地址
    │   ├── NO_TOKEN_BURN_ADDRESS      # NO 代币销毁地址
    │   │
    │   │  --- Market 管理 (onlyAdmin) ---
    │   ├── prepareMarket(feeBips, metadata)
    │   │       # 创建多选一市场, 返回 marketId
    │   │       # feeBips: 手续费率 (万分比)
    │   │
    │   ├── prepareQuestion(marketId, metadata)
    │   │       # 在市场下添加问题/选项
    │   │       # questionId = hash(marketId, questionIndex)
    │   │       # 同时调 ctf.prepareCondition()
    │   │
    │   ├── reportOutcome(questionId, outcome)
    │   │       # 报告问题结果 (true/false)
    │   │       # 内部保证同一市场只有一个问题能为 true
    │   │       # 调 ctf.reportPayouts()
    │   │
    │   │  --- 用户操作 ---
    │   ├── splitPosition(conditionId, amount)
    │   │       # USDC → YES + NO (包装后调 CTF)
    │   │
    │   ├── mergePositions(conditionId, amount)
    │   │       # YES + NO → USDC
    │   │
    │   ├── redeemPositions(conditionId, amounts[])
    │   │       # 结算后赎回
    │   │
    │   ├── convertPositions(marketId, indexSet, amount)
    │   │       # 核心特性: NO 仓位转换
    │   │       # 多个 NO → 剩余选项的 YES + (N-1) 份 USDC
    │   │       # 例: 持有选项 A,B,C 的 NO, indexSet=0b111
    │   │       #     转换后得到: 2 份 USDC (因为 3-1=2)
    │   │       # 这就是"负风险"名称的由来
    │   │
    │   │  --- 查询 ---
    │   ├── getConditionId(questionId)
    │   ├── getPositionId(questionId, outcome)
    │   ├── balanceOf(owner, id)           # 代理到 CTF
    │   └── balanceOfBatch(owners[], ids[])
    │
    ├── NegRiskOperator.sol            # ===== Oracle 中间层, 接收 UMA 结果 =====
    │   │
    │   ├── oracle                     # UMA adapter 地址 (setOracle 只能设一次)
    │   ├── DELAY_PERIOD               # 结果延迟期 (当前为 0)
    │   │
    │   │  --- Admin ---
    │   ├── setOracle(addr)            # 设置 oracle (仅一次)
    │   ├── prepareMarket(feeBips, data)
    │   ├── prepareQuestion(marketId, data, requestId)
    │   │       # requestId 关联 UMA 请求
    │   ├── flagQuestion(questionId)   # 标记问题, 阻止自动解析
    │   ├── unflagQuestion(questionId)
    │   ├── emergencyResolveQuestion(questionId, result)
    │   │       # 紧急手动解析 (需先 flag)
    │   │
    │   │  --- Oracle 回调 ---
    │   ├── reportPayouts(requestId, payouts[])
    │   │       # UMA adapter 调用, 记录结果
    │   │       # payouts=[1,0] → true, [0,1] → false
    │   │
    │   │  --- 任何人可调 ---
    │   └── resolveQuestion(questionId)
    │           # 将结果推送到 NegRiskAdapter
    │           # 需等待 DELAY_PERIOD
    │
    ├── NegRiskCtfExchange.sol         # CTFExchange 的薄封装
    │   │   # 构造时授权 NegRiskAdapter 操作 CTF
    │   └── (继承 CTFExchange 所有功能)
    │
    ├── WrappedCollateral.sol          # 包装抵押品 (WCOL)
    │   │   # CTF 只接受 ERC20, 所以需要把 USDC 包装一层
    │   ├── wrap(to, amount)           # onlyOwner, USDC → WCOL
    │   ├── unwrap(to, amount)         # WCOL → USDC
    │   ├── mint(amount)               # onlyOwner
    │   ├── burn(amount)               # onlyOwner
    │   └── release(to, amount)        # onlyOwner, 直接转 underlying
    │
    ├── Vault.sol                      # 手续费收款合约
    │
    ├── libraries/
    │   ├── NegRiskIdLib.sol           # ID 计算
    │   │   ├── getMarketId(questionId)
    │   │   ├── getQuestionId(marketId, index)
    │   │   └── getQuestionIndex(questionId)
    │   ├── CTHelpers.sol              # conditionId/positionId 计算
    │   └── Helpers.sol                # partition/payouts 工具函数
    │
    ├── modules/
    │   ├── Auth.sol                   # Admin 角色管理
    │   │   ├── addAdmin(addr)
    │   │   ├── removeAdmin(addr)
    │   │   ├── renounceAdmin()
    │   │   └── isAdmin(addr)
    │   │
    │   └── MarketDataManager.sol      # 市场状态存储
    │       ├── getMarketData(marketId)
    │       └── MarketData: oracle/questionCount/feeBips/结果状态
    │
    └── types/
        └── MarketData.sol             # 打包的市场数据结构
```
