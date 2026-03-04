#pragma once

#include "../core/database.hpp"
#include "../stage2/stage2_builder.hpp"
#include "../stage2/stage2_models.hpp"
#include "../stage2/stage2_types.hpp"

#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
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

  struct UserSummary {
    std::string addr;
    int64_t event_count = 0;
    int64_t realized_pnl = 0;
    int64_t unrealized_pnl = 0;
  };

  struct TimelineEntry {
    int64_t sort_key = 0;
    uint8_t event_type = 0;
    int64_t realized_pnl = 0;
    int64_t unrealized_pnl = 0;
    int32_t token_count = 0;
  };

  struct PositionRow {
    uint32_t cond_idx = 0;
    uint8_t token_idx = 0;
    int64_t qty = 0;
    int64_t cost = 0;
    int64_t last_price = 0;
  };

  StageSync(EventBuilder &builder, Database &stage2_db, Database &stage3_db);

  void start(asio::io_context &ioc);
  void stop();

  Status status() const;
  Stage2Data stage2_data() const;

  std::vector<UserSummary> get_users_sorted(int64_t limit = 200) const;
  std::vector<TimelineEntry> get_user_timeline(
      const std::string &addr,
      int64_t max_sort_key = std::numeric_limits<int64_t>::max()) const;
  std::vector<PositionRow> get_positions_at(const std::string &addr, int64_t sort_key) const;

private:
  struct CursorKey {
    int64_t sort_key = -1;
    std::string user_hex;
    int32_t cond_idx = std::numeric_limits<int32_t>::min();
    int32_t event_type = std::numeric_limits<int32_t>::min();
    int32_t token_idx = std::numeric_limits<int32_t>::min();
    int64_t processed_events = 0;
  };

  struct InputEvent {
    std::string user_hex;
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t event_type = 0;
    int32_t token_idx = 0;
    int32_t collateral = 0;
    int64_t amount = 0;
    int64_t price = 0;
  };

  struct TimelineEvent {
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t event_type = 0;
    int32_t token_idx = 0;
    int64_t realized_cum = 0;
    int64_t unrealized_pnl = 0;
    int32_t token_count = 0;
  };

  struct FactRow {
    std::string user_hex;
    int64_t sort_key = 0;
    int32_t cond_idx = 0;
    int32_t token_idx = 0;
    int32_t event_type = 0;
    int64_t realized_delta = 0;
    int64_t realized_cum = 0;
    int64_t unrealized_pnl = 0;
    int32_t token_count = 0;
  };

  struct CondState {
    std::array<int64_t, MAX_OUTCOMES> positions{};
    std::array<int64_t, MAX_OUTCOMES> cost{};
    std::array<int64_t, MAX_OUTCOMES> last_price{};
    int64_t realized_pnl = 0;
    int64_t unrealized_pnl = 0;
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

  static constexpr int64_t kStage3ChunkBlocks = 100000;
  static constexpr size_t kEtaWindowSize = 20;
  static constexpr int kBaseIntervalSeconds = 30;
  static constexpr int64_t kCursorSentinel = std::numeric_limits<int32_t>::min();

  static std::string normalize_addr(const std::string &addr);
  static int64_t mul_div_1e6(int64_t amount, int64_t price);
  static int64_t add_i64_checked(int64_t a, int64_t b);
  static int64_t sub_i64_checked(int64_t a, int64_t b);
  static bool is_usd_collateral(int32_t collateral);

  void init_schema() const;
  void load_conditions();
  void load_cursor();
  void save_cursor_locked(duckdb::Connection &conn) const;
  void refresh_status_locked() const;
  void schedule_sync(int delay_seconds);
  void do_sync_tick();
  bool process_chunk_locked() const;

  int64_t convert_payout_amount(const ConditionInfo &cond, int64_t qty) const;
  static bool is_trade_event(EventType ty);
  static int64_t compute_unrealized_pnl(const CondState &st);
  int64_t apply_event_to_state(const InputEvent &row, CondState &st) const;

  std::vector<TimelineEvent> load_timeline_events(const std::string &addr_lower, int64_t max_sort_key) const;
  std::vector<TimelineEntry> build_timeline(const std::vector<TimelineEvent> &events) const;
  std::unordered_map<int32_t, CondState> build_state_until(const std::string &addr_lower,
                                                           int64_t target_sort_key) const;
};

} // namespace stage3
