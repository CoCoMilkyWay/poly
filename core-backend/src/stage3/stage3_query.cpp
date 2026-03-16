#include "stage3.hpp"

#include <cassert>
#include <unordered_map>

namespace stage3 {

// ============================================================================
// stage3_query_status
// ============================================================================

QueryStatus stage3_query_status(const Stage3Runtime *rt, int64_t head_block) {
  QueryStatus status{};

  int64_t last_block = (rt->header->cursor_sort_key >= 0)
                           ? sort_key_to_block(rt->header->cursor_sort_key)
                           : 0;

  status.syncing = last_block < head_block;
  status.last_block = last_block;
  status.head_block = head_block;
  status.behind_blocks = std::max<int64_t>(0, head_block - last_block);
  status.behind_chunks = (status.behind_blocks == 0) ? 0 : 1 + status.behind_blocks / 10000000;
  status.blocks_per_second = 0.0; // Caller should compute from timing
  status.eta_seconds = -1.0;
  status.ready = status.behind_blocks < 1000;
  status.user_count = rt->header->user_count;
  status.processed_events = rt->header->cursor_processed_events;
  status.head_bucket = rt->header->head_bucket;

  return status;
}

// ============================================================================
// stage3_query_pnl
// ============================================================================

PnlResult stage3_query_pnl(Stage3Runtime *rt, const Address20 &user_addr) {
  PnlResult result{};
  result.user = user_addr;

  uint32_t user_idx = user_index_lookup(rt, user_addr);
  if (user_idx == NULL_IDX) {
    // User not found
    result.block = 0;
    result.total_events = 0;
    return result;
  }

  UserBlock *user = &rt->users[user_idx];
  result.block = (user->last_sort_key > 0) ? sort_key_to_block(user->last_sort_key) : 0;
  result.total_events = user->total_events;

  // Build timeline by traversing user's event chain
  result.timeline.reserve(user->timeline_count);

  uint64_t offset = user->timeline_head;
  while (offset != NULL_LOG_OFFSET) {
    const EventRecord *rec = reinterpret_cast<const EventRecord *>(
        reinterpret_cast<const uint8_t *>(rt->events_log) + offset);

    TimelineRow row{};
    row.sort_key = rec->sort_key;
    row.event_type = rec->event_type;
    row.realized_pnl = rec->realized_cum;
    row.unrealized_pnl = rec->unrealized_pnl;
    row.token_count = rec->token_count;

    result.timeline.push_back(row);
    offset = rec->next_user_event_offset;
  }

  return result;
}

// ============================================================================
// stage3_query_positions
// ============================================================================

namespace {

// Helper to apply event to replay state
void apply_event_to_replay_state(
    Stage3Runtime *rt,
    const EventRecord &rec,
    std::unordered_map<uint64_t, TokenSlot> &positions) {
  if (rec.cond_idx < 0)
    return;

  uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(rec.cond_idx)) << 16) |
                 static_cast<uint16_t>(rec.token_idx);

  auto [it, inserted] = positions.try_emplace(key, TokenSlot{});
  TokenSlot &replay_tok = it->second;
  if (inserted) {
    replay_tok.user_idx = 0;
    replay_tok.next = NULL_IDX;
    replay_tok.cond_idx = rec.cond_idx;
    replay_tok.token_idx = rec.token_idx;
    replay_tok.collateral = rec.collateral;
    replay_tok.pos = 0;
    replay_tok.cost = 0;
    replay_tok.lp = 0;
    replay_tok.entry_block = 0;
  }
  replay_tok.collateral = rec.collateral;

  EventInput evt{};
  evt.user_addr.fill(0);
  evt.sort_key = rec.sort_key;
  evt.cond_idx = rec.cond_idx;
  evt.event_type = rec.event_type;
  evt.token_idx = rec.token_idx;
  evt.collateral = rec.collateral;
  evt.amount = rec.amount;
  evt.price_1e6 = rec.price_1e6;
  (void)apply_trade_event(rt, evt, &replay_tok);

  if (replay_tok.pos == 0) {
    positions.erase(it);
  }
}

} // namespace

PositionsResult stage3_query_positions(Stage3Runtime *rt, const Address20 &user_addr, int64_t target_sort_key) {
  PositionsResult result{};
  result.user = user_addr;
  result.sort_key = target_sort_key;
  result.block = sort_key_to_block(target_sort_key);

  uint32_t user_idx = user_index_lookup(rt, user_addr);
  if (user_idx == NULL_IDX) {
    return result;
  }

  UserBlock *user = &rt->users[user_idx];

  // If target_sort_key >= last_sort_key, return current positions
  if (target_sort_key >= user->last_sort_key) {
    uint32_t idx = user->token_head;
    while (idx != NULL_IDX) {
      TokenSlot *t = &rt->token_pool[idx];
      // Only return effective holdings (faithful to old architecture: is_effective_holding_i64)
      if (is_effective_holding(t->pos)) {
        PositionRow row{};
        row.cond_idx = t->cond_idx;
        row.token_idx = t->token_idx;
        row.qty = t->pos;
        row.cost = t->cost;
        row.lp = t->lp;
        row.entry_block = t->entry_block;
        result.positions.push_back(row);
      }
      idx = t->next;
    }
    return result;
  }

  // Historical query: replay events from scratch up to target_sort_key
  std::unordered_map<uint64_t, TokenSlot> positions;

  uint64_t offset = user->timeline_head;
  while (offset != NULL_LOG_OFFSET) {
    const EventRecord *rec = reinterpret_cast<const EventRecord *>(
        reinterpret_cast<const uint8_t *>(rt->events_log) + offset);

    if (rec->sort_key > target_sort_key)
      break;

    apply_event_to_replay_state(rt, *rec, positions);
    offset = rec->next_user_event_offset;
  }

  // Build result - only return effective holdings (faithful to old architecture)
  for (const auto &[key, pos] : positions) {
    if (is_effective_holding(pos.pos)) {
      PositionRow row{};
      row.cond_idx = pos.cond_idx;
      row.token_idx = pos.token_idx;
      row.qty = pos.pos;
      row.cost = pos.cost;
      row.lp = pos.lp;
      row.entry_block = pos.entry_block;
      result.positions.push_back(row);
    }
  }

  return result;
}

} // namespace stage3
