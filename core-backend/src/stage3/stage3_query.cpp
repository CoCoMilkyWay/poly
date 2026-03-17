#include "stage3.hpp"

#include <algorithm>
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

namespace {

constexpr size_t kSnapshotStride = 256;

void apply_event_to_replay_state(
    Stage3Runtime *rt,
    const EventRecord &rec,
    std::unordered_map<uint64_t, TokenSlot> &positions) {
  if (rec.cond_idx < 0) {
    return;
  }

  const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(rec.cond_idx)) << 16) |
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
  evt.user_idx = 0;
  evt.sort_key = rec.sort_key;
  evt.current_block = sort_key_to_block(rec.sort_key);
  evt.cond_idx = rec.cond_idx;
  evt.event_type = rec.event_type;
  evt.token_idx = rec.token_idx;
  evt.collateral = rec.collateral;
  evt.bucket = block_to_bucket(evt.current_block);
  evt.block_offset = static_cast<int32_t>(evt.current_block % BLOCK_BUCKET_SIZE);
  evt.tag_id = rec.tag_id;
  evt.amount = rec.amount;
  evt.price_1e6 = rec.price_1e6;
  (void)apply_trade_event(rt, evt, &replay_tok);

  if (replay_tok.pos == 0) {
    positions.erase(it);
  }
}

PosSnapshot build_snapshot(const std::unordered_map<uint64_t, TokenSlot> &positions,
                           size_t timeline_idx) {
  PosSnapshot snap{};
  snap.timeline_idx = timeline_idx;
  snap.positions.reserve(positions.size());
  for (const auto &[key, tok] : positions) {
    (void)key;
    if (tok.pos == 0) {
      continue;
    }
    snap.positions.push_back({
        tok.cond_idx,
        tok.token_idx,
        tok.collateral,
        tok.pos,
        tok.cost,
        tok.lp,
        tok.entry_block,
    });
  }
  return snap;
}

void load_user_query_cache(Stage3Runtime *rt, const UserBlock *user, UserQueryCache *cache) {
  cache->timeline.clear();
  cache->snapshots.clear();
  cache->timeline.reserve(user->timeline_count);
  cache->snapshots.reserve((static_cast<size_t>(user->timeline_count) + kSnapshotStride - 1) / kSnapshotStride);

  std::unordered_map<uint64_t, TokenSlot> replay_positions;
  replay_positions.reserve(256);

  size_t timeline_idx = 0;
  uint64_t offset = user->timeline_head;
  while (offset != NULL_LOG_OFFSET) {
    const EventRecord *rec = reinterpret_cast<const EventRecord *>(
        reinterpret_cast<const uint8_t *>(rt->events_log) + offset);
    cache->timeline.push_back(*rec);
    apply_event_to_replay_state(rt, *rec, replay_positions);

    timeline_idx++;
    if (timeline_idx % kSnapshotStride == 0) {
      cache->snapshots.push_back(build_snapshot(replay_positions, timeline_idx));
    }
    offset = rec->next_user_event_offset;
  }

  if (timeline_idx > 0 &&
      (cache->snapshots.empty() || cache->snapshots.back().timeline_idx != timeline_idx)) {
    cache->snapshots.push_back(build_snapshot(replay_positions, timeline_idx));
  }

  cache->loaded_sort_key = user->last_sort_key;
}

} // namespace

UserQueryCache *stage3_get_user_query_cache(Stage3Runtime *rt, const Address20 &user_addr) {
  const uint32_t user_idx = user_index_lookup(rt, user_addr);
  if (user_idx == NULL_IDX) {
    return nullptr;
  }

  const UserBlock *user = &rt->users[user_idx];
  UserQueryCache *cache = rt->query_cache.get_or_create(user_addr);
  if (cache->loaded_sort_key < user->last_sort_key) {
    load_user_query_cache(rt, user, cache);
  }
  return cache;
}

// ============================================================================
// stage3_query_positions
// ============================================================================

PositionsResult stage3_query_positions(Stage3Runtime *rt, const Address20 &user_addr, int64_t target_sort_key) {
  PositionsResult result{};
  result.user = user_addr;
  result.sort_key = target_sort_key;
  result.block = sort_key_to_block(target_sort_key);

  const uint32_t user_idx = user_index_lookup(rt, user_addr);
  if (user_idx == NULL_IDX) {
    return result;
  }
  const UserBlock *user = &rt->users[user_idx];

  if (target_sort_key >= user->last_sort_key) {
    uint32_t idx = user->token_head;
    while (idx != NULL_IDX) {
      TokenSlot *t = &rt->token_pool[idx];
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

  UserQueryCache *cache = stage3_get_user_query_cache(rt, user_addr);
  assert(cache != nullptr);
  std::unordered_map<uint64_t, TokenSlot> positions;
  const auto &timeline = cache->timeline;
  const size_t target_idx = static_cast<size_t>(std::upper_bound(
                                                    timeline.begin(),
                                                    timeline.end(),
                                                    target_sort_key,
                                                    [](int64_t sort_key, const EventRecord &rec) {
                                                      return sort_key < rec.sort_key;
                                                    }) -
                                                timeline.begin());

  size_t replay_start_idx = 0;
  const auto snap_it = std::upper_bound(
      cache->snapshots.begin(),
      cache->snapshots.end(),
      target_idx,
      [](size_t timeline_idx, const PosSnapshot &snap) {
        return timeline_idx < snap.timeline_idx;
      });
  const PosSnapshot *best_snapshot = (snap_it == cache->snapshots.begin()) ? nullptr : &*(snap_it - 1);

  if (best_snapshot != nullptr) {
    replay_start_idx = best_snapshot->timeline_idx;
    positions.reserve(best_snapshot->positions.size() * 2 + 1);
    for (const TokenPos &tp : best_snapshot->positions) {
      if (tp.pos == 0) {
        continue;
      }
      const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(tp.cond_idx)) << 16) |
                           static_cast<uint16_t>(tp.token_idx);
      TokenSlot tok{};
      tok.user_idx = 0;
      tok.next = NULL_IDX;
      tok.cond_idx = tp.cond_idx;
      tok.token_idx = tp.token_idx;
      tok.collateral = tp.collateral;
      tok.pos = tp.pos;
      tok.cost = tp.cost;
      tok.lp = tp.lp;
      tok.entry_block = tp.entry_block;
      positions.emplace(key, tok);
    }
  }

  for (size_t i = replay_start_idx; i < target_idx; ++i) {
    apply_event_to_replay_state(rt, timeline[i], positions);
  }

  result.positions.reserve(positions.size());
  for (const auto &[key, pos] : positions) {
    (void)key;
    if (!is_effective_holding(pos.pos)) {
      continue;
    }
    PositionRow row{};
    row.cond_idx = pos.cond_idx;
    row.token_idx = pos.token_idx;
    row.qty = pos.pos;
    row.cost = pos.cost;
    row.lp = pos.lp;
    row.entry_block = pos.entry_block;
    result.positions.push_back(row);
  }

  return result;
}

} // namespace stage3
