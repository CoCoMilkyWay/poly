```
safe-factory/
└── contracts/
    ├── SafeProxyFactory.sol          # ===== 主合约: Safe 钱包工厂 =====
    │   │
    │   │  --- 状态变量 ---
    │   ├── masterCopy                # GnosisSafe 主合约地址
    │   ├── fallbackHandler           # Safe fallback handler 地址
    │   ├── domainSeparator           # EIP-712 domain separator
    │   │
    │   │  --- 常量 ---
    │   ├── DOMAIN_TYPEHASH           # EIP-712 domain typehash
    │   ├── CREATE_PROXY_TYPEHASH     # EIP-712 创建代理 typehash
    │   ├── NAME                      # "Polymarket Contract Proxy Factory"
    │   │
    │   │  --- 构造函数 ---
    │   │  constructor(_masterCopy, _fallbackHandler)
    │   │      # 设置 masterCopy, fallbackHandler
    │   │      # 计算 domainSeparator
    │   │
    │   │  --- 核心功能 ---
    │   ├── createProxy(paymentToken, payment, paymentReceiver, createSig)
    │   │       # 1. 从签名恢复 owner 地址
    │   │       # 2. 用 Create2 部署 GnosisSafeProxy
    │   │       #    salt = keccak256(owner)
    │   │       # 3. 调 proxy.setup() 初始化 Safe
    │   │       #    - owners: [owner]
    │   │       #    - threshold: 1
    │   │       #    - fallbackHandler: 配置的 handler
    │   │       #    - 支持部署时付费 (paymentToken, payment, paymentReceiver)
    │   │       # 4. emit ProxyCreation(proxy, owner)
    │   │
    │   │  --- 地址计算 ---
    │   ├── computeProxyAddress(user)
    │   │       # 预计算用户的 Safe 地址 (不部署)
    │   │       # address = keccak256(0xff, factory, salt, bytecodeHash)
    │   ├── getSalt(user)
    │   │       # salt = keccak256(user)
    │   ├── getContractBytecode()
    │   │       # 返回 GnosisSafeProxy 创建字节码 + masterCopy 参数
    │   ├── proxyCreationCode()
    │   │       # 返回 GnosisSafeProxy.creationCode
    │   │
    │   │  --- 内部函数 ---
    │   ├── _getSigner(paymentToken, payment, paymentReceiver, sig)
    │   │       # EIP-712 签名验证, 恢复 signer 地址
    │   └── _getChainIdInternal()
    │           # 获取当前 chainId
    │
    └── Deps.sol                      # 依赖导入 (GnosisSafeProxy, GnosisSafe)
```
