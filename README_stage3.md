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
    近月夏普                   1    精确计算           近100w块全账户夏普(仅全账户)
    近年夏普                   1    精确计算           近1000w块全账户夏普(仅全账户)
截面特征:
    输入: anchor bucket + 多条自由表达式(filters) + 排序表达式(sort_expr)
    输出: 内核返回匹配用户的 `user_idx + sort_value + filter_stats`；service 层再组装 top K 用户地址/近月均值/pnl
    目标: 支持“分行业 nested 特征”的横截面对比与选人

注:
  1. 用户首个 `cond_idx >= 0` 事件之前没有 bucket；首个事件所在 bucket 开始,直到该用户最新已 materialize 的 bucket 为止,bucket 必须稠密。
  2. 交易额里: 只记录会直接创造头寸暴露的操作(比如铸币, 合币就不应该记入, 转帐本身也不是创造暴露的操作, 更偏向转移暴露到当前账户,所以不记入), 暴露方向不重要
  3. 平均持仓: 需要统计周期内多个事件(非均匀)的持仓快照(记录不同token的平均持仓周期), 再按照token金额, 事件时间加权
  4. 持仓周期: 因为用户的特征bucket是尾部稠密的, 持仓周期这种特征, 在原本稀疏的插值bucket也应该按照定义increment,比较特殊
  5. 夏普:
     - 无风险=0
     - 没有账户的USDC类资产信息,所以无法计算净值曲线,需要拿 nav~f(exposure(t),realized_pnl(t),unrealized_pnl(t))来近似
     - 因为收益率的分布不平稳, 连续时间假设不成立, 所以必须用真实分布(density)来计算波动性
     - {(block_num, exposure, pnl)}的采样是极端不均匀的, 有时候中间很多个bucket都没有采样(事件), 有时候很多个采样在同一个block
     - 因为非均匀, 夏普的定义不唯一, 标准的时间加权夏普, 会惩罚波动性(预期内), 但是不会惩罚曲率(是时间无关的), 所以这里的夏普会加入曲率调整项
     - 对于计算周期[T1,T2] (需要normalize到年周期/1000w blk), 曲率衰减调整项a: 
       - 先把pnl标准化到周期内: pnl_0(t) = pnl(t) - pnl(T1)
       - 先把pnl抬高到非负: pnl+ = pnl_0 + abs(min(pnl_0))
       - a = avg({r(t)^2/(1+r(t)^2)}), 整个pnl+序列做quadratic fit, r(t)=abs(pnl+(t)-fit(t))/max(fit(t),eps)
       - 对于采样数不足的用户, 比如区间内采样点不足10个, 夏普=0 跳过计算
     - 夏普 S = S0 * (1 - a) * trade_decay * sqrt(10000000)
       - avg_exp = avg(|exp[i]|), 用固定的平均exposure作为分母, 避免仓位变化导致收益率稀释/放大(这里隐含了对用户仓位管理能力的衰减调整)
       - return per step: r[i] = (pnl+[i] - pnl+[i-1]) / avg_exp
       - time-weighted sharpe: S0 = (σ² > 0 ? μ / sqrt(σ²) : 0)
       - T = T2 - T1 + 1
       - μ = Σr / T
       - σ² = Σ(r^2) / T - μ^2
       - trade_decay: 交易次数线性衰减调整
         - 1000W window: 交易次数 < 100 时, trade_decay = count / 100
         - 100W window: 交易次数 < 10 时, trade_decay = count / 10
         - 否则 trade_decay = 1

// ============================================================================
// Stage3 完整数据结构设计 (mmap + pool)
// ============================================================================

// ------------------------------ 常量 ------------------------------

constexpr size_t MAX_CONDITIONS        = 1'000'000;      // 100万 conditions
constexpr size_t MAX_USERS             = 10'000'000;     // 1000万用户
constexpr size_t MAX_TOKENS            = 100'000'000;    // 1亿 token slots
constexpr size_t MAX_FEATURES          = 500'000'000;    // 5亿 feature slots
constexpr size_t MAX_SHARPE_BUCKETS    = 50'000'000;     // 5000万 sharpe bucket
constexpr size_t MAX_SHARPE_POINTS     = 500'000'000;    // 5亿 sharpe 点
constexpr size_t OUTCOME_MAX           = 256;            // 最大 outcome 数
constexpr uint32_t NULL_IDX            = UINT32_MAX;     // 空指针
constexpr uint64_t NULL_LOG_OFFSET     = UINT64_MAX;     // events.log 空指针
constexpr uint32_t STAGE3_SYNC_SHARD_COUNT = 64;         // 并行 shard 数

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
    
    // Per-shard Pool 使用情况 / Free list heads
    uint64_t token_pool_used[STAGE3_SYNC_SHARD_COUNT];
    uint64_t feature_pool_used[STAGE3_SYNC_SHARD_COUNT];
    uint64_t sharpe_bucket_pool_used[STAGE3_SYNC_SHARD_COUNT];
    uint64_t sharpe_point_pool_used[STAGE3_SYNC_SHARD_COUNT];
    uint32_t token_free_head[STAGE3_SYNC_SHARD_COUNT];
    uint32_t feature_free_head[STAGE3_SYNC_SHARD_COUNT];
    uint32_t sharpe_bucket_free_head[STAGE3_SYNC_SHARD_COUNT];
    uint32_t sharpe_point_free_head[STAGE3_SYNC_SHARD_COUNT];
    
    // Events log
    uint64_t events_log_tail;              // events.log 写入 byte offset
    
    uint8_t  _pad[...];                    // 填充到 4KB
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
    // key + chain (packed for alignment)
    uint32_t user_idx;                     // 所属用户 -> users[]
    uint32_t next;                         // 同用户下一个 token / free list
    int32_t  cond_idx;                     // condition index, -1=free slot
    int16_t  token_idx;                    // outcome index
    int16_t  collateral;                   // 1=USDC, 2=USDCe, 3=USDT, 4=WrappedUSDCe
    // value (对应原 TokenState)
    int64_t  pos;                          // 持仓量 (1e6, 可负=空头)
    int64_t  cost;                         // 成本基础 (1e6, 可负=空头信用)
    int64_t  lp;                           // 最近成交价 (1e6)
    int64_t  entry_block;                  // 加权平均建仓 block
  } token_pool[MAX_TOKENS];                // 1亿 × 48B = 4.8GB

  // ==================== FeaturePool[5亿] (132GB) ====================
  struct FeatureSlot {                     // 264B (对应原 FeatureTensorState)
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
    uint8_t  _pad2[8];

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
  } feature_pool[MAX_FEATURES];            // 5亿 × 264B = 132GB

  // ==================== SharpeBucketPool[5000万] (2.0GB) ====================
  // 按 bucket 组织 Sharpe 点, 只服务窗口取点 / 左边界锚点 / prune
  struct SharpeBucket {                    // 40B
    uint32_t user_idx;
    int32_t  bucket;
    int64_t  close_pnl;                    // bucket 末尾 pnl, 用作 Sharpe 左边界锚点 / prune
    int64_t  close_exposure;               // bucket 末尾 exposure, 用作 Sharpe 左边界 exp 锚点 / prune
    uint32_t point_head;                   // -> sharpe_point_pool 链表头
    uint32_t point_tail;                   // -> sharpe_point_pool 链表尾, O(1) append
    uint32_t point_count;
    uint32_t next;                         // 同用户下一个 bucket / free list
  } sharpe_bucket_pool[MAX_SHARPE_BUCKETS]; // 5000万 × 40B = 2.0GB

  // ==================== SharpePointPool[5亿] (12GB) ====================
  // 每次 `pnl` jump 记录一个点, 直接服务 Sharpe 计算
  struct SharpePoint {                     // 24B
    uint32_t next;                         // 同 bucket 下一个点 / free list
    int32_t  block_offset;                 // bucket 内 block 偏移 (0 ~ 99999)
    int64_t  exposure;                     // 当前点的全账户 exposure
    int64_t  pnl;                          // 当前点的全账户 pnl
  } sharpe_point_pool[MAX_SHARPE_POINTS];  // 5亿 × 24B = 12GB

  // ==================== Users[1000万] (1.28GB) ====================
  struct UserBlock {                       // 128B (对应原 UserSummaryState + 扩展)
    // 基础信息
    Address20 addr;                        // 20B, 用户地址
    uint32_t  flags;                       // 4B, bit0=occupied, bits[15:8]=sync shard

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
    uint32_t  sharpe_bucket_head;          // -> sharpe_bucket_pool
    uint32_t  sharpe_bucket_count;

    // Timeline 引用 (per-user 单链表 -> events.log)
    uint64_t  timeline_head;               // 首条事件的 byte offset, NULL_LOG_OFFSET 表示空
    uint64_t  timeline_tail;               // 末条事件的 byte offset
    uint32_t  timeline_count;              // 事件数

    // Sharpe 锚点
    int32_t   _pad0;                       // 4B padding for alignment
    int64_t   pnl_before_first_sharpe_bucket;      // 8B, Sharpe 窗口左边界 pnl 锚点
    int64_t   exposure_before_first_sharpe_bucket; // 8B, Sharpe 窗口左边界 exp 锚点

    uint8_t   _reserved[4];                // 填充到 128B
  } users[MAX_USERS];                      // 1000万 × 128B = 1.28GB

};

// Size 校验
static_assert(sizeof(Stage3Store::Header) == 4096);
static_assert(sizeof(Stage3Store::ConditionMeta) == 2056);
static_assert(sizeof(Stage3Store::TokenSlot) == 48);
static_assert(sizeof(Stage3Store::FeatureSlot) == 264);
static_assert(sizeof(Stage3Store::SharpeBucket) == 32);
static_assert(sizeof(Stage3Store::SharpePoint) == 24);
static_assert(sizeof(Stage3Store::UserBlock) == 128);


// ============================================================================
// 变长数据 (append-only logs, 单独 mmap)
// ============================================================================

// ==================== EventsLog (~190GB) ====================
// 对应原 EventFact, 定长 append-only 主体；每条记录带 per-user 单链表 next 指针
// 存储完整事件数据,支持历史持仓重放；runtime query cache 会按需派生 snapshot
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

// ==================== SharpeBucketIndex ====================
// 运行时哈希索引, O(1) 查找 sharpe bucket
// key: (user_idx, bucket) -> pool_idx
struct SharpeBucketIndex {
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
  std::vector<PosSnapshot> snapshots;      // 构建 timeline 时顺序生成 (每256事件一个 + 末尾补 final snapshot)
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

// ==================== DirtyUserSet ====================
// 本批次有事件的用户集合, 用于增量 prune
struct DirtyUserSet {
  std::vector<uint32_t> users;             // 本批次 dirty user_idx
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

// 对应原 EventInput (运行时预处理后)
struct EventInput {
  uint32_t  user_idx;                      // 用户索引 (预查找)
  int32_t   cond_idx;
  int32_t   event_type;
  int32_t   token_idx;
  int32_t   collateral;                    // 1=USDC, 2=USDCe, 3=USDT, 4=WrappedUSDCe
  int32_t   bucket;                        // 预计算 bucket
  int32_t   block_offset;                  // bucket 内 block 偏移
  int8_t    tag_id;                        // 预填充 tag_id
  uint8_t   _pad0[3];
  int64_t   sort_key;                      // block * 1e9 + log_idx
  int64_t   current_block;                 // 预计算 block
  int64_t   amount;                        // signed, 1e6
  int64_t   price_1e6;
};


// ============================================================================
// 文件布局
// ============================================================================

/*
data/stage3/
├── store.bin        # Stage3Store mmap (~154GB)
│                    # Header: 4KB
│                    # ConditionMeta[100万]: ~2GB
│                    # TokenPool[1亿]: 4.8GB
│                    # FeaturePool[5亿]: 132GB
│                    # SharpeBucketPool[5000万]: 2.0GB
│                    # SharpePointPool[5亿]: 12GB
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
| FeaturePool[5亿]      | 132GB       | 264B × 500M, 稀疏           |
| SharpeBucketPool      | 2.0GB       | 40B × 50M                   |
| SharpePointPool       | 12GB        | 24B × 500M                  |
| Users[1000万]         | 1.28GB      | 128B × 10M                  |
| store.bin 总计        | ~154GB      |                             |
| users.idx             | 512MB       | hash table                  |
| events.log            | ~160GB      | 96B × 2B events             |
| 总存储                | ~314GB      |                             |
| --------------------- | ----------- | --------------------------- |
| 运行时索引 (估算)     |             |                             |
| TokenIndex[64]        | ~2GB        | 1亿 token × 20B/entry       |
| FeatureIndex[64]      | ~10GB       | 5亿 feature × 20B/entry     |
| SharpeBucketIndex[64] | ~1GB        | 5000万 bucket × 20B/entry   |
| global_feature_user_counts | <1MB   | head_bucket 级别稠密数组    |
| QueryCacheManager     | ~500MB      | 1000 用户 × 500KB/user      |
| DirtyUserSet          | ~1MB        | 本批次 dirty users          |
| UserRankCache         | ~16KB       | 1000 entries × 16B          |
*/

# Stage3 Flow (mmap + pool + runtime index)

文件: `store.bin`(~154GB) + `events.log`(~160GB) + `users.idx`(512MB)
运行时索引: `TokenIndex[64]` + `FeatureIndex[64]` + `SharpeBucketIndex[64]` + `global_feature_user_counts` (启动时重建, 按用户 shard 分片, 支持并行同步)

```
stage3_bootstrap
├─ store.bin 不存在?
│  ├─ fallocate store.bin (Header 4KB + ConditionMeta 2GB + TokenPool 4.8GB + FeaturePool 132GB + SharpeBucketPool 2.0GB + SharpePointPool 12GB + Users 1.28GB)
│  ├─ mmap store.bin MAP_SHARED
│  ├─ header.magic = "STAGE3\0\0", header.version = 2
│  ├─ header.cursor_sort_key = -1, header.cursor_processed_events = 0
│  ├─ header.user_count = 0, header.head_bucket = 0
│  ├─ for shard in 0..64: header.*_pool_used[shard] = 0, header.*_free_head[shard] = NULL_IDX
│  └─ header.events_log_tail = 0
├─ store.bin 存在?
│  ├─ mmap store.bin MAP_SHARED
│  └─ assert header.magic == "STAGE3\0\0"
├─ mmap events.log MAP_SHARED (可扩展, 初始 1GB, 需要时 mremap)
├─ mmap users.idx MAP_SHARED (16M slots × 32B = 512MB)
├─ 从 stage2 加载 condition_meta 到 conditions[] (100万 × 2056B, outcome_count + tag_id + payout_numerators[256])
├─ 重建运行时索引 (按用户链表遍历, 自动分 shard):
│  ├─ global_feature_user_counts.resize(head_bucket + 1, 0)
│  ├─ for shard in 0..64: token_index[shard].clear(), feature_index[shard].clear(), sharpe_bucket_index[shard].clear()
│  ├─ for user_idx in 0..header.user_count:
│  │  ├─ shard = user_shard_from_flags(users[user_idx].flags)
│  │  ├─ 遍历 users[user_idx].token_head 链表, 插入 token_index[shard]
│  │  ├─ 遍历 users[user_idx].feature_head 链表, 插入 feature_index[shard]
│  │  │  └─ 若 `feat.tag_id == -1`: global_feature_user_counts[feat.bucket]++
│  │  └─ 遍历 users[user_idx].sharpe_bucket_head 链表, 插入 sharpe_bucket_index[shard]
└─ 返回 Stage3Runtime* {store, events_log, users_idx, token_index[64], feature_index[64], sharpe_bucket_index[64], global_feature_user_counts, query_cache, dirty_users, rank_cache}

stage3_sync_tick(runtime, head_block, batch_limit)
├─ 0) cursor = header.cursor_sort_key, head_sort_key = head_block*1e9 + (1e9-1)
├─ 1) 从 Stage2 RocksDB 拉事件
│  ├─ scan_by_sort_key(cursor, head_sort_key, batch_limit)
│  ├─ 若空: header.cursor_sort_key = head_sort_key, return 0
│  └─ 若命中 `batch_limit`, 按 block 边界截断,避免半个 block 被拆到下一批
├─ 2) 转成 `EventInput`
│  ├─ `user_idx_cache[address] -> user_idx`
│  ├─ 新用户通过 `user_get_or_create()` 初始化 `UserBlock + user_index`
│  └─ 预填 `current_block / bucket / block_offset / tag_id`
└─ 3) 调 `process_event_batch(batch)`

process_event_batch(batch)
├─ 1) 构造 `UserTask{user_idx, begin, count, touched_tag_mask, shard}`
│  ├─ 同用户事件收拢到同一个 task
│  ├─ `dirty_users.users =` 本批涉及的 user_idx
│  └─ 建 `event_order`, 保证 task 内事件仍按原 batch 顺序
├─ 2) 预留 `events.log` 空间, 并按 shard 切分每个 shard 的独占写入区间
├─ 3) 并行按 shard 处理 user task
│  ├─ 对每个 user task:
│  │  ├─ `init_feature_timelines(...)`
│  │  │  ├─ 扫当前 `feature_head` 链表
│  │  │  ├─ 得到每个 tag 的 `first_bucket / latest_bucket`
│  │  │  └─ 同时得到 `latest_feature_idx`,供后续 bucket 稠密推进
│  │  ├─ `materialized_feature_mask =` 历史已存在的 feature tag 集合
│  │  ├─ 若 `task.touched_tag_mask != 0`:
│  │  │  └─ 扫当前 `token_head`, 只为 `touched tags + global` 预装 `runtime_states{token_count, exposure, exposure_entry_sum}`
│  │  ├─ 按该 user 的事件顺序回放:
│  │  │  ├─ if `evt.cond_idx >= 0`:
│  │  │  │  ├─ `dense_feature_mask = materialized_feature_mask | tag(evt) | global`
│  │  │  │  ├─ `prepare_feature_buckets_for_mask(user, evt.bucket, dense_feature_mask)`
│  │  │  │  │  ├─ 对 mask 内每个 tag 调 `prepare_feature_bucket(...)`
│  │  │  │  │  ├─ 从 `latest_bucket+1` 一直 materialize 到当前 bucket
│  │  │  │  │  ├─ gap bucket / 当前 bucket 都会落表；即使 carry 为 0 也会生成全 0 bucket
│  │  │  │  │  └─ `100w/1000w` 统一从稠密 `10w` prefix 直接投影
│  │  │  │  ├─ `materialized_feature_mask |= dense_feature_mask`
│  │  │  │  ├─ `tok = token_get_or_create(...)`
│  │  │  │  ├─ `before_contrib = token_feature_contrib(*tok)`
│  │  │  │  ├─ `realized_delta = apply_trade_event(...)`
│  │  │  │  ├─ `after_contrib = token_feature_contrib(*tok)`
│  │  │  │  └─ `runtime_states[tag/global] += after_contrib - before_contrib`
│  │  │  ├─ 更新 `UserBlock`: `total_events / total_realized_pnl / total_unrealized_pnl / token_count / last_sort_key`
│  │  │  ├─ append `EventRecord` 到该 shard 的 `events.log` 区间,并挂到用户 timeline 单链表
│  │  │  ├─ if `evt.cond_idx >= 0`:
│  │  │  │  └─ `update_feature_on_event(...)`
│  │  │  │     ├─ 只更新 `{evt.tag_id, -1}` 当前 bucket 的尾部窗口
│  │  │  │     ├─ 刷新当前 bucket 的 Node-B / Node-C / Node-D
│  │  │  │     └─ 记录 `global_sharpe_recalc_start_bucket =` 本批次最早受影响的 global bucket
│  │  │  ├─ if `pnl != prev_pnl`: `update_sharpe_on_event(...)`
│  │  │  └─ if `tok->pos == 0`: `token_remove_if_empty(...)`
│  │  └─ 该 user task 结束后:
│  │     ├─ `global_first_bucket = feature_first_buckets[tag_slot(-1)]`
│  │     ├─ `global_latest_bucket = feature_latest_buckets[tag_slot(-1)]`
│  │     └─ 对 `[global_sharpe_recalc_start_bucket, global_latest_bucket]` 的每个 global feature bucket 调 `calc_sharpe_for_feature(...)`
├─ 4) 批次收尾
│  ├─ assert dirty user 的 `total_unrealized_pnl` 有限
│  ├─ `rank_cache.needs_rebuild = true`
│  ├─ `header.head_bucket = max(header.head_bucket, batch.back().bucket)`
│  ├─ `header.events_log_tail = reserved_log_tail`
│  ├─ `header.cursor_sort_key = batch.back().sort_key`
│  └─ `header.cursor_processed_events += batch.size()`
└─ 5) return `batch.size()`

stage3_post_sync_prune(runtime) // 仅处理 dirty_users, 而非全用户扫描
├─ if `header.cursor_sort_key < 0`: return
├─ `current_bucket = block_to_bucket(sort_key_to_block(header.cursor_sort_key))`
├─ `min_bucket_to_keep = max(0, current_bucket - 99)`
├─ for `user_idx in dirty_users.users`: `sharpe_prune_old_buckets(user_idx, min_bucket_to_keep)`
└─ `dirty_users.last_pruned_bucket = min_bucket_to_keep`

sharpe_prune_old_buckets(user_idx, min_bucket_to_keep)
├─ 遍历 `users[user_idx].sharpe_bucket_head`
├─ if `bucket->bucket < min_bucket_to_keep`:
│  ├─ 用“被裁掉部分中离保留区最近的 `close_pnl / close_exposure`”
│  │  更新 `users[user_idx].pnl_before_first_sharpe_bucket / exposure_before_first_sharpe_bucket`
│  ├─ 释放该 bucket 下全部 point
│  ├─ 从 `sharpe_bucket_index` 移除
│  ├─ 从用户 bucket 链表摘除
│  └─ `sharpe_bucket_free(bucket_idx)`
└─ else: 保留

update_sharpe_on_event(user_idx, exposure, prev_exposure, pnl, prev_pnl, bucket, block_offset)
├─ if `users[user_idx].sharpe_bucket_head == NULL_IDX`:
│  └─ `users[user_idx].pnl_before_first_sharpe_bucket = prev_pnl`
│     `users[user_idx].exposure_before_first_sharpe_bucket = prev_exposure`
├─ `bucket_node = sharpe_bucket_get_or_create(user_idx, bucket)`
├─ 追加一个 `SharpePoint{block_offset, exposure, pnl}`
├─ `bucket_node->close_pnl = pnl`
└─ `bucket_node->close_exposure = exposure`

collect_sharpe_points(user_idx, start_bucket, end_bucket)
├─ `p0 = get_p0(start_bucket)`
├─ `e0 = get_e0(start_bucket)`
├─ 依 bucket 顺序遍历 `[start_bucket, end_bucket]` 的全部 `SharpeBucket`
├─ 展开每个 bucket 下全部 `SharpePoint`, 保持事件原顺序
└─ 返回 `{p0, e0, points[]}`

calc_sharpe_for_feature(user_idx, feat, first_bucket) // 仅在 batch 结束时调用, 而非每事件
├─ if `feat->tag_id != -1`: return  // Sharpe 仅全局
├─ if `first_bucket < 0 || first_bucket > feat->bucket`: 直接把 `sharpe_{100w,1000w}` 置 0
├─ 对 `100w / 1000w` 两个窗口分别重算:
│  ├─ `start_bucket =` 当前窗口左边界 bucket
│  ├─ `win = collect_sharpe_points(user_idx, start_bucket, feat->bucket)`
│  ├─ if `win.points.size() < 10`: 对应窗口 Sharpe = 0
│  ├─ 用 `win.p0` 把窗口内 `pnl` 重标定成 `pnl_0`
│  ├─ 用 `pnl_0` 构造 `pnl+`
│  ├─ 在真实 `block_num` 时间轴上对 `pnl+` 做 quadratic fit, 计算曲率项 `a`
│  ├─ 对首个 point: 用 `win.e0 / win.p0` 作为 `t-1` 锚点
│  ├─ 对每个 point / jump `i`: `r[i] = (pnl+[i] - pnl+[i-1]) / max(abs(exp[i-1]), eps)`
│  ├─ `T = T2 - T1 + 1`
│  ├─ `μ = Σr / T`
│  ├─ `σ² = Σ(r^2) / T - μ^2`
│  ├─ `S0 = (σ² > 0 ? μ / sqrt(σ²) : 0)`
│  └─ `sharpe_W = S0 * (1 - a) * sqrt(10000000)`
└─ 返回

stage3_query_status() -> {syncing, last_block, head_block, behind_blocks, behind_chunks, blocks_per_second, eta_seconds, ready, user_count, processed_events, head_bucket}
├─ `last_block = (header.cursor_sort_key >= 0 ? header.cursor_sort_key / 1e9 : 0)`
├─ `behind_blocks = max(0, head_block - last_block)`
├─ `behind_chunks = (behind_blocks == 0 ? 0 : 1 + behind_blocks / 10000000)`
└─ `ready = behind_blocks < 1000`

load_user_query_cache(user_addr)
├─ 清空 `timeline / snapshots`
├─ 顺着 `user.timeline_head -> next_user_event_offset` 加载 `EventRecord`
├─ 一边 replay 持仓, 一边构建 snapshot
├─ 每 `256` 条 timeline 记录落一个 `PosSnapshot`
├─ 若末尾没有 snapshot, 额外补最后一个
└─ `cache->loaded_sort_key = user->last_sort_key`

stage3_query_pnl(user_addr) -> {user, block, total_events, timeline[]}
├─ `user_idx = user_index_lookup(user_addr)`
├─ if `user_idx == NULL_IDX`: return 空结果
├─ cache = `query_cache.get_or_create(user_addr)`
├─ if `cache->loaded_sort_key < user->last_sort_key`: `load_user_query_cache(...)`
└─ 返回 `{sort_key, event_type, realized_cum, unrealized_pnl, token_count}` 投影后的 timeline

stage3_query_positions(user_addr, target_sort_key) -> {user, sort_key, block, positions[]}
├─ if `user_idx == NULL_IDX`: return 空结果
├─ cache = `query_cache.get_or_create(user_addr)`
├─ if `cache->loaded_sort_key < user->last_sort_key`: `load_user_query_cache(...)`
├─ if `target_sort_key >= user->last_sort_key`: 直接遍历当前 `token_head`
├─ 否则:
│  ├─ `target_idx = upper_bound(cache->timeline, target_sort_key)`
│  ├─ 找到最大的 snapshot 使 `snapshot.timeline_idx <= target_idx`
│  ├─ 从 snapshot 复制 positions
│  └─ replay `[replay_start_idx, target_idx)` 的 timeline
└─ 输出有效持仓 (`is_effective_holding(pos)`)

stage3_query_filter(anchor_bucket, filters[], sort_expr, sort_asc, limit) -> {anchor_bucket, users[{user_idx, sort_value}], filter_stats[]}
├─ assert `anchor_bucket >= 0`
├─ assert `0 < limit <= 1000`
├─ for `user_idx in 0..header.user_count`:
│  ├─ if `!(users[user_idx].flags & 1)`: continue
│  ├─ `init_feature_timelines(...)` 得到每个 tag 的 `first_bucket / latest_bucket`
│  ├─ `get_feature(tag)`:
│  │  ├─ if `anchor_bucket < first_bucket[tag]`: return nullptr
│  │  └─ else: `feature_find(user_idx, min(anchor_bucket, latest_bucket[tag]), tag)`
│  ├─ 若 global feature 缺失: continue
│  ├─ 逐条计算 filters, 即使前面已失败也继续累计 `filter_stats.pass/reject`
│  ├─ `sort_value = (sort_expr.empty() ? 0 : eval_numeric(sort_expr))`
│  └─ `candidates.push_back({user_idx, sort_value})`
├─ 按 `sort_value` 排序
├─ 截断到 `limit`
└─ 返回 `FilterResult{anchor_bucket, users, filter_stats}`

StageSync::filter_users_by_features(req) -> {anchor_bucket, users[{addr, sort_value, month_avg_tok, month_avg_exp, month_avg_hp, pnl}], filter_stats[]}
├─ `r = stage3_query_filter(rt_, req)`
├─ for `row in r.users`:
│  ├─ `addr = users[row.user_idx].addr`
│  ├─ 再取一次该用户 global feature at `min(anchor_bucket, latest_bucket)`,补 `month_avg_*`
│  └─ `pnl = total_realized_pnl + total_unrealized_pnl`
└─ 组装 service 层返回

stage3_get_users_sorted(limit) -> {users[]}
├─ // 使用物化榜单缓存
├─ rank_cache.rebuild_if_needed(store)  // 仅在脏时重建
└─ return rank_cache.by_events[0..limit]

stage3_get_bucket_user_count(bucket) -> count
├─ if `bucket < 0`: return 0
├─ if `bucket >= global_feature_user_counts.size()`: return 0
└─ return `global_feature_user_counts[bucket]`

持久化: mmap MAP_SHARED, OS 自动 dirty page writeback; 关闭时 msync(MS_SYNC)
崩溃恢复: 从 header.cursor_sort_key 继续拉取事件; 启动时重建运行时索引
```
