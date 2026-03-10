#pragma once

#include "../core/database.hpp"
#include "../core/rocks_store.hpp"
#include "../stage2/stage2_builder.hpp"
#include "../stage2/stage2_models.hpp"
#include "../stage2/stage2_types.hpp"
#include "stage3_comp_feat.hpp"
#include "stage3_filter.hpp"

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
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

  StageSync(EventBuilder &builder, Database &stage0_db, Database &stage2_db, Database &stage3_db,
            int base_interval_seconds);

  void start(asio::io_context &ioc);
  void stop();

  Status status() const;
  int64_t get_max_bucket() const;
  Stage2Data stage2_data() const;
  json memory_breakdown() const;
  json stage2_rocksdb_memory_breakdown() const;
  json stage3_rocksdb_memory_breakdown() const;

  std::vector<UserSummaryRow> get_users_sorted(int64_t limit = 200) const;
  std::vector<TimelineRow> get_user_timeline(const std::string &addr) const;
  std::vector<PositionRow> get_positions_at(const std::string &addr, int64_t sort_key) const;
  filter::Result filter_users_by_features(const filter::Request &req) const;

private:
  struct SyncCursorState {
    int64_t sort_key = -1;
    int64_t processed_events = 0;
  };

  struct EventInput {
    uint32_t user_id = 0;
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t event_type = 0;
    int32_t token_idx = 0;
    int32_t collateral = 0;
    int64_t amount = 0;
    int64_t price = 0;
  };

  struct EventFact {
    uint32_t user_id = 0;
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t token_idx = 0;
    int32_t event_type = 0;
    int64_t realized_delta = 0;
    int64_t realized_cum = 0;
    int64_t unrealized_pnl = 0;
    int32_t token_count = 0;
    int8_t tag_id = 13;
    int64_t exposure = 0;
    int64_t volume = 0;
    int64_t holding_period = 0;
  };

  struct TokenState {
    double pos = 0.0;
    double cost = 0.0;
    double lp = 0.0;
    double entry_block = 0.0;
  };

  struct TokenKey {
    uint32_t user_id = 0;
    int32_t cond_idx = 0;
    int32_t token_idx = 0;
    bool operator==(const TokenKey &o) const {
      return cond_idx == o.cond_idx && token_idx == o.token_idx && user_id == o.user_id;
    }
  };

  struct TokenKeyHash {
    size_t operator()(const TokenKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int32_t>()(k.cond_idx) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      h ^= std::hash<int32_t>()(k.token_idx) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct AggKey {
    uint32_t user_id = 0;
    int64_t block_bucket = 0;
    int8_t tag_id = 13;
    bool operator==(const AggKey &o) const {
      return block_bucket == o.block_bucket && tag_id == o.tag_id && user_id == o.user_id;
    }
  };

  struct AggKeyHash {
    size_t operator()(const AggKey &k) const {
      size_t h = std::hash<uint32_t>()(k.user_id);
      h ^= std::hash<int64_t>()(k.block_bucket) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      h ^= std::hash<int8_t>()(k.tag_id) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      return h;
    }
  };

  using BucketAggState = feature_comp::BucketAggState;

  EventBuilder &builder_;
  Database &stage0_db_;
  Database &stage2_db_;
  Database &stage3_db_;
  std::unique_ptr<core::rocks::Stage3EventFactStore> event_fact_store_;

  mutable std::mutex sync_mu_;
  mutable Status sync_;
  mutable SyncCursorState sync_cursor_;
  struct CommitRecord {
    std::chrono::steady_clock::time_point committed_at;
    int64_t block = 0;
  };
  mutable std::deque<CommitRecord> commit_history_;

  asio::io_context *ioc_ = nullptr;
  std::shared_ptr<asio::steady_timer> timer_;
  std::atomic<bool> stop_requested_{false};

  std::vector<ConditionInfo> conditions_;
  std::vector<int8_t> cond_tag_ids_;
  std::unordered_map<std::string, int8_t> tag_to_industry_id_;

  struct UserQueryCache {
    struct Snapshot {
      int64_t sort_key = 0;
      std::vector<PositionRow> positions;
    };
    std::string addr_lower;
    std::vector<TimelineRow> timeline;
    std::vector<Snapshot> snapshots;
  };
  mutable std::mutex user_cache_mu_;
  mutable UserQueryCache user_cache_;
  struct RuntimeMemProbe {
    int64_t event_inputs_bytes = 0;
    int64_t user_blob_pool_bytes = 0;
    int64_t user_index_bytes = 0;
    int64_t token_states_bytes = 0;
    int64_t bucket_agg_bytes = 0;
    int64_t event_facts_bytes = 0;
    int64_t total_working_set_bytes = 0;
    int64_t peak_working_set_bytes = 0;
    int64_t row_count = 0;
    int64_t max_cond_idx = -1;
  };
  mutable RuntimeMemProbe runtime_mem_probe_;

  // Execution config
  static constexpr int64_t kStage3BatchEvents = 500000;
  static constexpr int64_t kBlockBucketSize = 100000;
  static constexpr double kPosEpsilon = 1e-9;
  static constexpr int64_t kMinHoldingQty = 10LL * 1000000LL;
  int base_interval_seconds_ = 0;

  // SQL identifiers (persistent tables)
  static constexpr const char *kSqlTableSyncCursorState = "sync_cursor_state";
  static constexpr const char *kSqlTableTokenState = "token_state";
  static constexpr const char *kSqlTableUserSummaryState = "user_summary_state";
  static constexpr const char *kSqlTableFeatureTensorState = "feature_tensor_state";

  // SQL identifiers (indexes) - 仅保留非PK前缀的有用索引
  static constexpr const char *kSqlIndexUserSummaryEvents = "idx_stage3_user_summary_events";
  static constexpr const char *kSqlIndexFeatureTensorBucketTagUser = "idx_stage3_feature_tensor_bucket_tag_user";

  // Normalization / key conversion helpers
  static std::string normalize_addr(const std::string &addr);
  static int64_t round_i64(double v);
  static int64_t sort_key_to_block(int64_t sort_key);
  static int64_t sort_key_to_block_bucket(int64_t sort_key);
  static int64_t bucket_end_block(int64_t block_bucket);
  static uint64_t pack_cond_token_key(int32_t cond_idx, int32_t token_idx);

  // Event / metadata predicates
  static bool is_trade_event(EventType ty);
  static bool is_usd_collateral(int32_t collateral);
  static bool is_effective_holding(double qty_1e6);
  static bool is_effective_holding_i64(int64_t qty_1e6);
  int8_t tag_name_to_id(const std::string &tag_name) const;

  // Feature calculators
  static double calc_unrealized_pnl(const TokenState &st);
  static int64_t calc_exposure_1e6(const TokenState &st);
  static int64_t calc_holding_period_blocks(int64_t current_block, const TokenState &st);
  static int64_t calc_volume_1e6(const EventInput &row);

  // Aggregation update helper
  static void update_tail_window(BucketAggState &agg,
                                 int64_t block_bucket,
                                 int64_t current_block,
                                 int64_t current_exposure,
                                 int64_t current_holding_period,
                                 int64_t current_token_count);

  // Lifecycle / sync pipeline
  void init_schema() const;
  void load_tag_mapping_from_md();
  void load_conditions();
  void load_cursor();
  void save_cursor_locked(duckdb::Connection &conn) const;
  void refresh_status_locked() const;
  void schedule_sync(int delay_seconds);
  void do_sync_tick();
  bool process_chunk_locked() const;

  // Event state transition
  double apply_event_input(const EventInput &row, TokenState &st) const;

  // Query cache build path
  std::vector<PositionRow> build_position_rows_from_states(const std::unordered_map<uint64_t, TokenState> &states) const;
  UserQueryCache build_user_query_cache(const std::string &addr_lower) const;
  void ensure_user_query_cache(const std::string &addr_lower) const;
};

} // namespace stage3
