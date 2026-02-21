#pragma once

#include <cstdint>
#include <vector>

namespace rebuild {

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
};

struct TokenInfo {
  uint32_t cond_idx;
  uint8_t is_yes;
};

struct RawEvent {
  int64_t sort_key;    // 8  block_number * 1e9 + log_index
  uint32_t cond_idx;   // 4
  uint8_t type;        // 1  EventType
  uint8_t token_idx;   // 1  0=YES, 1=NO, 0xFF=all
  uint16_t _pad;       // 2
  int64_t amount;      // 8  raw units (1e6 = $1)
  int64_t price;       // 8  price * 1e6, or extra data
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

struct RebuildProgress {
  int phase = 0;
  int64_t total_conditions = 0;
  int64_t total_tokens = 0;
  int64_t total_events = 0;
  int64_t total_users = 0;
  int64_t processed_users = 0;
  bool running = false;
  double phase1_ms = 0;
  double phase2_ms = 0;
  double phase3_ms = 0;

  int64_t order_filled_rows = 0;
  int64_t order_filled_events = 0;
  int64_t split_rows = 0;
  int64_t split_events = 0;
  int64_t merge_rows = 0;
  int64_t merge_events = 0;
  int64_t redemption_rows = 0;
  int64_t redemption_events = 0;
  int64_t fpmm_trade_rows = 0;
  int64_t fpmm_trade_events = 0;
  int64_t fpmm_funding_rows = 0;
  int64_t fpmm_funding_events = 0;
  int64_t convert_rows = 0;
  int64_t convert_events = 0;
  int64_t transfer_rows = 0;
  int64_t transfer_events = 0;
};

} // namespace rebuild
