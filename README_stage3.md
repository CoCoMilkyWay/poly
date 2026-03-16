## 交易口径事件(符号驱动, 支持做空)
> `qty = abs(amount)`
> `dir = sign(amount)`(`+1 / -1 / 0`)
> `px = price_1e6 / 1e6`
> `cost_before = cost`(本事件前该token的cost)
> `pos_before = pos`(本事件前该token的pos)
> `has_usd = collateral ∈ {USDC(1), USDCe(2), USDT(3), WrappedUSDCe(4)}`

| 事件族                                                     | `amount > 0` (正向腿)                                        | `amount < 0` (反向腿)                                         | realized 规则                                                | lp更新                   |
| ---------------------------------------------------------- | ------------------------------------------------------------ | ------------------------------------------------------------- | ------------------------------------------------------------ | ------------------------ |
| `Order* / FPMM* / Split* / Merge* / Redemption*`           | 先平空(`pos<0`)再开多；开多部分 `cost += open_long_qty * px` | 先平多(`pos>0`)再开空；开空部分 `cost -= open_short_qty * px` | 仅"平掉已有仓位"部分计 realized；开新仓部分不计当期 realized | 仅 `Order* / FPMM*` 更新 |
| `Convert`                                                  | 先平空再开多；开多部分 `cost += open_long_qty * convert_px`  | 先平多再开空；开空部分 `cost -= open_short_qty * convert_px`  | `convert_px = (popcount-1)/popcount`, 仅平仓部分计 realized  | 否                       |
| `TransferIn* / TransferOut* / FPMMLPRemove / FPMMLPReturn` | 先平空再开多；`cost` 不增加                                  | 先平多再开空；`cost` 不增加(保持 transfer 语义)               | 始终 `0`                                                     | 否                       |
| `FPMMLPAdd`                                                | 不改 `pos/cost`                                              | 不改 `pos/cost`                                               | `0`                                                          | 否                       |

- 只有 USD 类抵押物 (`has_usd=true`) 才计算 `cost / realized_delta / lp`；非 USD 仅更新 `pos`.
- `amount == 0` 视为 no-op(仅事件计数推进, 不改持仓/成本/PnL).
- `EventType` 决定“经济语义”, `amount` 符号决定“方向”；同一 `EventType` 可出现正负.
- unrealized_pnl = Σ(pos * lp / 1e6 - cost)  // 对所有 token_state 中 pos != 0 且 lp > 0 的token
- 只有 Order/FPMM 交易事件会更新 lp
- Split/Merge/Transfer 等不更新 lp
- 若某token从未交易过(如TransferIn获得),该token不计入unrealized(lp=0)

## 建立动态高玩池(特征工程, 统一表 + Compute Graph)

10w块≈2.3天(日) | 100w块≈23天(月) | 1000w块≈230天(年)
特征张量索引: `User * Time(per 10w bucket) * Feature`

时序特征(名称,数量,统计方式,描述):
注: 数量 N+1 表示 N 个行业 + 1 个综合(tag_id=-1)
    近期平均持仓token数        N+1  时间加权           近10w块移动平均持仓token数(分行业+综合)
    近月平均持仓token数        N+1  等权               近100w块移动平均持仓token数(分行业+综合)
    近年平均持仓token数        N+1  等权               近1000w块移动平均持仓token数(分行业+综合)
    近期平均持仓暴露额         N+1  时间加权           近10w块移动平均持仓暴露额(分行业+综合)
    近月平均持仓暴露额         N+1  等权               近100w块移动平均持仓暴露额(分行业+综合)
    近年平均持仓暴露额         N+1  等权               近1000w块移动平均持仓暴露额(分行业+综合)
    近期总和交易额             N+1  加总               近10w块总和交易额(分行业+综合)
    近月平均交易额             N+1  等权               近100w块平均交易额(分行业+综合)
    近年平均交易额             N+1  等权               近1000w块平均交易额(分行业+综合)
    近期平均持仓周期           N+1  时间,金额加权      近10w块移动平均持仓周期(分行业+综合)
    近月平均持仓周期           N+1  等权               近100w块移动平均持仓周期(分行业+综合)
    近年平均持仓周期           N+1  等权               近1000w块移动平均持仓周期(分行业+综合)
    近期夏普                   1    精确计算           近10w块全账户夏普(仅全账户)
    近月夏普                   1    精确计算           近100w块全账户夏普(仅全账户)
    近年夏普                   1    精确计算           近1000w块全账户夏普(仅全账户)
截面特征:
    输入: anchor bucket + 多条自由表达式(filters) + 排序表达式(sort_expr)
    输出: 过滤 + 排序后的 top 100 用户地址
    目标: 支持“分行业 nested 特征”的横截面对比与选人

注:
  1. 交易额里: 只记录会直接创造头寸暴露的操作(比如铸币, 合币就不应该记入), 暴露方向不重要
  2. 平均持仓: 需要统计周期内多个事件(非均匀)的持仓快照(记录不同token的平均持仓周期), 再按照token金额, 事件时间加权
  3. 夏普: 无风险=0, 基于账户级 `pnl` 的窗口级精确重算
     - 仅统计全账户 Sharpe(tag_id=-1)，不支持分行业 Sharpe
     - `sharpe_10w / sharpe_100w / sharpe_1000w` 最终统一 normalize 到 `1000w block` 量纲
     - 数据源: `account_bucket_pnl_state` + 进程内 `SharpeSparseCache`
     - 持久化只存有事件的 `(user_addr, block_bucket)`；空 bucket 不落库
     - bucket 内只存 PnL 变化点；同一 block 多事件先合并，只取块末 `(block, pnl)`
     - 运行时只保留最近 `100` 个 bucket；超窗 bucket 立即释放，空用户缓存立即擦除
     - 其中: `pnl = realized_cum + unrealized_pnl`
     - 核心动机:
       - 只关心“本窗口内”的 PnL 路径，不让窗口开始前的历史累计盈亏直接污染本窗口 return
       - 同样的 `Δpnl`，在更小的账户尺度上应该体现为更大的 return
       - 区间内出现大跳，不管是向上还是向下，都应该显著影响波动与 Sharpe
     - 对窗口 `W ∈ {10w, 100w, 1000w}`:
       - 左边界锚点 `p0` 使用窗口起点前最后一个已知 `pnl`；若不存在则记 `0`
       - 先做区间重标定: `x_i = pnl_i - p0`，因此左边界恒有 `x_0 = 0`
       - `min_interval_pnl_W = min(x_i)`，`max_interval_pnl_W = max(x_i)`，遍历窗口内全部 block 采样点以及左右边界锚点
       - `range_W = max_interval_pnl_W - min_interval_pnl_W`
       - 因 `x_0 = 0`，必有 `range_W >= abs(min_interval_pnl_W)`；当前实现保留 `range_W` 主要是为了理解区间跨度与做校验，不直接进入 `nav_base_W`
       - `avg_exposure_W`:
         - `10w`: 直接取当前 bucket 的 `exposure_avg_10w`
         - `100w/1000w`: 先对窗口内 bucket 的 `exposure_avg_10w` 做等权平均
       - `nav` 的直觉:
         - 若窗口内 `x_i` 从未跌到负数，则 `abs(min_interval_pnl_W) = 0`
         - 若窗口内 `x_i` 跌到负数，则先用 `abs(min_interval_pnl_W)` 把整条区间 PnL 曲线抬到正区间
         - 在此基础上，再叠加 `avg_exposure_W` 作为账户尺度的稳定底座；`+1 USD` 只是正数 floor
       - `nav_base_W = avg_exposure_W + abs(min_interval_pnl_W) + 1 USD`
       - `nav_i = nav_base_W + x_i`
       - 后续所有区间收益与单次跳变，统一都基于这条 `nav` 曲线计算
       - 相邻采样点收益: `r_i = (nav_i - nav_{i-1}) / nav_{i-1}`
       - `Δt_i = block_i - block_{i-1}`
     - 边界规则:
       - 右边界锚点使用窗口末 block；若末尾无新事件，则用最后一个 `pnl` 平推到窗口末尾
     - 时间加权平均收益率: `μ = Σr_i / T`
     - 时间加权方差: `σ² = Σ(r_i^2 / Δt_i) / T - μ²`
     - raw Sharpe = `μ / σ`
     - 输出 Sharpe = `raw_sharpe * sqrt(10000000)`，即统一归一到 `1000w block`
     - `nav_i <= 0`、`Δt_i <= 0`、`T <= 0` 或 `σ² <= 0` 时，该窗口 Sharpe 记 `0`
  4. 本次 Sharpe 口径变更不做兼容迁移；切换代码后必须重建 `data/stage3`

// ============================================================================
// Stage3 完整数据结构设计 (mmap + pool)
// ============================================================================

// ------------------------------ 常量 ------------------------------

constexpr size_t MAX_CONDITIONS     = 1'000'000;      // 100万 conditions
constexpr size_t MAX_USERS          = 10'000'000;     // 1000万用户
constexpr size_t MAX_TOKENS         = 100'000'000;    // 1亿 token slots
constexpr size_t MAX_FEATURES       = 500'000'000;    // 5亿 feature slots
constexpr size_t MAX_SHARPE_AGGS    = 50'000'000;     // 5000万 sharpe bucket 聚合
constexpr size_t MAX_SHARPE_SAMPLES = 500'000'000;    // 5亿 sharpe 样本点
constexpr size_t OUTCOME_MAX        = 256;            // 最大 outcome 数
constexpr uint32_t NULL_IDX         = UINT32_MAX;     // 空指针
constexpr uint64_t NULL_LOG_OFFSET  = UINT64_MAX;     // events.log 空指针

using Address20 = std::array<uint8_t, 20>;

// ------------------------------ 主存储结构 ------------------------------

struct Stage3Store {

  // ==================== Header (4KB) ====================
  struct Header {
    uint64_t magic;                        // "STAGE3\0\0"
    uint64_t version;                      // 数据版本
    
    // SyncCursorState
    int64_t  cursor_sort_key;              // 已同步到的 sort_key, -1 表示空库
    int64_t  cursor_processed_events;      // 已处理事件数
    
    // 全局统计
    uint64_t user_count;                   // 有效用户数
    int64_t  head_bucket;                  // 当前最新 bucket
    
    // Pool 使用情况
    uint64_t token_pool_used;
    uint64_t feature_pool_used;
    uint64_t sharpe_agg_pool_used;
    uint64_t sharpe_sample_pool_used;
    
    // Free list heads
    uint32_t token_free_head;
    uint32_t feature_free_head;
    uint32_t sharpe_agg_free_head;
    uint32_t sharpe_sample_free_head;
    
    // Events log
    uint64_t events_log_tail;              // events.log 写入 byte offset
    
    uint8_t  _pad[4096 - 104];
  } header;

  // ==================== ConditionMeta[100万] (~2GB) ====================
  struct ConditionMeta {                   // 2056B per condition
    uint8_t  outcome_count;                // outcome 数量
    int8_t   tag_id;                       // 行业分类 (-1=unknown, 0-13=行业)
    uint8_t  flags;                        // bit0=valid
    uint8_t  _pad0[5];
    int64_t  payout_numerators[OUTCOME_MAX]; // 256 × 8B = 2048B, 支持任意 outcome
  } conditions[MAX_CONDITIONS];            // 100万 × 2056B ≈ 2GB

  // ==================== TokenPool[1亿] (4.8GB) ====================
  struct TokenSlot {                       // 48B
    // key
    uint32_t user_idx;                     // 所属用户 -> users[]
    int32_t  cond_idx;                     // condition index, -1=free slot
    int16_t  token_idx;                    // outcome index
    int16_t  collateral;                   // 1=USDC, 2=USDCe, 3=USDT, 4=WrappedUSDCe
    // value (对应原 TokenState)
    int64_t  pos;                          // 持仓量 (1e6, 可负=空头)
    int64_t  cost;                         // 成本基础 (1e6, 可负=空头信用)
    int64_t  lp;                           // 最近成交价 (1e6)
    int64_t  entry_block;                  // 加权平均建仓 block
    // chain
    uint32_t next;                         // 同用户下一个 token / free list
    uint32_t _pad;
  } token_pool[MAX_TOKENS];                // 1亿 × 48B = 4.8GB

  // ==================== FeaturePool[5亿] (140GB) ====================
  struct FeatureSlot {                     // 280B (对应原 FeatureTensorState)
    // key
    uint32_t user_idx;                     // 所属用户
    int32_t  bucket;                       // block_bucket (10w blocks per bucket)
    int8_t   tag_id;                       // -1=全局聚合, 0-13=行业
    uint8_t  flags;                        // bit0=valid
    uint16_t _pad0;
    // chain
    uint32_t next;                         // 同用户下一个 feature / free list
    uint32_t _pad1;

    // Node-A0: 增量续算锚点 (持仓 tail 修正, 无需回扫 event_fact)
    int64_t  last_sort_key_10w;
    int64_t  last_block_10w;               // 当前 pending block
    int64_t  last_exposure_10w;            // 当前 pending exposure
    int64_t  last_holding_period_10w_lo;   // HUGEINT low part
    int64_t  last_holding_period_10w_hi;   // HUGEINT high part
    int64_t  last_token_count_10w;

    // Node-A: 10w 原子统计 (事件增量累加)
    int64_t  time_weight_sum_10w;
    int64_t  token_count_tw_sum_10w;
    int64_t  exposure_tw_sum_10w_lo;       // HUGEINT low
    int64_t  exposure_tw_sum_10w_hi;       // HUGEINT high
    int64_t  volume_sum_10w;
    int64_t  holding_period_exp_tw_sum_10w_lo;  // HUGEINT low
    int64_t  holding_period_exp_tw_sum_10w_hi;  // HUGEINT high

    // Node-B: 10w 归一化输出
    int64_t  token_avg_10w;
    int64_t  exposure_avg_10w;
    int64_t  volume_10w;
    int64_t  holding_period_avg_10w;
    float    sharpe_10w;
    float    _pad2;

    // Node-C: 前缀缓存 (按 bucket 单调推进, 用于窗口投影)
    int64_t  ps_token_avg_10w;
    int64_t  ps_exposure_avg_10w;
    int64_t  ps_volume_10w;
    int64_t  ps_holding_period_avg_10w;

    // Node-D: 窗口投影输出 (由 Node-C O(1) 计算)
    int64_t  token_avg_100w;
    int64_t  token_avg_1000w;
    int64_t  exposure_avg_100w;
    int64_t  exposure_avg_1000w;
    int64_t  volume_avg_100w;
    int64_t  volume_avg_1000w;
    int64_t  holding_period_avg_100w;
    int64_t  holding_period_avg_1000w;
    float    sharpe_100w;
    float    sharpe_1000w;

    int64_t  updated_sort_key;
  } feature_pool[MAX_FEATURES];            // 5亿 × 280B = 140GB

  // ==================== SharpeAggPool[5000万] (2.4GB) ====================
  // 对应原 AccountBucketPnlState 的 bucket 级聚合
  struct SharpeAgg {                       // 48B
    uint32_t user_idx;
    int32_t  bucket;
    int64_t  close_pnl;                    // bucket 末尾 pnl
    int64_t  min_pnl;                      // bucket 内最小 pnl
    int64_t  max_pnl;                      // bucket 内最大 pnl (用于 Sharpe 计算)
    uint32_t sample_head;                  // -> sharpe_sample_pool 链表头
    uint16_t sample_count;
    uint16_t _pad0;
    int32_t  last_block;                   // 最近更新的 block (用于去重同 block 多事件)
    uint32_t next;                         // 同用户下一个 agg / free list
  } sharpe_agg_pool[MAX_SHARPE_AGGS];      // 5000万 × 48B = 2.4GB

  // ==================== SharpeSamplePool[5亿] (16GB) ====================
  // 稀疏 (block, pnl) 样本, 用于精确 Sharpe 计算
  struct SharpeSample {                    // 32B
    uint32_t agg_idx;                      // 所属 SharpeAgg
    int32_t  block_offset;                 // bucket 内 block 偏移 (0 ~ 99999)
    int64_t  pnl;                          // realized_cum + unrealized_pnl
    uint32_t next;                         // 同 agg 下一个样本 / free list
    uint32_t _pad;
    int64_t  _reserved;
  } sharpe_sample_pool[MAX_SHARPE_SAMPLES]; // 5亿 × 32B = 16GB

  // ==================== Users[1000万] (1.28GB) ====================
  struct UserBlock {                       // 128B (对应原 UserSummaryState + 扩展)
    // 基础信息
    Address20 addr;                        // 20B, 用户地址
    uint32_t  flags;                       // 4B, bit0=occupied

    // 统计 (对应原 UserSummaryState)
    int64_t   total_events;                // 总事件数
    int64_t   total_realized_pnl;          // 累计已实现 PnL
    int64_t   total_unrealized_pnl;        // 当前未实现 PnL
    int64_t   last_sort_key;               // 最新事件 sort_key

    // Pool 引用 (链表头)
    uint32_t  token_head;                  // -> token_pool, 当前持仓
    uint32_t  token_count;                 // 有效持仓 token 数 (active_tokens)
    uint32_t  feature_head;                // -> feature_pool
    uint32_t  feature_count;
    uint32_t  sharpe_agg_head;             // -> sharpe_agg_pool
    uint32_t  sharpe_agg_count;

    // Timeline 引用 (per-user 单链表 -> events.log)
    uint64_t  timeline_head;               // 首条事件的 byte offset, NULL_LOG_OFFSET 表示空
    uint64_t  timeline_tail;               // 末条事件的 byte offset
    uint32_t  timeline_count;              // 事件数

    // Sharpe 锚点
    int32_t   _pad0;                       // 4B padding for alignment
    int64_t   pnl_before_first_sharpe_bucket; // 8B, Sharpe 窗口左边界锚点

    uint8_t   _reserved[128 - 20 - 4 - 8*4 - 4*6 - 8 - 8 - 4 - 4 - 8];
  } users[MAX_USERS];                      // 1000万 × 128B = 1.28GB

};

// Size 校验
static_assert(sizeof(Stage3Store::Header) == 4096);
static_assert(sizeof(Stage3Store::ConditionMeta) == 2056);
static_assert(sizeof(Stage3Store::TokenSlot) == 48);
static_assert(sizeof(Stage3Store::FeatureSlot) == 280);
static_assert(sizeof(Stage3Store::SharpeAgg) == 48);
static_assert(sizeof(Stage3Store::SharpeSample) == 32);
static_assert(sizeof(Stage3Store::UserBlock) == 128);


// ============================================================================
// 变长数据 (append-only logs, 单独 mmap)
// ============================================================================

// ==================== EventsLog (~190GB) ====================
// 对应原 EventFact, 定长 append-only 主体；每条记录带 per-user 单链表 next 指针
// 存储完整事件数据，支持历史持仓重放，省去 snapshot 机制
struct EventsLog {
  struct EventRecord {                     // 96B
    int64_t  sort_key;                     // block * 1e9 + log_idx
    int32_t  cond_idx;
    int16_t  token_idx;
    int8_t   event_type;
    int8_t   tag_id;
    int64_t  amount;                       // signed, 1e6 scale (for replay)
    int64_t  price_1e6;                    // price (for replay)
    int16_t  collateral;                   // collateral type (for replay)
    int16_t  _pad0;
    int32_t  token_count;                  // user 有效持仓数 (|qty| >= 10 token)
    int64_t  realized_delta;               // 本事件 realized 增量
    int64_t  realized_cum;                 // user 累计 realized pnl
    int64_t  unrealized_pnl;               // user unrealized pnl
    int64_t  exposure;                     // 该 token 暴露额 |pos * lp|
    int64_t  volume;                       // 交易额 |amount * price|
    int64_t  holding_period;               // 该 token 持仓周期 (blocks)
    uint64_t next_user_event_offset;       // 下一个同用户事件的 byte offset, NULL_LOG_OFFSET 表示尾
  } records[];
};

static_assert(sizeof(EventsLog::EventRecord) == 96);


// ============================================================================
// 索引结构 (单独 mmap)
// ============================================================================

// ==================== UserIndex (512MB) ====================
// hash(addr) -> user slot index
struct UserIndex {
  static constexpr size_t SLOT_COUNT = 16 * 1024 * 1024;  // 16M slots
  
  struct Entry {                           // 32B
    Address20 addr;                        // 20B
    uint32_t  user_idx;                    // -> users[]
    uint32_t  next;                        // collision chain
    uint32_t  _pad;
  } slots[SLOT_COUNT];                     // 16M × 32B = 512MB
};

static_assert(sizeof(UserIndex::Entry) == 32);


// ============================================================================
// 运行时索引结构 (不持久化, 启动时重建或按需构建)
// ============================================================================

// ==================== TokenIndex ====================
// 运行时哈希索引, O(1) 查找 token slot
// key: (user_idx, cond_idx, token_idx) -> pool_idx
struct TokenIndex {
  // key = (user_idx << 32) | ((cond_idx & 0xFFFF) << 16) | (token_idx & 0xFFFF)
  std::unordered_map<uint64_t, uint32_t> map;
  
  static uint64_t make_key(uint32_t user_idx, int32_t cond_idx, int16_t token_idx) {
    return (static_cast<uint64_t>(user_idx) << 32) |
           (static_cast<uint64_t>(static_cast<uint16_t>(cond_idx)) << 16) |
           static_cast<uint16_t>(token_idx);
  }
};

// ==================== FeatureIndex ====================
// 运行时哈希索引, O(1) 查找 feature slot
// key: (user_idx, bucket, tag_id) -> pool_idx
struct FeatureIndex {
  // key = (user_idx << 24) | ((bucket & 0xFFFF) << 8) | (tag_id & 0xFF)
  // bucket 用 16 bit 够用 (max bucket ~ 6500 for 650M blocks)
  std::unordered_map<uint64_t, uint32_t> map;
  
  static uint64_t make_key(uint32_t user_idx, int32_t bucket, int8_t tag_id) {
    return (static_cast<uint64_t>(user_idx) << 24) |
           (static_cast<uint64_t>(static_cast<uint16_t>(bucket)) << 8) |
           static_cast<uint8_t>(static_cast<int16_t>(tag_id) + 128);
  }
};

// ==================== SharpeAggIndex ====================
// 运行时哈希索引, O(1) 查找 sharpe agg
// key: (user_idx, bucket) -> pool_idx
struct SharpeAggIndex {
  // key = (user_idx << 16) | (bucket & 0xFFFF)
  std::unordered_map<uint64_t, uint32_t> map;
  
  static uint64_t make_key(uint32_t user_idx, int32_t bucket) {
    return (static_cast<uint64_t>(user_idx) << 16) |
           static_cast<uint16_t>(bucket);
  }
};


// ============================================================================
// 运行时内存结构 (不持久化)
// ============================================================================

// ==================== UserQueryCache ====================
// 运行时按需构建, 用于查询加速, 非持久化
// 使用 LRU 淘汰策略, 最大缓存 1000 用户
struct TokenPos {
  int32_t cond_idx;
  int16_t token_idx;
  int16_t collateral;
  int64_t pos;
  int64_t cost;
  int64_t lp;
  int64_t entry_block;
};

struct PosSnapshot {
  size_t  timeline_idx;                    // 对应 timeline 中的位置
  std::vector<TokenPos> positions;
};

struct UserQueryCache {
  Address20 addr;
  std::vector<EventsLog::EventRecord> timeline; // 沿用户事件链顺序加载
  std::vector<PosSnapshot> snapshots;      // 构建 timeline 时顺序生成 (每100事件一个)
  int64_t loaded_sort_key;
};

struct QueryCacheManager {
  static constexpr size_t MAX_CACHED_USERS = 1000;
  
  std::unordered_map<Address20, std::unique_ptr<UserQueryCache>> cache;
  std::list<Address20> lru_order;          // front = most recently used
  
  UserQueryCache* get_or_create(const Address20& addr);
  void touch(const Address20& addr);       // 更新 LRU 顺序
  void evict_if_needed();                  // 淘汰最旧条目
};

// ==================== SharpeSparseCache ====================
// 运行时 Sharpe 计算缓存 (最近 100 bucket)
struct SharpeSparseCache {
  struct BucketCache {
    int32_t bucket;
    std::vector<std::pair<int32_t, int64_t>> samples;  // (block_offset, pnl) 已排序
    int64_t close_pnl;
    int64_t min_pnl;
    int64_t max_pnl;
  };
  
  Address20 user_addr;
  std::map<int32_t, BucketCache> buckets;  // bucket -> cache
  int32_t oldest_bucket;                   // 用于 FIFO 淘汰
};

// ==================== DirtyUserSet ====================
// 本批次有事件的用户集合, 用于增量 prune
struct DirtyUserSet {
  std::unordered_set<uint32_t> users;      // 本批次 dirty user_idx
  int32_t last_pruned_bucket = -1;         // 上次 prune 到的 bucket
};

// ==================== UserRankCache ====================
// 物化的用户排行榜, 避免每次 get_users_sorted 全表扫描
struct UserRankCache {
  struct RankEntry {
    uint32_t user_idx;
    int64_t  total_events;
  };
  
  std::vector<RankEntry> by_events;        // 按 total_events 降序, 前 1000 名
  int64_t last_updated_sort_key = -1;      // 上次更新时的 cursor
  bool needs_rebuild = true;               // 是否需要重建
  
  static constexpr size_t MAX_RANK_SIZE = 1000;
  
  void mark_dirty(uint32_t user_idx);      // 标记用户需要更新
  void rebuild_if_needed(const Stage3Store& store);
};


// ============================================================================
// 输入结构 (临时, 不持久化)
// ============================================================================

// 对应原 EventInput
struct EventInput {
  Address20 user_addr;                     // 用户地址
  int64_t   sort_key;                      // block * 1e9 + log_idx
  int32_t   cond_idx;
  int32_t   event_type;
  int32_t   token_idx;
  int32_t   collateral;                    // 1=USDC, 2=USDCe, 3=USDT, 4=WrappedUSDCe
  int64_t   amount;                        // signed, 1e6
  int64_t   price_1e6;
};


// ============================================================================
// 文件布局
// ============================================================================

/*
data/stage3/
├── store.bin        # Stage3Store mmap (~167GB)
│                    # Header: 4KB
│                    # ConditionMeta[100万]: ~2GB
│                    # TokenPool[1亿]: 4.8GB
│                    # FeaturePool[5亿]: 140GB
│                    # SharpeAggPool[5000万]: 2.4GB
│                    # SharpeSamplePool[5亿]: 16GB
│                    # Users[1000万]: 1.28GB
├── events.log       # EventRecord 定长数组, 每条含 next_user_event_offset (~160GB for 2B events)
└── users.idx        # UserIndex hash table (512MB)
*/


// ============================================================================
// 内存估算
// ============================================================================

/*
| 组件                  | 大小        | 说明                        |
| --------------------- | ----------- | --------------------------- |
| Header                | 4KB         |                             |
| ConditionMeta[100万]  | 2GB         | 2056B × 1M                  |
| TokenPool[1亿]        | 4.8GB       | 48B × 100M, 稀疏            |
| FeaturePool[5亿]      | 140GB       | 280B × 500M, 稀疏           |
| SharpeAggPool         | 2.4GB       | 48B × 50M                   |
| SharpeSamplePool      | 16GB        | 32B × 500M                  |
| Users[1000万]         | 1.28GB      | 128B × 10M                  |
| store.bin 总计        | ~167GB      |                             |
| users.idx             | 512MB       | hash table                  |
| events.log            | ~160GB      | 96B × 2B events             |
| 总存储                | ~327GB      |                             |
| --------------------- | ----------- | --------------------------- |
| 运行时索引 (估算)     |             |                             |
| TokenIndex            | ~2GB        | 1亿 token × 20B/entry       |
| FeatureIndex          | ~10GB       | 5亿 feature × 20B/entry     |
| SharpeAggIndex        | ~1GB        | 5000万 agg × 20B/entry      |
| QueryCacheManager     | ~500MB      | 1000 用户 × 500KB/user      |
| DirtyUserSet          | ~1MB        | 本批次 dirty users          |
| UserRankCache         | ~16KB       | 1000 entries × 16B          |
*/

# Stage3 Flow (mmap + pool + runtime index)

文件: `store.bin`(~167GB) + `events.log`(~160GB) + `users.idx`(512MB)
运行时索引: `TokenIndex` + `FeatureIndex` + `SharpeAggIndex` (启动时重建)

```
stage3_bootstrap
├─ store.bin 不存在?
│  ├─ fallocate store.bin (Header 4KB + ConditionMeta 2GB + TokenPool 4.8GB + FeaturePool 140GB + SharpeAggPool 2.4GB + SharpeSamplePool 16GB + Users 1.28GB)
│  ├─ mmap store.bin MAP_SHARED
│  ├─ header.magic = "STAGE3\0\0", header.version = 1
│  ├─ header.cursor_sort_key = -1, header.cursor_processed_events = 0
│  ├─ header.user_count = 0, header.head_bucket = 0
│  ├─ header.*_pool_used = 0
│  ├─ header.token_free_head = NULL_IDX
│  ├─ header.feature_free_head = NULL_IDX
│  ├─ header.sharpe_agg_free_head = NULL_IDX
│  ├─ header.sharpe_sample_free_head = NULL_IDX
│  └─ header.events_log_tail = 0
├─ store.bin 存在?
│  ├─ mmap store.bin MAP_SHARED
│  └─ assert header.magic == "STAGE3\0\0"
├─ mmap events.log MAP_SHARED (可扩展, 初始 1GB, 需要时 mremap)
├─ mmap users.idx MAP_SHARED (16M slots × 32B = 512MB)
├─ 从 stage2 加载 condition_meta 到 conditions[] (100万 × 2056B, outcome_count + tag_id + payout_numerators[256])
├─ 重建运行时索引:
│  ├─ token_index.clear()
│  ├─ for i in 0..header.token_pool_used:
│  │  └─ if token_pool[i].cond_idx >= 0: token_index[make_key(user_idx, cond_idx, token_idx)] = i
│  ├─ feature_index.clear()
│  ├─ for i in 0..header.feature_pool_used:
│  │  └─ if feature_pool[i].flags & 1: feature_index[make_key(user_idx, bucket, tag_id)] = i
│  ├─ sharpe_agg_index.clear()
│  └─ for i in 0..header.sharpe_agg_pool_used:
│     └─ sharpe_agg_index[make_key(user_idx, bucket)] = i
└─ 返回 Stage3Runtime* {store, events_log, users_idx, token_index, feature_index, sharpe_agg_index, query_cache, dirty_users, rank_cache}

stage3_sync_tick(runtime, head_block, batch_limit)
├─ 0) cursor = header.cursor_sort_key
├─ 1) 拉取输入事件
│  ├─ scan_by_sort_key(start=cursor+1, end=head_block*1e9+(1e9-1), limit=batch_limit) 从 Stage2 RocksDB
│  ├─ 返回按 (sort_key, user_addr, cond_idx, event_type, token_idx) 排序
│  └─ 若无事件: header.cursor_sort_key = head_block*1e9+(1e9-1), return 0
├─ 2) 预处理 (使用 unordered_map 代替 set, 一次性缓存 user_idx)
│  ├─ user_idx_cache = unordered_map<Address20, uint32_t>{}
│  ├─ for evt in batch:
│  │  └─ if evt.user_addr not in user_idx_cache:
│  │     ├─ user_idx = user_index_lookup(evt.user_addr)
│  │     ├─ if user_idx == NULL_IDX:
│  │     │  ├─ user_idx = header.user_count++
│  │     │  ├─ users[user_idx].addr = evt.user_addr
│  │     │  ├─ users[user_idx].flags = 1 (occupied)
│  │     │  ├─ users[user_idx].total_events = 0
│  │     │  ├─ users[user_idx].total_realized_pnl = 0
│  │     │  ├─ users[user_idx].total_unrealized_pnl = 0
│  │     │  ├─ users[user_idx].last_sort_key = 0
│  │     │  ├─ users[user_idx].token_head = NULL_IDX
│  │     │  ├─ users[user_idx].token_count = 0
│  │     │  ├─ users[user_idx].feature_head = NULL_IDX
│  │     │  ├─ users[user_idx].feature_count = 0
│  │     │  ├─ users[user_idx].sharpe_agg_head = NULL_IDX
│  │     │  ├─ users[user_idx].sharpe_agg_count = 0
│  │     │  ├─ users[user_idx].timeline_head = NULL_LOG_OFFSET
│  │     │  ├─ users[user_idx].timeline_tail = NULL_LOG_OFFSET
│  │     │  ├─ users[user_idx].timeline_count = 0
│  │     │  ├─ users[user_idx].pnl_before_first_sharpe_bucket = 0
│  │     │  └─ user_index_insert(evt.user_addr, user_idx) 插入 hash 表
│  │     └─ user_idx_cache[evt.user_addr] = user_idx
│  └─ dirty_users.clear()
├─ 3) 回放循环 for evt in batch (按 sort_key 升序)
│  ├─ 3.1) user_idx = user_idx_cache[evt.user_addr]  // O(1) 查表
│  │  └─ dirty_users.insert(user_idx)
│  ├─ 3.2) if evt.cond_idx >= 0: 获取/创建 TokenSlot (使用 token_index O(1) 查找)
│  │  ├─ tok_key = TokenIndex::make_key(user_idx, evt.cond_idx, evt.token_idx)
│  │  ├─ it = token_index.find(tok_key)
│  │  ├─ if it != end: tok_idx = it->second
│  │  └─ else (创建新 slot):
│  │     ├─ if header.token_free_head != NULL_IDX: tok_idx = header.token_free_head, header.token_free_head = token_pool[tok_idx].next
│  │     ├─ else: tok_idx = header.token_pool_used++
│  │     ├─ token_pool[tok_idx].user_idx = user_idx
│  │     ├─ token_pool[tok_idx].cond_idx = evt.cond_idx
│  │     ├─ token_pool[tok_idx].token_idx = evt.token_idx
│  │     ├─ token_pool[tok_idx].collateral = evt.collateral
│  │     ├─ token_pool[tok_idx].pos = 0, .cost = 0, .lp = 0, .entry_block = 0
│  │     ├─ token_pool[tok_idx].next = users[user_idx].token_head
│  │     ├─ users[user_idx].token_head = tok_idx
│  │     └─ token_index[tok_key] = tok_idx  // 更新索引
│  ├─ 3.3) if evt.cond_idx >= 0: 应用交易规则
│  │  ├─ tok = &token_pool[tok_idx]
│  │  ├─ qty = abs(evt.amount), px = evt.price_1e6 / 1e6
│  │  ├─ has_usd = evt.collateral in {1,2,3,4}
│  │  ├─ pos_before = tok->pos, cost_before = tok->cost, lp_before = tok->lp
│  │  ├─ current_block = evt.sort_key / 1e9
│  │  ├─ realized_delta = 0
│  │  ├─ // 计算 unrealized 变化量 (用于增量更新)
│  │  ├─ old_mtm = (lp_before > 0) ? (pos_before * lp_before / 1e6 - cost_before) : 0
│  │  ├─ if evt.amount > 0 (正向腿, 先平空再开多):
│  │  │  ├─ close_short_qty = min(qty, max(0, -pos_before))
│  │  │  ├─ open_long_qty = qty - close_short_qty
│  │  │  ├─ if has_usd && close_short_qty > 0:
│  │  │  │  ├─ cost_removed = cost_before * close_short_qty / (-pos_before)
│  │  │  │  ├─ if event_type in {Order*, FPMM*, Split*, Merge*, Redemption*}: realized_delta = close_short_qty * px - cost_removed
│  │  │  │  ├─ if event_type == Convert: realized_delta = close_short_qty * ((popcount-1)/popcount) - cost_removed
│  │  │  │  ├─ if event_type in {TransferIn*, TransferOut*, FPMMLPRemove, FPMMLPReturn}: realized_delta = 0
│  │  │  │  └─ tok->cost -= cost_removed
│  │  │  └─ if has_usd && open_long_qty > 0 && event_type in {Order*, FPMM*, Split*, Merge*, Redemption*, Convert}:
│  │  │     └─ tok->cost += open_long_qty * px
│  │  ├─ if evt.amount < 0 (反向腿, 先平多再开空):
│  │  │  ├─ close_long_qty = min(qty, max(0, pos_before))
│  │  │  ├─ open_short_qty = qty - close_long_qty
│  │  │  ├─ if has_usd && close_long_qty > 0:
│  │  │  │  ├─ cost_removed = cost_before * close_long_qty / pos_before
│  │  │  │  ├─ if event_type in {Order*, FPMM*, Split*, Merge*, Redemption*}: realized_delta = close_long_qty * px - cost_removed
│  │  │  │  ├─ if event_type == Convert: realized_delta = close_long_qty * ((popcount-1)/popcount) - cost_removed
│  │  │  │  ├─ if event_type in {TransferIn*, TransferOut*, FPMMLPRemove, FPMMLPReturn}: realized_delta = 0
│  │  │  │  └─ tok->cost -= cost_removed
│  │  │  └─ if has_usd && open_short_qty > 0 && event_type in {Order*, FPMM*, Split*, Merge*, Redemption*, Convert}:
│  │  │     └─ tok->cost -= open_short_qty * px
│  │  ├─ if event_type != FPMMLPAdd: tok->pos += evt.amount
│  │  ├─ if event_type in {Order*, FPMM*} && evt.price_1e6 > 0: tok->lp = evt.price_1e6
│  │  ├─ 更新 entry_block:
│  │  │  ├─ if pos_before == 0 && tok->pos != 0: tok->entry_block = current_block
│  │  │  ├─ if abs(tok->pos) > abs(pos_before): tok->entry_block = (abs(pos_before) * tok->entry_block + abs(tok->pos - pos_before) * current_block) / abs(tok->pos)
│  │  │  └─ if abs(tok->pos) <= abs(pos_before): entry_block 不变
│  │  ├─ // 计算新的 mtm
│  │  ├─ new_mtm = (tok->lp > 0) ? (tok->pos * tok->lp / 1e6 - tok->cost) : 0
│  │  └─ if tok->pos == 0:
│  │     ├─ 从 user.token_head 链表移除 tok_idx
│  │     ├─ token_index.erase(tok_key)  // 从索引移除
│  │     ├─ tok->cond_idx = -1
│  │     ├─ tok->next = header.token_free_head
│  │     └─ header.token_free_head = tok_idx
│  ├─ 3.4) 更新 UserBlock (增量更新 unrealized, 无需全量扫描)
│  │  ├─ user = &users[user_idx]
│  │  ├─ user->total_events++
│  │  ├─ user->total_realized_pnl += realized_delta
│  │  ├─ if evt.cond_idx >= 0:
│  │  │  ├─ // 增量更新 unrealized: 只改变被修改 token 的差值
│  │  │  ├─ user->total_unrealized_pnl += (new_mtm - old_mtm)
│  │  │  ├─ // 增量更新 token_count
│  │  │  ├─ was_effective = (abs(pos_before) >= 10e6)
│  │  │  ├─ is_effective = (abs(tok->pos) >= 10e6)
│  │  │  ├─ if was_effective && !is_effective: user->token_count--
│  │  │  └─ if !was_effective && is_effective: user->token_count++
│  │  └─ user->last_sort_key = evt.sort_key
│  ├─ 3.5) Append EventRecord -> events.log + 用户 timeline 单链表
│  │  ├─ rec.sort_key = evt.sort_key
│  │  ├─ rec.cond_idx = evt.cond_idx
│  │  ├─ rec.token_idx = evt.token_idx
│  │  ├─ rec.event_type = evt.event_type
│  │  ├─ rec.tag_id = (evt.cond_idx >= 0 ? conditions[evt.cond_idx].tag_id : -1)
│  │  ├─ rec.realized_delta = realized_delta
│  │  ├─ rec.realized_cum = user->total_realized_pnl
│  │  ├─ rec.unrealized_pnl = user->total_unrealized_pnl
│  │  ├─ rec.token_count = user->token_count
│  │  ├─ rec.exposure = (evt.cond_idx >= 0 ? abs(tok->pos * tok->lp) : 0)
│  │  ├─ rec.volume = abs(evt.amount * evt.price_1e6) / 1e6
│  │  ├─ rec.holding_period = (evt.cond_idx >= 0 ? current_block - tok->entry_block : 0)
│  │  ├─ rec.next_user_event_offset = NULL_LOG_OFFSET
│  │  ├─ rec_off = header.events_log_tail
│  │  ├─ memcpy(events_log + rec_off, &rec, sizeof(EventRecord))
│  │  ├─ if user->timeline_head == NULL_LOG_OFFSET: user->timeline_head = rec_off
│  │  ├─ else: ((EventRecord*)(events_log + user->timeline_tail))->next_user_event_offset = rec_off
│  │  ├─ user->timeline_tail = rec_off
│  │  ├─ user->timeline_count++
│  │  └─ header.events_log_tail += sizeof(EventRecord)
│  ├─ 3.6) 更新 FeatureSlot (使用 feature_index O(1) 查找)
│  │  ├─ if evt.cond_idx < 0: skip
│  │  ├─ bucket = current_block / 100000
│  │  ├─ tag_id = conditions[evt.cond_idx].tag_id
│  │  ├─ for tag in {tag_id, -1}:
│  │  │  ├─ feat_key = FeatureIndex::make_key(user_idx, bucket, tag)
│  │  │  ├─ it = feature_index.find(feat_key)
│  │  │  ├─ if it != end: feat = &feature_pool[it->second]
│  │  │  ├─ else (创建新 slot):
│  │  │  │  ├─ if header.feature_free_head != NULL_IDX: feat_idx = header.feature_free_head, header.feature_free_head = feature_pool[feat_idx].next
│  │  │  │  ├─ else: feat_idx = header.feature_pool_used++
│  │  │  │  ├─ feat = &feature_pool[feat_idx]
│  │  │  │  ├─ feat->user_idx = user_idx, feat->bucket = bucket, feat->tag_id = tag
│  │  │  │  ├─ 初始化所有 Node-A0/A/B/C/D 字段为 0
│  │  │  │  ├─ feat->next = users[user_idx].feature_head
│  │  │  │  ├─ users[user_idx].feature_head = feat_idx, users[user_idx].feature_count++
│  │  │  │  └─ feature_index[feat_key] = feat_idx  // 更新索引
│  │  │  ├─ // Node-A0 续算锚点更新
│  │  │  ├─ delta_blocks = current_block - feat->last_block_10w
│  │  │  ├─ if delta_blocks > 0:
│  │  │  │  ├─ feat->time_weight_sum_10w += delta_blocks
│  │  │  │  ├─ feat->token_count_tw_sum_10w += feat->last_token_count_10w * delta_blocks
│  │  │  │  ├─ feat->exposure_tw_sum_10w += feat->last_exposure_10w * delta_blocks
│  │  │  │  └─ feat->holding_period_exp_tw_sum_10w += feat->last_holding_period_10w * feat->last_exposure_10w * delta_blocks
│  │  │  ├─ feat->last_sort_key_10w = evt.sort_key
│  │  │  ├─ feat->last_block_10w = current_block
│  │  │  ├─ feat->last_exposure_10w = rec.exposure
│  │  │  ├─ feat->last_holding_period_10w = rec.holding_period
│  │  │  ├─ feat->last_token_count_10w = rec.token_count
│  │  │  ├─ // Node-A 原子统计
│  │  │  ├─ feat->volume_sum_10w += rec.volume
│  │  │  ├─ // Node-B 归一化
│  │  │  ├─ if feat->time_weight_sum_10w > 0:
│  │  │  │  ├─ feat->token_avg_10w = feat->token_count_tw_sum_10w / feat->time_weight_sum_10w
│  │  │  │  ├─ feat->exposure_avg_10w = feat->exposure_tw_sum_10w / feat->time_weight_sum_10w
│  │  │  │  ├─ feat->volume_10w = feat->volume_sum_10w
│  │  │  │  └─ feat->holding_period_avg_10w = feat->holding_period_exp_tw_sum_10w / feat->exposure_tw_sum_10w (if > 0)
│  │  │  ├─ // Node-C 前缀缓存 (使用 feature_index O(1) 查找 bucket-1)
│  │  │  ├─ prev_key = FeatureIndex::make_key(user_idx, bucket-1, tag)
│  │  │  ├─ prev_it = feature_index.find(prev_key)
│  │  │  ├─ if prev_it != end: prev_feat = &feature_pool[prev_it->second], feat->ps_* = prev_feat->ps_* + feat->*_avg_10w
│  │  │  ├─ else: feat->ps_* = feat->*_avg_10w
│  │  │  ├─ // Node-D 窗口投影 (使用 feature_index O(1) 查找 bucket-10, bucket-100)
│  │  │  ├─ feat_100_it = feature_index.find(FeatureIndex::make_key(user_idx, bucket-10, tag))
│  │  │  ├─ feat_1000_it = feature_index.find(FeatureIndex::make_key(user_idx, bucket-100, tag))
│  │  │  ├─ feat->*_avg_100w = (feat->ps_* - (feat_100_it != end ? feature_pool[feat_100_it->second].ps_* : 0)) / min(10, bucket+1)
│  │  │  ├─ feat->*_avg_1000w = (feat->ps_* - (feat_1000_it != end ? feature_pool[feat_1000_it->second].ps_* : 0)) / min(100, bucket+1)
│  │  │  └─ feat->updated_sort_key = evt.sort_key
│  └─ 3.7) 更新 SharpeAgg + SharpeSample (使用 sharpe_agg_index O(1) 查找, 延迟 Sharpe 计算)
│     ├─ agg_key = SharpeAggIndex::make_key(user_idx, bucket)
│     ├─ it = sharpe_agg_index.find(agg_key)
│     ├─ if it != end: agg = &sharpe_agg_pool[it->second]
│     ├─ else (创建新 agg):
│     │  ├─ if header.sharpe_agg_free_head != NULL_IDX: agg_idx = header.sharpe_agg_free_head, ...
│     │  ├─ else: agg_idx = header.sharpe_agg_pool_used++
│     │  ├─ agg = &sharpe_agg_pool[agg_idx]
│     │  ├─ agg->user_idx = user_idx, agg->bucket = bucket
│     │  ├─ agg->close_pnl = 0, agg->min_pnl = INT64_MAX, agg->max_pnl = INT64_MIN
│     │  ├─ agg->sample_head = NULL_IDX, agg->sample_count = 0, agg->last_block = -1
│     │  ├─ agg->next = users[user_idx].sharpe_agg_head
│     │  ├─ users[user_idx].sharpe_agg_head = agg_idx, users[user_idx].sharpe_agg_count++
│     │  ├─ sharpe_agg_index[agg_key] = agg_idx  // 更新索引
│     │  └─ // 初始化用户 Sharpe 锚点 (如果是第一个 agg)
│     │     └─ if users[user_idx].sharpe_agg_count == 1: users[user_idx].pnl_before_first_sharpe_bucket = prev_pnl
│     ├─ pnl = user->total_realized_pnl + user->total_unrealized_pnl
│     ├─ block_offset = current_block % 100000
│     ├─ if current_block != agg->last_block:
│     │  ├─ if header.sharpe_sample_free_head != NULL_IDX: sample_idx = header.sharpe_sample_free_head, ...
│     │  ├─ else: sample_idx = header.sharpe_sample_pool_used++
│     │  ├─ sample = &sharpe_sample_pool[sample_idx]
│     │  ├─ sample->agg_idx = agg_idx
│     │  ├─ sample->block_offset = block_offset
│     │  ├─ sample->pnl = pnl
│     │  ├─ sample->next = agg->sample_head
│     │  ├─ agg->sample_head = sample_idx
│     │  ├─ agg->sample_count++
│     │  └─ agg->last_block = current_block
│     ├─ else: 更新最后一个 sample 的 pnl = pnl
│     ├─ agg->close_pnl = pnl
│     ├─ agg->min_pnl = min(agg->min_pnl, pnl)
│     └─ agg->max_pnl = max(agg->max_pnl, pnl)
├─ 4) 批次收尾
│  ├─ // 仅对 dirty users 计算 Sharpe (延迟到批次末尾, 而非每事件)
│  ├─ for user_idx in dirty_users:
│  │  ├─ global_feat_key = FeatureIndex::make_key(user_idx, header.head_bucket, -1)
│  │  ├─ feat_it = feature_index.find(global_feat_key)
│  │  └─ if feat_it != end: calc_sharpe_for_feature(user_idx, &feature_pool[feat_it->second])
│  ├─ for each user_idx in dirty_users: assert isfinite(users[user_idx].total_unrealized_pnl)
│  ├─ rank_cache.mark_dirty(dirty_users)  // 标记榜单需要更新
│  ├─ header.cursor_sort_key = batch.back().sort_key
│  └─ header.cursor_processed_events += batch.size()
└─ 5) return batch.size()

stage3_post_sync_prune(runtime) // 仅处理 dirty_users, 而非全用户扫描
├─ current_bucket = header.cursor_sort_key / 1e9 / 100000
├─ min_bucket_to_keep = max(0, current_bucket - 99)
├─ if min_bucket_to_keep <= dirty_users.last_pruned_bucket: return  // 无需 prune
├─ for user_idx in dirty_users.users:
│  ├─ // 遍历用户 sharpe_agg 链表, 仅淘汰旧 bucket
│  ├─ prev_ptr = &users[user_idx].sharpe_agg_head
│  ├─ agg_idx = *prev_ptr
│  └─ while agg_idx != NULL_IDX:
│     ├─ agg = &sharpe_agg_pool[agg_idx]
│     ├─ next_agg_idx = agg->next
│     ├─ if agg->bucket < min_bucket_to_keep:
│     │  ├─ // 更新用户 pnl 锚点
│     │  ├─ users[user_idx].pnl_before_first_sharpe_bucket = agg->close_pnl
│     │  ├─ // 释放 samples
│     │  ├─ sample_idx = agg->sample_head
│     │  ├─ while sample_idx != NULL_IDX: next = sharpe_sample_pool[sample_idx].next, sharpe_sample_free(sample_idx), sample_idx = next
│     │  ├─ // 从索引移除
│     │  ├─ sharpe_agg_index.erase(SharpeAggIndex::make_key(user_idx, agg->bucket))
│     │  ├─ // 从链表移除
│     │  ├─ *prev_ptr = next_agg_idx
│     │  ├─ users[user_idx].sharpe_agg_count--
│     │  └─ sharpe_agg_free(agg_idx)
│     ├─ else: prev_ptr = &agg->next
│     └─ agg_idx = next_agg_idx
└─ dirty_users.last_pruned_bucket = min_bucket_to_keep

calc_sharpe_for_feature(user_idx, feat) // 仅在 batch 结束时调用, 而非每事件
├─ if feat->tag_id != -1: return  // Sharpe 仅全局
├─ bucket = feat->bucket
├─ // 收集 10w 窗口样本 (当前 bucket)
├─ samples_10w = collect_bucket_samples(user_idx, bucket)  // 使用 sharpe_agg_index O(1)
├─ // 收集 100w 窗口样本 (10 buckets)
├─ samples_100w = []
├─ for b in max(0, bucket-9)..bucket: samples_100w += collect_bucket_samples(user_idx, b)
├─ // 收集 1000w 窗口样本 (100 buckets)
├─ samples_1000w = []
├─ for b in max(0, bucket-99)..bucket: samples_1000w += collect_bucket_samples(user_idx, b)
├─ // 计算各窗口 Sharpe
├─ p0_10w = get_p0(user_idx, bucket)  // 使用 sharpe_agg_index 找 bucket-1 的 close_pnl
├─ p0_100w = get_p0(user_idx, max(0, bucket-9))
├─ p0_1000w = get_p0(user_idx, max(0, bucket-99))
├─ feat->sharpe_10w = calc_sharpe_window(p0_10w, samples_10w, bucket*100000, (bucket+1)*100000-1, feat->exposure_avg_10w)
├─ feat->sharpe_100w = calc_sharpe_window(p0_100w, samples_100w, ..., avg_exposure_100w)
└─ feat->sharpe_1000w = calc_sharpe_window(p0_1000w, samples_1000w, ..., avg_exposure_1000w)

stage3_query_status() -> {syncing, last_block, head_block, behind_blocks, blocks_per_second, eta_seconds, ready}
├─ last_block = header.cursor_sort_key / 1e9
├─ behind_blocks = head_block - last_block
└─ ready = behind_blocks < 1000

stage3_query_pnl(user_addr) -> {user, block, total_events, timeline[]}
├─ user_idx = user_index_lookup(user_addr)
├─ if user_idx == NULL_IDX: return 404
├─ user = &users[user_idx]
├─ cache = query_cache.get_or_create(user_addr)  // LRU 缓存
├─ if cache->loaded_sort_key < user->last_sort_key:
│  ├─ cache->timeline.clear(), cache->snapshots.clear()
│  ├─ off = user->timeline_head
│  ├─ positions = {} 空 map
│  ├─ i = 0
│  ├─ while off != NULL_LOG_OFFSET:
│  │  ├─ rec = *(EventRecord*)(events_log + off)
│  │  ├─ cache->timeline.push_back(rec)
│  │  ├─ apply_event_to_positions(positions, rec) 重建 positions
│  │  ├─ if i % 100 == 0: cache->snapshots.push_back({i, positions.copy()})
│  │  ├─ off = rec.next_user_event_offset
│  │  └─ i++
│  └─ cache->loaded_sort_key = user->last_sort_key
└─ return {user_addr, user->last_sort_key/1e9, user->total_events, cache->timeline}

stage3_query_positions(user_addr, target_sort_key) -> {user, sort_key, block, positions[]}
├─ cache = query_cache.get_or_create(user_addr)  // 复用已加载的缓存
├─ // 如果请求的是最新持仓 (target_sort_key >= last_sort_key), 直接返回当前状态
├─ if target_sort_key >= user->last_sort_key:
│  ├─ positions = []
│  ├─ idx = user->token_head
│  ├─ while idx != NULL_IDX:
│  │  ├─ t = &token_pool[idx]
│  │  ├─ if abs(t->pos) >= 10e6: positions.push_back({t->cond_idx, t->token_idx, ...})
│  │  └─ idx = t->next
│  └─ return {user_addr, target_sort_key, ..., positions}
├─ // 历史持仓查询: 使用 snapshot + 增量 replay
├─ 确保 cache 已加载 (同 stage3_query_pnl)
├─ target_idx = lower_bound(cache->timeline, target_sort_key)
├─ snap_idx = 找最大的 i 使得 cache->snapshots[i].timeline_idx <= target_idx
├─ positions = cache->snapshots[snap_idx].positions.copy()
├─ for i in snap_idx .. target_idx: apply_event_to_positions(positions, cache->timeline[i])
└─ return {user_addr, target_sort_key, target_sort_key/1e9, positions}

stage3_query_filter(anchor_bucket, filters[], sort_expr, sort_asc, limit) -> {anchor_bucket, users[]}
├─ results = []
├─ for user_idx in 0..header.user_count:
│  ├─ if !(users[user_idx].flags & 1): continue
│  ├─ // 使用 feature_index O(1) 查找
│  ├─ feat_key = FeatureIndex::make_key(user_idx, anchor_bucket, filter_tag_id)
│  ├─ feat_it = feature_index.find(feat_key)
│  ├─ if feat_it == end: continue
│  ├─ feat = &feature_pool[feat_it->second]
│  ├─ if !eval_all_filters(feat, filters): continue
│  ├─ sort_value = eval_expr(feat, sort_expr)
│  └─ results.push_back({user_idx, sort_value})
├─ sort(results, by sort_value, asc=sort_asc)
├─ results.resize(min(results.size(), limit))
└─ return {anchor_bucket, [users[r.user_idx].addr for r in results]}

stage3_get_users_sorted(limit) -> {users[]}
├─ // 使用物化榜单缓存
├─ rank_cache.rebuild_if_needed(store)  // 仅在脏时重建
└─ return rank_cache.by_events[0..limit]

stage3_get_bucket_user_count(bucket) -> count
├─ // 遍历 feature_index 统计该 bucket 的用户数 (O(n) 但 n 是该 bucket 的 feature 数, 非全用户)
├─ count = 0
├─ for (key, idx) in feature_index:
│  ├─ if extract_bucket(key) == bucket && extract_tag(key) == -1:
│  │  └─ count++
└─ return count

持久化: mmap MAP_SHARED, OS 自动 dirty page writeback; 关闭时 msync(MS_SYNC)
崩溃恢复: 从 header.cursor_sort_key 继续拉取事件; 启动时重建运行时索引
```
