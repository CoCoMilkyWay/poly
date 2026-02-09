---
name: Polygon Full Node Setup
overview: 用 Erigon v3.3.7 在本地搭建 Polygon (bor-mainnet) full node,配置增量同步和数据完整性保障,为后续 custom indexer 提供 JSON-RPC (eth_getLogs) 接口。
todos:
  - id: dirs-config
    content: 创建 datadir 目录 + TOML 配置文件
    status: pending
  - id: first-start
    content: 首次启动 Erigon,确认连接 Heimdall API 并开始同步
    status: pending
  - id: systemd
    content: 配置 systemd 服务实现自动重启和开机启动
    status: pending
  - id: verify-sync
    content: 验证同步状态 (eth_syncing) 和 logs 可查询性
    status: pending
isProject: false
---

# Polygon Full Node 本地部署方案

## 硬件现状

- RAM: 128GB (要求 >= 32GB, 充裕)
- 磁盘: 3.7TB NVMe, 3.5TB 可用 (Polygon full node 约 2TB, 充裕)
- 二进制: `/home/chuyin/work/erigon_v3.3.7_linux_amd64v2/erigon` 已就绪

## 架构

Polygon 网络由两层组成：

```mermaid
graph LR
    Heimdall["Heimdall (共识层)"] -->|checkpoint 验证| Erigon["Erigon Bor (执行层)"]
    Erigon -->|JSON-RPC :8545| Indexer["Custom Indexer (eth_getLogs)"]
```



- **Heimdall**: Polygon 的共识/检查点服务,Erigon 通过 `--bor.heimdall` 连接
- **Erigon Bor**: 执行层,存储区块/交易/日志

## 方案选择: Heimdall 接入方式

Erigon 的 `--bor.heimdall` 支持指向**远程公共 API**,不一定要本地跑 Heimdall 节点：

- **选项 A (推荐先用)**: 用公共 Heimdall API `https://heimdall-api.polygon.technology`,省去部署 Heimdall 的复杂度。同步完成后再评估是否需要本地 Heimdall。
- **选项 B**: 本地跑 Heimdall + Erigon 全套。更自主,但 Heimdall 也需要额外磁盘和同步时间。

## Step 1: 创建目录结构和启动配置

```
/home/chuyin/work/polygon-data/     # datadir, 约 2TB
  chaindata/
  snapshots/
    domain/
    history/
    idx/
    accessor/
```

创建 TOML 配置文件 `/home/chuyin/work/poly/erigon-polygon.toml`:

```toml
datadir = "/home/chuyin/work/polygon-data"
chain = "bor-mainnet"

# 剪枝模式: full = 只保留最新状态 + 必要区块 (你只需要 event logs, 够用)
"prune.mode" = "full"

# Heimdall 公共 API (选项 A)
"bor.heimdall" = "https://heimdall-api.polygon.technology"

# JSON-RPC 配置 (indexer 需要)
http = true
"http.addr" = "127.0.0.1"
"http.port" = 8545
"http.api" = ["eth", "erigon", "net", "web3", "debug", "trace"]

# WebSocket (可选, indexer 实时订阅用)
ws = true
"ws.port" = 8546

# gRPC 内部接口
"private.api.addr" = "127.0.0.1:9090"

# P2P 端口
port = 30303
"torrent.port" = 42069

# 日志
"log.dir.path" = "/home/chuyin/work/polygon-data/logs"
"log.dir.verbosity" = "info"

# Polygon 特有: 增大 sync loop limit 提速
"sync.loop.block.limit" = 10000
```

## Step 2: 启动命令

```bash
/home/chuyin/work/erigon_v3.3.7_linux_amd64v2/erigon \
  --config /home/chuyin/work/poly/erigon-polygon.toml
```

## Step 3: systemd 服务 (增量同步 + 自动重启)

创建 `/etc/systemd/system/erigon-polygon.service`,确保机器重启后自动恢复同步：

```ini
[Unit]
Description=Erigon Polygon Full Node
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=chuyin
ExecStart=/home/chuyin/work/erigon_v3.3.7_linux_amd64v2/erigon --config /home/chuyin/work/poly/erigon-polygon.toml
Restart=on-failure
RestartSec=10
LimitNOFILE=65536
TimeoutStopSec=300

[Install]
WantedBy=multi-user.target
```

启用: `sudo systemctl enable --now erigon-polygon`

## Step 4: 数据完整性保障

1. **Erigon 内置**: 每个 snapshot segment 有 checksum 校验；chaindata 使用 MDBX 事务保证 ACID
2. **增量同步**: Erigon 天然支持 -- 停了再启动就从断点继续,不丢进度 (`--sync.loop.block.limit=10000` 控制每轮最大块数,防止部分进度丢失)
3. **监控**: 启动后通过日志和 `eth_syncing` RPC 判断同步状态

## 预估

- 初始同步: 约 **21 小时** (README 数据)
- 磁盘占用: 约 **2TB** (full node)
- 同步完成后: 自动追链尖,延迟秒级

## 关于你的 indexer

根据 `Polygon.md`,你只需要 `eth_getLogs` 从特定合约获取 events,`--prune.mode=full` 完全满足需求。同步完成后,indexer 连 `http://127.0.0.1:8545` 即可查询所有历史 event logs。