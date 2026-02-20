```
ctf-exchange/
└── src/
    ├── common/
    │   ├── auth/
    │   │   ├── Authorized.sol             # onlyAuthorized 修饰符
    │   │   ├── Owned.sol                  # owner + onlyOwner
    │   │   └── Ownable.sol                # + transferOwnership()
    │   ├── ERC20.sol
    │   ├── ReentrancyGuard.sol            # nonReentrant 防重入
    │   └── libraries/
    │       └── SafeTransferLib.sol
    │
    └── exchange/
        ├── BaseExchange.sol               # ERC1155Holder + ReentrancyGuard 基类
        │
        ├── CTFExchange.sol                # ===== 主合约, 继承所有 mixin =====
        │   │
        │   │  --- 交易 (onlyOperator) ---
        │   ├── fillOrder(order, amount)   # 执行单笔订单, operator 作为对手方垫资
        │   ├── fillOrders()               # 批量执行
        │   ├── matchOrders(takerOrder, makerOrders[], ...)
        │   │       # 撮合一个 taker 对多个 maker, 三种 MatchType:
        │   │       #   COMPLEMENTARY: buy vs sell, 直接交换
        │   │       #   MINT: 两个 buy, 调 CTF.splitPosition 铸造 YES+NO
        │   │       #   MERGE: 两个 sell, 调 CTF.mergePositions 合并成 USDC
        │   │
        │   │  --- 管理 (onlyAdmin) ---
        │   ├── registerToken(tokenId, complement, conditionId)
        │   │       # 注册交易对: YES tokenId ↔ NO tokenId, 关联 conditionId
        │   ├── pauseTrading()             # 紧急暂停所有交易
        │   ├── unpauseTrading()
        │   ├── setProxyFactory()          # 更新 Proxy 钱包工厂
        │   └── setSafeFactory()           # 更新 Gnosis Safe 工厂
        │
        ├── libraries/
        │   ├── OrderStructs.sol           # 核心数据结构
        │   │   ├── Order                  # salt/maker/signer/taker/tokenId/makerAmount/takerAmount/expiration/nonce/feeRateBps/side/signatureType/signature
        │   │   ├── Side                   # BUY=0, SELL=1
        │   │   ├── SignatureType          # EOA=0, POLY_PROXY=1, POLY_GNOSIS_SAFE=2, POLY_1271=3
        │   │   ├── MatchType              # COMPLEMENTARY=0, MINT=1, MERGE=2
        │   │   └── OrderStatus            # isFilledOrCancelled, remaining
        │   ├── CalculatorHelper.sol       # price/size → amounts 计算
        │   ├── TransferHelper.sol         # ERC20/ERC1155 转账封装
        │   ├── PolyProxyLib.sol           # CREATE2 算 Proxy 钱包地址
        │   └── PolySafeLib.sol            # CREATE2 算 Gnosis Safe 地址
        │
        └── mixins/
            ├── Trading.sol                # ===== 核心交易逻辑 =====
            │   │  --- 用户可调 ---
            │   ├── cancelOrder(order)     # maker 取消自己的订单
            │   ├── cancelOrders(orders[]) # 批量取消
            │   │  --- 查询 ---
            │   ├── getOrderStatus(hash)   # 返回 {isFilledOrCancelled, remaining}
            │   └── validateOrder(order)   # 校验订单有效性 (过期/签名/nonce/tokenId)
            │
            ├── Auth.sol                   # ===== 角色管理 =====
            │   ├── addAdmin(addr)         # admin 添加新 admin
            │   ├── removeAdmin(addr)
            │   ├── addOperator(addr)      # admin 添加 operator (撮合服务)
            │   ├── removeOperator(addr)
            │   ├── renounceAdminRole()    # 放弃自己的 admin
            │   ├── renounceOperatorRole()
            │   ├── isAdmin(addr)
            │   └── isOperator(addr)
            │
            ├── NonceManager.sol           # ===== 批量取消 =====
            │   ├── incrementNonce()       # nonce++, 使所有旧 nonce 订单失效
            │   └── isValidNonce(user, n)  # 校验 nonce 是否匹配
            │
            ├── Registry.sol               # ===== tokenId 注册 =====
            │   ├── getConditionId(tokenId)    # tokenId → conditionId
            │   ├── getComplement(tokenId)     # YES tokenId → NO tokenId
            │   ├── validateTokenId(tokenId)   # 检查是否已注册
            │   └── validateComplement(t1, t2) # 检查是否互为 complement
            │
            ├── Assets.sol                 # collateral(USDC) + ctf(ConditionalTokens) 地址
            │   ├── getCollateral()
            │   └── getCtf()
            │
            ├── Fees.sol                   # 手续费
            │   └── getMaxFeeRate()
            │
            ├── Hashing.sol                # EIP712
            │   └── hashOrder(order)       # 生成订单哈希用于签名
            │
            ├── Signatures.sol             # 签名验证, 支持 4 种钱包类型
            │   └── validateOrderSignature(hash, order)
            │
            ├── Pausable.sol               # 暂停状态
            │
            ├── AssetOperations.sol        # 调用 Gnosis CTF
            │   ├── _mint(conditionId, amount)   # 内部调 CTF.splitPosition
            │   └── _merge(conditionId, amount)  # 内部调 CTF.mergePositions
            │
            └── PolyFactoryHelper.sol      # 钱包工厂地址存储
```
