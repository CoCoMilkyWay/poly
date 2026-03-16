#include "../core/rocks_store.hpp"
#include "stage3.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace stage3 {

// Forward declarations from other translation units
void calc_sharpe_for_feature(Stage3Runtime *rt, uint32_t user_idx, FeatureSlot *feat);

// ============================================================================
// stage3_sync_tick - 同步主循环
// 从 Stage2 RocksDB 拉取事件并处理
// 返回处理的事件数
// ============================================================================

size_t stage3_sync_tick(Stage3Runtime *rt,
                        const core::rocks::Stage2UserEventStore &event_store,
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

  for (const auto &src : source_events) {
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

void stage3_post_sync_prune(Stage3Runtime *rt) {
  if (rt->header->cursor_sort_key < 0) {
    return;
  }

  const int32_t current_bucket = block_to_bucket(
      sort_key_to_block(rt->header->cursor_sort_key));
  const int32_t min_bucket_to_keep = std::max(0, current_bucket - 99);

  if (min_bucket_to_keep <= rt->dirty_users.last_pruned_bucket) {
    return;
  }

  for (uint32_t user_idx : rt->dirty_users.users) {
    sharpe_prune_old_buckets(rt, user_idx, min_bucket_to_keep);
  }
  rt->dirty_users.last_pruned_bucket = min_bucket_to_keep;
}

// ============================================================================
// process_event_batch - 处理一批事件
// ============================================================================

size_t process_event_batch(Stage3Runtime *rt, const std::vector<EventInput> &batch) {
  if (batch.empty())
    return 0;

  // ========== Step 1: Build user index cache ==========
  std::unordered_map<Address20, uint32_t, Address20Hash, Address20Equal> user_idx_cache;
  user_idx_cache.reserve(batch.size());
  rt->dirty_users.users.clear();

  for (const auto &evt : batch) {
    auto [it, inserted] = user_idx_cache.try_emplace(evt.user_addr, NULL_IDX);
    if (!inserted) {
      continue;
    }
    uint32_t user_idx = user_index_lookup(rt, evt.user_addr);
    if (user_idx == NULL_IDX) {
      user_idx = user_get_or_create(rt, evt.user_addr);
    }
    it->second = user_idx;
  }

  // ========== Step 2: Build runtime feature state for touched user/tag ==========
  struct TokenFeatureContrib {
    int64_t token_count = 0;
    int64_t exposure = 0;
    __int128 exposure_entry = 0;
  };
  auto token_feature_contrib = [](const TokenSlot &st) {
    TokenFeatureContrib c;
    c.token_count = is_effective_holding(st.pos) ? 1 : 0;
    if (st.pos != 0 && st.lp > 0) {
      c.exposure = static_cast<int64_t>(
          std::llround(std::abs(static_cast<long double>(st.pos) * static_cast<long double>(st.lp) / 1e6L)));
      c.exposure_entry = static_cast<__int128>(c.exposure) * st.entry_block;
    }
    return c;
  };
  auto runtime_pair_key = [](uint32_t user_idx, int8_t tag_id) {
    const uint64_t encoded_tag = static_cast<uint8_t>(static_cast<int16_t>(tag_id) + 128);
    return (static_cast<uint64_t>(user_idx) << 8) | encoded_tag;
  };
  auto apply_runtime_delta = [](FeatureRuntimeState &state,
                                int64_t token_delta,
                                int64_t exposure_delta,
                                __int128 exposure_entry_delta) {
    state.token_count += token_delta;
    state.exposure += exposure_delta;
    state.exposure_entry_sum += exposure_entry_delta;
    assert(state.token_count >= 0);
    assert(state.exposure >= 0);
    if (state.exposure == 0) {
      assert(state.exposure_entry_sum == 0);
    }
  };

  std::unordered_map<uint32_t, std::unordered_set<int8_t>> touched_tags_by_user;
  touched_tags_by_user.reserve(user_idx_cache.size());
  for (const auto &evt : batch) {
    if (evt.cond_idx < 0) {
      continue;
    }
    const ConditionMeta *cond = stage3_get_condition(rt, evt.cond_idx);
    assert(cond != nullptr);
    assert(evt.token_idx >= 0);
    assert(evt.token_idx < cond->outcome_count);
    auto user_it = user_idx_cache.find(evt.user_addr);
    assert(user_it != user_idx_cache.end());
    const uint32_t user_idx = user_it->second;
    auto &touched_tags = touched_tags_by_user[user_idx];
    touched_tags.insert(cond->tag_id);
    touched_tags.insert(-1);
  }

  std::unordered_map<uint64_t, FeatureRuntimeState> runtime_state_by_pair;
  runtime_state_by_pair.reserve(touched_tags_by_user.size() * 4);
  for (const auto &[user_idx, tags] : touched_tags_by_user) {
    for (int8_t tag_id : tags) {
      runtime_state_by_pair.try_emplace(runtime_pair_key(user_idx, tag_id), FeatureRuntimeState{});
    }
    uint32_t idx = rt->users[user_idx].token_head;
    while (idx != NULL_IDX) {
      TokenSlot *tok = &rt->token_pool[idx];
      if (tok->cond_idx >= 0) {
        const ConditionMeta *cond = stage3_get_condition(rt, tok->cond_idx);
        assert(cond != nullptr);
        const int8_t token_tag_id = cond->tag_id;
        const TokenFeatureContrib contrib = token_feature_contrib(*tok);
        if (tags.contains(token_tag_id)) {
          FeatureRuntimeState &tag_state = runtime_state_by_pair[runtime_pair_key(user_idx, token_tag_id)];
          apply_runtime_delta(tag_state, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
        if (tags.contains(-1)) {
          FeatureRuntimeState &global_state = runtime_state_by_pair[runtime_pair_key(user_idx, -1)];
          apply_runtime_delta(global_state, contrib.token_count, contrib.exposure, contrib.exposure_entry);
        }
      }
      idx = tok->next;
    }
  }

  // ========== Step 3: Process each event ==========
  for (const auto &evt : batch) {
    auto user_it = user_idx_cache.find(evt.user_addr);
    assert(user_it != user_idx_cache.end());
    const uint32_t user_idx = user_it->second;
    rt->dirty_users.users.insert(user_idx);

    UserBlock *user = &rt->users[user_idx];
    const int64_t prev_pnl = user->total_realized_pnl + user->total_unrealized_pnl;
    int64_t realized_delta = 0;
    TokenSlot *tok = nullptr;
    int64_t old_mtm = 0;
    int64_t new_mtm = 0;
    int64_t pos_before = 0;
    TokenFeatureContrib after_contrib{};
    int8_t event_tag_id = -1;
    FeatureRuntimeState tag_runtime_state{};
    FeatureRuntimeState global_runtime_state{};

    // Process token state for valid condition events
    if (evt.cond_idx >= 0) {
      const ConditionMeta *cond = stage3_get_condition(rt, evt.cond_idx);
      assert(cond != nullptr);
      assert(evt.token_idx >= 0);
      assert(evt.token_idx < cond->outcome_count);
      event_tag_id = cond->tag_id;

      tok = token_get_or_create(rt, user_idx, evt.cond_idx,
                                static_cast<int16_t>(evt.token_idx),
                                static_cast<int16_t>(evt.collateral));
      pos_before = tok->pos;
      const int64_t cost_before = tok->cost;
      const int64_t lp_before = tok->lp;
      if (lp_before > 0) {
        old_mtm = static_cast<int64_t>(
            static_cast<long double>(pos_before) * static_cast<long double>(lp_before) / 1e6L) - cost_before;
      }

      const TokenFeatureContrib before_contrib = token_feature_contrib(*tok);
      realized_delta = apply_trade_event(rt, evt, tok);
      after_contrib = token_feature_contrib(*tok);
      if (tok->lp > 0) {
        new_mtm = static_cast<int64_t>(
            static_cast<long double>(tok->pos) * static_cast<long double>(tok->lp) / 1e6L) - tok->cost;
      }

      const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
      const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
      const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;

      FeatureRuntimeState &tag_state = runtime_state_by_pair[runtime_pair_key(user_idx, event_tag_id)];
      FeatureRuntimeState &global_state = runtime_state_by_pair[runtime_pair_key(user_idx, -1)];
      apply_runtime_delta(tag_state, token_count_delta, exposure_delta, exposure_entry_delta);
      apply_runtime_delta(global_state, token_count_delta, exposure_delta, exposure_entry_delta);
      tag_runtime_state = tag_state;
      global_runtime_state = global_state;
    }

    // ========== Step 3.4: Update UserBlock ==========
    user->total_events++;
    user->total_realized_pnl += realized_delta;

    // Incremental update: only touched token changes unrealized / effective count.
    if (evt.cond_idx >= 0) {
      user->total_unrealized_pnl += (new_mtm - old_mtm);
      const bool was_effective = is_effective_holding(pos_before);
      const bool is_effective = is_effective_holding(tok->pos);
      if (was_effective && !is_effective) {
        assert(user->token_count > 0);
        user->token_count--;
      } else if (!was_effective && is_effective) {
        user->token_count++;
      }
    }

    user->last_sort_key = evt.sort_key;

    // ========== Step 3.5: Append EventRecord ==========
    EventRecord rec{};
    rec.sort_key = evt.sort_key;
    rec.cond_idx = evt.cond_idx;
    rec.token_idx = static_cast<int16_t>(evt.token_idx);
    rec.event_type = static_cast<int8_t>(evt.event_type);
    rec.tag_id = event_tag_id;
    rec.amount = evt.amount;
    rec.price_1e6 = evt.price_1e6;
    rec.collateral = static_cast<int16_t>(evt.collateral);
    rec.realized_delta = realized_delta;
    rec.realized_cum = user->total_realized_pnl;
    rec.unrealized_pnl = user->total_unrealized_pnl;
    rec.token_count = static_cast<int32_t>(user->token_count);

    // Calculate exposure, volume, holding_period
    if (evt.cond_idx >= 0 && tok) {
      rec.exposure = after_contrib.exposure;
      rec.volume = static_cast<int64_t>(
          std::abs(static_cast<double>(evt.amount) * static_cast<double>(evt.price_1e6) / 1e6));
      int64_t current_block = sort_key_to_block(evt.sort_key);
      rec.holding_period = (tok->entry_block > 0) ? (current_block - tok->entry_block) : 0;
    }

    events_log_append(rt, rec, user_idx);

    // ========== Step 3.6: Update Features ==========
    if (evt.cond_idx >= 0) {
      update_feature_on_event(rt, user_idx, evt, rec, tag_runtime_state, global_runtime_state);
    }

    // ========== Step 3.7: Update Sharpe samples ==========
    int64_t pnl = user->total_realized_pnl + user->total_unrealized_pnl;
    int64_t current_block = sort_key_to_block(evt.sort_key);
    int32_t bucket = block_to_bucket(current_block);
    int32_t block_offset = static_cast<int32_t>(current_block % BLOCK_BUCKET_SIZE);
    if (pnl != prev_pnl) {
      update_sharpe_on_event(rt, user_idx, pnl, prev_pnl, bucket, block_offset);
    }

    // ========== Step 3.8: Clean up empty token ==========
    if (evt.cond_idx >= 0 && tok && tok->pos == 0) {
      token_remove_if_empty(rt, user_idx, tok);
    }
  }

  // ========== Step 4: Batch finalization ==========
  for (uint32_t user_idx : rt->dirty_users.users) {
    const int32_t user_bucket = block_to_bucket(sort_key_to_block(rt->users[user_idx].last_sort_key));
    FeatureSlot *global_feat = feature_find(rt, user_idx, user_bucket, -1);
    if (global_feat != nullptr) {
      calc_sharpe_for_feature(rt, user_idx, global_feat);
    }
    assert(std::isfinite(static_cast<double>(rt->users[user_idx].total_unrealized_pnl)));
    rt->rank_cache.mark_dirty(user_idx);
  }

  rt->header->cursor_sort_key = batch.back().sort_key;
  rt->header->cursor_processed_events += batch.size();

  return batch.size();
}

} // namespace stage3
