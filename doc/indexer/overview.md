## Indexer 架构对比

| 维度             | The Graph          | Subsquid        | Goldsky           | Envio                    | Ponder            | 自建 (RPC+DB) |
| ---------------- | ------------------ | --------------- | ----------------- | ------------------------ | ----------------- | ------------- |
| **架构**         | 去中心化网络       | 去中心化Archive | 中心化托管        | 中心化/可自托管          | 中心化/可自托管   | 完全自控      |
| **去中心化程度** | ⭐⭐⭐⭐⭐              | ⭐⭐⭐⭐            | ⭐                 | ⭐⭐                       | ⭐⭐                | ⭐             |
| **开发语言**     | AssemblyScript     | TypeScript      | AssemblyScript    | TypeScript/Rust/ReScript | TypeScript        | 任意          |
| **开发体验**     | ⭐⭐                 | ⭐⭐⭐⭐            | ⭐⭐                | ⭐⭐⭐⭐                     | ⭐⭐⭐⭐⭐             | ⭐⭐            |
| **索引速度**     | 慢                 | 快              | 中等              | 非常快                   | 快                | 取决于实现    |
| **历史数据回填** | 慢                 | 快(Archive网络) | 中等              | 快(HyperSync)            | 中等              | 慢(依赖RPC)   |
| **实时性**       | 秒级               | 秒级            | 秒级              | 毫秒级                   | 秒级              | 取决于实现    |
| **查询接口**     | GraphQL            | GraphQL         | GraphQL           | GraphQL                  | GraphQL/SQL       | 自定义        |
| **成熟度**       | ⭐⭐⭐⭐⭐              | ⭐⭐⭐⭐            | ⭐⭐⭐               | ⭐⭐⭐                      | ⭐⭐                | -             |
| **生态/文档**    | ⭐⭐⭐⭐⭐              | ⭐⭐⭐⭐            | ⭐⭐⭐               | ⭐⭐⭐                      | ⭐⭐⭐               | -             |
| **多链支持**     | 广泛               | 广泛            | 广泛              | 广泛                     | 有限              | 需自己实现    |
| **成本模型**     | GRT代币付费查询    | SQD代币         | 订阅制            | 免费/订阅制              | 免费开源          | 基础设施成本  |
| **调试难度**     | 高(WASM)           | 中              | 高(WASM)          | 低                       | 低                | 中            |
| **热重载**       | ❌                  | ✅               | ❌                 | ✅                        | ✅                 | 取决于实现    |
| **类型安全**     | 弱                 | 强              | 弱                | 强                       | 强                | 取决于实现    |
| **单元测试**     | Matchstick(有限)   | Jest/Vitest     | Matchstick        | 原生支持                 | 原生支持          | 原生支持      |
| **适用场景**     | 生产级去中心化应用 | 高性能数据需求  | 快速部署兼容Graph | 高吞吐量应用             | 快速原型/中小项目 | 完全定制需求  |

---

## PolyMarket 的 the Graph 官方实现覆盖率分析(链上信息是不是覆盖完全)

| 合约                   | 事件                              | 索引 | 说明 / 缺失后果                                                            |
| ---------------------- | --------------------------------- | ---- | -------------------------------------------------------------------------- |
| **ConditionalTokens**  | `ConditionPreparation`            | ✅    | 条件创建: 记录conditionId、oracle、questionId、outcomeSlotCount            |
|                        | `ConditionResolution`             | ✅    | 条件解决: 记录payout比例数组, 用于结算                                     |
|                        | `PositionSplit`                   | ✅    | 拆分仓位: 用户用抵押品铸造outcome tokens                                   |
|                        | `PositionsMerge`                  | ✅    | 合并仓位: 用户将完整outcome tokens赎回抵押品                               |
|                        | `PayoutRedemption`                | ✅    | 结算赎回: 条件解决后用户赎回获胜方token                                    |
|                        | `TransferSingle`                  | ✅    | ERC1155单个token转账: 追踪用户持仓变化                                     |
|                        | `TransferBatch`                   | ✅    | ERC1155批量token转账: 追踪用户持仓变化                                     |
|                        | `ApprovalForAll`                  | ❌    | 授权事件: 无法追踪用户对合约的授权关系                                     |
| **CTFExchange**        | `OrderFilled`                     | ✅    | 订单成交: 记录maker/taker/价格/数量/fee                                    |
|                        | `OrdersMatched`                   | ✅    | 订单撮合: taker订单与多个maker订单匹配                                     |
|                        | `TokenRegistered`                 | ✅    | Token注册: 将tokenId/complement/conditionId关联并允许交易                  |
|                        | `OrderCancelled`                  | ❌    | 订单取消: 无法追踪用户主动取消的订单记录                                   |
|                        | `FeeCharged`                      | ❌    | 费用收取: 无法独立追踪费用(需从OrderFilled.fee间接算)                    |
| **NegRiskAdapter**     | `MarketPrepared`                  | ✅    | NegRisk市场创建: 记录marketId、oracle、feeBips                             |
|                        | `QuestionPrepared`                | ✅    | NegRisk问题创建: 在市场下新增一个question                                  |
|                        | `PositionSplit`                   | ✅    | NegRisk拆分: 用USDC铸造该question的yes/no tokens                           |
|                        | `PositionsMerge`                  | ✅    | NegRisk合并: 将yes/no tokens赎回USDC                                       |
|                        | `PositionsConverted`              | ✅    | NegRisk转换: 多个NO仓位转换为YES仓位+抵押品                                |
|                        | `PayoutRedemption`                | ✅    | NegRisk结算: 解决后赎回获胜token                                           |
|                        | `OutcomeReported`                 | ❌    | 结果报告: 无法追踪NegRisk市场被报告outcome的时刻和结果                     |
| **UmaCtfAdapter**      | `QuestionInitialized`             | ✅    | 问题初始化: 创建question并向OO发起价格请求                                 |
|                        | `QuestionReset`                   | ✅    | 问题重置: dispute后重新发起价格请求                                        |
|                        | `QuestionResolved`                | ✅    | 问题解决: 从OO获取价格并report到CTF                                        |
|                        | `QuestionPaused`                  | ❌    | 问题暂停: 无法追踪市场被admin暂停解决                                      |
|                        | `QuestionUnpaused`                | ❌    | 问题恢复: 无法追踪市场被admin恢复                                          |
|                        | `QuestionFlagged`                 | ❌    | 问题标记: 无法追踪市场被标记需人工干预                                     |
|                        | `QuestionUnflagged`               | ❌    | 问题取消标记: 无法追踪市场标记被移除                                       |
|                        | `QuestionManuallyResolved`        | ❌    | 人工解决: 无法追踪admin强制设定payout的记录                                |
| **OptimisticOracle**   | `RequestPrice`                    | ✅    | 价格请求: 向OO发起解决请求                                                 |
|                        | `ProposePrice`                    | ✅    | 价格提议: proposer提交答案                                                 |
|                        | `DisputePrice`                    | ✅    | 价格争议: disputer对提议发起挑战                                           |
|                        | `Settle`                          | ❌    | 价格结算: 无法追踪OO最终settle确认时刻                                     |
| **FeeModule**          | `FeeRefunded`                     | ✅    | 费用退还: maker/taker订单的fee退款记录                                     |
|                        | `FeeWithdrawn`                    | ❌    | 费用提取: 无法追踪运营方从FeeModule提取费用                                |
| **FPMM**               | `FixedProductMarketMakerCreation` | ✅    | AMM创建: 创建固定乘积做市商合约                                            |
|                        | `FPMMBuy`                         | ✅    | AMM买入: 用户通过AMM买入outcome tokens                                     |
|                        | `FPMMSell`                        | ✅    | AMM卖出: 用户通过AMM卖出outcome tokens                                     |
|                        | `FPMMFundingAdded`                | ✅    | AMM注资: LP向AMM添加流动性                                                 |
|                        | `FPMMFundingRemoved`              | ✅    | AMM撤资: LP从AMM移除流动性                                                 |
| **positions-subgraph** | `ConditionPreparation` (binary)   | ⚠️    | 代码bug: outcomeSlotCount==2时被return跳过, 普通二元市场不被该subgraph索引 |
