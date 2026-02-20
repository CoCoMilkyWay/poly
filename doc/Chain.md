## Overview
- https://dune.com/datadashboards/prediction-markets
- https://defillama.com/protocols/prediction-market
- https://defillama.com/chains
- https://app.artemisanalytics.com/sectors
- https://defillama.com/categories

## TVL by Chains
![TVL](pic/TVL.png)

## TVL by Categories
- DeFi目前还是交易+资产配置属性主导, 其他服务属性还在发展中

![Category](pic/Category.png)

## 主要区块链对比

| Chain           | TVL    | Stable MCap | Protocols | Type            | Consensus         | VM/Runtime | TPS     | 数据结构             | 数据模型       | 存储    | 网络层         | 智能合约       | 执行环境 | 并行 | 最终性          | L2安全模型 | L2数据可用性  | L2提款 | 主要协议                                  | 主要用途                                       |
| --------------- | ------ | ----------- | --------- | --------------- | ----------------- | ---------- | ------- | -------------------- | -------------- | ------- | -------------- | -------------- | -------- | ---- | --------------- | ---------- | ------------- | ------ | ----------------------------------------- | ---------------------------------------------- |
| **Ethereum**    | $92.7b | $160.1b     | 1706      | L1 Public       | PoS               | EVM        | ~15     | Merkle Patricia Tree | Account        | LevelDB | devp2p         | Solidity/Vyper | EVM      | X    | 确定性 (~13min) | -          | -             | -      | Lido, Aave, Uniswap, MakerDAO, EigenLayer | DeFi基础设施, 流动性质押, 借贷, DEX, Restaking |
| **Solana**      | $16.6b | $14.1b      | 454       | L1 Public       | PoH+PoS           | SVM(Rust)  | ~65000  | Gulf Stream          | Account        | RocksDB | QUIC/UDP       | Rust (SVM)     | Sealevel | V    | 确定性 (~0.4s)  | -          | -             | -      | Jito, Marinade, Raydium, Jupiter, Kamino  | 高频交易, MEV, 聚合DEX, 借贷                   |
| **BSC**         | $7.36b | $15.75b     | 1091      | L1 Public       | PoSA              | EVM        | ~160    | Merkle Patricia Tree | Account        | LevelDB | devp2p         | Solidity/Vyper | EVM      | X    | 确定性 (~3s)    | -          | -             | -      | PancakeSwap, Venus, ListaDAO, Radiant     | DEX, 借贷, LSD, 跨链                           |
| **Bitcoin**     | $5.63b | -           | 97        | L1 Public       | PoW               | Script     | ~7      | Merkle Tree          | UTXO           | LevelDB | TCP P2P        | Script (有限)  | -        | X    | 概率性 (~60min) | -          | -             | -      | Babylon, Lightning, Stacks, WBTC          | 价值存储, L2扩展, 跨链锚定                     |
| **Tron**        | $4.17b | $84.3b      | 54        | L1 Public       | DPoS              | TVM        | ~2000   | Merkle Patricia Tree | Account        | LevelDB | TCP P2P        | Solidity       | TVM      | X    | 确定性 (~3s)    | -          | -             | -      | JustLend, SunSwap, stUSDT                 | 稳定币转账, 借贷, DEX                          |
| **Base**        | $4.12b | $4.68b      | 815       | L2 (ETH)        | Optimistic        | EVM        | ~1000   | Merkle Patricia Tree | Account        | -       | -              | Solidity/Vyper | EVM      | X    | 继承ETH         | 欺诈证明   | 链上 calldata | ~7天   | Aerodrome, Morpho, Extra Finance          | DEX ve(3,3), 借贷, 杠杆                        |
| **Plasma**      | $3.03b | $1.94b      | 74        | L2 (ETH)        | Plasma            | EVM        | -       | Merkle Tree          | Account        | -       | -              | Solidity       | EVM      | X    | 继承ETH         | 欺诈证明   | 链下          | ~7天   | -                                         | 扩展方案                                       |
| **Arbitrum**    | $2.55b | $4.77b      | 1027      | L2 (ETH)        | Optimistic        | EVM        | ~4000   | Merkle Patricia Tree | Account        | -       | -              | Solidity/Vyper | EVM      | X    | 继承ETH         | 欺诈证明   | 链上 calldata | ~7天   | GMX, Aave, Camelot, Pendle                | 永续合约, 借贷, DEX, 收益代币化                |
| **Hyperliquid** | $2.32b | $4.63b      | 211       | L1 Public       | HyperBFT          | HyperEVM   | ~100000 | -                    | Account        | -       | -              | -              | HyperEVM | V    | 确定性 (~0.2s)  | -          | -             | -      | Hyperliquid Perps, HLP                    | 链上订单簿, 永续合约                           |
| **Avalanche**   | $1.20b | $1.85b      | 571       | L1 Public       | Avalanche         | EVM        | ~4500   | Merkle Patricia Tree | Account        | LevelDB | Avalanche P2P  | Solidity/Vyper | EVM      | X    | 确定性 (~1s)    | -          | -             | -      | Aave, Trader Joe, Benqi, GMX              | 借贷, DEX, 子网扩展                            |
| **Polygon**     | $1.17b | $3.05b      | 750       | Sidechain (ETH) | PoS               | EVM        | ~7000   | Merkle Patricia Tree | Account        | LevelDB | devp2p         | Solidity/Vyper | EVM      | X    | 确定性 (~2s)    | 欺诈证明   | 链下          | ~7天   | Aave, Uniswap, QuickSwap                  | 低成本DeFi, 游戏, NFT                          |
| **Provenance**  | $918m  | $161m       | 2         | L1 Consortium   | Tendermint        | CosmWasm   | -       | IAVL+ Tree           | Account        | LevelDB | Tendermint P2P | CosmWasm/Rust  | CosmWasm | X    | 确定性 (~6s)    | -          | -             | -      | Figure                                    | RWA, 机构金融                                  |
| **Sui**         | $866m  | $516m       | 118       | L1 Public       | Narwhal+Bullshark | Move       | ~120000 | Jellyfish Merkle     | Object-centric | RocksDB | P2P            | Move           | MoveVM   | V    | 确定性 (~0.5s)  | -          | -             | -      | Navi, Scallop, Cetus, Turbos              | 借贷, DEX, 流动性                              |
| **Cronos**      | $439m  | $181m       | 152       | L1 Public       | Tendermint        | EVM        | ~10000  | IAVL+ Tree           | Account        | LevelDB | Tendermint P2P | Solidity       | EVM      | X    | 确定性 (~6s)    | -          | -             | -      | VVS Finance, Tectonic, Ferro              | DEX, 借贷, 稳定币                              |
| **Ink**         | $432m  | $557m       | 42        | L2 (ETH)        | Optimistic        | EVM        | -       | Merkle Patricia Tree | Account        | -       | -              | Solidity/Vyper | EVM      | X    | 继承ETH         | 欺诈证明   | 链上 calldata | ~7天   | -                                         | Kraken生态                                     |
| **Aptos**       | $428m  | $1.62b      | 90        | L1 Public       | AptosBFT          | Move       | ~160000 | Jellyfish Merkle     | Object-centric | RocksDB | P2P            | Move           | MoveVM   | V    | 确定性 (~0.5s)  | -          | -             | -      | Thala, Aries, Liquidswap, Amnis           | DEX, 借贷, LSD                                 |
| **Vaulta**      | $358m  | -           | 25        | L1 Public       | DPoS              | EVM        | -       | -                    | Account        | -       | -              | -              | EVM      | X    | 确定性 (~0.5s)  | -          | -             | -      | -                                         | (原EOS)                                        |
| **Katana**      | $335m  | $79m        | 34        | Sidechain (ETH) | -                 | EVM        | -       | -                    | Account        | -       | -              | Solidity       | EVM      | X    | -               | -          | -             | -      | Ronin DEX                                 | 游戏链 (Axie)                                  |
| **PolyNetwork** | $333m  | -           | 0         | Bridge          | -                 | -          | -       | -                    | -              | -       | -              | -              | -        | -    | -               | -          | -             | -      | -                                         | 跨链互操作                                     |
| **Starknet**    | $329m  | $282m       | 47        | L2 (ETH)        | STARK             | Cairo      | ~1000   | Patricia Merkle      | Account        | -       | -              | Cairo          | Cairo VM | V    | 继承ETH         | 有效性证明 | 链上          | 即时   | Nostra, Ekubo, zkLend                     | ZK借贷, DEX                                    |
| **Mantle**      | $287m  | $574m       | 159       | L2 (ETH)        | Optimistic        | EVM        | -       | Merkle Patricia Tree | Account        | -       | -              | Solidity/Vyper | EVM      | X    | 继承ETH         | 欺诈证明   | 链上 calldata | ~7天   | Agni, Lendle, mETH                        | DEX, 借贷, LSD                                 |
| **OP Mainnet**  | $240m  | $582m       | 412       | L2 (ETH)        | Optimistic        | EVM        | ~2000   | Merkle Patricia Tree | Account        | -       | -              | Solidity/Vyper | EVM      | X    | 继承ETH         | 欺诈证明   | 链上 calldata | ~7天   | Velodrome, Aave, Synthetix                | DEX ve(3,3), 借贷, 合成资产                    |
| **Monad**       | $231m  | $415m       | 106       | L1 Public       | MonadBFT          | EVM        | ~10000  | Merkle Patricia Tree | Account        | MonadDB | P2P            | Solidity/Vyper | EVM      | V    | 确定性 (~1s)    | -          | -             | -      | (测试网阶段)                              | 高性能EVM                                      |
| **Scroll**      | $211m  | $44m        | 175       | L2 (ETH)        | zkEVM             | EVM        | ~2000   | Merkle Patricia Tree | Account        | -       | -              | Solidity/Vyper | zkEVM    | X    | 继承ETH         | 有效性证明 | 链上          | 即时   | Ambient, Aave, LayerBank                  | ZK扩展, 借贷                                   |
| **Thorchain**   | $199m  | -           | 7         | L1 Public       | Tendermint        | -          | -       | IAVL+ Tree           | Account        | LevelDB | Tendermint P2P | -              | -        | X    | 确定性 (~6s)    | -          | -             | -      | THORChain Swap                            | 原生跨链交换                                   |

## 链上资产成分分解
```
═══════════════════════════════════════════════════════════════════════════════
                        DeFi 资产锁定层级模型
═══════════════════════════════════════════════════════════════════════════════

  【自由态】                    
  ══════════                   ┌─────────────────────────────────────┐
                               │  Layer -1: 自由资产 (Free Assets)   │
                               │                                     │
                               │     ETH  BTC  USDC  ...             │
                               │                                     │
                               │  特征: 无合约约束, 可自由转移       │
                               └──────────────┬──────────────────────┘
                                              │
                                              │ 资产进入合约 (存入)
                                              ▼
  【实体锁】                   ┌─────────────────────────────────────┐
  ══════════                   │  Layer 0: 原子锁定 (Atomic Lock)    │
   锁定的是                    │                                     │
   具体资产                    │  定义: 资产首次被合约托管           │
   本身                        │  性质: 每单位资产只能在一处         │
                               │        (互斥性 / partition)         │
                               │                                     │
                               │  例: Staking, LP存入, 借贷存款      │
                               └──────────────┬──────────────────────┘
                                              │
                                              │ 合约发放凭证 (mint)
                                              ▼
  【凭证锁】                   ┌─────────────────────────────────────┐
  ══════════                   │  Layer 1: 索取权 (Claim Token)      │
   锁定的是                    │                                     │
   对L0的                      │  定义: 代表L0资产的可转让凭证       │
   提取权                      │  性质: 与L0一一映射                 │
                               │        (但凭证可独立流通)           │
                               │                                     │
                               │  例: LST, LP Token, aToken          │
                               └──────────────┬──────────────────────┘
                                              │
                                              │ 凭证作为抵押品 (reuse)
                                              ▼
  【承诺锁】                   ┌─────────────────────────────────────┐
  ══════════                   │  Layer 2: 杠杆/复用 (Leverage)      │
   锁定的是                    │                                     │
   风险承诺                    │  定义: 用L1凭证再次获取敞口         │
   清算权                      │  性质: 纯粹的double-count           │
                               │        (同一底层资产被多重计算)     │
                               │                                     │
                               │  例: 抵押借贷, 递归LP, Restaking    │
                               └──────────────┬──────────────────────┘
                                              │
                                              │ 非经济约束 (governance)
                                              ▼
  【权利锁】                   ┌─────────────────────────────────────┐
  ══════════                   │  Layer 3: 治理/发行 (Rights Lock)   │
   锁定的是                    │                                     │
   发行权                      │  定义: 约束代币的释放或投票权       │
   治理权                      │  性质: 不对应实际锁定资产           │
                               │        (纯会计/治理层面)            │
                               │                                     │
                               │  例: Vesting, Gov Lock, Emission    │
                               └─────────────────────────────────────┘

```


| Layer  | 类别                    | 资产形态          | 锁定对象       | 锁定强度   | 经济功能      | Double Count | 操纵风险 | TVL计算     |
| ------ | ----------------------- | ----------------- | -------------- | ---------- | ------------- | ------------ | -------- | ----------- |
| **L0** | Native Staking          | ETH / SOL         | 底层资产本身   | 强         | 共识安全      | X            | ⭐ 低     | V 必算      |
| **L0** | Liquid Staking(底层)  | ETH(已质押)     | 底层资产       | 强         | 共识安全      | X            | ⭐⭐ 中    | V 必算      |
| **L0** | Supplied Assets         | 稳定币 / 蓝筹     | 真实资产       | 中         | 借贷供给      | X            | ⭐⭐ 中    | V 必算      |
| **L0** | Pool1 LP                | 外部资产对        | 真实资产       | 中         | 交易流动性    | X            | ⭐⭐ 中    | V 必算      |
| **L0** | Pool2 LP                | GovToken+外部     | 一半真实       | 中         | 流动性控制    | X            | ⭐⭐⭐⭐ 高  | ⚠️ 只算外部¹ |
| **L0** | Treasury(稳定币)      | USDC / DAI        | 真实资产       | 中         | 财政缓冲      | X            | ⭐⭐ 中    | V 必算      |
| **L0** | Treasury(波动资产)    | ETH / GovToken    | 市场价值       | 中         | 储备          | X            | ⭐⭐⭐ 中   | ⚠️ 打折      |
| ------ | ----------------------- | ----------------- | -------------  | ------     | ------------  | ------------ | -------- | -------     |
| **L1** | LST (stETH/rETH)        | 索取权凭证        | 对L0的提取权   | 弱(可转) | DeFi抵押/杠杆 | ⚠️ 映射L0     | ⭐⭐⭐⭐ 高  | ⚠️ 条件算    |
| **L1** | LP Token                | 池份额凭证        | 对LP的提取权   | 弱(可转) | 可组合流动性  | ⚠️ 映射L0     | ⭐⭐⭐ 中   | ⚠️ 条件算    |
| **L1** | aToken / cToken         | 存款凭证          | 对存款的提取权 | 弱(可转) | 可组合借贷    | ⚠️ 映射L0     | ⭐⭐⭐ 中   | ⚠️ 条件算    |
| -----  | ----------------------- | ----------------- | -------------  | ------     | ------------  | ------------ | -------- | -------     |
| **L2** | Borrows(已借出)       | 借出资产          | 抵押品仍锁     | 中         | 杠杆/流动性   | V            | ⭐⭐⭐ 中   | ⚠️ Net=S-B²  |
| **L2** | Restaking(底层)       | 已质押 ETH        | 同一资产       | 强         | 复用安全      | V            | ⭐⭐⭐⭐ 高  | X 不重复    |
| **L2** | Restaking(AVS 层)     | 再质押凭证        | 风险承诺       | 弱         | 安全背书      | V            | ⭐⭐⭐⭐⭐    | X 不算      |
| **L2** | Recursive DeFi          | LP/LST/aToken     | 衍生资产       | 弱         | 杠杆放大      | V            | ⭐⭐⭐⭐⭐    | X 不算      |
| -----  | ----------------------- | ----------------- | -------------  | ------     | ------------  | ------------ | -------- | -------     |
| **L3** | GovToken Lock           | 治理代币          | 内部会计价值   | 强         | 治理绑定      | X            | ⭐⭐⭐⭐⭐    | X 不算      |
| **L3** | Vesting / Lockup        | 未解锁代币        | 未来发行权     | 强         | 激励管理      | X            | ⭐⭐⭐⭐⭐    | X 不算      |

## 主链协议升级历史示例

| Chain        | 升级名称                        | 时间          | 主要内容                                                                 |
| ------------ | ------------------------------- | ------------- | ------------------------------------------------------------------------ |
| **Ethereum** | Pectra                          | 2025初        | 多项EIP激活, 优化执行层/共识层, 增强UX, 提高L2数据可用性, 优化验证者操作 |
| **Ethereum** | Fusaka                          | 2025-12-03    | PeerDAS、Gas Limit提升、Blob参数优化, 增强L2数据能力, 减轻同步负担       |
| **Ethereum** | Glamsterdam/Heze-Bogotá         | 2026规划      | 提高并行执行能力、抗审查性、数据结构效率                                 |
| **BSC**      | Pascal Hard Fork                | 2025-03       | 智能合约钱包、免Gas体验、EIP-7702兼容、批量交易                          |
| **BSC**      | Fermi Hard Fork                 | 2025          | 区块时间从750ms降至450ms (~40%), 提升吞吐量                              |
| **BSC**      | Lorentz/Maxwell                 | 测试网        | 更深层网络优化                                                           |
| **Solana**   | Firedancer                      | 2025          | 第二验证器客户端, 提升网络可靠性和性能                                   |
| **Solana**   | 区块扩展                        | 2025末-2026初 | 增加compute units/block                                                  |
| **Solana**   | Alpenglow                       | 规划中        | 更快共识/最终性机制, 目标ms级最终性                                      |
| **Tron**     | Proposal 102                    | 2025-06       | 区块奖励16→8 TRX, 投票奖励160→128 TRX                                    |
| **Tron**     | Proposal 103                    | 2025-06       | 启用临时存储、内存复制, 提高合约执行效率                                 |
| **Tron**     | GreatVoyage-v4.8.1 (Democritus) | 2025          | ARM支持、P2P稳定性、EVM兼容性增强                                        |
| **Bitcoin**  | OP_RETURN扩展                   | 2025          | 链上数据能力提升, 数据限制扩展                                           |
