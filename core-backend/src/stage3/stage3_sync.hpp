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
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <deque>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stage3 {

using namespace stage2;
namespace asio = boost::asio;

class StageSync {
public:
  struct Status {
    bool syncing = false;
    int64_t last_block = 0;
    int64_t head_block = 0;
    int64_t behind_blocks = 0;
    int64_t behind_chunks = 0;
    double blocks_per_second = 0.0;
    double eta_seconds = -1.0;
  };

  struct Stage2Data {
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

  StageSync(EventBuilder &builder, Database &stage2_db, Database &stage3_db)
      : builder_(builder), stage2_db_(stage2_db), stage3_db_(stage3_db) {
    init_schema();
    load_conditions();
    load_cursor();
    refresh_status_locked();
  }

  void start(asio::io_context &ioc) {
    ioc_ = &ioc;
    stop_requested_ = false;
    schedule_sync(1);
  }

  void stop() { stop_requested_ = true; }

  Status status() const {
    std::lock_guard<std::mutex> lock(sync_mu_);
    return sync_;
  }

  Stage2Data stage2_data() const {
    const auto &wp = builder_.progress();
    const auto &bp = builder_.committed_progress();
    Stage2Data p;
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
    const auto s = status();
    if (s.behind_blocks == 0 && p.total_users > 0) {
      p.phase = 7;
    }
    return p;
  }

  std::vector<UserSummary> get_users_sorted(int64_t limit = 200) const {
    TraceN("s3/users");
    auto conn = stage3_db_.create_connection();
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
    return build_timeline(events);
  }

  std::vector<PositionAtTime> get_positions_at(const std::string &addr, int64_t sort_key) const {
    TraceN("s3/positions");
    std::string lower = normalize_addr(addr);
    if (lower.empty()) {
      return {};
    }
    auto state = build_state_until(lower, sort_key);
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

private:
  struct CursorKey {
    int64_t sort_key = -1;
    std::string user_hex;
    int32_t cond_idx = std::numeric_limits<int32_t>::min();
    int32_t event_type = std::numeric_limits<int32_t>::min();
    int32_t token_idx = std::numeric_limits<int32_t>::min();
    int64_t processed_events = 0;
  };

  struct EventRow {
    std::string user_hex;
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t event_type = 0;
    int32_t token_idx = 0;
    int32_t collateral = 0;
    int64_t amount = 0;
    int64_t price = 0;
    int64_t realized_cum = 0;
    int32_t token_count_cum = 0;
  };

  struct FactRow {
    std::string user_hex;
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t token_idx = 0;
    int32_t event_type = 0;
    int32_t collateral = 0;
    int64_t amount = 0;
    int64_t price = 0;
    int64_t realized_delta = 0;
    int64_t realized_cum = 0;
    int32_t token_count_cum = 0;
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
  Database &stage3_db_;

  mutable std::mutex sync_mu_;
  mutable Status sync_;
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

  static constexpr int64_t kStage3ChunkBlocks = 100000;
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

  void init_schema() const {
    stage3_db_.execute(R"(
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
    stage3_db_.execute(R"(
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
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_summary (
        user_addr BLOB PRIMARY KEY,
        total_events BIGINT NOT NULL,
        total_realized_pnl BIGINT NOT NULL,
        active_conditions BIGINT NOT NULL,
        last_sort_key BIGINT NOT NULL
      )
    )");
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_event_fact (
        user_addr BLOB NOT NULL,
        sort_key BIGINT NOT NULL,
        cond_idx INTEGER NOT NULL,
        token_idx INTEGER NOT NULL,
        event_type INTEGER NOT NULL,
        collateral INTEGER NOT NULL,
        amount BIGINT NOT NULL,
        price BIGINT NOT NULL,
        realized_delta BIGINT NOT NULL,
        realized_cum BIGINT NOT NULL,
        token_count_cum INTEGER NOT NULL,
        PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
      )
    )");
    stage3_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS s3_user_cond_checkpoint (
        user_addr BLOB NOT NULL,
        checkpoint_sort_key BIGINT NOT NULL,
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
        PRIMARY KEY (user_addr, checkpoint_sort_key, cond_idx)
      )
    )");
    stage3_db_.execute(
        "CREATE INDEX IF NOT EXISTS idx_s3_fact_user_sk "
        "ON s3_user_event_fact(user_addr, sort_key)");
    stage3_db_.execute(
        "CREATE INDEX IF NOT EXISTS idx_s3_ckpt_user_sk "
        "ON s3_user_cond_checkpoint(user_addr, checkpoint_sort_key)");
    stage3_db_.execute(
        "CREATE INDEX IF NOT EXISTS idx_s3_summary_events "
        "ON s3_user_summary(total_events)");
    auto conn = stage3_db_.create_connection();
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
    auto conn = stage3_db_.create_connection();
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

  void refresh_status_locked() const {
    sync_.head_block = builder_.cursor();
    sync_.last_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
    sync_.behind_blocks = std::max<int64_t>(0, sync_.head_block - sync_.last_block);
    sync_.behind_chunks = (sync_.behind_blocks + kStage3ChunkBlocks - 1) / kStage3ChunkBlocks;
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
    auto refresh_timing_metrics = [&](int64_t remaining_blocks) {
      if (commit_history_.size() < 2) {
        sync_.blocks_per_second = 0.0;
        sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
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
        sync_.eta_seconds = (remaining_blocks == 0) ? 0.0 : -1.0;
        return;
      }
      sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
      sync_.eta_seconds =
          (remaining_blocks == 0) ? 0.0 : static_cast<double>(remaining_blocks) / sync_.blocks_per_second;
    };

    int64_t before_block = 0;
    {
      // 只在更新状态时持锁，避免 API 读取 status 被整段同步阻塞。
      std::lock_guard<std::mutex> lock(sync_mu_);
      sync_.syncing = true;
      before_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
    }

    bool advanced = process_chunk_locked();

    int next_delay = kBaseIntervalSeconds;
    {
      std::lock_guard<std::mutex> lock(sync_mu_);
      refresh_status_locked();
      int64_t after_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
      if (advanced && after_block > before_block) {
        commit_history_.push_back({std::chrono::steady_clock::now(), after_block});
        if (commit_history_.size() > kEtaWindowSize) {
          commit_history_.pop_front();
        }
      }
      refresh_timing_metrics(sync_.behind_blocks);
      sync_.syncing = false;
      next_delay = (sync_.behind_chunks > 1) ? 0 : kBaseIntervalSeconds;
    }
    schedule_sync(next_delay);
  }

  bool process_chunk_locked() const {
    TraceN("s3/sync_chunk");
    int64_t current_block = (cursor_.sort_key < 0) ? 0 : cursor_.sort_key / SORT_KEY_SCALE;
    int64_t head_block = builder_.cursor();
    if (current_block >= head_block) {
      return false;
    }
    int64_t target_block = std::min(current_block + kStage3ChunkBlocks, head_block);
    int64_t upper_sort_key = target_block * SORT_KEY_SCALE + (SORT_KEY_SCALE - 1);

    auto source_conn = stage2_db_.create_connection();
    auto sink_conn = stage3_db_.create_connection();
    std::string user_hex = cursor_.user_hex;
    auto qr = source_conn->Query(
        "SELECT lower(hex(user_addr)) AS user_hex, sort_key, cond_idx, event_type, token_idx, collateral, amount, price "
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
      auto tx = sink_conn->Query("BEGIN");
      assert(tx && !tx->HasError());
      save_cursor_locked(*sink_conn);
      auto cm = sink_conn->Query("COMMIT");
      assert(cm && !cm->HasError());
      return true;
    }

    std::vector<EventRow> rows;
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
      EventRow row;
      row.user_hex = qr->GetValue(0, i).GetValueUnsafe<std::string>();
      row.sort_key = qr->GetValue(1, i).GetValue<int64_t>();
      row.cond_idx = qr->GetValue(2, i).GetValue<int32_t>();
      row.event_type = qr->GetValue(3, i).GetValue<int32_t>();
      row.token_idx = qr->GetValue(4, i).GetValue<int32_t>();
      row.collateral = qr->GetValue(5, i).GetValue<int32_t>();
      row.amount = qr->GetValue(6, i).GetValue<int64_t>();
      row.price = qr->GetValue(7, i).GetValue<int64_t>();
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

    std::unordered_map<std::string, int64_t> user_realized_cum;
    std::unordered_map<std::string, int32_t> user_token_count_cum;
    user_realized_cum.reserve(touched_users.size() + 1);
    user_token_count_cum.reserve(touched_users.size() + 1);
    if (!touched_users.empty()) {
      sink_conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_s3_touched_users (user_addr BLOB)");
      sink_conn->Query("DELETE FROM tmp_s3_touched_users");
      {
        duckdb::Appender ap(*sink_conn, "tmp_s3_touched_users");
        for (const auto &uhex : touched_users) {
          std::string user_blob = hex_to_blob("0x" + uhex);
          ap.BeginRow();
          ap.Append(duckdb::Value::BLOB(
              reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
              user_blob.size()));
          ap.EndRow();
        }
        ap.Close();
      }
      auto sum_r = sink_conn->Query(
          "SELECT lower(hex(s.user_addr)) AS uh, s.total_realized_pnl "
          "FROM s3_user_summary s "
          "JOIN tmp_s3_touched_users t ON s.user_addr = t.user_addr");
      assert(sum_r && !sum_r->HasError());
      for (idx_t i = 0; i < sum_r->RowCount(); ++i) {
        user_realized_cum.emplace(sum_r->GetValue(0, i).GetValueUnsafe<std::string>(),
                                  sum_r->GetValue(1, i).GetValue<int64_t>());
      }
      for (const auto &uhex : touched_users) {
        if (!user_realized_cum.count(uhex)) {
          user_realized_cum.emplace(uhex, 0);
        }
      }

      auto tk_r = sink_conn->Query(
          "SELECT lower(hex(st.user_addr)) AS uh, "
          "SUM(CASE WHEN st.pos_0 != 0 THEN 1 ELSE 0 END + "
          "    CASE WHEN st.pos_1 != 0 THEN 1 ELSE 0 END + "
          "    CASE WHEN st.pos_2 != 0 THEN 1 ELSE 0 END + "
          "    CASE WHEN st.pos_3 != 0 THEN 1 ELSE 0 END + "
          "    CASE WHEN st.pos_4 != 0 THEN 1 ELSE 0 END + "
          "    CASE WHEN st.pos_5 != 0 THEN 1 ELSE 0 END + "
          "    CASE WHEN st.pos_6 != 0 THEN 1 ELSE 0 END + "
          "    CASE WHEN st.pos_7 != 0 THEN 1 ELSE 0 END) AS tk "
          "FROM s3_user_cond_state st "
          "JOIN tmp_s3_touched_users t ON st.user_addr = t.user_addr "
          "GROUP BY st.user_addr");
      assert(tk_r && !tk_r->HasError());
      for (idx_t i = 0; i < tk_r->RowCount(); ++i) {
        user_token_count_cum.emplace(tk_r->GetValue(0, i).GetValueUnsafe<std::string>(),
                                     tk_r->GetValue(1, i).GetValue<int32_t>());
      }
      for (const auto &uhex : touched_users) {
        if (!user_token_count_cum.count(uhex)) {
          user_token_count_cum.emplace(uhex, 0);
        }
      }
    }

    if (!states.empty()) {
      sink_conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_s3_keys (user_addr BLOB, cond_idx INTEGER)");
      sink_conn->Query("DELETE FROM tmp_s3_keys");
      {
        duckdb::Appender ap(*sink_conn, "tmp_s3_keys");
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
      auto old = sink_conn->Query(
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

    std::vector<FactRow> fact_rows;
    fact_rows.reserve(rows.size());
    for (const auto &row : rows) {
      int64_t realized_delta = 0;
      if (row.cond_idx >= 0) {
        PairKey key{row.user_hex, row.cond_idx};
        auto it = states.find(key);
        assert(it != states.end());
        int before_nonzero = 0;
        int after_nonzero = 0;
        const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
        for (int j = 0; j < cond.outcome_count; ++j) {
          if (it->second.positions[j] != 0) {
            before_nonzero++;
          }
        }
        realized_delta = apply_event_to_state(row, it->second);
        for (int j = 0; j < cond.outcome_count; ++j) {
          if (it->second.positions[j] != 0) {
            after_nonzero++;
          }
        }
        user_token_count_cum[row.user_hex] += (after_nonzero - before_nonzero);
        it->second.event_count++;
        it->second.last_sort_key = row.sort_key;
      }
      int64_t &realized_cum = user_realized_cum[row.user_hex];
      realized_cum += realized_delta;
      int32_t token_count_cum = user_token_count_cum[row.user_hex];
      fact_rows.push_back({
          row.user_hex,
          row.sort_key,
          row.cond_idx,
          row.token_idx,
          row.event_type,
          row.collateral,
          row.amount,
          row.price,
          realized_delta,
          realized_cum,
          token_count_cum,
      });
    }
    assert(fact_rows.size() == rows.size());
    for (const auto &[key, st] : states) {
      assert(key.cond_idx >= 0);
      assert(static_cast<size_t>(key.cond_idx) < conditions_.size());
      const auto &cond = conditions_[static_cast<size_t>(key.cond_idx)];
      for (int j = 0; j < cond.outcome_count; ++j) {
        assert(st.positions[j] >= 0);
        assert(st.cost[j] >= 0);
      }
    }

    {
      TraceN("s3/write");
      auto tx = sink_conn->Query("BEGIN");
      assert(tx && !tx->HasError());

      if (!states.empty()) {
        sink_conn->Query(
          "CREATE TEMP TABLE IF NOT EXISTS tmp_s3_state ("
          "user_addr BLOB, cond_idx INTEGER, "
          "pos_0 BIGINT, pos_1 BIGINT, pos_2 BIGINT, pos_3 BIGINT, pos_4 BIGINT, pos_5 BIGINT, pos_6 BIGINT, pos_7 BIGINT, "
          "cost_0 BIGINT, cost_1 BIGINT, cost_2 BIGINT, cost_3 BIGINT, cost_4 BIGINT, cost_5 BIGINT, cost_6 BIGINT, cost_7 BIGINT, "
          "realized_pnl BIGINT, event_count BIGINT, last_sort_key BIGINT)");
        sink_conn->Query("DELETE FROM tmp_s3_state");
      {
          duckdb::Appender ap(*sink_conn, "tmp_s3_state");
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
        auto up = sink_conn->Query(
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

      if (!fact_rows.empty()) {
        sink_conn->Query(
            "CREATE TEMP TABLE IF NOT EXISTS tmp_s3_fact ("
            "user_addr BLOB, sort_key BIGINT, cond_idx INTEGER, token_idx INTEGER, event_type INTEGER, "
            "collateral INTEGER, amount BIGINT, price BIGINT, realized_delta BIGINT, realized_cum BIGINT, token_count_cum INTEGER)");
        sink_conn->Query("DELETE FROM tmp_s3_fact");
        {
          duckdb::Appender ap(*sink_conn, "tmp_s3_fact");
          for (const auto &fr : fact_rows) {
            std::string user_blob = hex_to_blob("0x" + fr.user_hex);
            ap.BeginRow();
            ap.Append(duckdb::Value::BLOB(
                reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()),
                user_blob.size()));
            ap.Append(fr.sort_key);
            ap.Append(fr.cond_idx);
            ap.Append(fr.token_idx);
            ap.Append(fr.event_type);
            ap.Append(fr.collateral);
            ap.Append(fr.amount);
            ap.Append(fr.price);
            ap.Append(fr.realized_delta);
            ap.Append(fr.realized_cum);
            ap.Append(fr.token_count_cum);
            ap.EndRow();
          }
          ap.Close();
        }
        auto insf = sink_conn->Query(
            "INSERT INTO s3_user_event_fact "
            "SELECT * FROM tmp_s3_fact "
            "ON CONFLICT(user_addr, sort_key, cond_idx, event_type, token_idx) DO UPDATE SET "
            "token_idx=excluded.token_idx, collateral=excluded.collateral, amount=excluded.amount, "
            "price=excluded.price, realized_delta=excluded.realized_delta, realized_cum=excluded.realized_cum, "
            "token_count_cum=excluded.token_count_cum");
        assert(insf && !insf->HasError());
      }

      sink_conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_s3_users (user_addr BLOB, event_inc BIGINT, last_sort_key BIGINT)");
      sink_conn->Query("DELETE FROM tmp_s3_users");
      {
        duckdb::Appender ap(*sink_conn, "tmp_s3_users");
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
      auto su = sink_conn->Query(
          "INSERT INTO s3_user_summary "
          "SELECT user_addr, event_inc, 0, 0, last_sort_key FROM tmp_s3_users "
          "ON CONFLICT(user_addr) DO UPDATE SET "
          "total_events=s3_user_summary.total_events + excluded.total_events, "
          "last_sort_key=GREATEST(s3_user_summary.last_sort_key, excluded.last_sort_key)");
      assert(su && !su->HasError());

      auto sr = sink_conn->Query(
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

      if (!touched_users.empty()) {
        auto ckpt = sink_conn->Query(
            "INSERT INTO s3_user_cond_checkpoint "
            "SELECT st.user_addr, " + std::to_string(cursor_.sort_key) + " AS checkpoint_sort_key, st.cond_idx, "
            "st.pos_0,st.pos_1,st.pos_2,st.pos_3,st.pos_4,st.pos_5,st.pos_6,st.pos_7, "
            "st.cost_0,st.cost_1,st.cost_2,st.cost_3,st.cost_4,st.cost_5,st.cost_6,st.cost_7, "
            "st.realized_pnl,st.event_count,st.last_sort_key "
            "FROM s3_user_cond_state st "
            "JOIN tmp_s3_users tu ON st.user_addr = tu.user_addr "
            "WHERE ("
            "st.pos_0 != 0 OR st.pos_1 != 0 OR st.pos_2 != 0 OR st.pos_3 != 0 OR "
            "st.pos_4 != 0 OR st.pos_5 != 0 OR st.pos_6 != 0 OR st.pos_7 != 0 OR "
            "st.realized_pnl != 0"
            ") "
            "ON CONFLICT(user_addr, checkpoint_sort_key, cond_idx) DO UPDATE SET "
            "pos_0=excluded.pos_0, pos_1=excluded.pos_1, pos_2=excluded.pos_2, pos_3=excluded.pos_3, "
            "pos_4=excluded.pos_4, pos_5=excluded.pos_5, pos_6=excluded.pos_6, pos_7=excluded.pos_7, "
            "cost_0=excluded.cost_0, cost_1=excluded.cost_1, cost_2=excluded.cost_2, cost_3=excluded.cost_3, "
            "cost_4=excluded.cost_4, cost_5=excluded.cost_5, cost_6=excluded.cost_6, cost_7=excluded.cost_7, "
            "realized_pnl=excluded.realized_pnl, event_count=excluded.event_count, last_sort_key=excluded.last_sort_key");
        assert(ckpt && !ckpt->HasError());
      }

      save_cursor_locked(*sink_conn);
      auto cm = sink_conn->Query("COMMIT");
      assert(cm && !cm->HasError());
    }

    return true;
  }

  int64_t normalize_redemption_price(const EventRow &row) const {
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

  int64_t convert_payout_amount(const EventRow &row, int64_t qty) const {
    auto it = cond_question_count_.find(row.cond_idx);
    if (it == cond_question_count_.end() || it->second <= 1) {
      return 0;
    }
    int64_t qcnt = it->second;
    return (qty * (qcnt - 1)) / qcnt;
  }

  int64_t apply_event_to_state(const EventRow &row, CondState &st) const {
    assert(row.cond_idx >= 0);
    assert(static_cast<size_t>(row.cond_idx) < conditions_.size());
    const auto &cond = conditions_[static_cast<size_t>(row.cond_idx)];
    assert(cond.outcome_count > 0 && cond.outcome_count <= MAX_OUTCOMES);
    assert(row.token_idx >= 0 && row.token_idx < cond.outcome_count);
    int i = row.token_idx;

    auto remove_cost = [&](int64_t qty) {
      assert(qty > 0);
      int64_t pos = st.positions[i];
      assert(pos > 0 && pos >= qty);
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
      assert(row.amount >= 0);
      int64_t qty = row.amount;
      st.positions[i] += qty;
      st.cost[i] += mul_div_1e6(qty, row.price);
      return 0;
    }
    case EventType::FPMMLPAdd:
      assert(row.amount >= 0);
      // LP资金流事件：只保留事实行，不修改 token 状态。
      return 0;
    case EventType::FPMMLPRemove:
      assert(row.amount >= 0);
      return 0;
    case EventType::FPMMLPReturn:
      assert(row.amount >= 0);
      return 0;
    case EventType::TransferInNegRisk:
    case EventType::TransferInOther:
    case EventType::TransferInNonPoly: {
      assert(row.amount >= 0);
      int64_t qty = row.amount;
      st.positions[i] += qty;
      return 0;
    }
    case EventType::OrderSell:
    case EventType::FPMMSell:
    case EventType::MergeNormal:
    case EventType::MergeNegRisk:
    case EventType::MergeNonPoly: {
      assert(row.amount <= 0);
      int64_t qty = -row.amount;
      int64_t cost_removed = remove_cost(qty);
      int64_t proceeds = mul_div_1e6(qty, row.price);
      int64_t realized_delta = proceeds - cost_removed;
      st.realized_pnl += realized_delta;
      return realized_delta;
    }
    case EventType::Redemption:
    case EventType::RedemptionNonPoly: {
      assert(row.amount <= 0);
      int64_t qty = -row.amount;
      int64_t cost_removed = remove_cost(qty);
      int64_t payout_price = normalize_redemption_price(row);
      int64_t proceeds = mul_div_1e6(qty, payout_price);
      int64_t realized_delta = proceeds - cost_removed;
      st.realized_pnl += realized_delta;
      return realized_delta;
    }
    case EventType::Convert: {
      assert(row.amount <= 0);
      int64_t qty = -row.amount;
      int64_t cost_removed = remove_cost(qty);
      int64_t proceeds = convert_payout_amount(row, qty);
      int64_t realized_delta = proceeds - cost_removed;
      st.realized_pnl += realized_delta;
      return realized_delta;
    }
    case EventType::TransferOutNegRisk:
    case EventType::TransferOutOther:
    case EventType::TransferOutNonPoly: {
      assert(row.amount <= 0);
      int64_t qty = -row.amount;
      (void)remove_cost(qty);
      return 0;
    }
    default:
      assert(false);
      return 0;
    }
  }

  std::vector<EventRow> load_user_events(const std::string &addr_lower) const {
    auto conn = stage3_db_.create_connection();
    std::string hex40 = addr_lower.substr(2);
    auto q = conn->Query(
        "SELECT sort_key, cond_idx, event_type, token_idx, collateral, amount, price, realized_cum "
        ", token_count_cum "
        "FROM s3_user_event_fact "
        "WHERE user_addr = from_hex('" + hex40 + "') "
        "ORDER BY sort_key, cond_idx, event_type, token_idx");
    assert(q && !q->HasError());
    std::vector<EventRow> out;
    out.reserve(static_cast<size_t>(q->RowCount()));
    for (idx_t i = 0; i < q->RowCount(); ++i) {
      out.push_back({"",
                     q->GetValue(0, i).GetValue<int64_t>(),
                     q->GetValue(1, i).GetValue<int32_t>(),
                     q->GetValue(2, i).GetValue<int32_t>(),
                     q->GetValue(3, i).GetValue<int32_t>(),
                     q->GetValue(4, i).GetValue<int32_t>(),
                     q->GetValue(5, i).GetValue<int64_t>(),
                     q->GetValue(6, i).GetValue<int64_t>(),
                     q->GetValue(7, i).GetValue<int64_t>(),
                     q->GetValue(8, i).GetValue<int32_t>()});
    }
    return out;
  }

  std::vector<TimelineEntry> build_timeline(const std::vector<EventRow> &events) const {
    std::vector<TimelineEntry> timeline;
    timeline.reserve(events.size());
    for (const auto &row : events) {
      if (row.cond_idx < 0) {
        timeline.push_back({row.sort_key, static_cast<uint8_t>(row.event_type), row.realized_cum,
                            row.amount, row.price, static_cast<uint32_t>(UNKNOWN_COND_IDX),
                            static_cast<uint8_t>(UNKNOWN_TOKEN_IDX), row.token_count_cum});
        continue;
      }
      timeline.push_back({row.sort_key, static_cast<uint8_t>(row.event_type), row.realized_cum,
                          row.amount, row.price, static_cast<uint32_t>(row.cond_idx),
                          static_cast<uint8_t>(row.token_idx), row.token_count_cum});
    }
    return timeline;
  }

  std::unordered_map<int32_t, CondState> build_state_until(const std::string &addr_lower,
                                                           int64_t target_sort_key) const {
    auto conn = stage3_db_.create_connection();
    std::string hex40 = addr_lower.substr(2);
    std::unordered_map<int32_t, CondState> states;
    int64_t checkpoint_sort_key = -1;

    auto ck = conn->Query(
        "SELECT MAX(checkpoint_sort_key) "
        "FROM s3_user_cond_checkpoint "
        "WHERE user_addr = from_hex('" + hex40 + "') "
        "AND checkpoint_sort_key <= " + std::to_string(target_sort_key));
    assert(ck && !ck->HasError() && ck->RowCount() == 1);
    if (!ck->GetValue(0, 0).IsNull()) {
      checkpoint_sort_key = ck->GetValue(0, 0).GetValue<int64_t>();
    }

    if (checkpoint_sort_key >= 0) {
      auto base = conn->Query(
          "SELECT cond_idx, "
          "pos_0,pos_1,pos_2,pos_3,pos_4,pos_5,pos_6,pos_7, "
          "cost_0,cost_1,cost_2,cost_3,cost_4,cost_5,cost_6,cost_7, "
          "realized_pnl,event_count,last_sort_key "
          "FROM s3_user_cond_checkpoint "
          "WHERE user_addr = from_hex('" + hex40 + "') "
          "AND checkpoint_sort_key = " + std::to_string(checkpoint_sort_key));
      assert(base && !base->HasError());
      states.reserve(static_cast<size_t>(base->RowCount()));
      for (idx_t i = 0; i < base->RowCount(); ++i) {
        int32_t cond_idx = base->GetValue(0, i).GetValue<int32_t>();
        CondState st;
        for (int j = 0; j < MAX_OUTCOMES; ++j) {
          st.positions[j] = base->GetValue(1 + j, i).GetValue<int64_t>();
          st.cost[j] = base->GetValue(9 + j, i).GetValue<int64_t>();
        }
        st.realized_pnl = base->GetValue(17, i).GetValue<int64_t>();
        st.event_count = base->GetValue(18, i).GetValue<int64_t>();
        st.last_sort_key = base->GetValue(19, i).GetValue<int64_t>();
        states.emplace(cond_idx, st);
      }
    }

    auto delta = conn->Query(
        "SELECT sort_key, cond_idx, event_type, token_idx, collateral, amount, price, realized_cum, token_count_cum "
        "FROM s3_user_event_fact "
        "WHERE user_addr = from_hex('" + hex40 + "') "
        "AND sort_key > " + std::to_string(checkpoint_sort_key) + " "
        "AND sort_key <= " + std::to_string(target_sort_key) + " "
        "ORDER BY sort_key, cond_idx, event_type, token_idx");
    assert(delta && !delta->HasError());
    for (idx_t i = 0; i < delta->RowCount(); ++i) {
      EventRow row{
          "",
          delta->GetValue(0, i).GetValue<int64_t>(),
          delta->GetValue(1, i).GetValue<int32_t>(),
          delta->GetValue(2, i).GetValue<int32_t>(),
          delta->GetValue(3, i).GetValue<int32_t>(),
          delta->GetValue(4, i).GetValue<int32_t>(),
          delta->GetValue(5, i).GetValue<int64_t>(),
          delta->GetValue(6, i).GetValue<int64_t>(),
          delta->GetValue(7, i).GetValue<int64_t>(),
          delta->GetValue(8, i).GetValue<int32_t>(),
      };
      if (row.cond_idx < 0) {
        continue;
      }
      auto &st = states[row.cond_idx];
      (void)apply_event_to_state(row, st);
    }
    return states;
  }

};

} // namespace stage3
