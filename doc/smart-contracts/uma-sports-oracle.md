```
uma-sports-oracle/
└── src/
    ├── UmaSportsOracle.sol           # ===== 主合约: 体育赛事预言机 =====
    │   │
    │   │  --- 不可变量 ---
    │   ├── ctf                       # ConditionalTokens 地址
    │   ├── optimisticOracle          # UMA Optimistic Oracle V2 地址
    │   ├── addressWhitelist          # UMA 允许的奖励代币白名单
    │   ├── IDENTIFIER                # "MULTIPLE_VALUES" (UMIP-183)
    │   │
    │   │  --- 状态 ---
    │   ├── games[gameId]             # 存储 GameData
    │   ├── markets[marketId]         # 存储 MarketData
    │   │
    │   │  --- Game 生命周期: Created → Settled/Canceled/EmergencySettled ---
    │   ├── createGame(ancillaryData, ordering, token, reward, bond, liveness)
    │   │       # 创建比赛, 同时向 UMA OO 发起价格请求
    │   │       # gameId = keccak256(ancillaryData + creator)
    │   │       # ordering: Home vs Away 的顺序
    │   │
    │   │  --- Market 创建 (基于 Game) ---
    │   ├── createWinnerMarket(gameId)
    │   │       # 创建胜负盘 (Team A vs Team B)
    │   ├── createSpreadsMarket(gameId, underdog, line)
    │   │       # 创建让分盘, line 必须是半点 (如 2.5 = 2_500_000)
    │   ├── createTotalsMarket(gameId, line)
    │   │       # 创建大小盘, line 如 218.5 = 218_500_000
    │   │
    │   │  --- Market 解析 ---
    │   ├── resolveMarket(marketId)
    │   │       # 使用已结算 Game 的比分解析 Market
    │   │       # 调 ctf.reportPayouts() 设置结果
    │   │
    │   │  --- OO 回调 ---
    │   ├── priceSettled(...)         # OO 结算回调, 解码比分并结算 Game
    │   │       # int256.max = 取消, int256.min = 忽略(重置)
    │   ├── priceDisputed(...)        # OO 争议回调, 重置 Game 并重新请求价格
    │   │
    │   │  --- 查询 ---
    │   ├── getGame(gameId)           # 返回完整 GameData
    │   ├── getMarket(marketId)       # 返回完整 MarketData
    │   │
    │   │  --- Admin 管理 (Game) ---
    │   ├── pauseGame(gameId)         # Created → Paused
    │   ├── unpauseGame(gameId)       # Paused → Created
    │   ├── resetGame(gameId)         # 重置, 重新向 OO 请求价格
    │   ├── emergencySettleGame(gameId, home, away)
    │   │       # Paused → EmergencySettled, 手动设置比分
    │   │
    │   │  --- Admin 管理 (Market) ---
    │   ├── pauseMarket(marketId)     # Created → Paused
    │   ├── unpauseMarket(marketId)   # Paused → Created
    │   ├── emergencyResolveMarket(marketId, payouts[])
    │   │       # Paused → EmergencyResolved, 手动设置 payout
    │   │
    │   │  --- Admin 管理 (OO 参数) ---
    │   ├── setBond(gameId, bond)     # 更新 UMA 押金
    │   └── setLiveness(gameId, liveness)  # 更新 liveness 期限
    │
    ├── interfaces/
    │   ├── IUmaSportsOracle.sol      # 主合约接口
    │   ├── IConditionalTokens.sol    # CTF 接口
    │   ├── IOptimisticOracleV2.sol   # UMA OO 接口
    │   └── IAddressWhitelist.sol     # 白名单接口
    │
    ├── libraries/
    │   ├── Structs.sol               # 数据结构定义
    │   │   ├── GameData              # 比赛数据 (state, ordering, scores, ...)
    │   │   ├── MarketData            # 市场数据 (gameId, line, underdog, marketType)
    │   │   ├── GameState             # Created, Paused, Settled, Canceled, EmergencySettled
    │   │   ├── MarketState           # Created, Paused, Resolved, EmergencyResolved
    │   │   ├── MarketType            # Winner, Spreads, Totals
    │   │   ├── Ordering              # HomeAway, AwayHome
    │   │   └── Underdog              # Home, Away
    │   │
    │   ├── AncillaryDataLib.sol      # 拼接 ancillaryData
    │   ├── LineLib.sol               # 验证 line 是否为有效半点
    │   ├── PayoutLib.sol             # 根据比分构建 payout 数组
    │   └── ScoreDecoderLib.sol       # 从 OO 返回值解码比分
    │
    └── modules/
        └── Auth.sol                  # Admin 角色管理
            ├── addAdmin(addr)
            ├── removeAdmin(addr)
            ├── renounceAdmin()
            └── isAdmin(addr)
```
