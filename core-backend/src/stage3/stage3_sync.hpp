#pragma once

#include "../core/database.hpp"
#include "../stage2/stage2_builder.hpp"
#include "../stage2/stage2_models.hpp"
#include "stage3.hpp"

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace stage3 {

namespace asio = boost::asio;
using json = nlohmann::json;

namespace filter {

using Request = FilterRequest;

struct UserRow {
  std::string addr;
  double sort_value = 0.0;
  int64_t month_avg_tok = 0;
  int64_t month_avg_exp = 0;
  int64_t month_avg_hp = 0;
  int64_t pnl = 0;
};

struct Result {
  int64_t anchor_bucket = 0;
  int64_t scanned_user_count = 0;
  int64_t matched_user_count = 0;
  std::vector<UserRow> users;
  std::vector<FilterItemStat> item_stats;
};

} // namespace filter

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

    stage2::ConditionTree cond_tree;
    stage2::TokenTree token_tree;

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

    stage2::SplitSemanticTree split_sem_tree;
    stage2::MergeSemanticTree merge_sem_tree;
    stage2::ConvertSemanticTree convert_sem_tree;
    stage2::OrderSemanticTree order_sem_tree;
    std::unordered_map<uint16_t, int64_t> event_by_collateral;
  };

  struct UserSummaryRow {
    std::string addr;
    int64_t event_count = 0;
    int64_t realized_pnl = 0;
    int64_t unrealized_pnl = 0;
  };

  struct TimelineRow {
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t token_idx = 0;
    uint8_t event_type = 0;
    int64_t amount = 0;
    int64_t price = 0;
    int64_t realized_pnl = 0;
    int64_t unrealized_pnl = 0;
    int32_t token_count = 0;
  };

  struct PositionRow {
    uint32_t cond_idx = 0;
    uint8_t token_idx = 0;
    uint8_t outcome_count = 0;
    int64_t qty = 0;
    int64_t cost = 0;
    int64_t last_price = 0;
    int64_t entry_block = 0;
  };

  StageSync(stage2::EventBuilder &builder,
            Database &stage0_db,
            Database &stage2_db,
            Database &stage3_db,
            int base_interval_seconds);
  ~StageSync();

  void start(asio::io_context &ioc);
  void stop();

  Status status() const;
  int64_t get_max_bucket() const;
  int64_t get_bucket_user_count(int64_t bucket) const;
  // Stage3 mmap pool usage (per-shard + hottest shard), for frontend display.
  json pool_usage() const;
  Stage2Data stage2_data() const;
  json memory_breakdown() const;
  json stage2_rocksdb_memory_breakdown() const;
  json stage3_rocksdb_memory_breakdown() const;

  std::vector<UserSummaryRow> get_users_sorted(int64_t limit = 200) const;
  std::vector<TimelineRow> get_user_timeline(const std::string &addr) const;
  std::vector<PositionRow> get_positions_at(const std::string &addr, int64_t sort_key) const;
  filter::Result filter_users_by_features(const filter::Request &req) const;

private:
  static constexpr size_t kStage3BatchEvents = 1'000'000;
  static constexpr size_t kCommitHistoryWindow = 20;
  static constexpr int kStage2YieldDelaySeconds = 1;
  static constexpr int kPauseRetryDelaySeconds = 1;
  static constexpr uint8_t kRtStateIdle = 0;
  static constexpr uint8_t kRtStateSyncing = 1;
  static constexpr uint8_t kRtStateQuerying = 2;

  struct SyncCommitPoint {
    std::chrono::steady_clock::time_point committed_at;
    int64_t block = 0;
  };

  class QueryPauseGuard;

  static std::string normalize_addr(const std::string &addr);
  static std::string normalize_tag_key(const std::string &raw);
  int8_t tag_name_to_id(const std::string &tag_name) const;
  void load_tag_mapping();
  void load_conditions();
  void refresh_conditions_if_needed();
  void refresh_status_locked();
  void schedule_sync(int delay_seconds);
  void do_sync_tick();

  stage2::EventBuilder &builder_;
  Database &stage0_db_;
  Database &stage2_db_;
  Database &stage3_db_;
  Stage3Runtime *rt_ = nullptr;

  asio::io_context *ioc_ = nullptr;
  std::shared_ptr<asio::steady_timer> timer_;
  std::atomic<bool> stop_requested_{false};
  mutable std::atomic<bool> pause_requested_{false};
  mutable std::atomic<uint8_t> rt_state_{kRtStateIdle};
  int base_interval_seconds_ = 1;

  mutable std::mutex query_mu_;
  mutable std::mutex sync_mu_;
  mutable Status sync_;
  mutable std::deque<SyncCommitPoint> sync_commit_points_;
  std::unordered_map<std::string, int8_t> tag_to_industry_id_;
  int64_t loaded_max_cond_idx_ = -1;

  // Cached for lightweight status API (updated by refresh_status_locked)
  std::atomic<int64_t> cached_max_bucket_{-1};
  std::atomic<int64_t> cached_bucket_user_count_{0};
};

} // namespace stage3
