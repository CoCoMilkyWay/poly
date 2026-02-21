```
conditional-tokens-market-makers/
└── src/
    ├── FixedProductMarketMaker.sol     # ===== FPMM 主合约 (类似 Uniswap AMM) =====
    │   │
    │   │  --- 状态变量 ---
    │   ├── conditionalTokens           # ConditionalTokens 合约地址
    │   ├── collateralToken             # 抵押品 (如 USDC)
    │   ├── conditionIds[]              # 支持多个 condition 组合
    │   ├── fee                         # 交易手续费 (1e18 = 100%)
    │   ├── positionIds[]               # 所有原子 outcome token 的 ERC1155 tokenId
    │   ├── feePoolWeight               # 累计手续费池
    │   │
    │   │  --- LP 操作 ---
    │   ├── addFunding(amount, distributionHint[])
    │   │       # 添加流动性, 铸造 LP 份额
    │   │       # 首次添加可用 hint 设置初始价格分布
    │   │       # 后续添加按池比例注入, 多余 outcome token 退回
    │   │
    │   ├── removeFunding(sharesToBurn)
    │   │       # 移除流动性, 销毁 LP 份额
    │   │       # 按份额比例取回各 outcome token
    │   │
    │   │  --- 交易 (核心: x*y=k) ---
    │   ├── buy(investmentAmount, outcomeIndex, minOutcomeTokensToBuy)
    │   │       # 用抵押品买入指定 outcome token
    │   │       # 扣除手续费后计算可获得数量
    │   │
    │   ├── sell(returnAmount, outcomeIndex, maxOutcomeTokensToSell)
    │   │       # 卖出 outcome token 换回抵押品
    │   │
    │   │  --- 价格计算 ---
    │   ├── calcBuyAmount(investmentAmount, outcomeIndex)
    │   │       # 给定投入, 计算可买到的 outcome token 数量
    │   │       # 公式: 恒定乘积 ∏(balances) = k
    │   │
    │   ├── calcSellAmount(returnAmount, outcomeIndex)
    │   │       # 给定期望收回金额, 计算需卖出多少 outcome token
    │   │
    │   │  --- 手续费 ---
    │   ├── collectedFees()              # 查询池中未提取手续费
    │   ├── feesWithdrawableBy(account)  # 某账户可提取手续费
    │   └── withdrawFees(account)        # 提取手续费 (按 LP 份额比例)
    │
    ├── FixedProductMarketMakerFactory.sol  # FPMM 工厂
    │   └── createFixedProductMarketMaker(ctf, collateral, conditionIds[], fee)
    │           # 创建新 FPMM 实例 (Clone 模式)
    │
    ├── FPMMDeterministicFactory.sol    # FPMM 确定性地址工厂
    │   └── create2FixedProductMarketMaker(saltNonce, ctf, collateral, conditionIds[], fee, initialFunds, hint[])
    │           # CREATE2 创建, 可预测合约地址
    │           # 可同时注入初始流动性
    │
    ├── MarketMaker.sol                 # ===== LMSR 基类 (对数市场评分规则) =====
    │   │
    │   │  --- 状态变量 ---
    │   ├── pmSystem                     # ConditionalTokens 合约
    │   ├── collateralToken
    │   ├── conditionIds[]
    │   ├── atomicOutcomeSlotCount       # 原子 outcome 总数 (多 condition 笛卡尔积)
    │   ├── fee                          # 手续费率
    │   ├── funding                      # 做市资金 (影响流动性深度)
    │   ├── stage                        # Running / Paused / Closed
    │   ├── whitelist                    # 可选白名单
    │   │
    │   │  --- 生命周期 (onlyOwner) ---
    │   ├── changeFunding(fundingChange) # 增减做市资金 (需先 pause)
    │   ├── pause()                      # 暂停交易
    │   ├── resume()                     # 恢复交易
    │   ├── changeFee(newFee)            # 修改手续费
    │   ├── close()                      # 关闭市场, 取回所有 token
    │   ├── withdrawFees()               # 提取累计手续费
    │   │
    │   │  --- 交易 ---
    │   └── trade(outcomeTokenAmounts[], collateralLimit)
    │           # 通用交易接口, 正数=买入, 负数=卖出
    │           # 支持同时买卖多个 outcome
    │           # collateralLimit: 最大支付/最小收取限制
    │
    ├── LMSRMarketMaker.sol             # ===== LMSR 实现 =====
    │   │  对数市场评分规则, 流动性由 funding 参数决定
    │   │
    │   ├── calcNetCost(outcomeTokenAmounts[])
    │   │       # 计算交易净成本
    │   │       # LMSR 公式: C(q) = b * ln(Σexp(qi/b))
    │   │       # b = funding / ln(N), N = outcome 数量
    │   │
    │   └── calcMarginalPrice(outcomeIndex)
    │           # 计算边际价格 = exp(qi/b) / Σexp(qj/b)
    │           # 价格总和恒为 1
    │
    ├── LMSRMarketMakerFactory.sol      # LMSR 工厂
    │   └── createLMSRMarketMaker(pmSystem, collateral, conditionIds[], fee, whitelist, funding)
    │           # 创建 LMSR 实例并注入初始资金
    │           # 自动 resume 启动交易
    │
    ├── Whitelist.sol                   # 白名单管理
    │   ├── isWhitelisted[addr]
    │   ├── addToWhitelist(users[])     # onlyOwner
    │   └── removeFromWhitelist(users[])
    │
    ├── ConstructedCloneFactory.sol     # Clone 工厂基类
    │   └── createClone(target, consData)
    │           # 最小代理 + 构造参数注入
    │
    └── Create2CloneFactory.sol         # CREATE2 Clone 工厂
        └── create2Clone(target, salt, consData)
                # 确定性地址部署
```
