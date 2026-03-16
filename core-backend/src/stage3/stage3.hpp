#pragma once

// ============================================================================
// Stage3 mmap-centric design
// Files: store.bin (~167GB) + events.log (~160GB) + users.idx (512MB)
// ============================================================================

#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Forward declaration for Stage2 integration
namespace core { namespace rocks { class Stage2UserEventStore; } }

namespace stage3 {

// ============================================================================
// Constants
// ============================================================================

constexpr size_t MAX_CONDITIONS     = 1'000'000;      // 100万 conditions
constexpr size_t MAX_USERS          = 10'000'000;     // 1000万用户
constexpr size_t MAX_TOKENS         = 100'000'000;    // 1亿 token slots
constexpr size_t MAX_FEATURES       = 500'000'000;    // 5亿 feature slots
constexpr size_t MAX_SHARPE_AGGS    = 50'000'000;     // 5000万 sharpe bucket 聚合
constexpr size_t MAX_SHARPE_SAMPLES = 500'000'000;    // 5亿 sharpe 样本点
constexpr size_t OUTCOME_MAX        = 256;            // 最大 outcome 数
constexpr uint32_t NULL_IDX         = UINT32_MAX;     // 空指针
constexpr uint64_t NULL_LOG_OFFSET  = UINT64_MAX;     // events.log 空指针

constexpr int64_t SORT_KEY_SCALE    = 1'000'000'000LL; // sort_key = block * 1e9 + log_idx
constexpr int64_t BLOCK_BUCKET_SIZE = 100'000;         // 10w blocks per bucket
constexpr int64_t MIN_HOLDING_QTY   = 10'000'000LL;    // 10 tokens (1e6 scale)
constexpr double POS_EPSILON        = 1e-9;

constexpr size_t USER_INDEX_SLOT_COUNT = 16 * 1024 * 1024;  // 16M slots

using Address20 = std::array<uint8_t, 20>;

// ============================================================================
// Store Header (4KB)
// ============================================================================

struct StoreHeader {
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
};

static_assert(sizeof(StoreHeader) == 4096);

// ============================================================================
// ConditionMeta[100万] (~2GB)
// ============================================================================

struct ConditionMeta {                   // 2056B per condition
  uint8_t  outcome_count;                // outcome 数量
  int8_t   tag_id;                       // 行业分类 (-1=unknown, 0-13=行业)
  uint8_t  flags;                        // bit0=valid
  uint8_t  _pad0[5];
  int64_t  payout_numerators[OUTCOME_MAX]; // 256 × 8B = 2048B
};

static_assert(sizeof(ConditionMeta) == 2056);

// ============================================================================
// TokenPool[1亿] (4.8GB)
// ============================================================================

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
};

static_assert(sizeof(TokenSlot) == 48);

// ============================================================================
// FeaturePool[5亿] (140GB)
// ============================================================================

struct FeatureSlot {                     // 280B
  // key
  uint32_t user_idx;                     // 所属用户
  int32_t  bucket;                       // block_bucket (10w blocks per bucket)
  int8_t   tag_id;                       // -1=全局聚合, 0-13=行业
  uint8_t  flags;                        // bit0=valid
  uint16_t _pad0;
  // chain
  uint32_t next;                         // 同用户下一个 feature / free list
  uint32_t _pad1;

  // Node-A0: 增量续算锚点
  int64_t  last_sort_key_10w;
  int64_t  last_block_10w;
  int64_t  last_exposure_10w;
  int64_t  last_holding_period_10w_lo;   // HUGEINT low part
  int64_t  last_holding_period_10w_hi;   // HUGEINT high part
  int64_t  last_token_count_10w;

  // Node-A: 10w 原子统计
  int64_t  time_weight_sum_10w;
  int64_t  token_count_tw_sum_10w;
  int64_t  exposure_tw_sum_10w_lo;       // HUGEINT low
  int64_t  exposure_tw_sum_10w_hi;       // HUGEINT high
  int64_t  volume_sum_10w;
  int64_t  holding_period_exp_tw_sum_10w_lo;
  int64_t  holding_period_exp_tw_sum_10w_hi;

  // Node-B: 10w 归一化输出
  int64_t  token_avg_10w;
  int64_t  exposure_avg_10w;
  int64_t  volume_10w;
  int64_t  holding_period_avg_10w;
  float    sharpe_10w;
  float    _pad2;

  // Node-C: 前缀缓存
  int64_t  ps_token_avg_10w;
  int64_t  ps_exposure_avg_10w;
  int64_t  ps_volume_10w;
  int64_t  ps_holding_period_avg_10w;

  // Node-D: 窗口投影输出
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
};

static_assert(sizeof(FeatureSlot) == 280);

// ============================================================================
// SharpeAggPool[5000万] (2.4GB)
// ============================================================================

struct SharpeAgg {                       // 48B
  uint32_t user_idx;
  int32_t  bucket;
  int64_t  close_pnl;                    // bucket 末尾 pnl
  int64_t  min_pnl;                      // bucket 内最小 pnl
  int64_t  max_pnl;                      // bucket 内最大 pnl
  uint32_t sample_head;                  // -> sharpe_sample_pool 链表头
  uint16_t sample_count;
  uint16_t _pad0;
  int32_t  last_block;                   // 最近更新的 block
  uint32_t next;                         // 同用户下一个 agg / free list
};

static_assert(sizeof(SharpeAgg) == 48);

// ============================================================================
// SharpeSamplePool[5亿] (16GB)
// ============================================================================

struct SharpeSample {                    // 32B
  uint32_t agg_idx;                      // 所属 SharpeAgg
  int32_t  block_offset;                 // bucket 内 block 偏移 (0 ~ 99999)
  int64_t  pnl;                          // realized_cum + unrealized_pnl
  uint32_t next;                         // 同 agg 下一个样本 / free list
  uint32_t _pad;
  int64_t  _reserved;
};

static_assert(sizeof(SharpeSample) == 32);

// ============================================================================
// Users[1000万] (1.28GB)
// ============================================================================

struct UserBlock {                       // 128B
  // 基础信息
  Address20 addr;                        // 20B
  uint32_t  flags;                       // 4B, bit0=occupied

  // 统计
  int64_t   total_events;
  int64_t   total_realized_pnl;
  int64_t   total_unrealized_pnl;
  int64_t   last_sort_key;

  // Pool 引用
  uint32_t  token_head;
  uint32_t  token_count;
  uint32_t  feature_head;
  uint32_t  feature_count;
  uint32_t  sharpe_agg_head;
  uint32_t  sharpe_agg_count;

  // Timeline 引用
  uint64_t  timeline_head;
  uint64_t  timeline_tail;
  uint32_t  timeline_count;

  uint8_t   _reserved[128 - 20 - 4 - 8*4 - 4*6 - 8 - 8 - 4];
};

static_assert(sizeof(UserBlock) == 128);

// ============================================================================
// EventsLog (append-only, ~160GB for 2B events)
// ============================================================================

struct EventRecord {                     // 96B
  int64_t  sort_key;
  int32_t  cond_idx;
  int16_t  token_idx;
  int8_t   event_type;
  int8_t   tag_id;
  int64_t  amount;                       // signed, 1e6 scale (for replay)
  int64_t  price_1e6;                    // price (for replay)
  int16_t  collateral;                   // collateral type (for replay)
  int16_t  _pad0;
  int32_t  token_count;
  int64_t  realized_delta;
  int64_t  realized_cum;
  int64_t  unrealized_pnl;
  int64_t  exposure;
  int64_t  volume;
  int64_t  holding_period;
  uint64_t next_user_event_offset;
};

static_assert(sizeof(EventRecord) == 96);

// ============================================================================
// UserIndex (512MB hash table)
// ============================================================================

struct UserIndexEntry {                  // 32B
  Address20 addr;
  uint32_t  user_idx;
  uint32_t  next;                        // collision chain
  uint32_t  _pad;
};

static_assert(sizeof(UserIndexEntry) == 32);

// ============================================================================
// Input structure (temporary, not persisted)
// ============================================================================

struct EventInput {
  Address20 user_addr;
  int64_t   sort_key;
  int32_t   cond_idx;
  int32_t   event_type;
  int32_t   token_idx;
  int32_t   collateral;
  int64_t   amount;
  int64_t   price_1e6;
};

// ============================================================================
// Runtime cache (not persisted)
// ============================================================================

struct TokenPos {
  int32_t cond_idx;
  int16_t token_idx;
  int16_t collateral;
  int64_t pos;
  int64_t cost;
  int64_t lp;
};

struct PosSnapshot {
  size_t  timeline_idx;
  std::vector<TokenPos> positions;
};

struct UserQueryCache {
  Address20 addr;
  std::vector<EventRecord> timeline;
  std::vector<PosSnapshot> snapshots;
  int64_t loaded_sort_key;
};

struct SharpeBucketCache {
  int32_t bucket;
  std::vector<std::pair<int32_t, int64_t>> samples;  // (block_offset, pnl)
  int64_t close_pnl;
  int64_t min_pnl;
  int64_t max_pnl;
};

struct SharpeSparseCache {
  Address20 user_addr;
  std::map<int32_t, SharpeBucketCache> buckets;
  int32_t oldest_bucket;
};

// ============================================================================
// Runtime handle
// ============================================================================

struct Stage3Runtime {
  // mmap pointers
  StoreHeader*     header;
  ConditionMeta*   conditions;
  TokenSlot*       token_pool;
  FeatureSlot*     feature_pool;
  SharpeAgg*       sharpe_agg_pool;
  SharpeSample*    sharpe_sample_pool;
  UserBlock*       users;
  
  EventRecord*     events_log;
  size_t           events_log_capacity;
  
  UserIndexEntry*  user_index;
  
  // file descriptors
  int fd_store;
  int fd_events;
  int fd_index;
  
  // mmap sizes
  size_t store_size;
  size_t events_size;
  size_t index_size;
};

// ============================================================================
// API declarations - Store
// ============================================================================

Stage3Runtime* stage3_open(const char* data_dir);
void stage3_close(Stage3Runtime* rt);
void stage3_sync(Stage3Runtime* rt);

// ============================================================================
// API declarations - ConditionMeta loading
// ============================================================================

void stage3_set_condition(Stage3Runtime* rt, int32_t cond_idx, 
                          uint8_t outcome_count, int8_t tag_id,
                          const int64_t* payout_numerators);
void stage3_mark_condition_valid(Stage3Runtime* rt, int32_t cond_idx);
const ConditionMeta* stage3_get_condition(const Stage3Runtime* rt, int32_t cond_idx);

// ============================================================================
// API declarations - UserIndex
// ============================================================================

uint32_t user_index_lookup(const Stage3Runtime* rt, const Address20& addr);
uint32_t user_index_insert(Stage3Runtime* rt, const Address20& addr);
uint32_t user_get_or_create(Stage3Runtime* rt, const Address20& addr);

// ============================================================================
// API declarations - Pool operations
// ============================================================================

uint32_t token_alloc(Stage3Runtime* rt);
void token_free(Stage3Runtime* rt, uint32_t idx);
uint32_t feature_alloc(Stage3Runtime* rt);
void feature_free(Stage3Runtime* rt, uint32_t idx);
uint32_t sharpe_agg_alloc(Stage3Runtime* rt);
void sharpe_agg_free(Stage3Runtime* rt, uint32_t idx);
uint32_t sharpe_sample_alloc(Stage3Runtime* rt);
void sharpe_sample_free(Stage3Runtime* rt, uint32_t idx);

// ============================================================================
// API declarations - Token state operations
// ============================================================================

TokenSlot* token_find(Stage3Runtime* rt, uint32_t user_idx, int32_t cond_idx, int16_t token_idx);
TokenSlot* token_get_or_create(Stage3Runtime* rt, uint32_t user_idx, int32_t cond_idx, int16_t token_idx, int16_t collateral);
void token_remove_if_empty(Stage3Runtime* rt, uint32_t user_idx, TokenSlot* tok);

// ============================================================================
// API declarations - Feature operations
// ============================================================================

FeatureSlot* feature_find(Stage3Runtime* rt, uint32_t user_idx, int32_t bucket, int8_t tag_id);
FeatureSlot* feature_get_or_create(Stage3Runtime* rt, uint32_t user_idx, int32_t bucket, int8_t tag_id);

// ============================================================================
// API declarations - Sharpe operations
// ============================================================================

SharpeAgg* sharpe_agg_find(Stage3Runtime* rt, uint32_t user_idx, int32_t bucket);
SharpeAgg* sharpe_agg_get_or_create(Stage3Runtime* rt, uint32_t user_idx, int32_t bucket);
void sharpe_prune_old_buckets(Stage3Runtime* rt, uint32_t user_idx, int32_t min_bucket_to_keep);
void sharpe_prune_all_users(Stage3Runtime* rt, int32_t min_bucket_to_keep);

// ============================================================================
// API declarations - Trade
// ============================================================================

int64_t apply_trade_event(Stage3Runtime* rt, const EventInput& evt, TokenSlot* tok);

// ============================================================================
// API declarations - Feature update
// ============================================================================

void update_feature_on_event(Stage3Runtime* rt, uint32_t user_idx, const EventInput& evt, const EventRecord& rec);

// ============================================================================
// API declarations - Sharpe update
// ============================================================================

void update_sharpe_on_event(Stage3Runtime* rt, uint32_t user_idx, int64_t pnl, int32_t bucket, int32_t block_offset);

// ============================================================================
// API declarations - Events log
// ============================================================================

uint64_t events_log_append(Stage3Runtime* rt, const EventRecord& rec, uint32_t user_idx);

// ============================================================================
// API declarations - Sync
// ============================================================================

// Process a batch of EventInput (internal use)
size_t process_event_batch(Stage3Runtime* rt, const std::vector<EventInput>& batch);

// Main sync entry point with Stage2 integration
size_t stage3_sync_tick(Stage3Runtime* rt, 
                        core::rocks::Stage2UserEventStore& event_store,
                        int64_t head_block, 
                        size_t batch_limit);

// Perform Sharpe pruning after sync (called periodically)
void stage3_post_sync_prune(Stage3Runtime* rt);

// ============================================================================
// API declarations - Query
// ============================================================================

struct QueryStatus {
  bool syncing;
  int64_t last_block;
  int64_t head_block;
  int64_t behind_blocks;
  int64_t behind_chunks;
  double blocks_per_second;
  double eta_seconds;
  bool ready;
  uint64_t user_count;
  int64_t processed_events;
  int32_t head_bucket;
};

struct TimelineRow {
  int64_t sort_key;
  int8_t event_type;
  int64_t realized_pnl;
  int64_t unrealized_pnl;
  int32_t token_count;
};

struct PositionRow {
  int32_t cond_idx;
  int16_t token_idx;
  int64_t qty;
  int64_t cost;
  int64_t lp;
  int64_t entry_block;
};

struct PnlResult {
  Address20 user;
  int64_t block;
  int64_t total_events;
  std::vector<TimelineRow> timeline;
};

struct PositionsResult {
  Address20 user;
  int64_t sort_key;
  int64_t block;
  std::vector<PositionRow> positions;
};

QueryStatus stage3_query_status(const Stage3Runtime* rt, int64_t head_block);
PnlResult stage3_query_pnl(Stage3Runtime* rt, const Address20& user_addr);
PositionsResult stage3_query_positions(Stage3Runtime* rt, const Address20& user_addr, int64_t target_sort_key);

// ============================================================================
// API declarations - Filter
// ============================================================================

struct FilterRequest {
  int64_t anchor_bucket;
  std::vector<std::string> filters;
  std::string sort_expr;
  bool sort_asc;
  int32_t limit;
};

struct FilterUserRow {
  Address20 addr;
  double sort_value;
};

struct FilterResult {
  int64_t anchor_bucket;
  std::vector<FilterUserRow> users;
};

FilterResult stage3_query_filter(Stage3Runtime* rt, const FilterRequest& req);

// ============================================================================
// Utility functions
// ============================================================================

inline int64_t sort_key_to_block(int64_t sort_key) {
  return sort_key / SORT_KEY_SCALE;
}

inline int32_t block_to_bucket(int64_t block) {
  return static_cast<int32_t>(block / BLOCK_BUCKET_SIZE);
}

inline int64_t bucket_end_block(int32_t bucket) {
  return static_cast<int64_t>(bucket + 1) * BLOCK_BUCKET_SIZE;
}

inline bool is_usd_collateral(int32_t collateral) {
  return collateral >= 1 && collateral <= 4;
}

inline bool is_effective_holding(int64_t qty) {
  return std::abs(qty) >= MIN_HOLDING_QTY;
}

// Address20 utilities
Address20 parse_address(const std::string& hex);
std::string format_address(const Address20& addr);
uint64_t address_hash(const Address20& addr);
bool address_equal(const Address20& a, const Address20& b);

} // namespace stage3
