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

    uint8_t   _reserved[128 - 20 - 4 - 8*4 - 4*6 - 8 - 8 - 4];
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

// ==================== EventsLog (~160GB) ====================
// 对应原 EventFact, 定长 append-only 主体；每条记录带 per-user 单链表 next 指针
struct EventsLog {
  struct EventRecord {                     // 80B
    int64_t  sort_key;                     // block * 1e9 + log_idx
    int32_t  cond_idx;
    int16_t  token_idx;
    int8_t   event_type;
    int8_t   tag_id;
    int64_t  realized_delta;               // 本事件 realized 增量
    int64_t  realized_cum;                 // user 累计 realized pnl
    int64_t  unrealized_pnl;               // user unrealized pnl
    int32_t  token_count;                  // user 有效持仓数 (|qty| >= 10 token)
    int32_t  _pad0;
    int64_t  exposure;                     // 该 token 暴露额 |pos * lp|
    int64_t  volume;                       // 交易额 |amount * price|
    int64_t  holding_period;               // 该 token 持仓周期 (blocks)
    uint64_t next_user_event_offset;       // 下一个同用户事件的 byte offset, NULL_LOG_OFFSET 表示尾
  } records[];
};

static_assert(sizeof(EventsLog::EventRecord) == 80);


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
// 运行时内存结构 (不持久化)
// ============================================================================

// ==================== UserQueryCache ====================
// 运行时按需构建, 用于查询加速, 非持久化
struct TokenPos {
  int32_t cond_idx;
  int16_t token_idx;
  int16_t collateral;
  int64_t pos;
  int64_t cost;
  int64_t lp;
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

// ==================== SharpeSparseCache ====================
// 运行时 Sharpe 计算缓存 (最近 100 bucket)
struct SharpeSparseCache {
  struct BucketCache {
    int32_t bucket;
    std::vector<std::pair<int32_t, int64_t>> samples;  // (block_offset, pnl)
    int64_t close_pnl;
    int64_t min_pnl;
    int64_t max_pnl;
  };
  
  Address20 user_addr;
  std::map<int32_t, BucketCache> buckets;  // bucket -> cache
  int32_t oldest_bucket;                   // 用于 FIFO 淘汰
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
| 组件                | 大小      | 说明                        |
|---------------------|-----------|---------------------------|
| Header              | 4KB       |                           |
| ConditionMeta[100万]| 2GB       | 2056B × 1M                |
| TokenPool[1亿]      | 4.8GB     | 48B × 100M, 稀疏          |
| FeaturePool[5亿]    | 140GB     | 280B × 500M, 稀疏         |
| SharpeAggPool       | 2.4GB     | 48B × 50M                 |
| SharpeSamplePool    | 16GB      | 32B × 500M                |
| Users[1000万]       | 1.28GB    | 128B × 10M                |
| store.bin 总计      | ~167GB    |                           |
| users.idx           | 512MB     | hash table                |
| events.log          | ~160GB    | 80B × 2B events           |
| 总存储              | ~327GB    |                           |
*/