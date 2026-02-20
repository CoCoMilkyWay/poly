```
exchange-fee-module/
└── src/
    ├── FeeModule.sol                 # ===== 手续费代理模块 =====
    │   │   # 代理 CTFExchange 并处理手续费退款
    │   │   # 核心功能: 如果 operator 收取的费用 < 订单签名的最大费率, 退还差额
    │   │
    │   │  --- 不可变量 ---
    │   ├── exchange                  # CTFExchange 地址
    │   ├── collateral                # 抵押品 (USDC)
    │   ├── ctf                       # ConditionalTokens 地址
    │   │
    │   │  --- 核心操作 (onlyAdmin) ---
    │   ├── matchOrders(takerOrder, makerOrders[], takerFillAmount,
    │   │               takerReceiveAmount, makerFillAmounts[],
    │   │               takerFeeAmount, makerFeeAmounts[])
    │   │       # 1. 调 exchange.matchOrders() 执行撮合
    │   │       # 2. 计算并退还 taker 多付的手续费
    │   │       # 3. 计算并退还每个 maker 多付的手续费
    │   │       #
    │   │       # 退款逻辑:
    │   │       #   exchangeFee = order.feeRateBps 计算出的费用
    │   │       #   operatorFee = operator 实际收取的费用
    │   │       #   refund = exchangeFee - operatorFee (如果 > 0)
    │   │
    │   └── withdrawFees(to, tokenId, amount)
    │           # 提取收集的手续费
    │           # tokenId=0 → USDC, 否则 → CTF outcome token
    │
    ├── Collector.sol                 # 批量提取手续费
    │   └── withdrawFees(WithdrawOpts[])
    │           # 循环调 feeModule.withdrawFees()
    │
    ├── NegRiskFeeModule.sol          # NegRisk 版本
    │   │   # 继承 FeeModule
    │   └── 构造时额外授权 NegRiskAdapter 操作 CTF
    │
    ├── libraries/
    │   ├── Structs.sol               # 数据结构
    │   │   ├── Side                  # BUY=0, SELL=1
    │   │   ├── SignatureType         # EOA=0, POLY_PROXY=1, POLY_GNOSIS_SAFE=2
    │   │   ├── OrderStatus           # {isFilledOrCancelled, remaining}
    │   │   └── Order                 # 订单结构体
    │   │
    │   ├── CalculatorHelper.sol      # ===== 手续费计算 =====
    │   │   │
    │   │   ├── calculateRefund(feeRateBps, operatorFee, outcomeTokens, ...)
    │   │   │       # refund = exchangeFee - operatorFee
    │   │   │       # 如果 exchangeFee <= operatorFee, 返回 0
    │   │   │
    │   │   ├── calculateExchangeFee(feeRateBps, outcomeTokens, makerAmt, takerAmt, side)
    │   │   │       # 手续费公式 (与 CTFExchange 一致):
    │   │   │       #   price = 订单价格
    │   │   │       #   surcharge = min(price, 1-price)  # 越接近 0.5 费率越高
    │   │   │       #
    │   │   │       #   BUY:  fee = feeRateBps * surcharge * (tokens/price) / 10000
    │   │   │       #   SELL: fee = feeRateBps * surcharge * tokens / 10000
    │   │   │
    │   │   └── calculateTakingAmount(makingAmt, makerAmt, takerAmt)
    │   │           # 按比例计算接收数量
    │   │
    │   └── TransferHelper.sol        # ERC20/ERC1155 转账封装
    │
    ├── mixins/
    │   ├── Auth.sol                  # Admin 角色管理
    │   │   ├── admins[addr]          # 1=是, 0=否
    │   │   ├── addAdmin(addr)        # onlyAdmin
    │   │   ├── removeAdmin(addr)
    │   │   ├── renounceAdmin()
    │   │   └── isAdmin(addr)
    │   │
    │   └── Transfers.sol             # 统一转账接口
    │       └── _transfer(token, from, to, id, amount)
    │               # id=0 → ERC20, 否则 → ERC1155
    │
    └── interfaces/
        ├── IFeeModule.sol
        ├── IExchange.sol
        ├── IConditionalTokens.sol
        └── IAuth.sol
```
