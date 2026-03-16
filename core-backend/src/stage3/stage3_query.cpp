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

// Forward declaration from stage3_trade.cpp
int64_t apply_trade_event(Stage3Runtime* rt, const EventInput& evt, TokenSlot* tok);

// Helper to apply event to replay state
void apply_event_to_replay_state(
    const Stage3Runtime* rt,
    const EventRecord& rec,
    std::unordered_map<uint64_t, TokenPos>& positions) {
  if (rec.cond_idx < 0) return;
  
  uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(rec.cond_idx)) << 16) | 
                  static_cast<uint16_t>(rec.token_idx);
  
  auto& pos = positions[key];
  pos.cond_idx = rec.cond_idx;
  pos.token_idx = rec.token_idx;
  pos.collateral = rec.collateral;
  
  // Replay trade logic using stored amount/price
  const int64_t amount = rec.amount;
  const int64_t price_1e6 = rec.price_1e6;
  const bool has_usd = is_usd_collateral(rec.collateral);
  const int64_t qty = std::abs(amount);
  
  if (qty == 0) return;
  
  // Skip LP events
  if (rec.event_type == 5 || rec.event_type == 6 || rec.event_type == 7) {
    return;
  }
  
  // Update lp for trade events
  if ((rec.event_type >= 1 && rec.event_type <= 4) && price_1e6 > 0 && has_usd) {
    pos.lp = price_1e6;
  }
  
  // Determine price per unit
  double price_per_unit = 0.0;
  if (rec.event_type == 16) { // Convert
    const auto& cond = rt->conditions[rec.cond_idx];
    price_per_unit = static_cast<double>(cond.outcome_count - 1) / static_cast<double>(cond.outcome_count);
  } else if (rec.event_type >= 1 && rec.event_type <= 15) { // price events
    price_per_unit = static_cast<double>(price_1e6) / 1e6;
  }
  
  // Check if transfer event (17-22)
  bool is_transfer = (rec.event_type >= 17 && rec.event_type <= 22);
  
  if (amount > 0) {
    // Positive leg: cover short then open long
    if (has_usd) {
      const int64_t short_qty = std::max<int64_t>(0, -pos.pos);
      const int64_t cover_qty = std::min(qty, short_qty);
      const int64_t open_long_qty = qty - cover_qty;
      
      if (cover_qty > 0 && short_qty > 0) {
        pos.cost -= pos.cost * cover_qty / short_qty;
      }
      pos.pos += qty;
      if (open_long_qty > 0 && !is_transfer) {
        pos.cost += static_cast<int64_t>(open_long_qty * price_per_unit);
      }
    } else {
      pos.pos += qty;
    }
  } else {
    // Negative leg: close long then open short
    if (has_usd) {
      const int64_t long_qty = std::max<int64_t>(0, pos.pos);
      const int64_t close_qty = std::min(qty, long_qty);
      const int64_t open_short_qty = qty - close_qty;
      
      if (close_qty > 0 && long_qty > 0) {
        pos.cost -= pos.cost * close_qty / long_qty;
      }
      pos.pos -= qty;
      if (open_short_qty > 0 && !is_transfer) {
        pos.cost -= static_cast<int64_t>(open_short_qty * price_per_unit);
      }
    } else {
      pos.pos -= qty;
    }
  }
  
  // Remove if position is zero
  if (pos.pos == 0) {
    positions.erase(key);
  }
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
  
  // If target_sort_key >= last_sort_key, return current positions
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
  
  // Historical query: replay events from scratch up to target_sort_key
  std::unordered_map<uint64_t, TokenPos> positions;
  
  uint64_t offset = user->timeline_head;
  while (offset != NULL_LOG_OFFSET) {
    const EventRecord* rec = reinterpret_cast<const EventRecord*>(
        reinterpret_cast<const uint8_t*>(rt->events_log) + offset);
    
    if (rec->sort_key > target_sort_key) break;
    
    apply_event_to_replay_state(rt, *rec, positions);
    offset = rec->next_user_event_offset;
  }
  
  // Build result
  for (const auto& [key, pos] : positions) {
    if (pos.pos != 0) {
      PositionRow row{};
      row.cond_idx = pos.cond_idx;
      row.token_idx = pos.token_idx;
      row.qty = pos.pos;
      row.cost = pos.cost;
      row.lp = pos.lp;
      row.entry_block = 0; // entry_block not tracked in replay
      result.positions.push_back(row);
    }
  }
  
  return result;
}

} // namespace stage3
