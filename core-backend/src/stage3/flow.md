# Stage3 Flow (mmap + pool)

文件: `store.bin`(~167GB) + `events.log`(~160GB) + `users.idx`(512MB)

```
stage3_bootstrap
├─ store.bin 不存在?
│  ├─ fallocate store.bin (Header 4KB + ConditionMeta 2GB + TokenPool 4.8GB + FeaturePool 140GB + SharpeAggPool 2.4GB + SharpeSamplePool 16GB + Users 1.28GB)
│  ├─ mmap store.bin MAP_SHARED
│  ├─ header.magic = "STAGE3\0\0", header.version = 1
│  ├─ header.cursor_sort_key = -1, header.cursor_processed_events = 0
│  ├─ header.user_count = 0, header.head_bucket = 0
│  ├─ header.*_pool_used = 0
│  ├─ header.token_free_head = NULL_IDX
│  ├─ header.feature_free_head = NULL_IDX
│  ├─ header.sharpe_agg_free_head = NULL_IDX
│  ├─ header.sharpe_sample_free_head = NULL_IDX
│  └─ header.events_log_tail = 0
├─ store.bin 存在?
│  ├─ mmap store.bin MAP_SHARED
│  └─ assert header.magic == "STAGE3\0\0"
├─ mmap events.log MAP_SHARED (可扩展, 初始 1GB, 需要时 mremap)
├─ mmap users.idx MAP_SHARED (16M slots × 32B = 512MB)
├─ 从 stage2 加载 condition_meta 到 conditions[] (100万 × 2056B, outcome_count + tag_id + payout_numerators[256])
└─ 返回 Stage3Runtime* {store, events_log, users_idx}

stage3_sync_tick(runtime, head_block, batch_limit)
├─ 0) cursor = header.cursor_sort_key
├─ 1) 拉取输入事件
│  ├─ scan_by_sort_key(start=cursor+1, end=head_block*1e9+(1e9-1), limit=batch_limit) 从 Stage2 RocksDB
│  ├─ 返回按 (sort_key, user_addr, cond_idx, event_type, token_idx) 排序
│  └─ 若无事件: header.cursor_sort_key = head_block*1e9+(1e9-1), return 0
├─ 2) 预处理
│  ├─ touched_users = {} 去重 set
│  ├─ for evt in batch: touched_users.insert(evt.user_addr)
│  ├─ for addr in touched_users:
│  │  ├─ user_idx = user_index_lookup(addr) 遍历 hash 链表
│  │  └─ if user_idx == NULL_IDX:
│  │     ├─ user_idx = header.user_count++
│  │     ├─ users[user_idx].addr = addr
│  │     ├─ users[user_idx].flags = 1 (occupied)
│  │     ├─ users[user_idx].total_events = 0
│  │     ├─ users[user_idx].total_realized_pnl = 0
│  │     ├─ users[user_idx].total_unrealized_pnl = 0
│  │     ├─ users[user_idx].last_sort_key = 0
│  │     ├─ users[user_idx].token_head = NULL_IDX
│  │     ├─ users[user_idx].token_count = 0
│  │     ├─ users[user_idx].feature_head = NULL_IDX
│  │     ├─ users[user_idx].feature_count = 0
│  │     ├─ users[user_idx].sharpe_agg_head = NULL_IDX
│  │     ├─ users[user_idx].sharpe_agg_count = 0
│  │     ├─ users[user_idx].timeline_head = NULL_LOG_OFFSET
│  │     ├─ users[user_idx].timeline_tail = NULL_LOG_OFFSET
│  │     ├─ users[user_idx].timeline_count = 0
│  │     └─ user_index_insert(addr, user_idx) 插入 hash 表
│  └─ dirty_users = {}
├─ 3) 回放循环 for evt in batch (按 sort_key 升序)
│  ├─ 3.1) user_idx = user_index_lookup(evt.user_addr)
│  ├─ 3.2) if evt.cond_idx >= 0: 获取/创建 TokenSlot
│  │  ├─ tok_idx = users[user_idx].token_head
│  │  ├─ while tok_idx != NULL_IDX:
│  │  │  ├─ if token_pool[tok_idx].cond_idx == evt.cond_idx && token_pool[tok_idx].token_idx == evt.token_idx: found
│  │  │  └─ tok_idx = token_pool[tok_idx].next
│  │  └─ if not found:
│  │     ├─ if header.token_free_head != NULL_IDX: tok_idx = header.token_free_head, header.token_free_head = token_pool[tok_idx].next
│  │     ├─ else: tok_idx = header.token_pool_used++
│  │     ├─ token_pool[tok_idx].user_idx = user_idx
│  │     ├─ token_pool[tok_idx].cond_idx = evt.cond_idx
│  │     ├─ token_pool[tok_idx].token_idx = evt.token_idx
│  │     ├─ token_pool[tok_idx].collateral = evt.collateral
│  │     ├─ token_pool[tok_idx].pos = 0, .cost = 0, .lp = 0, .entry_block = 0
│  │     ├─ token_pool[tok_idx].next = users[user_idx].token_head
│  │     └─ users[user_idx].token_head = tok_idx
│  ├─ 3.3) if evt.cond_idx >= 0: 应用交易规则
│  │  ├─ tok = &token_pool[tok_idx]
│  │  ├─ qty = abs(evt.amount), px = evt.price_1e6 / 1e6
│  │  ├─ has_usd = evt.collateral in {1,2,3,4}
│  │  ├─ pos_before = tok->pos, cost_before = tok->cost
│  │  ├─ current_block = evt.sort_key / 1e9
│  │  ├─ realized_delta = 0
│  │  ├─ if evt.amount > 0 (正向腿, 先平空再开多):
│  │  │  ├─ close_short_qty = min(qty, max(0, -pos_before))
│  │  │  ├─ open_long_qty = qty - close_short_qty
│  │  │  ├─ if has_usd && close_short_qty > 0:
│  │  │  │  ├─ cost_removed = cost_before * close_short_qty / (-pos_before)
│  │  │  │  ├─ if event_type in {Order*, FPMM*, Split*, Merge*, Redemption*}: realized_delta = close_short_qty * px - cost_removed
│  │  │  │  ├─ if event_type == Convert: realized_delta = close_short_qty * ((popcount-1)/popcount) - cost_removed
│  │  │  │  ├─ if event_type in {TransferIn*, TransferOut*, FPMMLPRemove, FPMMLPReturn}: realized_delta = 0
│  │  │  │  └─ tok->cost -= cost_removed
│  │  │  └─ if has_usd && open_long_qty > 0 && event_type in {Order*, FPMM*, Split*, Merge*, Redemption*, Convert}:
│  │  │     └─ tok->cost += open_long_qty * px
│  │  ├─ if evt.amount < 0 (反向腿, 先平多再开空):
│  │  │  ├─ close_long_qty = min(qty, max(0, pos_before))
│  │  │  ├─ open_short_qty = qty - close_long_qty
│  │  │  ├─ if has_usd && close_long_qty > 0:
│  │  │  │  ├─ cost_removed = cost_before * close_long_qty / pos_before
│  │  │  │  ├─ if event_type in {Order*, FPMM*, Split*, Merge*, Redemption*}: realized_delta = close_long_qty * px - cost_removed
│  │  │  │  ├─ if event_type == Convert: realized_delta = close_long_qty * ((popcount-1)/popcount) - cost_removed
│  │  │  │  ├─ if event_type in {TransferIn*, TransferOut*, FPMMLPRemove, FPMMLPReturn}: realized_delta = 0
│  │  │  │  └─ tok->cost -= cost_removed
│  │  │  └─ if has_usd && open_short_qty > 0 && event_type in {Order*, FPMM*, Split*, Merge*, Redemption*, Convert}:
│  │  │     └─ tok->cost -= open_short_qty * px
│  │  ├─ if event_type != FPMMLPAdd: tok->pos += evt.amount
│  │  ├─ if event_type in {Order*, FPMM*} && evt.price_1e6 > 0: tok->lp = evt.price_1e6
│  │  ├─ 更新 entry_block:
│  │  │  ├─ if pos_before == 0 && tok->pos != 0: tok->entry_block = current_block
│  │  │  ├─ if abs(tok->pos) > abs(pos_before): tok->entry_block = (abs(pos_before) * tok->entry_block + abs(tok->pos - pos_before) * current_block) / abs(tok->pos)
│  │  │  └─ if abs(tok->pos) <= abs(pos_before): entry_block 不变
│  │  └─ if tok->pos == 0:
│  │     ├─ 从 user.token_head 链表移除 tok_idx
│  │     ├─ tok->cond_idx = -1
│  │     ├─ tok->next = header.token_free_head
│  │     └─ header.token_free_head = tok_idx
│  ├─ 3.4) 更新 UserBlock
│  │  ├─ user = &users[user_idx]
│  │  ├─ user->total_events++
│  │  ├─ user->total_realized_pnl += realized_delta
│  │  ├─ if evt.cond_idx >= 0:
│  │  │  ├─ user->total_unrealized_pnl = 0
│  │  │  ├─ user->token_count = 0
│  │  │  ├─ idx = user->token_head
│  │  │  ├─ while idx != NULL_IDX:
│  │  │  │  ├─ t = &token_pool[idx]
│  │  │  │  ├─ if t->lp > 0: user->total_unrealized_pnl += t->pos * t->lp / 1e6 - t->cost
│  │  │  │  ├─ if abs(t->pos) >= 10e6: user->token_count++
│  │  │  │  └─ idx = t->next
│  │  └─ user->last_sort_key = evt.sort_key
│  ├─ 3.5) Append EventRecord -> events.log + 用户 timeline 单链表
│  │  ├─ rec.sort_key = evt.sort_key
│  │  ├─ rec.cond_idx = evt.cond_idx
│  │  ├─ rec.token_idx = evt.token_idx
│  │  ├─ rec.event_type = evt.event_type
│  │  ├─ rec.tag_id = (evt.cond_idx >= 0 ? conditions[evt.cond_idx].tag_id : -1)
│  │  ├─ rec.realized_delta = realized_delta
│  │  ├─ rec.realized_cum = user->total_realized_pnl
│  │  ├─ rec.unrealized_pnl = user->total_unrealized_pnl
│  │  ├─ rec.token_count = user->token_count
│  │  ├─ rec.exposure = (evt.cond_idx >= 0 ? abs(tok->pos * tok->lp) : 0)
│  │  ├─ rec.volume = abs(evt.amount * evt.price_1e6) / 1e6
│  │  ├─ rec.holding_period = (evt.cond_idx >= 0 ? current_block - tok->entry_block : 0)
│  │  ├─ rec.next_user_event_offset = NULL_LOG_OFFSET
│  │  ├─ rec_off = header.events_log_tail
│  │  ├─ memcpy(events_log + rec_off, &rec, sizeof(EventRecord))
│  │  ├─ if user->timeline_head == NULL_LOG_OFFSET: user->timeline_head = rec_off
│  │  ├─ else: ((EventRecord*)(events_log + user->timeline_tail))->next_user_event_offset = rec_off
│  │  ├─ user->timeline_tail = rec_off
│  │  ├─ user->timeline_count++
│  │  ├─ header.events_log_tail += sizeof(EventRecord)
│  ├─ 3.6) 更新 FeatureSlot (仅 evt.cond_idx >= 0 时更新行业 + 全局)
│  │  ├─ if evt.cond_idx < 0: skip
│  │  ├─ bucket = current_block / 100000
│  │  ├─ tag_id = conditions[evt.cond_idx].tag_id
│  │  ├─ for tag in {tag_id, -1}:
│  │  │  ├─ feat = find_feature(user_idx, bucket, tag)
│  │  │  ├─ if feat == null:
│  │  │  │  ├─ if header.feature_free_head != NULL_IDX: feat_idx = header.feature_free_head, header.feature_free_head = feature_pool[feat_idx].next
│  │  │  │  ├─ else: feat_idx = header.feature_pool_used++
│  │  │  │  ├─ feat = &feature_pool[feat_idx]
│  │  │  │  ├─ feat->user_idx = user_idx, feat->bucket = bucket, feat->tag_id = tag
│  │  │  │  ├─ 初始化所有 Node-A0/A/B/C/D 字段为 0
│  │  │  │  ├─ feat->next = users[user_idx].feature_head
│  │  │  │  └─ users[user_idx].feature_head = feat_idx, users[user_idx].feature_count++
│  │  │  ├─ // Node-A0 续算锚点更新
│  │  │  ├─ delta_blocks = current_block - feat->last_block_10w
│  │  │  ├─ if delta_blocks > 0:
│  │  │  │  ├─ feat->time_weight_sum_10w += delta_blocks
│  │  │  │  ├─ feat->token_count_tw_sum_10w += feat->last_token_count_10w * delta_blocks
│  │  │  │  ├─ feat->exposure_tw_sum_10w += feat->last_exposure_10w * delta_blocks
│  │  │  │  └─ feat->holding_period_exp_tw_sum_10w += feat->last_holding_period_10w * feat->last_exposure_10w * delta_blocks
│  │  │  ├─ feat->last_sort_key_10w = evt.sort_key
│  │  │  ├─ feat->last_block_10w = current_block
│  │  │  ├─ feat->last_exposure_10w = rec.exposure
│  │  │  ├─ feat->last_holding_period_10w = rec.holding_period
│  │  │  ├─ feat->last_token_count_10w = rec.token_count
│  │  │  ├─ // Node-A 原子统计
│  │  │  ├─ feat->volume_sum_10w += rec.volume
│  │  │  ├─ // Node-B 归一化 (bucket 结束时或查询时计算)
│  │  │  ├─ if feat->time_weight_sum_10w > 0:
│  │  │  │  ├─ feat->token_avg_10w = feat->token_count_tw_sum_10w / feat->time_weight_sum_10w
│  │  │  │  ├─ feat->exposure_avg_10w = feat->exposure_tw_sum_10w / feat->time_weight_sum_10w
│  │  │  │  ├─ feat->volume_10w = feat->volume_sum_10w
│  │  │  │  └─ feat->holding_period_avg_10w = feat->holding_period_exp_tw_sum_10w / feat->exposure_tw_sum_10w (if > 0)
│  │  │  ├─ // Node-C 前缀缓存 (需要前一个 bucket)
│  │  │  ├─ prev_feat = find_feature(user_idx, bucket-1, tag)
│  │  │  ├─ if prev_feat: feat->ps_* = prev_feat->ps_* + feat->*_avg_10w
│  │  │  ├─ else: feat->ps_* = feat->*_avg_10w
│  │  │  ├─ // Node-D 窗口投影 (100w = 10 bucket, 1000w = 100 bucket)
│  │  │  ├─ feat_100 = find_feature(user_idx, bucket-10, tag)
│  │  │  ├─ feat_1000 = find_feature(user_idx, bucket-100, tag)
│  │  │  ├─ feat->*_avg_100w = (feat->ps_* - (feat_100 ? feat_100->ps_* : 0)) / min(10, bucket+1)
│  │  │  ├─ feat->*_avg_1000w = (feat->ps_* - (feat_1000 ? feat_1000->ps_* : 0)) / min(100, bucket+1)
│  │  │  └─ feat->updated_sort_key = evt.sort_key
│  └─ 3.7) 更新 SharpeAgg + SharpeSample
│     ├─ agg = find_sharpe_agg(user_idx, bucket)
│     ├─ if agg == null:
│     │  ├─ if header.sharpe_agg_free_head != NULL_IDX: agg_idx = header.sharpe_agg_free_head, ...
│     │  ├─ else: agg_idx = header.sharpe_agg_pool_used++
│     │  ├─ agg = &sharpe_agg_pool[agg_idx]
│     │  ├─ agg->user_idx = user_idx, agg->bucket = bucket
│     │  ├─ agg->close_pnl = 0, agg->min_pnl = INT64_MAX, agg->max_pnl = INT64_MIN
│     │  ├─ agg->sample_head = NULL_IDX, agg->sample_count = 0, agg->last_block = -1
│     │  ├─ agg->next = users[user_idx].sharpe_agg_head
│     │  └─ users[user_idx].sharpe_agg_head = agg_idx, users[user_idx].sharpe_agg_count++
│     ├─ pnl = user->total_realized_pnl + user->total_unrealized_pnl
│     ├─ block_offset = current_block % 100000
│     ├─ if current_block != agg->last_block:
│     │  ├─ if header.sharpe_sample_free_head != NULL_IDX: sample_idx = header.sharpe_sample_free_head, ...
│     │  ├─ else: sample_idx = header.sharpe_sample_pool_used++
│     │  ├─ sample = &sharpe_sample_pool[sample_idx]
│     │  ├─ sample->agg_idx = agg_idx
│     │  ├─ sample->block_offset = block_offset
│     │  ├─ sample->pnl = pnl
│     │  ├─ sample->next = agg->sample_head
│     │  ├─ agg->sample_head = sample_idx
│     │  ├─ agg->sample_count++
│     │  └─ agg->last_block = current_block
│     ├─ else: 更新最后一个 sample 的 pnl = pnl
│     ├─ agg->close_pnl = pnl
│     ├─ agg->min_pnl = min(agg->min_pnl, pnl)
│     └─ agg->max_pnl = max(agg->max_pnl, pnl)
├─ 4) 批次收尾
│  ├─ for each dirty user: assert isfinite(total_unrealized_pnl)
│  ├─ header.cursor_sort_key = batch.back().sort_key
│  └─ header.cursor_processed_events += batch.size()
└─ 5) return batch.size()

stage3_query_status() -> {syncing, last_block, head_block, behind_blocks, blocks_per_second, eta_seconds, ready}
├─ last_block = header.cursor_sort_key / 1e9
├─ behind_blocks = head_block - last_block
└─ ready = behind_blocks < 1000

stage3_query_pnl(user_addr) -> {user, block, total_events, timeline[]}
├─ user_idx = user_index_lookup(user_addr)
├─ if user_idx == NULL_IDX: return 404
├─ user = &users[user_idx]
├─ cache = query_cache_get_or_create(user_addr)
├─ if cache->loaded_sort_key < user->last_sort_key:
│  ├─ cache->timeline.clear(), cache->snapshots.clear()
│  ├─ off = user->timeline_head
│  ├─ positions = {} 空 map
│  ├─ i = 0
│  ├─ while off != NULL_LOG_OFFSET:
│  │  ├─ rec = *(EventRecord*)(events_log + off)
│  │  ├─ cache->timeline.push_back(rec)
│  │  ├─ apply_event_to_positions(positions, rec) 重建 positions
│  │  ├─ if i % 100 == 0: cache->snapshots.push_back({i, positions.copy()})
│  │  ├─ off = rec.next_user_event_offset
│  │  └─ i++
│  └─ cache->loaded_sort_key = user->last_sort_key
└─ return {user_addr, user->last_sort_key/1e9, user->total_events, cache->timeline}

stage3_query_positions(user_addr, target_sort_key) -> {user, sort_key, block, positions[]}
├─ 确保 cache 已加载 (同 stage3_query_pnl)
├─ target_idx = lower_bound(cache->timeline, target_sort_key)
├─ snap_idx = 找最大的 i 使得 cache->snapshots[i].timeline_idx <= target_idx
├─ positions = cache->snapshots[snap_idx].positions.copy()
├─ for i in snap_idx .. target_idx: apply_event_to_positions(positions, cache->timeline[i])
└─ return {user_addr, target_sort_key, target_sort_key/1e9, positions}

stage3_query_filter(anchor_bucket, filters[], sort_expr, sort_asc, limit) -> {anchor_bucket, users[]}
├─ results = []
├─ for user_idx in 0..header.user_count:
│  ├─ if !(users[user_idx].flags & 1): continue
│  ├─ feat = find_feature(user_idx, anchor_bucket, filter_tag_id)
│  ├─ if feat == null: continue
│  ├─ if !eval_all_filters(feat, filters): continue
│  ├─ sort_value = eval_expr(feat, sort_expr)
│  └─ results.push_back({user_idx, sort_value})
├─ sort(results, by sort_value, asc=sort_asc)
├─ results.resize(min(results.size(), limit))
└─ return {anchor_bucket, [users[r.user_idx].addr for r in results]}

持久化: mmap MAP_SHARED, OS 自动 dirty page writeback; 关闭时 msync(MS_SYNC)
崩溃恢复: 从 header.cursor_sort_key 继续拉取事件
```
