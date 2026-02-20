```
proxy-factory/
└── contracts/
    ├── ProxyWallet/
    │   ├── ProxyWalletFactory.sol    # ===== 主合约: 代理钱包工厂 =====
    │   │   │
    │   │   │  --- 继承 ---
    │   │   │  Ownable, GSNRecipient (支持 Gas Station Network 代付 gas)
    │   │   │
    │   │   │  --- 构造函数 ---
    │   │   │  constructor()
    │   │   │      # 1. 用 Create2 部署 ProxyWallet 实现
    │   │   │      # 2. 部署 GSNModule01 作为默认 GSN 模块
    │   │   │
    │   │   │  --- 核心功能 ---
    │   │   ├── proxy(ProxyCall[] calls)
    │   │   │       # 通过 GSN relayer 执行批量调用
    │   │   │       # 如果用户没有钱包则自动创建
    │   │   │       # ProxyCall = { to, value, data }
    │   │   │
    │   │   │  --- 钱包管理 ---
    │   │   ├── makeWallet(_implementation, msgSender)
    │   │   │       # 用 Create2 克隆实现合约, 创建用户钱包
    │   │   │       # salt = keccak256(msgSender)
    │   │   ├── maybeMakeWallet(_implementation, instanceAddress, msgSender)
    │   │   │       # 如果地址无代码则创建钱包
    │   │   │
    │   │   │  --- Admin ---
    │   │   ├── setGSNModule(gsnModule)    # 更新 GSN 模块 (onlyOwner)
    │   │   ├── getGSNModule()             # 查询当前 GSN 模块
    │   │   └── getImplementation()        # 查询实现合约地址
    │   │
    │   ├── ProxyWallet.sol               # 用户代理钱包
    │   │   ├── initialize()              # 初始化 (仅一次)
    │   │   └── proxy(ProxyCall[] calls)  # 执行批量调用
    │   │
    │   └── ProxyWalletLib.sol            # 存储 slot 管理
    │       ├── setImplementation(addr)
    │       ├── getImplementation()
    │       ├── setGSNModule(addr)
    │       ├── getGSNModule()
    │       └── WALLET_FACTORY_SALT()
    │
    ├── GSNModules/                       # Gas Station Network 模块
    │   ├── GSNLib.sol                    # GSN 工具库
    │   ├── GSNModule01.sol               # 默认 GSN 模块
    │   ├── GSNModule02.sol
    │   ├── GSNModule03.sol
    │   └── GSNModule04.sol
    │
    ├── interfaces/
    │   ├── IGSNModule.sol                # GSN 模块接口
    │   ├── IProxyWalletFactory.sol       # 工厂接口
    │   ├── IChi.sol                      # Chi gas token 接口
    │   └── IRootChain.sol                # Polygon RootChain 接口
    │
    └── libraries/
        ├── FactoryLib.sol                # Create2 克隆工具
        │   ├── create2Clone(impl, salt)  # 克隆合约
        │   └── deriveInstanceAddress(impl, salt)  # 预计算地址
        ├── MemcpyLib.sol                 # 内存拷贝
        ├── RevertCaptureLib.sol          # 捕获 revert 信息
        ├── RStoreLib.sol                 # 存储读写
        ├── SliceLib.sol                  # bytes 切片
        └── StringLib.sol                 # 字符串工具
```
