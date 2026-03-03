#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace stage2 {

static constexpr int MAX_OUTCOMES = 8;
static constexpr uint32_t UNKNOWN_COND_IDX = UINT32_MAX; // TransferInferred token 的特殊值
static constexpr uint8_t UNKNOWN_TOKEN_IDX = 0xFF;       // TransferInferred token 的特殊值
static constexpr int64_t SORT_KEY_SCALE = 1000000000LL;
static constexpr int64_t TRANSFER_FLAT_LOG_SCALE = 10000;

enum EventType : uint8_t {
  OrderBuy = 0,
  OrderSell = 1,
  SplitNormal = 2,
  SplitNegRisk = 3,
  SplitNonPoly = 4,
  MergeNormal = 5,
  MergeNegRisk = 6,
  MergeNonPoly = 7,
  Redemption = 8,
  RedemptionNonPoly = 9,
  Convert = 10,
  FPMMBuy = 11,
  FPMMSell = 12,
  FPMMLPAdd = 13,
  FPMMLPRemove = 14,
  FPMMLPReturn = 15,
  TransferInNegRisk = 16,
  TransferInOther = 17,
  TransferInNonPoly = 18,
  TransferOutNegRisk = 19,
  TransferOutOther = 20,
  TransferOutNonPoly = 21,
};

enum class ConditionSource : uint8_t {
  ConditionPrep = 0,      // 直接从ConditionPreparation事件(通用,无法确定协议)
  PolymarketTokenReg = 1, // Polymarket Exchange的TokenRegistered
  PolymarketFPMM = 2,     // Polymarket FPMM Factory创建
  OtherFPMM = 3,          // 其他协议FPMM创建(预留,需要扫描其他Factory才有数据)
  SplitEvent = 4,         // 从Split事件推断(无法确定协议)
  TransferInferred = 5,   // 从Transfer事件推断(未知协议,无condition信息)
  MergeEvent = 6,         // 从Merge事件推断(无法确定协议)
  RedemptionEvent = 7,    // 从Redemption事件推断(无法确定协议)
};

enum class TokenSource : uint8_t {
  PolymarketTokenReg = 0, // Polymarket Exchange的TokenRegistered
  PolymarketFPMM = 1,     // Polymarket FPMM Factory计算
  OtherFPMM = 2,          // 其他协议FPMM计算(预留,需要扫描其他Factory才有数据)
  SplitEvent = 3,         // 从Split事件计算(无法确定协议)
  TransferInferred = 4,   // 从Transfer事件推断(未知协议)
  MergeEvent = 5,         // 从Merge事件计算(无法确定协议)
  RedemptionEvent = 6,    // 从Redemption事件计算(无法确定协议)
};

enum class Collateral : uint8_t {
  Unknown = 0,
  USDC = 1,         // 0x3c499c542cef5e3811e1192ce70d8cc03d5c3359 (native)
  USDCe = 2,        // 0x2791bca1f2de4661ed88a30c99a7a9449aa84174 (PoS bridged)
  USDT = 3,         // 0xc2132d05d31c914a87c6611c10748aeb04b58e8f
  WrappedUSDCe = 4, // 0x3a3bd7bb9528e159577f7c2e685cc81a765002e2 (NegRisk wrapped collateral)
  // 预留更多...
};

struct ConditionInfo {
  uint8_t outcome_count = 0;
  std::vector<int64_t> payout_numerators;
  std::string question_id;
  ConditionSource source = ConditionSource::ConditionPrep;
};

struct TokenInfo {
  uint32_t cond_idx;
  uint8_t token_idx;
  TokenSource source = TokenSource::PolymarketTokenReg;
};

struct FPMMInfo {
  uint32_t cond_idx;
  uint8_t collateral = static_cast<uint8_t>(Collateral::USDC);
};

struct RawEvent {
  int64_t sort_key;   // 8  block_number * SORT_KEY_SCALE + flat_log_index
  uint32_t cond_idx;  // 4
  uint8_t type;       // 1  EventType
  uint8_t token_idx;  // 1  known: 0..outcome_count-1, unknown: 255
  uint8_t collateral; // 1  Collateral enum
  uint8_t _pad;       // 1
  int64_t amount;     // 8  raw units (1e6 = $1)
  int64_t price;      // 8  price * 1e6, 非USDC时为0
};
static_assert(sizeof(RawEvent) == 32);

struct Snapshot {
  int64_t sort_key;                // 8
  int64_t delta;                   // 8
  int64_t price;                   // 8
  int64_t positions[MAX_OUTCOMES]; // 64
  int64_t cost_basis;              // 8
  int64_t realized_pnl;            // 8
  uint8_t event_type;              // 1
  uint8_t token_idx;               // 1
  uint8_t outcome_count;           // 1
  uint8_t _pad[5];                 // 5
};
static_assert(sizeof(Snapshot) == 112);

struct UserConditionHistory {
  uint32_t cond_idx;
  std::vector<Snapshot> snapshots;
};

struct UserState {
  std::vector<UserConditionHistory> conditions;
};

struct ReplayState {
  int64_t positions[MAX_OUTCOMES] = {};
  int64_t cost[MAX_OUTCOMES] = {};
  int64_t realized_pnl = 0;
};

// ============================================================================
// 语义索引结构体 (Phase 2 构建, 供 Transfer 分类使用)
// Transfer sort_key: block_number * SORT_KEY_SCALE + flat_log_index
// ============================================================================

struct TxKey {
  int64_t block;
  std::array<uint8_t, 32> tx_hash;

  bool operator==(const TxKey &o) const {
    return block == o.block && tx_hash == o.tx_hash;
  }
};

struct TxTokenKey {
  int64_t block;
  std::array<uint8_t, 32> tx_hash;
  std::string token_id;

  bool operator==(const TxTokenKey &o) const {
    return block == o.block && tx_hash == o.tx_hash && token_id == o.token_id;
  }
};

struct TxMarketKey {
  int64_t block;
  std::array<uint8_t, 32> tx_hash;
  std::string market_id;

  bool operator==(const TxMarketKey &o) const {
    return block == o.block && tx_hash == o.tx_hash && market_id == o.market_id;
  }
};

struct TxFPMMKey {
  int64_t block;
  std::array<uint8_t, 32> tx_hash;
  std::string fpmm_addr;

  bool operator==(const TxFPMMKey &o) const {
    return block == o.block && tx_hash == o.tx_hash && fpmm_addr == o.fpmm_addr;
  }
};

struct TxOpBounds {
  int64_t left_exclusive = -1;
  int64_t right_inclusive = -1;
};

struct SplitInfo {
  int64_t log_index = -1;
  std::string stakeholder;
  std::string collateral_token;
  std::string parent_collection_id;
  std::string cond_id;
  std::vector<std::string> partition;
  int64_t amount;
  int consumed_count = 0;
  bool covered_by_parent = false;
};

struct MergeInfo {
  int64_t log_index = -1;
  std::string stakeholder;
  std::string collateral_token;
  std::string parent_collection_id;
  std::string cond_id;
  std::vector<std::string> partition;
  int64_t amount;
  int consumed_count = 0;
  bool covered_by_parent = false;
};

struct RedemptionInfo {
  int64_t log_index = -1;
  std::string redeemer;
  std::string collateral_token;
  std::string parent_collection_id;
  std::string cond_id;
  std::vector<std::string> index_sets;
  int64_t payout;
  int consumed_count = 0;
  bool covered_by_parent = false;
};

struct ConvertInfo {
  int64_t log_index = -1;
  std::string market_id;
  std::string index_set;
  int64_t amount;
  std::string stakeholder;
  int consumed_count = 0;
};

struct OrderInfo {
  int64_t log_index = -1;
  std::string token_id;
  std::string maker;
  std::string taker;
  int maker_side; // 1=maker买, 2=maker卖
  int64_t quote_amount;
  int64_t tokens;
  int64_t fee;
  bool consumed = false;
};

struct FPMMTradeInfo {
  int64_t log_index = -1;
  std::string fpmm_addr;
  std::string trader;
  int side; // 1=Buy, 2=Sell
  int outcome_idx;
  int64_t collateral_amount;
  int64_t tokens;
  // Whether this semantic row is expected to have a consumable ERC1155 leg.
  bool requires_erc1155_leg = true;
  bool consumed = false;
  bool explained_without_direct_leg = false;
};

struct FPMMFundingInfo {
  int64_t log_index = -1;
  std::string fpmm_addr;
  std::string funder;
  int side; // 1=Added, 2=Removed
  std::vector<int64_t> amounts;
  int consumed_count = 0;
};

inline size_t hash_combine(size_t lhs, size_t rhs) {
  lhs ^= rhs + 0x9e3779b97f4a7c15ULL + (lhs << 6) + (lhs >> 2);
  return lhs;
}

inline size_t hash_bytes32(const std::array<uint8_t, 32> &bytes) {
  size_t h = 0xcbf29ce484222325ULL;
  for (uint8_t b : bytes) {
    h ^= static_cast<size_t>(b);
    h *= 0x100000001b3ULL;
  }
  return h;
}

} // namespace stage2

namespace std {
template <>
struct hash<stage2::TxKey> {
  size_t operator()(const stage2::TxKey &k) const {
    size_t h = stage2::hash_bytes32(k.tx_hash);
    return stage2::hash_combine(h, std::hash<int64_t>()(k.block));
  }
};

template <>
struct hash<stage2::TxTokenKey> {
  size_t operator()(const stage2::TxTokenKey &k) const {
    size_t h = stage2::hash_bytes32(k.tx_hash);
    h = stage2::hash_combine(h, std::hash<int64_t>()(k.block));
    h = stage2::hash_combine(h, std::hash<std::string>()(k.token_id));
    return h;
  }
};

template <>
struct hash<stage2::TxMarketKey> {
  size_t operator()(const stage2::TxMarketKey &k) const {
    size_t h = stage2::hash_bytes32(k.tx_hash);
    h = stage2::hash_combine(h, std::hash<int64_t>()(k.block));
    h = stage2::hash_combine(h, std::hash<std::string>()(k.market_id));
    return h;
  }
};

template <>
struct hash<stage2::TxFPMMKey> {
  size_t operator()(const stage2::TxFPMMKey &k) const {
    size_t h = stage2::hash_bytes32(k.tx_hash);
    h = stage2::hash_combine(h, std::hash<int64_t>()(k.block));
    h = stage2::hash_combine(h, std::hash<std::string>()(k.fpmm_addr));
    return h;
  }
};

} // namespace std
