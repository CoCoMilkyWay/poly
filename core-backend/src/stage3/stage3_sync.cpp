#include "stage3.hpp"
#include "../core/rocks_store.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <set>

namespace stage3 {

// Forward declarations from other translation units
void calc_sharpe_for_feature(Stage3Runtime* rt, uint32_t user_idx, FeatureSlot* feat);

// ============================================================================
// stage3_sync_tick - 同步主循环
// 从 Stage2 RocksDB 拉取事件并处理
// 返回处理的事件数
// ============================================================================

size_t stage3_sync_tick(Stage3Runtime* rt, 
                        core::rocks::Stage2UserEventStore& event_store,
                        int64_t head_block, 
                        size_t batch_limit) {
  const int64_t cursor = rt->header->cursor_sort_key;
  const int64_t head_sort_key = head_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);
  
  // Check if already synced
  if (cursor >= head_sort_key) {
    return 0;
  }
  
  // Fetch events from Stage2
  std::vector<core::rocks::Stage2UserEventRecord> source_events = 
      event_store.scan_by_sort_key(cursor, head_sort_key, batch_limit);
  
  if (source_events.empty()) {
    // No events, advance cursor
    rt->header->cursor_sort_key = head_sort_key;
    return 0;
  }
  
  // Cut at block boundary if we hit the limit
  if (source_events.size() == batch_limit) {
    const int64_t last_block = sort_key_to_block(source_events.back().sort_key);
    size_t cut = source_events.size();
    while (cut > 0 && sort_key_to_block(source_events[cut - 1].sort_key) == last_block) {
      --cut;
    }
    assert(cut > 0); // Single block should not exceed batch limit
    source_events.resize(cut);
  }
  
  // Convert to EventInput
  std::vector<EventInput> batch;
  batch.reserve(source_events.size());
  
  for (const auto& src : source_events) {
    assert(src.user_addr.size() == 20);
    
    EventInput evt{};
    std::memcpy(evt.user_addr.data(), src.user_addr.data(), 20);
    evt.sort_key = src.sort_key;
    evt.cond_idx = src.cond_idx;
    evt.event_type = src.event_type;
    evt.token_idx = src.token_idx;
    evt.collateral = src.collateral;
    evt.amount = src.amount;
    evt.price_1e6 = src.price;
    
    batch.push_back(evt);
  }
  
  // Process the batch
  return process_event_batch(rt, batch);
}

// ============================================================================
// stage3_post_sync_prune - Sharpe 淘汰 (定期调用)
// ============================================================================

void stage3_post_sync_prune(Stage3Runtime* rt) {
  const int32_t current_bucket = block_to_bucket(
      sort_key_to_block(rt->header->cursor_sort_key));
  const int32_t min_bucket_to_keep = std::max(0, current_bucket - 99);
  
  sharpe_prune_all_users(rt, min_bucket_to_keep);
}

// ============================================================================
// process_event_batch - 处理一批事件
// ============================================================================

size_t process_event_batch(Stage3Runtime* rt, const std::vector<EventInput>& batch) {
  if (batch.empty()) return 0;
  
  // ========== Step 1: Collect touched users ==========
  std::set<Address20, std::less<>> touched_addrs;
  for (const auto& evt : batch) {
    touched_addrs.insert(evt.user_addr);
  }
  
  // ========== Step 2: Ensure all users exist ==========
  for (const auto& addr : touched_addrs) {
    user_get_or_create(rt, addr);
  }
  
  // ========== Step 3: Process each event ==========
  for (const auto& evt : batch) {
    uint32_t user_idx = user_index_lookup(rt, evt.user_addr);
    assert(user_idx != NULL_IDX);
    
    UserBlock* user = &rt->users[user_idx];
    int64_t realized_delta = 0;
    TokenSlot* tok = nullptr;
    
    // Process token state for valid condition events
    if (evt.cond_idx >= 0) {
      tok = token_get_or_create(rt, user_idx, evt.cond_idx, 
                                 static_cast<int16_t>(evt.token_idx),
                                 static_cast<int16_t>(evt.collateral));
      
      realized_delta = apply_trade_event(rt, evt, tok);
    }
    
    // ========== Step 3.4: Update UserBlock ==========
    user->total_events++;
    user->total_realized_pnl += realized_delta;
    
    // Recalculate unrealized PnL and token count
    if (evt.cond_idx >= 0) {
      user->total_unrealized_pnl = 0;
      uint32_t effective_token_count = 0;
      
      uint32_t idx = user->token_head;
      while (idx != NULL_IDX) {
        TokenSlot* t = &rt->token_pool[idx];
        if (t->lp > 0) {
          // unrealized = pos * lp / 1e6 - cost
          int64_t mtm = static_cast<int64_t>(
              static_cast<double>(t->pos) * static_cast<double>(t->lp) / 1e6);
          user->total_unrealized_pnl += mtm - t->cost;
        }
        if (is_effective_holding(t->pos)) {
          effective_token_count++;
        }
        idx = t->next;
      }
    }
    
    user->last_sort_key = evt.sort_key;
    
    // ========== Step 3.5: Append EventRecord ==========
    EventRecord rec{};
    rec.sort_key = evt.sort_key;
    rec.cond_idx = evt.cond_idx;
    rec.token_idx = static_cast<int16_t>(evt.token_idx);
    rec.event_type = static_cast<int8_t>(evt.event_type);
    rec.tag_id = (evt.cond_idx >= 0) ? rt->conditions[evt.cond_idx].tag_id : -1;
    rec.amount = evt.amount;
    rec.price_1e6 = evt.price_1e6;
    rec.collateral = static_cast<int16_t>(evt.collateral);
    rec.realized_delta = realized_delta;
    rec.realized_cum = user->total_realized_pnl;
    rec.unrealized_pnl = user->total_unrealized_pnl;
    
    // Count effective tokens
    uint32_t token_count = 0;
    uint32_t idx = user->token_head;
    while (idx != NULL_IDX) {
      if (is_effective_holding(rt->token_pool[idx].pos)) {
        token_count++;
      }
      idx = rt->token_pool[idx].next;
    }
    rec.token_count = static_cast<int32_t>(token_count);
    
    // Calculate exposure, volume, holding_period
    if (evt.cond_idx >= 0 && tok) {
      rec.exposure = static_cast<int64_t>(
          std::abs(static_cast<double>(tok->pos) * static_cast<double>(tok->lp) / 1e6));
      rec.volume = static_cast<int64_t>(
          std::abs(static_cast<double>(evt.amount) * static_cast<double>(evt.price_1e6) / 1e6));
      int64_t current_block = sort_key_to_block(evt.sort_key);
      rec.holding_period = (tok->entry_block > 0) ? (current_block - tok->entry_block) : 0;
    }
    
    events_log_append(rt, rec, user_idx);
    
    // ========== Step 3.6: Update Features ==========
    update_feature_on_event(rt, user_idx, evt, rec);
    
    // ========== Step 3.7: Update Sharpe samples ==========
    int64_t pnl = user->total_realized_pnl + user->total_unrealized_pnl;
    int64_t current_block = sort_key_to_block(evt.sort_key);
    int32_t bucket = block_to_bucket(current_block);
    int32_t block_offset = static_cast<int32_t>(current_block % BLOCK_BUCKET_SIZE);
    update_sharpe_on_event(rt, user_idx, pnl, bucket, block_offset);
    
    // ========== Step 3.8: Clean up empty token ==========
    if (evt.cond_idx >= 0 && tok && tok->pos == 0) {
      token_remove_if_empty(rt, user_idx, tok);
    }
  }
  
  // ========== Step 4: Update cursor ==========
  rt->header->cursor_sort_key = batch.back().sort_key;
  rt->header->cursor_processed_events += batch.size();
  
  return batch.size();
}

// ============================================================================
// Utility: Calculate user stats from scratch (for verification)
// ============================================================================

void recalc_user_unrealized(Stage3Runtime* rt, uint32_t user_idx) {
  UserBlock* user = &rt->users[user_idx];
  
  user->total_unrealized_pnl = 0;
  user->token_count = 0;
  
  uint32_t idx = user->token_head;
  while (idx != NULL_IDX) {
    TokenSlot* t = &rt->token_pool[idx];
    if (t->lp > 0) {
      int64_t mtm = static_cast<int64_t>(
          static_cast<double>(t->pos) * static_cast<double>(t->lp) / 1e6);
      user->total_unrealized_pnl += mtm - t->cost;
    }
    if (is_effective_holding(t->pos)) {
      user->token_count++;
    }
    idx = t->next;
  }
}

} // namespace stage3
