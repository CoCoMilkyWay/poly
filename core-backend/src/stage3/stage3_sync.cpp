#include "../core/rocks_store.hpp"
#include "stage3.hpp"

#include "misc/profiler.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace stage3 {

// Forward declarations from other translation units
void calc_sharpe_for_feature(Stage3Runtime *rt, uint32_t user_idx, FeatureSlot *feat, int32_t first_bucket);

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
  std::vector<core::rocks::Stage2UserEventRecord> source_events;
  {
    TraceN("s3/sync_tick/scan_events");
    source_events = event_store.scan_by_sort_key(cursor, head_sort_key, batch_limit);
  }

  if (source_events.empty()) {
    // No events, advance cursor
    rt->header->cursor_sort_key = head_sort_key;
    return 0;
  }

  // Cut at block boundary if we hit the limit
  {
    TraceN("s3/sync_tick/cut_boundary");
    if (source_events.size() == batch_limit) {
      const int64_t last_block = sort_key_to_block(source_events.back().sort_key);
      size_t cut = source_events.size();
      while (cut > 0 && sort_key_to_block(source_events[cut - 1].sort_key) == last_block) {
        --cut;
      }
      assert(cut > 0); // Single block should not exceed batch limit
      source_events.resize(cut);
    }
  }

  // Convert to EventInput
  std::vector<EventInput> batch;
  {
    TraceN("s3/sync_tick/convert_events");
    std::unordered_map<Address20, uint32_t, Address20Hash, Address20Equal> user_idx_cache;
    user_idx_cache.reserve(source_events.size());
    batch.reserve(source_events.size());

    for (const auto &src : source_events) {
      assert(src.user_addr.size() == 20);

      Address20 user_addr{};
      std::memcpy(user_addr.data(), src.user_addr.data(), 20);
      auto [user_it, inserted] = user_idx_cache.try_emplace(user_addr, NULL_IDX);
      if (inserted) {
        uint32_t user_idx = user_index_lookup(rt, user_addr);
        if (user_idx == NULL_IDX) {
          user_idx = user_get_or_create(rt, user_addr);
        }
        user_it->second = user_idx;
      }

      EventInput evt{};
      evt.user_idx = user_it->second;
      evt.cond_idx = src.cond_idx;
      evt.event_type = src.event_type;
      evt.token_idx = src.token_idx;
      evt.collateral = src.collateral;
      evt.sort_key = src.sort_key;
      evt.current_block = sort_key_to_block(src.sort_key);
      evt.bucket = block_to_bucket(evt.current_block);
      evt.block_offset = static_cast<int32_t>(evt.current_block % BLOCK_BUCKET_SIZE);
      evt.tag_id = -1;
      evt.amount = src.amount;
      evt.price_1e6 = src.price;
      if (evt.cond_idx >= 0) {
        const ConditionMeta *cond = stage3_get_condition(rt, evt.cond_idx);
        assert(cond != nullptr);
        assert(evt.token_idx >= 0);
        assert(evt.token_idx < cond->outcome_count);
        evt.tag_id = cond->tag_id;
        assert(evt.tag_id >= 0);
        assert(tag_slot(evt.tag_id) < FEATURE_TAG_SLOT_COUNT);
      }

      batch.push_back(evt);
    }
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

  struct UserTask {
    uint32_t user_idx = NULL_IDX;
    uint32_t begin = 0;
    uint32_t count = 0;
    uint16_t touched_tag_mask = 0;
    uint8_t shard = 0;
  };

  const uint16_t global_tag_mask = static_cast<uint16_t>(1u << tag_slot(-1));
  auto tag_mask = [](int8_t tag_id) -> uint16_t {
    return static_cast<uint16_t>(1u << tag_slot(tag_id));
  };

  std::unordered_map<uint32_t, uint32_t> task_idx_by_user;
  std::vector<UserTask> user_tasks;
  std::vector<uint32_t> event_order(batch.size());
  std::array<std::vector<uint32_t>, STAGE3_SYNC_SHARD_COUNT> tasks_by_shard;
  std::array<uint32_t, STAGE3_SYNC_SHARD_COUNT> shard_event_counts{};
  {
    TraceN("s3/sync_tick/batch/tasks");
    task_idx_by_user.reserve(batch.size());
    user_tasks.reserve(batch.size() / 8 + 1);
    rt->dirty_users.users.clear();
    rt->dirty_users.users.reserve(batch.size() / 8 + 1);

    for (const auto &evt : batch) {
      auto [it, inserted] = task_idx_by_user.try_emplace(evt.user_idx, static_cast<uint32_t>(user_tasks.size()));
      if (inserted) {
        const uint32_t shard = user_shard(rt, evt.user_idx);
        user_tasks.push_back({evt.user_idx, 0, 0, 0, static_cast<uint8_t>(shard)});
        tasks_by_shard[shard].push_back(static_cast<uint32_t>(user_tasks.size() - 1));
        rt->dirty_users.users.push_back(evt.user_idx);
      }
      UserTask &task = user_tasks[it->second];
      task.count++;
      shard_event_counts[task.shard]++;
      if (evt.cond_idx >= 0) {
        task.touched_tag_mask |= tag_mask(evt.tag_id);
        task.touched_tag_mask |= global_tag_mask;
      }
    }

    uint32_t next_begin = 0;
    for (auto &task : user_tasks) {
      task.begin = next_begin;
      next_begin += task.count;
    }

    std::vector<uint32_t> write_pos(user_tasks.size());
    for (size_t i = 0; i < user_tasks.size(); ++i) {
      write_pos[i] = user_tasks[i].begin;
    }

    for (uint32_t event_idx = 0; event_idx < batch.size(); ++event_idx) {
      auto it = task_idx_by_user.find(batch[event_idx].user_idx);
      assert(it != task_idx_by_user.end());
      event_order[write_pos[it->second]++] = event_idx;
    }
  }

  uint64_t reserved_log_tail = rt->header->events_log_tail + static_cast<uint64_t>(batch.size()) * sizeof(EventRecord);
  {
    TraceN("s3/sync_tick/batch/events_reserve");
    events_log_ensure_capacity(rt, reserved_log_tail);
  }

  std::array<uint64_t, STAGE3_SYNC_SHARD_COUNT> shard_log_begin{};
  {
    uint64_t next_offset = rt->header->events_log_tail;
    for (uint32_t shard = 0; shard < STAGE3_SYNC_SHARD_COUNT; ++shard) {
      shard_log_begin[shard] = next_offset;
      next_offset += static_cast<uint64_t>(shard_event_counts[shard]) * sizeof(EventRecord);
    }
    assert(next_offset == reserved_log_tail);
  }

  auto process_user_task = [&](const UserTask &task, uint64_t &log_cursor) {
    UserBlock *user = &rt->users[task.user_idx];
    std::array<FeatureRuntimeState, FEATURE_TAG_SLOT_COUNT> runtime_states{};
    std::array<int32_t, FEATURE_TAG_SLOT_COUNT> feature_first_buckets{};
    std::array<int32_t, FEATURE_TAG_SLOT_COUNT> feature_latest_buckets{};
    std::array<uint32_t, FEATURE_TAG_SLOT_COUNT> feature_latest_indices{};
    uint16_t materialized_feature_mask = 0;
    int32_t global_sharpe_recalc_start_bucket = -1;

    init_feature_timelines(rt, task.user_idx, feature_first_buckets, feature_latest_buckets, &feature_latest_indices);
    auto assert_dense_feature_slot = [&](int8_t tag_id) {
      assert(tag_id >= -1);
      assert(tag_slot(tag_id) < FEATURE_TAG_SLOT_COUNT);
      const size_t slot = tag_slot(tag_id);
      const int32_t first_bucket = feature_first_buckets[slot];
      const int32_t latest_bucket = feature_latest_buckets[slot];
      const uint32_t latest_idx = feature_latest_indices[slot];
      if (first_bucket < 0) {
        assert(latest_bucket < 0);
        assert(latest_idx == NULL_IDX);
        return;
      }
      assert(latest_bucket >= first_bucket);
      assert(latest_idx != NULL_IDX);
      const FeatureSlot *latest_feat = &rt->feature_pool[latest_idx];
      assert(latest_feat->user_idx == task.user_idx);
      assert(latest_feat->bucket == latest_bucket);
      assert(latest_feat->tag_id == tag_id);
      for (int32_t bucket = first_bucket; bucket <= latest_bucket; ++bucket) {
        FeatureSlot *feat = feature_find(rt, task.user_idx, bucket, tag_id);
        assert(feat != nullptr);
        assert(feat->user_idx == task.user_idx);
        assert(feat->bucket == bucket);
        assert(feat->tag_id == tag_id);
      }
    };
    for (size_t slot = 0; slot < FEATURE_TAG_SLOT_COUNT; ++slot) {
      if (feature_first_buckets[slot] >= 0) {
        assert_dense_feature_slot(static_cast<int8_t>(static_cast<int>(slot) - 1));
      }
    }
    for (size_t slot = 0; slot < FEATURE_TAG_SLOT_COUNT; ++slot) {
      if (feature_first_buckets[slot] >= 0) {
        materialized_feature_mask |= static_cast<uint16_t>(1u << slot);
      }
    }

    if (task.touched_tag_mask != 0) {
      uint32_t idx = user->token_head;
      while (idx != NULL_IDX) {
        TokenSlot *tok = &rt->token_pool[idx];
        if (tok->cond_idx >= 0) {
          const ConditionMeta *cond = stage3_get_condition(rt, tok->cond_idx);
          assert(cond != nullptr);
          const int8_t token_tag_id = cond->tag_id;
          assert(token_tag_id >= 0);
          assert(tag_slot(token_tag_id) < FEATURE_TAG_SLOT_COUNT);
          const TokenFeatureContrib contrib = token_feature_contrib(*tok);
          const uint16_t token_tag_mask = tag_mask(token_tag_id);
          if (task.touched_tag_mask & token_tag_mask) {
            apply_runtime_delta(runtime_states[tag_slot(token_tag_id)], contrib.token_count, contrib.exposure, contrib.exposure_entry);
          }
          if (task.touched_tag_mask & global_tag_mask) {
            apply_runtime_delta(runtime_states[tag_slot(-1)], contrib.token_count, contrib.exposure, contrib.exposure_entry);
          }
        }
        idx = tok->next;
      }
    }

    for (uint32_t pos = task.begin; pos < task.begin + task.count; ++pos) {
      const EventInput &evt = batch[event_order[pos]];
      const uint32_t user_idx = task.user_idx;
      const int64_t prev_pnl = user->total_realized_pnl + user->total_unrealized_pnl;
      const int64_t prev_global_exposure = runtime_states[tag_slot(-1)].exposure;
      int64_t realized_delta = 0;
      TokenSlot *tok = nullptr;
      int64_t old_mtm = 0;
      int64_t new_mtm = 0;
      int64_t pos_before = 0;
      TokenFeatureContrib after_contrib{};

      if (evt.cond_idx >= 0) {
        const size_t global_slot_idx = tag_slot(-1);
        const int32_t prev_global_latest_bucket = feature_latest_buckets[global_slot_idx];
        const uint16_t dense_feature_mask =
            static_cast<uint16_t>(materialized_feature_mask | tag_mask(evt.tag_id) | global_tag_mask);
        prepare_feature_buckets_for_mask(
            rt, user_idx, evt.bucket, dense_feature_mask, feature_first_buckets, feature_latest_buckets, feature_latest_indices);
        assert_dense_feature_slot(evt.tag_id);
        assert_dense_feature_slot(-1);
        materialized_feature_mask |= dense_feature_mask;
        const int32_t affected_global_bucket =
            (prev_global_latest_bucket >= 0 && prev_global_latest_bucket < evt.bucket)
                ? (prev_global_latest_bucket + 1)
                : evt.bucket;
        if (global_sharpe_recalc_start_bucket < 0 || affected_global_bucket < global_sharpe_recalc_start_bucket) {
          global_sharpe_recalc_start_bucket = affected_global_bucket;
        }

        tok = token_get_or_create(rt, user_idx, evt.cond_idx,
                                  static_cast<int16_t>(evt.token_idx),
                                  static_cast<int16_t>(evt.collateral));
        pos_before = tok->pos;
        const int64_t cost_before = tok->cost;
        const int64_t lp_before = tok->lp;
        if (lp_before > 0) {
          old_mtm = static_cast<int64_t>(
                        static_cast<long double>(pos_before) * static_cast<long double>(lp_before) / 1e6L) -
                    cost_before;
        }

        const TokenFeatureContrib before_contrib = token_feature_contrib(*tok);
        realized_delta = apply_trade_event(rt, evt, tok);
        after_contrib = token_feature_contrib(*tok);
        if (tok->lp > 0) {
          new_mtm = static_cast<int64_t>(
                        static_cast<long double>(tok->pos) * static_cast<long double>(tok->lp) / 1e6L) -
                    tok->cost;
        }

        const int64_t token_count_delta = after_contrib.token_count - before_contrib.token_count;
        const int64_t exposure_delta = after_contrib.exposure - before_contrib.exposure;
        const __int128 exposure_entry_delta = after_contrib.exposure_entry - before_contrib.exposure_entry;

        FeatureRuntimeState &tag_state = runtime_states[tag_slot(evt.tag_id)];
        FeatureRuntimeState &global_state = runtime_states[tag_slot(-1)];
        apply_runtime_delta(tag_state, token_count_delta, exposure_delta, exposure_entry_delta);
        apply_runtime_delta(global_state, token_count_delta, exposure_delta, exposure_entry_delta);
      }

      user->total_events++;
      user->total_realized_pnl += realized_delta;

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

      EventRecord rec{};
      rec.sort_key = evt.sort_key;
      rec.cond_idx = evt.cond_idx;
      rec.token_idx = static_cast<int16_t>(evt.token_idx);
      rec.event_type = static_cast<int8_t>(evt.event_type);
      rec.tag_id = evt.tag_id;
      rec.amount = evt.amount;
      rec.price_1e6 = evt.price_1e6;
      rec.collateral = static_cast<int16_t>(evt.collateral);
      rec.realized_delta = realized_delta;
      rec.realized_cum = user->total_realized_pnl;
      rec.unrealized_pnl = user->total_unrealized_pnl;
      rec.token_count = static_cast<int32_t>(user->token_count);

      if (evt.cond_idx >= 0 && tok) {
        rec.exposure = after_contrib.exposure;
        rec.volume = static_cast<int64_t>(
            std::abs(static_cast<double>(evt.amount) * static_cast<double>(evt.price_1e6) / 1e6));
        rec.holding_period = (tok->entry_block > 0) ? (evt.current_block - tok->entry_block) : 0;
      }

      events_log_write_at(rt, rec, user_idx, log_cursor);
      log_cursor += sizeof(EventRecord);

      if (evt.cond_idx >= 0) {
        update_feature_on_event(
            rt,
            user_idx,
            evt.current_block,
            evt.bucket,
            evt,
            rec,
            runtime_states[tag_slot(evt.tag_id)],
            runtime_states[tag_slot(-1)],
            feature_first_buckets,
            feature_latest_buckets,
            feature_latest_indices);
      }

      {
        const int64_t pnl = user->total_realized_pnl + user->total_unrealized_pnl;
        if (pnl != prev_pnl) {
          update_sharpe_on_event(
              rt,
              user_idx,
              runtime_states[tag_slot(-1)].exposure,
              prev_global_exposure,
              pnl,
              prev_pnl,
              evt.bucket,
              evt.block_offset);
        }
      }

      if (evt.cond_idx >= 0 && tok && tok->pos == 0) {
        token_remove_if_empty(rt, user_idx, tok);
      }
    }

    const int32_t global_first_bucket = feature_first_buckets[tag_slot(-1)];
    const int32_t global_latest_bucket = feature_latest_buckets[tag_slot(-1)];
    if (global_sharpe_recalc_start_bucket >= 0 && global_first_bucket >= 0) {
      for (int32_t bucket = global_sharpe_recalc_start_bucket; bucket <= global_latest_bucket; ++bucket) {
        FeatureSlot *feat = feature_find(rt, task.user_idx, bucket, -1);
        assert(feat != nullptr);
        calc_sharpe_for_feature(rt, task.user_idx, feat, global_first_bucket);
      }
    }
    assert(std::isfinite(static_cast<double>(user->total_unrealized_pnl)));
  };

  {
    TraceN("s3/sync_tick/batch/parallel");
    std::vector<uint32_t> active_shards;
    active_shards.reserve(STAGE3_SYNC_SHARD_COUNT);
    for (uint32_t shard = 0; shard < STAGE3_SYNC_SHARD_COUNT; ++shard) {
      if (!tasks_by_shard[shard].empty()) {
        active_shards.push_back(shard);
      }
    }

    if (!active_shards.empty()) {
      auto run_shard = [&](uint32_t shard) {
        uint64_t log_cursor = shard_log_begin[shard];
        for (uint32_t task_idx : tasks_by_shard[shard]) {
          process_user_task(user_tasks[task_idx], log_cursor);
        }
        assert(log_cursor == shard_log_begin[shard] + static_cast<uint64_t>(shard_event_counts[shard]) * sizeof(EventRecord));
      };

      const uint32_t hw_threads = std::max<uint32_t>(1, std::thread::hardware_concurrency());
      const size_t worker_count = std::min(active_shards.size(), static_cast<size_t>(hw_threads));
      auto run_worker = [&](size_t worker_idx) {
        for (size_t i = worker_idx; i < active_shards.size(); i += worker_count) {
          run_shard(active_shards[i]);
        }
      };

      std::vector<std::thread> workers;
      workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
      for (size_t worker_idx = 1; worker_idx < worker_count; ++worker_idx) {
        workers.emplace_back([&, worker_idx]() { run_worker(worker_idx); });
      }
      run_worker(0);
      for (auto &worker : workers) {
        worker.join();
      }
    }
  }

  rt->rank_cache.needs_rebuild = true;
  rt->header->head_bucket = std::max<int64_t>(rt->header->head_bucket, batch.back().bucket);
  rt->header->events_log_tail = reserved_log_tail;
  rt->header->cursor_sort_key = batch.back().sort_key;
  rt->header->cursor_processed_events += batch.size();

  return batch.size();
}

} // namespace stage3
