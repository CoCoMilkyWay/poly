```
poly-lend/
└── src/
    ├── PolyLend.sol                  # ===== 条件代币抵押借贷 =====
    │   │   # 用 YES/NO token 作抵押, 借 USDC
    │   │   # 核心流程: request → offer → accept → (call) → repay/reclaim
    │   │
    │   │  --- 常量 ---
    │   ├── MAX_INTEREST              # 最大利率 ≈ 1000% APY
    │   ├── AUCTION_DURATION          # 1 天, call 后的转让拍卖时长
    │   ├── PAYBACK_BUFFER            # 1 分钟, 还款时间戳容差
    │   │
    │   │  --- 数据结构 ---
    │   ├── Request                   # 借款请求
    │   │   └── {borrower, positionId, collateralAmount, minimumDuration}
    │   │
    │   ├── Offer                     # 放贷报价
    │   │   └── {requestId, lender, loanAmount, rate}
    │   │
    │   ├── Loan                      # 活跃贷款
    │   │   └── {borrower, lender, positionId, collateralAmount,
    │   │        loanAmount, rate, startTime, minimumDuration, callTime}
    │   │
    │   │  --- 借款人操作 ---
    │   ├── request(positionId, collateralAmount, minimumDuration) → requestId
    │   │       # 发起借款请求
    │   │       # 需先 approve CTF 给合约
    │   │
    │   ├── cancelRequest(requestId)
    │   │
    │   ├── accept(offerId) → loanId
    │   │       # 接受放贷报价
    │   │       # 抵押品转入合约, USDC 从 lender 转给 borrower
    │   │
    │   ├── repay(loanId, repayTimestamp)
    │   │       # 还款 (本金 + 利息)
    │   │       # 未 call: repayTimestamp 可为当前时间 (有 1 分钟容差)
    │   │       # 已 call: repayTimestamp 必须等于 callTime
    │   │       # 利息 = loanAmount * rate^duration
    │   │
    │   │  --- 放贷人操作 ---
    │   ├── offer(requestId, loanAmount, rate) → offerId
    │   │       # 对请求报价
    │   │       # rate 为每秒复利因子 (1e18 = 无利息)
    │   │
    │   ├── cancelOffer(offerId)
    │   │
    │   ├── call(loanId)
    │   │       # 催收 (需过 minimumDuration)
    │   │       # 启动 1 天拍卖期
    │   │       # 借款人必须在 callTime 还款, 否则开始转让拍卖
    │   │
    │   ├── reclaim(loanId)
    │   │       # 拍卖结束后无人接手, lender 收走抵押品
    │   │
    │   │  --- 第三方操作 ---
    │   └── transfer(loanId, newRate)
    │           # 贷款转让 (Dutch Auction 荷兰式拍卖)
    │           # call 后 1 天内, 利率从 0 线性升到 MAX_INTEREST
    │           # 新 lender 出价 ≤ 当前拍卖利率即可接手
    │           # 新 lender 付清旧贷款, 获得新贷款债权
    │
    ├── InterestLib.sol               # 利率计算库
    │   ├── ONE = 1e18                # 基准值
    │   ├── ONE_THOUSAND_APY          # 1000% 年化对应的每秒利率
    │   └── pow(base, exponent)       # 快速幂 (用于复利计算)
    │
    └── ERC1155TokenReceiver.sol      # ERC1155 接收回调

```
