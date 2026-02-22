#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace stage2 {

static constexpr int MAX_OUTCOMES = 8;

enum EventType : uint8_t {
  Buy = 0,
  Sell = 1,
  Split = 2,
  Merge = 3,
  Redemption = 4,
  FPMMBuy = 5,
  FPMMSell = 6,
  FPMMLPAdd = 7,
  FPMMLPRemove = 8,
  Convert = 9,
  TransferIn = 10,
  TransferOut = 11,
};

struct ConditionInfo {
  uint8_t outcome_count = 2;
  std::vector<int64_t> payout_numerators;
  std::string question_id;  // 用于NegRisk convert的market查找
};

struct TokenInfo {
  uint32_t cond_idx;
  uint8_t is_yes;
};

struct FPMMInfo {
  uint32_t cond_idx;
};

struct RawEvent {
  int64_t sort_key;  // 8  block_number * 1e9 + log_index
  uint32_t cond_idx; // 4
  uint8_t type;      // 1  EventType
  uint8_t token_idx; // 1  0=YES, 1=NO
  uint16_t _pad;     // 2
  int64_t amount;    // 8  raw units (1e6 = $1)
  int64_t price;     // 8  price * 1e6
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
// Key 格式: block_number * 1e9 + log_index 的低 32 位 tx_hash 哈希拼接
// ============================================================================

struct TxCondKey {
  int64_t block;
  std::array<uint8_t, 32> tx_hash;
  std::string cond_id;

  bool operator==(const TxCondKey &o) const {
    return block == o.block && tx_hash == o.tx_hash && cond_id == o.cond_id;
  }
};

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

struct SplitInfo {
  int64_t amount;
  std::string stakeholder;
  std::string cond_id;
};

struct MergeInfo {
  int64_t amount;
  std::string stakeholder;
  std::string cond_id;
};

struct RedemptionInfo {
  int64_t payout;
  std::string redeemer;
  std::string cond_id;
};

struct ConvertInfo {
  std::string market_id;
  int64_t index_set;
  int64_t amount;
  std::string stakeholder;
};

struct OrderInfo {
  std::string maker;
  std::string taker;
  int maker_side; // 1=maker买, 2=maker卖
  int64_t usdc;
  int64_t tokens;
  int64_t fee;
};

struct FPMMTradeInfo {
  std::string fpmm_addr;
  std::string trader;
  int side; // 1=Buy, 2=Sell
  int outcome_idx;
  int64_t usdc;
  int64_t tokens;
};

struct FPMMFundingInfo {
  std::string fpmm_addr;
  std::string funder;
  int side; // 1=Added, 2=Removed
  int64_t amount0;
  int64_t amount1;
};

} // namespace stage2

namespace std {
template <>
struct hash<stage2::TxCondKey> {
  size_t operator()(const stage2::TxCondKey &k) const {
    size_t h = std::hash<int64_t>()(k.block);
    for (size_t i = 0; i < 8; ++i)
      h ^= std::hash<uint8_t>()(k.tx_hash[i]) << (i % 8);
    h ^= std::hash<std::string>()(k.cond_id);
    return h;
  }
};

template <>
struct hash<stage2::TxKey> {
  size_t operator()(const stage2::TxKey &k) const {
    size_t h = std::hash<int64_t>()(k.block);
    for (size_t i = 0; i < 8; ++i)
      h ^= std::hash<uint8_t>()(k.tx_hash[i]) << (i % 8);
    return h;
  }
};

template <>
struct hash<stage2::TxTokenKey> {
  size_t operator()(const stage2::TxTokenKey &k) const {
    size_t h = std::hash<int64_t>()(k.block);
    for (size_t i = 0; i < 8; ++i)
      h ^= std::hash<uint8_t>()(k.tx_hash[i]) << (i % 8);
    h ^= std::hash<std::string>()(k.token_id);
    return h;
  }
};

template <>
struct hash<stage2::TxMarketKey> {
  size_t operator()(const stage2::TxMarketKey &k) const {
    size_t h = std::hash<int64_t>()(k.block);
    for (size_t i = 0; i < 8; ++i)
      h ^= std::hash<uint8_t>()(k.tx_hash[i]) << (i % 8);
    h ^= std::hash<std::string>()(k.market_id);
    return h;
  }
};
} // namespace std
