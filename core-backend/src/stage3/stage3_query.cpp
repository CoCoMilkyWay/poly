#include "stage3.hpp"

#include <cassert>
#include <unordered_map>

namespace stage3 {

// ============================================================================
// stage3_query_status
// ============================================================================

QueryStatus stage3_query_status(const Stage3Runtime* rt, int64_t head_block) {
  QueryStatus status{};
  
  int64_t last_block = (rt->header->cursor_sort_key >= 0) 
                        ? sort_key_to_block(rt->header->cursor_sort_key) 
                        : 0;
  
  status.syncing = true; // Assume always syncing for simplicity
  status.last_block = last_block;
  status.head_block = head_block;
  status.behind_blocks = head_block - last_block;
  status.behind_chunks = status.behind_blocks / 10000; // Rough estimate
  status.blocks_per_second = 0.0; // Would need timing data
  status.eta_seconds = -1.0;
  status.ready = status.behind_blocks < 1000;
  
  return status;
}

// ============================================================================
// stage3_query_pnl
// ============================================================================

PnlResult stage3_query_pnl(Stage3Runtime* rt, const Address20& user_addr) {
  PnlResult result{};
  result.user = user_addr;
  
  uint32_t user_idx = user_index_lookup(rt, user_addr);
  if (user_idx == NULL_IDX) {
    // User not found
    result.block = 0;
    result.total_events = 0;
    return result;
  }
  
  UserBlock* user = &rt->users[user_idx];
  result.block = (user->last_sort_key > 0) ? sort_key_to_block(user->last_sort_key) : 0;
  result.total_events = user->total_events;
  
  // Build timeline by traversing user's event chain
  result.timeline.reserve(user->timeline_count);
  
  uint64_t offset = user->timeline_head;
  while (offset != NULL_LOG_OFFSET) {
    const EventRecord* rec = reinterpret_cast<const EventRecord*>(
        reinterpret_cast<const uint8_t*>(rt->events_log) + offset);
    
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

// Helper to apply event to positions map for replay
void apply_event_to_positions(std::unordered_map<uint64_t, TokenPos>& positions, 
                              const EventRecord& rec) {
  if (rec.cond_idx < 0) return;
  
  uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(rec.cond_idx)) << 16) | 
                  static_cast<uint16_t>(rec.token_idx);
  
  // We need original event data to properly replay
  // Since EventRecord doesn't store amount, we track cumulative state
  // This is a simplified version - full implementation would store more data
  
  // For now, positions are retrieved from current token state at sort_key
  // A proper implementation would maintain snapshots
}

} // namespace

PositionsResult stage3_query_positions(Stage3Runtime* rt, const Address20& user_addr, int64_t target_sort_key) {
  PositionsResult result{};
  result.user = user_addr;
  result.sort_key = target_sort_key;
  result.block = sort_key_to_block(target_sort_key);
  
  uint32_t user_idx = user_index_lookup(rt, user_addr);
  if (user_idx == NULL_IDX) {
    return result;
  }
  
  UserBlock* user = &rt->users[user_idx];
  
  // For simplicity, if target_sort_key >= last_sort_key, return current positions
  // A full implementation would replay events to reconstruct historical state
  if (target_sort_key >= user->last_sort_key) {
    uint32_t idx = user->token_head;
    while (idx != NULL_IDX) {
      TokenSlot* t = &rt->token_pool[idx];
      if (t->pos != 0) {
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
  
  // Historical query: need to replay events
  // This requires building positions from scratch up to target_sort_key
  
  // Token state during replay
  struct ReplayTokenState {
    int64_t pos = 0;
    int64_t cost = 0;
    int64_t lp = 0;
    int64_t entry_block = 0;
  };
  
  std::unordered_map<uint64_t, ReplayTokenState> token_states;
  
  // Walk timeline up to target_sort_key
  uint64_t offset = user->timeline_head;
  while (offset != NULL_LOG_OFFSET) {
    const EventRecord* rec = reinterpret_cast<const EventRecord*>(
        reinterpret_cast<const uint8_t*>(rt->events_log) + offset);
    
    if (rec->sort_key > target_sort_key) break;
    
    // We can't fully replay without original event amount/price
    // This is a limitation of the current EventRecord structure
    // For a complete implementation, we'd need to store more data
    
    offset = rec->next_user_event_offset;
  }
  
  // Build result from token_states
  for (const auto& [key, state] : token_states) {
    if (state.pos != 0) {
      PositionRow row{};
      row.cond_idx = static_cast<int32_t>(key >> 16);
      row.token_idx = static_cast<int16_t>(key & 0xFFFF);
      row.qty = state.pos;
      row.cost = state.cost;
      row.lp = state.lp;
      row.entry_block = state.entry_block;
      result.positions.push_back(row);
    }
  }
  
  return result;
}

} // namespace stage3
