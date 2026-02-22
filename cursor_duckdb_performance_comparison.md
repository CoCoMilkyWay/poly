# DuckDB performance comparison
_Exported on 2/23/2026 at 00:47:40 GMT+8 from Cursor (2.5.20)_

---

**User**

duckdb已经是最快的了吗？ 我们从RPC拿chunk才需要10秒， 但是插入write需要一分钟

---

**Cursor**

让我看看你的数据库写入代码。

看了你的代码，DuckDB Appender 已经是比较快的批量插入方式了，但你的瓶颈可能在这几个地方：

## 主要问题

1. **`make_blob` hex解析** - 每个blob字段都在逐字符解析hex字符串，transfer表一行就有5个blob字段。如果transfer有几十万行，就是几百万次hex解析。

2. **有索引的表插入会慢** - 你有很多索引（idx_transfer_from、idx_transfer_to等），每次插入都要更新索引。

## 快速优化建议

**方案1**: 在RPC层预先转换hex为bytes，而不是插入时转

**方案2**: 批量插入前临时删除索引，插入后重建

```cpp
// 插入前
execute("DROP INDEX IF EXISTS idx_transfer_from");
execute("DROP INDEX IF EXISTS idx_transfer_to");
// ... 批量插入 ...
// 插入后重建
execute("CREATE INDEX idx_transfer_from ON transfer(from_addr)");
```

**方案3**: 用列式批量写入代替逐行Appender

```cpp
// 把数据先组装成列向量，一次性写入
std::vector<int64_t> block_numbers;
std::vector<duckdb::Value> tx_hashes;
// ... 预先准备好所有数据 ...
// 然后用 DataChunk 或 COPY 一次写入
```

**方案4**: 先写内存表，再批量同步

```cpp
execute("CREATE TABLE transfer_temp AS SELECT * FROM transfer WHERE false");
// 插入到 transfer_temp (无索引)
// 最后 INSERT INTO transfer SELECT * FROM transfer_temp
```

---

你能告诉我每次大概插入多少行吗？主要是哪个表数据量大？这样我可以给更具体的建议。

---

**User**

你看一下 @README.md  @README_stage1.md  @README_stage2.md  @README_stage3.md 
分析一下行为模式， 有比duckdb更快的数据库吗

---

**Cursor**

看完了。分析一下你的使用模式：

## 行为模式

| 阶段 | 写入 | 读取 | 特点 |
|------|------|------|------|
| Stage1 | **批量追加** ~2800万行transfer | 无 | 高吞吐写入 |
| Stage2 | 批量追加 user_event | 按block范围顺序扫 | 读写混合 |
| Stage3 | 无 | 按用户查询 | 纯读取 |

**数据特点**：追加型、时序、列式查询友好

## 更快的方案

### 1. **直接写Parquet文件** (最快写入)

```cpp
// 绕过数据库，直接写列式文件
// DuckDB 可以直接查询 Parquet: SELECT * FROM 'transfer.parquet'
arrow::RecordBatchBuilder builder;
// ... 填充数据 ...
parquet::WriteTable(*table, pool, output_file);
```

写入速度提升 5-10x，因为：
- 无事务开销
- 无索引维护
- 列式压缩一次完成

### 2. **ClickHouse** (高吞吐写入 + 分析)

批量插入比DuckDB快 3-5x，专为这种场景设计。但需要部署服务。

### 3. **优化当前DuckDB** (最小改动)

```cpp
// 方案A: 写入时关闭索引
execute("DROP INDEX IF EXISTS idx_transfer_from");
// ... 批量插入 ...
execute("CREATE INDEX idx_transfer_from ON transfer(from_addr)");

// 方案B: 用 COPY 代替 Appender
write_conn_->Query("COPY transfer FROM '/tmp/transfer.csv'");

// 方案C: 写入前禁用WAL
config.SetOption("wal_autocheckpoint", duckdb::Value("1GB"));  // 增大到1GB
```

### 4. **Arrow + DataFusion** (内存处理)

RPC返回直接转Arrow格式，全程零拷贝，最后一次写盘。

---

## 我的建议

**如果可以改架构**：Stage1写Parquet文件，Stage2/3用DuckDB查询Parquet。

**如果不想改架构**：
1. 写入前删除所有索引，写完再建
2. 把 `make_blob` 的hex解析移到RPC层
3. 增大checkpoint阈值

要试哪个方案？

---

**User**

上限最高的是哪个

---

**Cursor**

**Parquet + Arrow** 上限最高。

原因：

| 方案 | 瓶颈 | 理论上限 |
|------|------|----------|
| DuckDB | 事务/WAL/索引 | ~50-100 MB/s |
| ClickHouse | 网络+服务开销 | ~200-500 MB/s |
| **Parquet** | **磁盘顺序IO** | **1-3 GB/s (SSD)** |

Parquet 直接写列式文件，无任何中间层：
- 无事务开销
- 无WAL
- 无索引维护
- 纯顺序写入

你的数据量（~2800万行transfer，约3GB），用Parquet理论上1-2秒能写完。

**最妙的是**：DuckDB 可以直接查询Parquet，不需要导入：

```sql
SELECT * FROM 'transfer.parquet' WHERE from_addr = x'...'
```

所以架构变成：
- Stage1: 写 `*.parquet` 文件
- Stage2/3: DuckDB 直接查询 Parquet

改动不大，收益最大。要实现吗？

---

**User**

arrow是什么？

---

**Cursor**

**Apache Arrow** 是一个内存中的列式数据格式标准。

核心概念：

```
传统行式存储 (JSON/CSV/数据库行):
Row 1: [block, tx_hash, from, to, amount]
Row 2: [block, tx_hash, from, to, amount]
Row 3: [block, tx_hash, from, to, amount]

Arrow 列式存储:
blocks:   [1, 2, 3, ...]      ← 连续内存
tx_hash:  [0x.., 0x.., ...]   ← 连续内存
from:     [0x.., 0x.., ...]   ← 连续内存
to:       [0x.., 0x.., ...]   ← 连续内存
amount:   [100, 200, 300, ...]← 连续内存
```

**为什么快**：

1. **零拷贝** - 数据在内存中直接操作，不需要序列化/反序列化
2. **CPU友好** - 同类型数据连续存放，SIMD向量化加速
3. **跨语言** - C++/Python/Rust/Go 共享同一内存布局

**和Parquet的关系**：

```
Arrow = 内存格式 (快速计算)
Parquet = 磁盘格式 (高效存储)

Arrow ←→ Parquet 转换极快，因为都是列式
```

**在你的场景**：

```cpp
// 当前：RPC → JSON解析 → hex转bytes → Appender逐行写 → DuckDB
// Arrow：RPC → Arrow列 (零拷贝) → 直接写Parquet
```

省掉了大量中间转换开销。

---

**User**

好的没问题， 用当前方案重构

---

**Cursor**

让我先看一下当前的代码结构。

让我检查一下系统上的Arrow库情况。

Arrow库没有安装。有两个选择：

**选项A**：安装Arrow并重构（需要 `sudo apt install libarrow-dev libparquet-dev`）

**选项B**：先用更简单的优化方案，不需要新依赖：
1. 写入时临时删除索引，写完重建
2. 优化 `make_blob` hex解析
3. 增大WAL阈值

**选项B** 改动小，预计能提升3-5倍，可以先试试。如果不够再上Arrow。

你想：
1. 先试选项B（快速优化）
2. 直接上选项A（Arrow重构）

哪个？

---

**User**

arrow套parquet是上限最高的方案吗？

---

**Cursor**

不完全是。更准确地说：

| 方案 | 写入速度 | 存储大小 | 查询效率 |
|------|----------|----------|----------|
| **Arrow IPC (Feather)** | **最快** | 中等 | 高 |
| Parquet | 快 | **最小** | 高 |
| CSV | 一般 | 大 | 低 |

**Feather (Arrow IPC)** 写入比 Parquet 更快，因为：
- Parquet 需要压缩、编码、分页
- Feather 直接dump Arrow内存布局，几乎零开销

DuckDB 同样可以直接查询 Feather：

```sql
SELECT * FROM 'transfer.feather' WHERE from_addr = x'...'
```

**结论**：
- 追求**写入极致** → Feather
- 追求**存储最小** → Parquet
- 你的场景（一次写入、多次查询）→ **Feather更合适**

写入速度排序：
```
Feather > Parquet > DuckDB Appender > INSERT语句
```

要上 Arrow + Feather 方案吗？

---

**User**

好的，用Feather + duckdb的方案
stage2/3用duckdb是最好的方案？

---

**Cursor**

Stage2/3的使用模式：

| 阶段 | 操作 | 最佳方案 |
|------|------|----------|
| Stage2 | 读Stage1数据 | DuckDB查Feather |
| Stage2 | 写user_event | **也用Feather** |
| Stage3 | 按用户查询 | DuckDB查Feather |

**DuckDB对读取/分析确实是最好的**：
- 嵌入式，无需部署
- 直接查询Feather，零导入成本
- 列式扫描效率高
- SQL灵活

**最终架构**：

```
Stage1: RPC → Arrow → Feather文件 (transfer.feather, order_filled.feather, ...)
Stage2: DuckDB查Feather → Arrow → user_event.feather
Stage3: DuckDB查user_event.feather → 内存回放
```

所有**写入用Feather**，所有**查询用DuckDB**。

---

唯一的"更极致"选项是直接用Arrow内存操作（不经过DuckDB SQL解析），但收益不大，复杂度高很多。

确认这个方案？我开始重构。

---

**User**

好的用此方案， 你告诉我arrow/feather怎么安装， 然后开始重构

---

**Cursor**

计划已创建。

**安装Arrow**（你先执行这个）：

```bash
# 1. 安装依赖
sudo apt update
sudo apt install -y -V ca-certificates lsb-release wget

# 2. 添加Apache Arrow仓库
wget https://packages.apache.org/artifactory/arrow/$(lsb_release --id --short | tr 'A-Z' 'a-z')/apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb
sudo apt install -y -V ./apache-arrow-apt-source-latest-$(lsb_release --codename --short).deb

# 3. 安装Arrow C++
sudo apt update
sudo apt install -y -V libarrow-dev
```

安装完成后告诉我，我开始重构代码。

---

**User**

已经安装了， 详细准备具体怎么重构

---

**Cursor**

让我先详细了解Stage2如何读取Stage1的数据。

我已经理解了完整架构。现在更新详细的重构计划。

明白了。计划已清晰，开始实施。

重构完成。修改了以下文件：

**1. `projects/core/CMakeLists.txt`** - 添加Arrow依赖

**2. `src/core/feather_writer.hpp`** (新建) - Feather写入实现，14个表全部支持

**3. `src/core/database.hpp`** - 删除了：
   - Stage1所有表的CREATE TABLE语句
   - `atomic_multi_insert_appender` 方法
   - `make_blob` 方法
   
   添加了 `feather_path()` 和 `data_dir()` 方法

**4. `src/stage1/chain_sync.hpp`** - 改用 `FeatherWriter::append_events()` + `db_.set_last_block()`

**5. `src/stage2/event_build.hpp`** - 所有SQL查询改为查询Feather文件路径

数据目录结构：
```
data/
├── stage1/
│   ├── transfer.feather
│   ├── order_filled.feather
│   └── ... (共14个)
└── poly.duckdb
```

你可以编译测试了。注意首次运行需要删除旧的 `poly.duckdb` 或在空数据目录启动。

