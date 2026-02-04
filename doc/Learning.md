## 目标：重建历史交易数据库

### 方案A：用Subgraph(推荐)

```
步骤1: polymarket-subgraph (核心)
       └── 理解schema定义、event handler、entity映射
       └── 重点文件: schema.graphql, src/mapping.ts
       └── 实体: Trade, Market, User, Position, Liquidity

步骤2: 部署自己的Graph Node
       └── 需要: Postgres + IPFS + Graph Node + Archive Node RPC
       └── Docker Compose一键部署

步骤3: resolution-subgraph + positions-subgraph
       └── 补充结算和持仓数据
```

### 方案B：直接解析链上日志

```
步骤1: 看ctf-exchange合约的event定义
       └── OrderFilled, OrderCancelled等事件

步骤2: 用Archive Node RPC拉取历史logs
       └── Polygon主网，需要archive节点访问
       └── 推荐: Alchemy/QuickNode archive tier

步骤3: 参考polymarket-subgraph的handler逻辑
       └── 理解如何从event解析出完整交易信息
```

## 需要的基础设施

| 组件                 | 说明                           |
| -------------------- | ------------------------------ |
| Polygon Archive Node | Alchemy/QuickNode archive tier |
| Graph Node           | Docker部署                     |
| PostgreSQL           | 存储索引数据                   |
| IPFS                 | Graph Node依赖                 |

## 快速开始建议

**想做交易：**

- py-clob-client + python-order-utils

**想做AI交易：**

- agents

**想做市：**

- poly-market-maker

**想理解合约：**

- conditional-tokens-contracts → ctf-exchange → uma-ctf-adapter

**想重建数据：**

- fork polymarket-subgraph → 部署Graph Node → 同步
