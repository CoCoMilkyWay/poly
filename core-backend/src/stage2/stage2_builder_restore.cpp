#include "misc/profiler.hpp"
#include "stage2_builder.hpp"

namespace stage2 {

void EventBuilder::restore_users_and_event_stats_parallel() {
  TraceN("s2/restore/rebuild_user_event_stats");
  seen_users_.clear();
  progress_.event_by_collateral.clear();
  progress_.total_events = 0;
  progress_.cnt_split = 0;
  progress_.cnt_merge = 0;
  progress_.cnt_redemption = 0;
  progress_.cnt_convert = 0;
  progress_.cnt_order = 0;
  progress_.cnt_fpmm_trade = 0;
  progress_.cnt_fpmm_funding = 0;
  progress_.cnt_transfer = 0;

  int64_t min_sort_key = 0;
  int64_t max_sort_key = 0;
  if (!user_event_store_->sort_key_bounds(min_sort_key, max_sort_key)) {
    progress_.total_users = 0;
    return;
  }

  std::unordered_map<uint16_t, int64_t> rebuilt_event_by_collateral;
  rebuilt_event_by_collateral.reserve(64);
  user_event_store_->for_each_event_brief_in_sort_key_range(
      min_sort_key, max_sort_key,
      [this, &rebuilt_event_by_collateral](std::string_view,
                                           int32_t cond_idx,
                                           int32_t event_type,
                                           int32_t collateral) {
        stage2_assert(event_type >= static_cast<int32_t>(EventType::OrderBuy) &&
                          event_type <= static_cast<int32_t>(EventType::TransferOutNonPoly),
                      AssertLevel::L0, "DB", "LoadEventTypeInRange");
        stage2_assert(collateral >= 0 &&
                          collateral <= static_cast<int32_t>(std::numeric_limits<uint8_t>::max()),
                      AssertLevel::L0, "DB", "LoadCollateralInRange");
        progress_.total_events++;
        const uint16_t key = static_cast<uint16_t>(event_type) * 256 +
                             static_cast<uint16_t>(collateral);
        rebuilt_event_by_collateral[key]++;
      });
  progress_.event_by_collateral = std::move(rebuilt_event_by_collateral);

  std::unordered_set<std::string> rebuilt_seen_user_blobs = user_event_store_->collect_distinct_users();
  seen_users_.reserve(rebuilt_seen_user_blobs.size());
  for (const auto &user_blob : rebuilt_seen_user_blobs) {
    seen_users_.insert(blob_to_hex(user_blob));
  }
  progress_.total_users = seen_users_.size();

  int64_t total_events_from_collateral = 0;
  for (const auto &[key, cnt] : progress_.event_by_collateral) {
    const int32_t event_type = static_cast<int32_t>(key / 256);
    stage2_assert(event_type >= static_cast<int32_t>(EventType::OrderBuy) &&
                      event_type <= static_cast<int32_t>(EventType::TransferOutNonPoly),
                  AssertLevel::L0, "DB", "LoadEventTypeInRange");
    bump_event_counter(static_cast<EventType>(event_type), cnt);
    total_events_from_collateral += cnt;
  }
  stage2_assert(total_events_from_collateral == progress_.total_events,
                AssertLevel::L0, "DB", "RestoreEventCountMatch");
}

} // namespace stage2
