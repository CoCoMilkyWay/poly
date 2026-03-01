#pragma once

#include "../core/database.hpp"
#include "../stage2/stage2_builder.hpp"
#include "../stage2/stage2_models.hpp"
#include "../stage2/stage2_types.hpp"
#include "../stage2/stage2_utils.hpp"
#include "misc/profiler.hpp"

#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <deque>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stage3 {

using namespace stage2;
namespace asio = boost::asio;

struct ReplayProgress {
  int64_t total_users = 0;
  int64_t processed_users = 0;
  bool running = false;
  double replay_ms = 0;
};

class StageSync {
public:
  struct SyncStatus {
    bool syncing = false;
    int64_t last_block = 0;
    int64_t head_block = 0;
    int64_t behind_blocks = 0;
    int64_t behind_chunks = 0;
    double blocks_per_second = 0.0;
    double eta_seconds = -1.0;
    int64_t processed_events = 0;
    int64_t stage3_sort_key = -1;
  };

  struct RebuildProgress {
    int phase = 0;
    bool running = false;
    int64_t total_users = 0;
    int64_t total_events = 0;

    ConditionTree cond_tree;
    TokenTree token_tree;

    int64_t xfer_total = 0;
    int64_t xfer_split_normal = 0;
    int64_t xfer_split_negrisk = 0;
    int64_t xfer_split_non_poly = 0;
    int64_t xfer_merge_normal = 0;
    int64_t xfer_merge_negrisk = 0;
    int64_t xfer_merge_non_poly = 0;
    int64_t xfer_redemption = 0;
    int64_t xfer_redemption_non_poly = 0;
    int64_t xfer_convert = 0;
    int64_t xfer_order_buy = 0;
    int64_t xfer_order_sell = 0;
    int64_t xfer_fpmm_buy = 0;
    int64_t xfer_fpmm_sell = 0;
    int64_t xfer_lp_add = 0;
    int64_t xfer_lp_remove = 0;
    int64_t xfer_lp_return = 0;
    int64_t xfer_transfer_in_negrisk = 0;
    int64_t xfer_transfer_in_other = 0;
    int64_t xfer_transfer_in_non_poly = 0;
    int64_t xfer_transfer_out_negrisk = 0;
    int64_t xfer_transfer_out_other = 0;
    int64_t xfer_transfer_out_non_poly = 0;
    int64_t xfer_internal_mint_negrisk = 0;
    int64_t xfer_internal_mint_fpmm = 0;
    int64_t xfer_internal_burn_negrisk = 0;
    int64_t xfer_internal_burn_fpmm = 0;
    int64_t xfer_internal_burn_convert = 0;
    int64_t xfer_internal_transfer_zero = 0;
    int64_t xfer_internal_transfer_order = 0;
    int64_t xfer_internal_transfer_negrisk = 0;
    int64_t xfer_internal_transfer_fpmm = 0;
    int64_t xfer_internal_transfer_other = 0;

    SplitSemanticTree split_sem_tree;
    MergeSemanticTree merge_sem_tree;
    ConvertSemanticTree convert_sem_tree;
    OrderSemanticTree order_sem_tree;

    std::unordered_map<uint16_t, int64_t> event_by_collateral;
    SyncStatus stage3_sync;
  };

  struct UserSummary {
    std::string addr;
    int64_t event_count = 0;
    int64_t realized_pnl = 0;
  };

  struct TimelineEntry {
    int64_t sort_key = 0;
    uint8_t event_type = 0;
    int64_t realized_pnl = 0;
    int64_t delta = 0;
    int64_t price = 0;
    uint32_t cond_idx = 0;
    uint8_t token_idx = 0;
    int token_count = 0;
  };

  struct PositionAtTime {
    std::string condition_id;
    int64_t positions[MAX_OUTCOMES]{};
    int64_t cost_basis = 0;
    int64_t realized_pnl = 0;
    int outcome_count = 0;
  };

  struct TradeEntry {
    int64_t sort_key = 0;
    uint8_t event_type = 0;
    int64_t delta = 0;
    int64_t price = 0;
    uint32_t cond_idx = 0;
    uint8_t token_idx = 0;
  };

  StageSync(EventBuilder &builder, Database &stage2_db)
      : builder_(builder), stage2_db_(stage2_db) {
    init_schema();
    load_conditions();
    load_cursor();
    refresh_sync_status(true);
  }

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    stop_requested_ = false;
    schedule_sync(1);
  }

  void stop() { stop_requested_ = true; }

  void rebuild_all() {
    std::lock_guard<std::mutex> lock(sync_mu_);
    while (process_chunk_locked()) {
    }
    refresh_sync_status_locked(true);
  }

  const BuildProgress &build_progress() const { return builder_.progress(); }
  const ReplayProgress &replay_progress() const { return replay_progress_; }

  SyncStatus status() const {
    std::lock_guard<std::mutex> lock(sync_mu_);
    return sync_;
  }

  SyncStatus sync_status() const { return status(); }

  RebuildProgress progress() const {
    const auto &wp = builder_.progress();
    const auto &bp = builder_.committed_progress();
    RebuildProgress p;
    p.phase = wp.phase;
    p.running = wp.running;
    p.total_users = bp.total_users;
    p.total_events = bp.total_events;
    p.cond_tree = bp.cond_tree;
    p.token_tree = bp.token_tree;
    p.xfer_total = bp.xfer_stats.total;
    p.xfer_split_normal = bp.xfer_stats.split_normal;
    p.xfer_split_negrisk = bp.xfer_stats.split_negrisk;
    p.xfer_split_non_poly = bp.xfer_stats.split_non_poly;
    p.xfer_merge_normal = bp.xfer_stats.merge_normal;
    p.xfer_merge_negrisk = bp.xfer_stats.merge_negrisk;
    p.xfer_merge_non_poly = bp.xfer_stats.merge_non_poly;
    p.xfer_redemption = bp.xfer_stats.redemption;
    p.xfer_redemption_non_poly = bp.xfer_stats.redemption_non_poly;
    p.xfer_convert = bp.xfer_stats.convert;
    p.xfer_order_buy = bp.xfer_stats.order_buy;
    p.xfer_order_sell = bp.xfer_stats.order_sell;
    p.xfer_fpmm_buy = bp.xfer_stats.fpmm_buy;
    p.xfer_fpmm_sell = bp.xfer_stats.fpmm_sell;
    p.xfer_lp_add = bp.xfer_stats.fpmm_lp_add;
    p.xfer_lp_remove = bp.xfer_stats.fpmm_lp_remove;
    p.xfer_lp_return = bp.xfer_stats.fpmm_lp_return;
    p.xfer_transfer_in_negrisk = bp.xfer_stats.transfer_in_negrisk;
    p.xfer_transfer_in_other = bp.xfer_stats.transfer_in_other;
    p.xfer_transfer_in_non_poly = bp.xfer_stats.transfer_in_non_poly;
    p.xfer_transfer_out_negrisk = bp.xfer_stats.transfer_out_negrisk;
    p.xfer_transfer_out_other = bp.xfer_stats.transfer_out_other;
    p.xfer_transfer_out_non_poly = bp.xfer_stats.transfer_out_non_poly;
    p.xfer_internal_mint_negrisk = bp.xfer_stats.internal_mint_negrisk;
    p.xfer_internal_mint_fpmm = bp.xfer_stats.internal_mint_fpmm;
    p.xfer_internal_burn_negrisk = bp.xfer_stats.internal_burn_negrisk;
    p.xfer_internal_burn_fpmm = bp.xfer_stats.internal_burn_fpmm;
    p.xfer_internal_burn_convert = bp.xfer_stats.internal_burn_convert;
    p.xfer_internal_transfer_zero = bp.xfer_stats.internal_transfer_zero;
    p.xfer_internal_transfer_order = bp.xfer_stats.internal_transfer_order;
    p.xfer_internal_transfer_negrisk = bp.xfer_stats.internal_transfer_negrisk;
    p.xfer_internal_transfer_fpmm = bp.xfer_stats.internal_transfer_fpmm;
    p.xfer_internal_transfer_other = bp.xfer_stats.internal_transfer_other;
    p.split_sem_tree = bp.split_sem_tree;
    p.merge_sem_tree = bp.merge_sem_tree;
    p.convert_sem_tree = bp.convert_sem_tree;
    p.order_sem_tree = bp.order_sem_tree;
    p.event_by_collateral = bp.event_by_collateral;
    p.stage3_sync = status();
    if (p.stage3_sync.behind_blocks == 0 && p.total_users > 0) {
      p.phase = 7;
    }
    return p;
  }

  const UserState *get_user_state(const std::string &addr) const {
    std::string lower = normalize_addr(addr);
    if (lower.empty()) {
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(cache_mu_);
    auto it = user_state_cache_.find(lower);
    if (it != user_state_cache_.end()) {
      return &it->second;
    }
    auto loaded = load_user_state_locked(lower);
    if (!loaded.has_value()) {
      return nullptr;
    }
    auto [ins_it, ok] = user_state_cache_.emplace(lower, std::move(*loaded));
    assert(ok);
    return &ins_it->second;
  }

  const UserState *find_user(const std::string &addr) const { return get_user_state(addr); }

  const ConditionInfo &get_condition(uint32_t idx) const {
    assert(idx < conditions_.size());
    return conditions_[idx];
  }

  const std::string &get_condition_id(uint32_t idx) const {
    assert(idx < cond_ids_.size());
    return cond_ids_[idx];
  }

  const std::vector<ConditionInfo> &conditions() const { return conditions_; }
  const std::vector<std::string> &condition_ids() const { return cond_ids_; }

  const std::vector<std::string> &users() const { return empty_users_; }
  const std::vector<UserState> &user_states() const { return empty_user_states_; }

  std::vector<UserSummary> get_users_sorted(int64_t limit = 200) const {
    TraceN("s3/users");
    auto conn = stage2_db_.create_connection();
    int64_t safe_limit = std::max<int64_t>(1, limit);
    auto r = conn->Query(
        "SELECT lower(hex(user_addr)) AS user_hex, total_events, total_realized_pnl "
        "FROM s3_user_summary "
        "ORDER BY total_events DESC "
        "LIMIT " +
        std::to_string(safe_limit));
    assert(r && !r->HasError());
    std::vector<UserSummary> out;
    out.reserve(static_cast<size_t>(r->RowCount()));
    for (idx_t i = 0; i < r->RowCount(); ++i) {
      std::string hx = r->GetValue(0, i).GetValueUnsafe<std::string>();
      out.push_back({"0x" + hx,
                     r->GetValue(1, i).GetValue<int64_t>(),
                     r->GetValue(2, i).GetValue<int64_t>()});
    }
    return out;
  }

  std::vector<TimelineEntry> get_user_timeline(const std::string &addr) const {
    TraceN("s3/timeline");
    std::string lower = normalize_addr(addr);
    if (lower.empty()) {
      return {};
    }
    auto events = load_user_events(lower);
    if (events.empty()) {
      return {};
    }
    return replay_timeline(events);
  }

  std::vector<PositionAtTime> get_positions_at(const std::string &addr, int64_t sort_key) const {
    TraceN("s3/positions");
    auto timeline = get_user_timeline(addr);
    if (timeline.empty()) {
      return {};
    }
    std::string lower = normalize_addr(addr);
    auto events = load_user_events(lower);
    auto state = replay_until(events, sort_key);
    std::vector<PositionAtTime> out;
    out.reserve(state.size());
    for (const auto &[cond_idx, st] : state) {
      if (cond_idx < 0) {
        continue;
      }
      uint32_t uidx = static_cast<uint32_t>(cond_idx);
      assert(uidx < conditions_.size());
      const auto &cond = conditions_[uidx];
      bool has_pos = false;
      for (int i = 0; i < cond.outcome_count; ++i) {
        if (st.positions[i] != 0) {
          has_pos = true;
          break;
        }
      }
      if (!has_pos && st.realized_pnl == 0) {
        continue;
      }
      PositionAtTime p;
      p.condition_id = cond_ids_[uidx];
      std::memcpy(p.positions, st.positions.data(), sizeof(p.positions));
      p.outcome_count = cond.outcome_count;
      p.realized_pnl = st.realized_pnl;
      p.cost_basis = 0;
      for (int i = 0; i < cond.outcome_count; ++i) {
        p.cost_basis += st.cost[i];
      }
      out.push_back(std::move(p));
    }
    return out;
  }

  std::vector<TradeEntry> get_trades_near(const std::string &addr, int64_t sort_key, int radius = 20) const {
    TraceN("s3/trades");
    auto timeline = get_user_timeline(addr);
    if (timeline.empty()) {
      return {};
    }
    auto it = std::lower_bound(
        timeline.begin(), timeline.end(), sort_key,
        [](const TimelineEntry &e, int64_t sk) { return e.sort_key < sk; });
    size_t center = (it == timeline.end())
                        ? timeline.size() - 1
                        : static_cast<size_t>(it - timeline.begin());
    size_t start = (center > static_cast<size_t>(radius)) ? center - radius : 0;
    size_t end = std::min(center + static_cast<size_t>(radius) + 1, timeline.size());
    std::vector<TradeEntry> out;
    out.reserve(end - start);
    for (size_t i = start; i < end; ++i) {
      const auto &e = timeline[i];
      out.push_back({e.sort_key, e.event_type, e.delta, e.price, e.cond_idx, e.token_idx});
    }
    return out;
  }

  size_t get_trades_center_index(const std::string &addr, int64_t sort_key, int radius = 20) const {
    auto timeline = get_user_timeline(addr);
    if (timeline.empty()) {
      return 0;
    }
    auto it = std::lower_bound(
        timeline.begin(), timeline.end(), sort_key,
        [](const TimelineEntry &e, int64_t sk) { return e.sort_key < sk; });
    size_t center = (it == timeline.end())
                        ? timeline.size() - 1
                        : static_cast<size_t>(it - timeline.begin());
    size_t start = (center > static_cast<size_t>(radius)) ? center - radius : 0;
    return center - start;
  }

private:
  struct CursorKey {
    int64_t sort_key = -1;
    std::string user_hex;
    int32_t cond_idx = std::numeric_limits<int32_t>::min();
    int32_t event_type = std::numeric_limits<int32_t>::min();
    int32_t token_idx = std::numeric_limits<int32_t>::min();
    int64_t processed_events = 0;
  };

  struct ReplayRow {
    std::string user_hex;
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t event_type = 0;
    int32_t token_idx = 0;
    int64_t amount = 0;
    int64_t price = 0;
  };

  struct CondState {
    std::array<int64_t, MAX_OUTCOMES> positions{};
    std::array<int64_t, MAX_OUTCOMES> cost{};
    int64_t realized_pnl = 0;
    int64_t event_count = 0;
    int64_t last_sort_key = 0;
  };

  struct PairKey {
    std::string user_hex;
    int32_t cond_idx = 0;
    bool operator==(const PairKey &o) const {
      return cond_idx == o.cond_idx && user_hex == o.user_hex;
    }
  };

  struct PairKeyHash {
    size_t operator()(const PairKey &k) const {
      size_t h = std::hash<std::string>()(k.user_hex);
      h ^= std::hash<int32_t>()(k.cond_idx) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  EventBuilder &builder_;
  Database &stage2_db_;

  mutable std::mutex sync_mu_;
  mutable std::mutex cache_mu_;
  mutable ReplayProgress replay_progress_;
  mutable SyncStatus sync_;
  mutable CursorKey cursor_;
  struct CommitRecord {
    std::chrono::steady_clock::time_point committed_at;
    int64_t block = 0;
  };
  mutable std::deque<CommitRecord> commit_history_;

  asio::io_context *ioc_ = nullptr;
  std::shared_ptr<asio::steady_timer> timer_;
  std::atomic<bool> stop_requested_{false};

  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<int32_t, int32_t> cond_question_count_;

  mutable std::unordered_map<std::string, UserState> user_state_cache_;
  std::vector<std::string> empty_users_;
  std::vector<UserState> empty_user_states_;

  static constexpr int64_t kSyncChunkBlocks = 100000;
  static constexpr size_t kEtaWindowSize = 20;
  static constexpr int kBaseIntervalSeconds = 30;
  static constexpr int64_t kCursorSentinel = std::numeric_limits<int32_t>::min();

  static std::string normalize_addr(const std::string &addr) {
    std::string lower = to_lower(addr);
    if (lower.rfind("0x", 0) != 0 || lower.size() != 42) {
      return {};
    }
    return lower;
  }

  static int64_t mul_div_1e6(int64_t amount, int64_t price) {
    __int128 v = static_cast<__int128>(amount) * static_cast<__int128>(price);
    v /= 1000000;
    assert(v >= std::numeric_limits<int64_t>::min() && v <= std::numeric_limits<int64_t>::max());
    return static_cast<int64_t>(v);
  }

  static bool has_position(const CondState &st, uint8_t outcome_count) {
    for (int i = 0; i < outcome_count; ++i) {
      if (st.positions[i] != 0) {
        return true;
      }
    }
    return false;
  }

  void init_schema() const {
    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_sync_cursor (
        id INTEGER PRIMARY KEY,
        sort_key BIGINT NOT NULL,
        user_addr BLOB NOT NULL,
        cond_idx INTEGER NOT NULL,
        event_type INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        processed_events BIGINT NOT NULL
      )
    )");
    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_cond_state (
        user_addr BLOB NOT NULL,
        cond_idx INTEGER NOT NULL,
        pos_0 BIGINT NOT NULL,
        pos_1 BIGINT NOT NULL,
        pos_2 BIGINT NOT NULL,
        pos_3 BIGINT NOT NULL,
        pos_4 BIGINT NOT NULL,
        pos_5 BIGINT NOT NULL,
        pos_6 BIGINT NOT NULL,
        pos_7 BIGINT NOT NULL,
        cost_0 BIGINT NOT NULL,
        cost_1 BIGINT NOT NULL,
        cost_2 BIGINT NOT NULL,
        cost_3 BIGINT NOT NULL,
        cost_4 BIGINT NOT NULL,
        cost_5 BIGINT NOT NULL,
        cost_6 BIGINT NOT NULL,
        cost_7 BIGINT NOT NULL,
        realized_pnl BIGINT NOT NULL,
        event_count BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL,
        PRIMARY KEY (user_addr, cond_idx)
      )
    )");
    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_summary (
        user_addr BLOB PRIMARY KEY,
        total_events BIGINT NOT NULL,
        total_realized_pnl BIGINT NOT NULL,
        active_conditions BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL
      )
    )");
    auto conn = stage2_db_.create_connection();
    auto r = conn->Query("SELECT COUNT(*) FROM s3_sync_cursor WHERE id=1");
    assert(r && !r->HasError());
    if (r->GetValue(0, 0).GetValue<int64_t>() == 0) {
      auto ins = conn->Query(
          "INSERT INTO s3_sync_cursor VALUES (1, -1, from_hex(''), " +
          std::to_string(kCursorSentinel) + ", " +
          std::to_string(kCursorSentinel) + ", " +
          std::to_string(kCursorSentinel) + ", 0)");
      assert(ins && !ins->HasError());
    }
  }

  void load_conditions() {
    conditions_.clear();
    cond_ids_.clear();
    cond_question_count_.clear();

    auto conn = stage2_db_.create_connection();
    auto rc = conn->Query(
        "SELECT cond_idx, lower(hex(cond_id)) AS cond_hex, outcome_cnt, "
        "payout_0, payout_1, payout_2, payout_3, payout_4, payout_5, payout_6, payout_7, "
        "CASE WHEN question_id IS NULL THEN '' ELSE lower(hex(question_id)) END AS qid, source "
        "FROM rb_condition ORDER BY cond_idx");
    assert(rc && !rc->HasError());
    conditions_.resize(static_cast<size_t>(rc->RowCount()));
    cond_ids_.resize(static_cast<size_t>(rc->RowCount()));
    for (idx_t i = 0; i < rc->RowCount(); ++i) {
      uint32_t idx = rc->GetValue(0, i).GetValue<uint32_t>();
      assert(idx == i);
      cond_ids_[idx] = "0x" + rc->GetValue(1, i).GetValueUnsafe<std::string>();
      ConditionInfo info;
      info.outcome_count = rc->GetValue(2, i).GetValue<uint8_t>();
      info.payout_numerators.reserve(info.outcome_count);
      for (int j = 0; j < info.outcome_count; ++j) {
        auto v = rc->GetValue(3 + j, i);
        info.payout_numerators.push_back(v.IsNull() ? -1 : v.GetValue<int64_t>());
      }
      std::string qid = rc->GetValue(11, i).GetValueUnsafe<std::string>();
      if (!qid.empty()) {
        info.question_id = "0x" + qid;
      }
      info.source = static_cast<ConditionSource>(rc->GetValue(12, i).GetValue<int32_t>());
      conditions_[idx] = std::move(info);
    }

    std::unordered_map<std::string, int32_t> market_qcnt;
    auto mq = conn->Query(
        "SELECT lower(hex(market_id)) AS mid, COUNT(*) AS c "
        "FROM rb_neg_risk_market GROUP BY 1");
    assert(mq && !mq->HasError());
    for (idx_t i = 0; i < mq->RowCount(); ++i) {
      market_qcnt.emplace("0x" + mq->GetValue(0, i).GetValueUnsafe<std::string>(),
                          mq->GetValue(1, i).GetValue<int32_t>());
    }

    auto cq = conn->Query(
        "SELECT rc.cond_idx, lower(hex(nm.market_id)) AS mid "
        "FROM rb_condition rc "
        "JOIN rb_neg_risk_market nm "
        "ON rc.question_id = nm.question_id");
    assert(cq && !cq->HasError());
    for (idx_t i = 0; i < cq->RowCount(); ++i) {
      int32_t cond_idx = cq->GetValue(0, i).GetValue<int32_t>();
      std::string mid = "0x" + cq->GetValue(1, i).GetValueUnsafe<std::string>();
      auto it = market_qcnt.find(mid);
      if (it != market_qcnt.end()) {
        cond_question_count_[cond_idx] = it->second;
      }
    }
  }

  void load_cursor() {
    auto conn = stage2_db_.create_connection();
    auto r = conn->Query(
        "SELECT sort_key, lower(hex(user_addr)), cond_idx, event_type, token_idx, processed_events "
        "FROM s3_sync_cursor WHERE id=1");
    assert(r && !r->HasError() && r->RowCount() == 1);
    cursor_.sort_key = r->GetValue(0, 0).GetValue<int64_t>();
    cursor_.user_hex = r->GetValue(1, 0).GetValueUnsafe<std::string>();
    cursor_.cond_idx = r->GetValue(2, 0).GetValue<int32_t>();
    cursor_.event_type = r->GetValue(3, 0).GetValue<int32_t>();
    cursor_.token_idx = r->GetValue(4, 0).GetValue<int32_t>();
    cursor_.processed_events = r->GetValue(5, 0).GetValue<int64_t>();
    if (cursor_.user_hex.empty()) {
      cursor_.user_hex = "";
    }
  }

  void save_cursor_locked(duckdb::Connection &conn) const {
    std::string user_hex = cursor_.user_hex;
    auto q = conn.Query(
        "UPDATE s3_sync_cursor SET "
        "sort_key=" + std::to_string(cursor_.sort_key) +
        ", user_addr=from_hex('" + user_hex + "')" +
        ", cond_idx=" + std::to_string(cursor_.cond_idx) +
        ", event_type=" + std::to_string(cursor_.event_type) +
        ", token_idx=" + std::to_string(cursor_.token_idx) +
        ", processed_events=" + std::to_string(cursor_.processed_events) +
        " WHERE id=1");
    assert(q && !q->HasError());
  }

  void refresh_sync_status_locked(bool with_event_count) const {
    sync_.syncing = replay_progress_.running;
    sync_.head_block = builder_.cursor();
    sync_.stage3_sort_key = cursor_.sort_key;
    sync_.last_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
    sync_.behind_blocks = std::max<int64_t>(0, sync_.head_block - sync_.last_block);
    sync_.behind_chunks = (sync_.behind_blocks + kSyncChunkBlocks - 1) / kSyncChunkBlocks;
    sync_.processed_events = cursor_.processed_events;

    if (!with_event_count) {
      return;
    }
  }

  void refresh_sync_status(bool with_event_count) const {
    std::lock_guard<std::mutex> lock(sync_mu_);
    refresh_sync_status_locked(with_event_count);
  }

  void schedule_sync(int delay_seconds) {
    if (ioc_ == nullptr || stop_requested_) {
      return;
    }
    timer_ = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
    timer_->async_wait([this](const boost::system::error_code &ec) {
      if (ec || stop_requested_) {
        return;
      }
      do_sync_tick();
    });
  }

  void do_sync_tick() {
    TraceN("s3/sync");
    {
      std::lock_guard<std::mutex> lock(sync_mu_);
      replay_progress_.running = true;
      int64_t before_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
      bool advanced = process_chunk_locked();
      refresh_sync_status_locked(false);
      int64_t after_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
      if (advanced && after_block > before_block) {
        commit_history_.push_back({std::chrono::steady_clock::now(), after_block});
        if (commit_history_.size() > kEtaWindowSize) {
          commit_history_.pop_front();
        }
      } else {
        sync_.blocks_per_second = 0.0;
        sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0 : -1.0;
      }
      refresh_speed_locked();
      replay_progress_.running = false;
      int next_delay = (sync_.behind_chunks > 1) ? 0 : kBaseIntervalSeconds;
      schedule_sync(next_delay);
    }
  }

  void refresh_speed_locked() const {
    if (commit_history_.size() < 2) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    const auto &first = commit_history_.front();
    const auto &last = commit_history_.back();
    double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
    if (elapsed_s <= 0.0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = -1.0;
      return;
    }
    int64_t committed_blocks = std::max<int64_t>(0, last.block - first.block);
    if (committed_blocks == 0) {
      sync_.blocks_per_second = 0.0;
      sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0 : -1.0;
      return;
    }
    sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
    sync_.eta_seconds =
        (sync_.behind_blocks == 0) ? 0.0 : static_cast<double>(sync_.behind_blocks) / sync_.blocks_per_second;
  }

  bool process_chunk_locked() const {
    TraceN("s3/sync_chunk");
    int64_t current_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
    int64_t head_block = builder_.cursor();
    if (current_block >= head_block) {
      return false;
    }
    int64_t target_block = std::min(current_block + kSyncChunkBlocks, head_block);
    int64_t upper_sort_key = target_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

    auto conn = stage2_db_.create_connection();
    std::string user_hex = cursor_.user_hex;
    auto qr = conn->Query(
        "SELECT lower(hex(user_addr)) AS user_hex, sort_key, cond_idx, event_type, token_idx, amount, price "
        "FROM user_event WHERE ("
        "(sort_key > " + std::to_string(cursor_.sort_key) + ") OR "
        "(sort_key = " + std::to_string(cursor_.sort_key) + " AND user_addr > from_hex('" + user_hex + "')) OR "
        "(sort_key = " + std::to_string(cursor_.sort_key) + " AND user_addr = from_hex('" + user_hex + "') "
        "AND cond_idx > " + std::to_string(cursor_.cond_idx) + ") OR "
        "(sort_key = " + std::to_string(cursor_.sort_key) + " AND user_addr = from_hex('" + user_hex + "') "
        "AND cond_idx = " + std::to_string(cursor_.cond_idx) + " AND event_type > " + std::to_string(cursor_.event_type) + ") OR "
        "(sort_key = " + std::to_string(cursor_.sort_key) + " AND user_addr = from_hex('" + user_hex + "') "
        "AND cond_idx = " + std::to_string(cursor_.cond_idx) + " AND event_type = " + std::to_string(cursor_.event_type) +
        " AND token_idx > " + std::to_string(cursor_.token_idx) + ")) "
        "AND sort_key <= " + std::to_string(upper_sort_key) + " "
        "ORDER BY sort_key, user_addr, cond_idx, event_type, token_idx");
    assert(qr && !qr->HasError());
    if (qr->RowCount() == 0) {
      cursor_.sort_key = upper_sort_key;
      cursor_.user_hex.clear();
      cursor_.cond_idx = kCursorSentinel;
      cursor_.event_type = kCursorSentinel;
      cursor_.token_idx = kCursorSentinel;
      auto tx = conn->Query("BEGIN");
      assert(tx && !tx->HasError());
      save_cursor_locked(*conn);
      auto cm = conn->Query("COMMIT");
      assert(cm && !cm->HasError());
      return true;
    }

    std::vector<ReplayRow> rows;
    rows.reserve(static_cast<size_t>(qr->RowCount()));
    std::unordered_set<std::string> touched_users;
    touched_users.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));

    std::unordered_map<PairKey, CondState, PairKeyHash> states;
    states.reserve(static_cast<size_t>(qr->RowCount() / 2 + 1));
    std::unordered_map<std::string, int64_t> user_event_inc;
    user_event_inc.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));
    std::unordered_map<std::string, int64_t> user_last_sk;
    user_last_sk.reserve(static_cast<size_t>(qr->RowCount() / 10 + 1));

    for (idx_t i = 0; i < qr->RowCount(); ++i) {
      ReplayRow row;
      row.user_hex = qr->GetValue(0, i).GetValueUnsafe<std::string>();
      row.sort_key = qr->GetValue(1, i).GetValue<int64_t>();
      row.cond_idx = qr->GetValue(2, i).GetValue<int32_t>();
      row.event_type = qr->GetValue(3, i).GetValue<int32_t>();
      row.token_idx = qr->GetValue(4, i).GetValue<int32_t>();
      row.amount = qr->GetValue(5, i).GetValue<int64_t>();
      row.price = qr->GetValue(6, i).GetValue<int64_t>();
      rows.push_back(row);
      touched_users.insert(row.user_hex);
      user_event_inc[row.user_hex]++;
      user_last_sk[row.user_hex] = row.sort_key;
      if (row.cond_idx >= 0) {
        PairKey key{row.user_hex, row.cond_idx};
        if (states.find(key) == states.end()) {
          states.emplace(key, CondState{});
        }
      }
      cursor_.sort_key = row.sort_key;
      cursor_.user_hex = row.user_hex;
      cursor_.cond_idx = row.cond_idx;
      cursor_.event_type = row.event_type;
      cursor_.token_idx = row.token_idx;
      cursor_.processed_events++;
    }

    int32_t max_cond_idx = -1;
    for (const auto &row : rows) {
      if (row.cond_idx > max_cond_idx) {
        max_cond_idx = row.cond_idx;
      }
    }
    if (max_cond_idx >= 0 && static_cast<size_t>(max_cond_idx) >= conditions_.size()) {
      const_cast<StageSync *>(this)->load_conditions();
    }

    if (!states.empty()) {
      conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_s3_keys (user_addr BLOB, cond_idx INTEGER)");
      conn->Query("DELETE FROM tmp_s3_keys");
      {
        duckdb::Appender ap(*conn, "tmp_s3_keys");
        for (const auto &[key, _] : states) {
          std::string user_blob = hex_to_blob("0x" + key.user_hex);
          ap.BeginRow();
          ap.Append(duckdb::Value::BLOB(
              reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
              user_blob.size()));
          ap.Append(key.cond_idx);
          ap.EndRow();
        }
        ap.Close();
      }
      auto old = conn->Query(
          "SELECT lower(hex(s.user_addr)) AS uh, s.cond_idx, "
          "s.pos_0,s.pos_1,s.pos_2,s.pos_3,s.pos_4,s.pos_5,s.pos_6,s.pos_7, "
          "s.cost_0,s.cost_1,s.cost_2,s.cost_3,s.cost_4,s.cost_5,s.cost_6,s.cost_7, "
          "s.realized_pnl, s.event_count, s.last_sort_key "
          "FROM s3_user_cond_state s "
          "JOIN tmp_s3_keys k "
          "ON s.user_addr = k.user_addr AND s.cond_idx = k.cond_idx");
      assert(old && !old->HasError());
      for (idx_t i = 0; i < old->RowCount(); ++i) {
        PairKey key{old->GetValue(0, i).GetValueUnsafe<std::string>(),
                    old->GetValue(1, i).GetValue<int32_t>()};
        auto it = states.find(key);
        assert(it != states.end());
        CondState &st = it->second;
        for (int j = 0; j < MAX_OUTCOMES; ++j) {
          st.positions[j] = old->GetValue(2 + j, i).GetValue<int64_t>();
          st.cost[j] = old->GetValue(10 + j, i).GetValue<int64_t>();
        }
        st.realized_pnl = old->GetValue(18, i).GetValue<int64_t>();
        st.event_count = old->GetValue(19, i).GetValue<int64_t>();
        st.last_sort_key = old->GetValue(20, i).GetValue<int64_t>();
      }
    }

    for (const auto &row : rows) {
      if (row.cond_idx < 0) {
        continue;
      }
      PairKey key{row.user_hex, row.cond_idx};
      auto it = states.find(key);
      assert(it != states.end());
      apply_event_to_state(row, it->second);
      it->second.event_count++;
      it->second.last_sort_key = row.sort_key;
    }

    {
      TraceN("s3/write");
      auto tx = conn->Query("BEGIN");
      assert(tx && !tx->HasError());

      if (!states.empty()) {
      conn->Query(
          "CREATE TEMP TABLE IF NOT EXISTS tmp_s3_state ("
          "user_addr BLOB, cond_idx INTEGER, "
          "pos_0 BIGINT, pos_1 BIGINT, pos_2 BIGINT, pos_3 BIGINT, pos_4 BIGINT, pos_5 BIGINT, pos_6 BIGINT, pos_7 BIGINT, "
          "cost_0 BIGINT, cost_1 BIGINT, cost_2 BIGINT, cost_3 BIGINT, cost_4 BIGINT, cost_5 BIGINT, cost_6 BIGINT, cost_7 BIGINT, "
          "realized_pnl BIGINT, event_count BIGINT, last_sort_key BIGINT)");
      conn->Query("DELETE FROM tmp_s3_state");
      {
        duckdb::Appender ap(*conn, "tmp_s3_state");
        for (const auto &[key, st] : states) {
          std::string user_blob = hex_to_blob("0x" + key.user_hex);
          ap.BeginRow();
          ap.Append(duckdb::Value::BLOB(
              reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
              user_blob.size()));
          ap.Append(key.cond_idx);
          for (int j = 0; j < MAX_OUTCOMES; ++j) {
            ap.Append(st.positions[j]);
          }
          for (int j = 0; j < MAX_OUTCOMES; ++j) {
            ap.Append(st.cost[j]);
          }
          ap.Append(st.realized_pnl);
          ap.Append(st.event_count);
          ap.Append(st.last_sort_key);
          ap.EndRow();
        }
        ap.Close();
      }
      auto up = conn->Query(
          "INSERT INTO s3_user_cond_state "
          "SELECT * FROM tmp_s3_state "
          "ON CONFLICT(user_addr, cond_idx) DO UPDATE SET "
          "pos_0=excluded.pos_0, pos_1=excluded.pos_1, pos_2=excluded.pos_2, pos_3=excluded.pos_3, "
          "pos_4=excluded.pos_4, pos_5=excluded.pos_5, pos_6=excluded.pos_6, pos_7=excluded.pos_7, "
          "cost_0=excluded.cost_0, cost_1=excluded.cost_1, cost_2=excluded.cost_2, cost_3=excluded.cost_3, "
          "cost_4=excluded.cost_4, cost_5=excluded.cost_5, cost_6=excluded.cost_6, cost_7=excluded.cost_7, "
          "realized_pnl=excluded.realized_pnl, event_count=excluded.event_count, last_sort_key=excluded.last_sort_key");
        assert(up && !up->HasError());
      }

      conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_s3_users (user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT)");
      conn->Query("DELETE FROM tmp_s3_users");
      {
        duckdb::Appender ap(*conn, "tmp_s3_users");
        for (const auto &[uhex, inc] : user_event_inc) {
          std::string user_blob = hex_to_blob("0x" + uhex);
          ap.BeginRow();
          ap.Append(duckdb::Value::BLOB(
              reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
              user_blob.size()));
          ap.Append(inc);
          ap.Append(user_last_sk[uhex]);
          ap.EndRow();
        }
        ap.Close();
      }
      auto su = conn->Query(
          "INSERT INTO s3_user_summary "
          "SELECT user_addr, event_inc, 0, 0, last_sort_key FROM tmp_s3_users "
          "ON CONFLICT(user_addr) DO UPDATE SET "
          "total_events=s3_user_summary.total_events + excluded.total_events, "
          "last_sort_key=GREATEST(s3_user_summary.last_sort_key, excluded.last_sort_key)");
      assert(su && !su->HasError());

      auto sr = conn->Query(
          "UPDATE s3_user_summary AS s SET "
          "total_realized_pnl = t.rpnl, "
          "active_conditions = t.active_cnt "
          "FROM ("
          "  SELECT st.user_addr AS user_addr, "
          "         COALESCE(SUM(st.realized_pnl), 0) AS rpnl, "
          "         SUM(CASE WHEN "
          "              st.pos_0 != 0 OR st.pos_1 != 0 OR st.pos_2 != 0 OR st.pos_3 != 0 OR "
          "              st.pos_4 != 0 OR st.pos_5 != 0 OR st.pos_6 != 0 OR st.pos_7 != 0 "
          "            THEN 1 ELSE 0 END) AS active_cnt "
          "  FROM s3_user_cond_state st "
          "  JOIN tmp_s3_users tu ON st.user_addr = tu.user_addr "
          "  GROUP BY st.user_addr"
          ") AS t "
          "WHERE s.user_addr = t.user_addr");
      assert(sr && !sr->HasError());

      save_cursor_locked(*conn);
      auto cm = conn->Query("COMMIT");
      assert(cm && !cm->HasError());
    }

    {
      std::lock_guard<std::mutex> lk(cache_mu_);
      user_state_cache_.clear();
    }
    return true;
  }

  int64_t normalize_redemption_price(const ReplayRow &row) const {
    if (row.cond_idx < 0) {
      return normalize_price_fallback(row.price);
    }
    assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
    const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
    if (cond.payout_numerators.empty()) {
      return normalize_price_fallback(row.price);
    }
    assert(row.token_idx >= 0);
    if (row.token_idx >= static_cast<int32_t>(cond.payout_numerators.size())) {
      return 0;
    }
    int64_t sum = 0;
    for (int64_t p : cond.payout_numerators) {
      if (p > 0) {
        sum += p;
      }
    }
    if (sum <= 0) {
      return normalize_price_fallback(row.price);
    }
    int64_t pi = cond.payout_numerators[row.token_idx];
    if (pi <= 0) {
      return 0;
    }
    return static_cast<int64_t>((static_cast<__int128>(pi) * 1000000) / sum);
  }

  static int64_t normalize_price_fallback(int64_t raw_price) {
    if (raw_price <= 0) {
      return 0;
    }
    int64_t p = raw_price;
    for (int i = 0; i < 4 && p > 1000000 && p % 1000000 == 0; ++i) {
      p /= 1000000;
    }
    return p;
  }

  int64_t convert_payout_amount(const ReplayRow &row, int64_t qty) const {
    auto it = cond_question_count_.find(row.cond_idx);
    if (it == cond_question_count_.end() || it->second <= 1) {
      return 0;
    }
    int64_t qcnt = it->second;
    return (qty * (qcnt - 1)) / qcnt;
  }

  void apply_event_to_state(const ReplayRow &row, CondState &st) const {
    assert(row.cond_idx >= 0);
    assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
    const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
    assert(cond.outcome_count > 0 && cond.outcome_count <= MAX_OUTCOMES);
    assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
    int i = row.token_idx;

    auto remove_cost = [&](int64_t qty) {
      assert(qty >= 0);
      if (qty == 0) {
        return int64_t{0};
      }
      int64_t pos = st.positions[i];
      if (pos <= 0) {
        st.positions[i] -= qty;
        return int64_t{0};
      }
      if (qty >= pos) {
        int64_t all_cost = st.cost[i];
        st.cost[i] = 0;
        st.positions[i] -= qty;
        return all_cost;
      }
      int64_t cost_removed = st.cost[i] * qty / pos;
      st.cost[i] -= cost_removed;
      st.positions[i] -= qty;
      return cost_removed;
    };

    switch (static_cast<EventType>(row.event_type)) {
    case EventType::OrderBuy:
    case EventType::FPMMBuy:
    case EventType::SplitNormal:
    case EventType::SplitNegRisk:
    case EventType::SplitNonPoly: {
      int64_t qty = std::llabs(row.amount);
      if (row.amount >= 0) {
        st.positions[i] += qty;
        st.cost[i] += mul_div_1e6(qty, row.price);
      } else {
        (void)remove_cost(qty);
      }
      return;
    }
    case EventType::FPMMLPAdd:
      // LPAdd 表示资金进池，不直接增加可用 token 仓位（后续再细化 LP 成本池）。
      return;
    case EventType::FPMMLPReturn:
    case EventType::TransferInNegRisk:
    case EventType::TransferInOther:
    case EventType::TransferInNonPoly: {
      int64_t qty = std::llabs(row.amount);
      if (row.amount >= 0) {
        st.positions[i] += qty;
      } else {
        (void)remove_cost(qty);
      }
      return;
    }
    case EventType::OrderSell:
    case EventType::FPMMSell:
    case EventType::MergeNormal:
    case EventType::MergeNegRisk:
    case EventType::MergeNonPoly: {
      int64_t qty = std::llabs(row.amount);
      if (row.amount <= 0) {
        int64_t cost_removed = remove_cost(qty);
        int64_t proceeds = mul_div_1e6(qty, row.price);
        st.realized_pnl += proceeds - cost_removed;
      } else {
        st.positions[i] += qty;
        st.cost[i] += mul_div_1e6(qty, row.price);
      }
      return;
    }
    case EventType::FPMMLPRemove: {
      int64_t qty = std::llabs(row.amount);
      if (row.amount >= 0) {
        st.positions[i] += qty;
      } else {
        (void)remove_cost(qty);
      }
      return;
    }
    case EventType::Redemption:
    case EventType::RedemptionNonPoly: {
      int64_t qty = std::llabs(row.amount);
      if (row.amount <= 0) {
        int64_t cost_removed = remove_cost(qty);
        int64_t payout_price = normalize_redemption_price(row);
        int64_t proceeds = mul_div_1e6(qty, payout_price);
        st.realized_pnl += proceeds - cost_removed;
      } else {
        st.positions[i] += qty;
      }
      return;
    }
    case EventType::Convert: {
      int64_t qty = std::llabs(row.amount);
      if (row.amount <= 0) {
        int64_t cost_removed = remove_cost(qty);
        int64_t proceeds = convert_payout_amount(row, qty);
        st.realized_pnl += proceeds - cost_removed;
      } else {
        st.positions[i] += qty;
      }
      return;
    }
    case EventType::TransferOutNegRisk:
    case EventType::TransferOutOther:
    case EventType::TransferOutNonPoly: {
      int64_t qty = std::llabs(row.amount);
      if (row.amount <= 0) {
        (void)remove_cost(qty);
      } else {
        st.positions[i] += qty;
      }
      return;
    }
    default:
      assert(false);
      return;
    }
  }

  std::vector<ReplayRow> load_user_events(const std::string &addr_lower) const {
    auto conn = stage2_db_.create_connection();
    std::string hex40 = addr_lower.substr(2);
    auto q = conn->Query(
        "SELECT sort_key, cond_idx, event_type, token_idx, amount, price "
        "FROM user_event "
        "WHERE user_addr = from_hex('" + hex40 + "') "
        "ORDER BY sort_key, cond_idx, event_type, token_idx");
    assert(q && !q->HasError());
    std::vector<ReplayRow> out;
    out.reserve(static_cast<size_t>(q->RowCount()));
    for (idx_t i = 0; i < q->RowCount(); ++i) {
      out.push_back({"", q->GetValue(0, i).GetValue<int64_t>(),
                     q->GetValue(1, i).GetValue<int32_t>(),
                     q->GetValue(2, i).GetValue<int32_t>(),
                     q->GetValue(3, i).GetValue<int32_t>(),
                     q->GetValue(4, i).GetValue<int64_t>(),
                     q->GetValue(5, i).GetValue<int64_t>()});
    }
    return out;
  }

  std::vector<TimelineEntry> replay_timeline(const std::vector<ReplayRow> &events) const {
    std::unordered_map<int32_t, CondState> states;
    std::unordered_map<int32_t, int> cond_token_nonzero;
    std::vector<TimelineEntry> timeline;
    timeline.reserve(events.size());
    int64_t total_realized = 0;
    int total_token_count = 0;
    for (const auto &row : events) {
      if (row.cond_idx < 0) {
        timeline.push_back({row.sort_key, static_cast<uint8_t>(row.event_type), total_realized,
                            row.amount, row.price, static_cast<uint32_t>(UNKNOWN_COND_IDX),
                            static_cast<uint8_t>(UNKNOWN_TOKEN_IDX), total_token_count});
        continue;
      }
      auto it = states.find(row.cond_idx);
      if (it == states.end()) {
        it = states.emplace(row.cond_idx, CondState{}).first;
      }
      CondState &st = it->second;
      int64_t before_rpnl = st.realized_pnl;
      int before_nonzero = 0;
      {
        const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
        for (int i = 0; i < cond.outcome_count; ++i) {
          if (st.positions[i] != 0) {
            before_nonzero++;
          }
        }
      }
      apply_event_to_state(row, st);
      int after_nonzero = 0;
      {
        const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
        for (int i = 0; i < cond.outcome_count; ++i) {
          if (st.positions[i] != 0) {
            after_nonzero++;
          }
        }
      }
      total_realized += (st.realized_pnl - before_rpnl);
      total_token_count += (after_nonzero - before_nonzero);
      timeline.push_back({row.sort_key, static_cast<uint8_t>(row.event_type), total_realized,
                          row.amount, row.price, static_cast<uint32_t>(row.cond_idx),
                          static_cast<uint8_t>(row.token_idx), total_token_count});
    }
    return timeline;
  }

  std::unordered_map<int32_t, CondState> replay_until(const std::vector<ReplayRow> &events,
                                                      int64_t sort_key) const {
    std::unordered_map<int32_t, CondState> states;
    for (const auto &row : events) {
      if (row.sort_key > sort_key) {
        break;
      }
      if (row.cond_idx < 0) {
        continue;
      }
      auto &st = states[row.cond_idx];
      apply_event_to_state(row, st);
    }
    return states;
  }

  std::optional<UserState> load_user_state_locked(const std::string &addr_lower) const {
    auto events = load_user_events(addr_lower);
    if (events.empty()) {
      return std::nullopt;
    }
    std::unordered_map<int32_t, CondState> states;
    std::unordered_map<int32_t, std::vector<Snapshot>> snaps;
    for (const auto &row : events) {
      if (row.cond_idx < 0) {
        continue;
      }
      auto &st = states[row.cond_idx];
      apply_event_to_state(row, st);
      const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
      Snapshot s{};
      s.sort_key = row.sort_key;
      s.delta = row.amount;
      s.price = row.price;
      s.event_type = static_cast<uint8_t>(row.event_type);
      s.token_idx = static_cast<uint8_t>(row.token_idx);
      s.outcome_count = cond.outcome_count;
      std::memcpy(s.positions, st.positions.data(), sizeof(s.positions));
      s.cost_basis = 0;
      for (int i = 0; i < cond.outcome_count; ++i) {
        s.cost_basis += st.cost[i];
      }
      s.realized_pnl = st.realized_pnl;
      snaps[row.cond_idx].push_back(s);
    }
    UserState out;
    out.conditions.reserve(snaps.size());
    for (auto &[cond_idx, vec] : snaps) {
      out.conditions.push_back({static_cast<uint32_t>(cond_idx), std::move(vec)});
    }
    return out;
  }
};

} // namespace stage3
