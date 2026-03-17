#include "stage2_builder.hpp"
#include "misc/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>

namespace stage2 {

namespace {
constexpr int64_t kRestoreCacheVersion = 1;
}

EventBuilder::EventBuilder(Database &stage1_db, Database &stage2_db)
    : stage1_db_(stage1_db), stage2_db_(stage2_db),
      restore_cache_db_(std::make_unique<Database>(stage2_db_.db_path() + ".restore_cache")),
      user_event_store_(std::make_unique<core::rocks::Stage2UserEventStore>(
          stage2_db_.data_dir() + "/user_event.rocks")) {
  commit_thread_ = std::thread([this] { commit_worker_loop(); });
  refresh_memory_snapshot("constructed");
}

EventBuilder::~EventBuilder() {
  {
    std::unique_lock<std::mutex> lock(commit_mu_);
    while (commit_busy_) {
      if (commit_result_.has_value()) {
        reap_commit_result_locked();
        continue;
      }
      commit_cv_.wait(lock);
    }
    commit_stop_ = true;
  }
  commit_cv_.notify_all();
  if (commit_thread_.joinable()) {
    commit_thread_.join();
  }
}

void EventBuilder::init_schema() {
  stage2_db_.execute(R"(
    CREATE TABLE IF NOT EXISTS stage2_cursor (
      key TEXT PRIMARY KEY,
      value BIGINT
    )
  )");

  stage2_db_.execute(R"(
    CREATE TABLE IF NOT EXISTS rb_condition (
      cond_idx    INTEGER PRIMARY KEY,
      cond_id     BLOB NOT NULL UNIQUE,
      outcome_cnt INTEGER NOT NULL,
      payout_0    BIGINT,
      payout_1    BIGINT,
      payout_2    BIGINT,
      payout_3    BIGINT,
      payout_4    BIGINT,
      payout_5    BIGINT,
      payout_6    BIGINT,
      payout_7    BIGINT,
      question_id BLOB,
      source      INTEGER NOT NULL DEFAULT 0
    )
  )");

  stage2_db_.execute(R"(
    CREATE TABLE IF NOT EXISTS rb_token (
      token_id  BLOB PRIMARY KEY,
      cond_idx  INTEGER NOT NULL,
      token_idx INTEGER NOT NULL,
      source    INTEGER NOT NULL DEFAULT 0
    )
  )");

  stage2_db_.execute(R"(
    CREATE TABLE IF NOT EXISTS rb_fpmm (
      fpmm_addr    BLOB PRIMARY KEY,
      cond_idx     INTEGER NOT NULL,
      collateral   INTEGER NOT NULL DEFAULT 1
    )
  )");

  stage2_db_.execute(R"(
    CREATE TABLE IF NOT EXISTS rb_collateral (
      coll_id         INTEGER PRIMARY KEY,
      collateral_addr BLOB NOT NULL UNIQUE
    )
  )");

  stage2_db_.execute(R"(
    CREATE TABLE IF NOT EXISTS rb_cond_collateral (
      cond_idx INTEGER PRIMARY KEY,
      coll_id  INTEGER NOT NULL
    )
  )");

  stage2_db_.execute(R"(
    CREATE TABLE IF NOT EXISTS rb_neg_risk_market (
      question_id BLOB PRIMARY KEY,
      market_id   BLOB NOT NULL
    )
  )");

  auto conn = stage2_db_.create_connection();
  auto assert_table_columns = [&](const char *table_name, std::initializer_list<const char *> expected_cols) {
    auto cols = conn->Query("PRAGMA table_info(" + std::string(table_name) + ")");
    stage2_assert(cols && !cols->HasError(), AssertLevel::L0, "DB", "TableInfoQuerySuccess");
    stage2_assert(cols->RowCount() == static_cast<idx_t>(expected_cols.size()),
                  AssertLevel::L0, "DB", "TableColumnCountMatch");
    idx_t i = 0;
    for (const char *expect : expected_cols) {
      std::string got = cols->GetValue(1, i).GetValueUnsafe<std::string>();
      stage2_assert(got == expect, AssertLevel::L0, "DB", "TableColumnNameMatch");
      i++;
    }
  };

  assert_table_columns("stage2_cursor", {"key", "value"});
  assert_table_columns("rb_condition",
                       {"cond_idx", "cond_id", "outcome_cnt", "payout_0", "payout_1",
                        "payout_2", "payout_3", "payout_4", "payout_5", "payout_6",
                        "payout_7", "question_id", "source"});
  assert_table_columns("rb_token", {"token_id", "cond_idx", "token_idx", "source"});

  auto r = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
  stage2_assert(r && !r->HasError(), AssertLevel::L0, "DB", "CursorQuerySuccess");
  if (r->RowCount() == 0) {
    auto ins = conn->Query("INSERT INTO stage2_cursor(key, value) VALUES ('last_block', 0)");
    stage2_assert(ins && !ins->HasError(), AssertLevel::L0, "DB", "CursorInitInsertSuccess");
  }
}

void EventBuilder::init_restore_cache_schema() {
  auto conn = restore_cache_db_->create_connection();
  auto exec_sql = [&](const std::string &sql) {
    auto r = conn->Query(sql);
    stage2_assert(r && !r->HasError(), AssertLevel::L0, "DB", "RestoreCacheSchemaSQLSuccess");
  };
  exec_sql(R"(
    CREATE TABLE IF NOT EXISTS restore_cache_meta (
      key TEXT PRIMARY KEY,
      value BIGINT
    )
  )");
  exec_sql(R"(
    CREATE TABLE IF NOT EXISTS restore_cache_users (
      user_addr BLOB PRIMARY KEY
    )
  )");
  exec_sql(R"(
    CREATE TABLE IF NOT EXISTS restore_cache_event_by_collateral (
      event_type INTEGER NOT NULL,
      collateral INTEGER NOT NULL,
      cnt BIGINT NOT NULL,
      PRIMARY KEY (event_type, collateral)
    )
  )");
}

void EventBuilder::clear_restore_cache_locked(duckdb::Connection &conn) const {
  auto exec_sql = [&](const std::string &sql) {
    auto r = conn.Query(sql);
    stage2_assert(r && !r->HasError(), AssertLevel::L0, "DB", "RestoreCacheClearSQLSuccess");
  };
  exec_sql("DELETE FROM restore_cache_users");
  exec_sql("DELETE FROM restore_cache_event_by_collateral");
  exec_sql("DELETE FROM restore_cache_meta");
}

void EventBuilder::purge_restore_cache_db_files() {
  const std::string cache_path = stage2_db_.db_path() + ".restore_cache";
  restore_cache_db_.reset();
  auto remove_file = [&](const std::string &path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    stage2_assert(!ec, AssertLevel::L0, "DB", "RestoreCacheFileRemoveSuccess");
  };
  remove_file(cache_path);
  remove_file(cache_path + ".wal");
  remove_file(cache_path + ".lock");
  restore_cache_db_ = std::make_unique<Database>(cache_path);
}

bool EventBuilder::load_users_and_event_stats_from_cache_if_cursor_match(int64_t expected_cursor) {
  init_restore_cache_schema();
  auto conn = restore_cache_db_->create_connection();

  auto meta_r = conn->Query("SELECT key, value FROM restore_cache_meta");
  stage2_assert(meta_r && !meta_r->HasError(), AssertLevel::L0, "DB", "RestoreCacheMetaLoadSuccess");
  if (meta_r->RowCount() == 0) {
    if (expected_cursor > 0) {
      stage2_log_info("Restore cache meta empty while cursor>0, purge cache db files and fallback to rebuild");
      purge_restore_cache_db_files();
    }
    return false;
  }
  std::unordered_map<std::string, int64_t> meta;
  meta.reserve(static_cast<size_t>(meta_r->RowCount()));
  for (idx_t i = 0; i < meta_r->RowCount(); ++i) {
    std::string key = meta_r->GetValue(0, i).GetValueUnsafe<std::string>();
    int64_t value = meta_r->GetValue(1, i).GetValue<int64_t>();
    meta[key] = value;
  }

  bool meta_ready = meta.count("version") > 0 && meta.count("ready") > 0 &&
                    meta.count("cursor") > 0;
  if (!meta_ready || meta["version"] != kRestoreCacheVersion || meta["ready"] != 1 ||
      meta["cursor"] != expected_cursor) {
    stage2_log_info("Restore cache invalidated, purge cache db files and fallback to rebuild");
    purge_restore_cache_db_files();
    return false;
  }

  auto users_r = conn->Query("SELECT user_addr FROM restore_cache_users");
  stage2_assert(users_r && !users_r->HasError(), AssertLevel::L0, "DB", "RestoreCacheUsersLoadSuccess");
  seen_users_.clear();
  seen_users_.reserve(static_cast<size_t>(users_r->RowCount()));
  for (idx_t i = 0; i < users_r->RowCount(); ++i) {
    seen_users_.insert(blob_to_hex(users_r->GetValue(0, i).GetValueUnsafe<std::string>()));
  }

  auto ebc_r = conn->Query("SELECT event_type, collateral, cnt FROM restore_cache_event_by_collateral");
  stage2_assert(ebc_r && !ebc_r->HasError(), AssertLevel::L0, "DB", "RestoreCacheEventByCollateralLoadSuccess");
  progress_.event_by_collateral.clear();
  progress_.event_by_collateral.reserve(static_cast<size_t>(ebc_r->RowCount()));
  for (idx_t i = 0; i < ebc_r->RowCount(); ++i) {
    int32_t event_type = ebc_r->GetValue(0, i).GetValue<int32_t>();
    int32_t collateral = ebc_r->GetValue(1, i).GetValue<int32_t>();
    int64_t cnt = ebc_r->GetValue(2, i).GetValue<int64_t>();
    stage2_assert(event_type >= static_cast<int32_t>(EventType::OrderBuy) &&
                      event_type <= static_cast<int32_t>(EventType::TransferOutNonPoly),
                  AssertLevel::L0, "DB", "RestoreCacheEventTypeInRange");
    stage2_assert(collateral >= 0 &&
                      collateral <= static_cast<int32_t>(std::numeric_limits<uint8_t>::max()),
                  AssertLevel::L0, "DB", "RestoreCacheCollateralInRange");
    stage2_assert(cnt >= 0, AssertLevel::L0, "DB", "RestoreCacheCountNonNegative");
    uint16_t key = static_cast<uint16_t>(event_type) * 256 + static_cast<uint16_t>(collateral);
    progress_.event_by_collateral[key] += cnt;
  }

  progress_.total_events = 0;
  progress_.total_users = static_cast<int64_t>(seen_users_.size());
  progress_.cnt_split = 0;
  progress_.cnt_merge = 0;
  progress_.cnt_redemption = 0;
  progress_.cnt_convert = 0;
  progress_.cnt_order = 0;
  progress_.cnt_fpmm_trade = 0;
  progress_.cnt_fpmm_funding = 0;
  progress_.cnt_transfer = 0;
  for (const auto &[key, cnt] : progress_.event_by_collateral) {
    stage2_assert(cnt >= 0, AssertLevel::L0, "DB", "RestoreCacheCountNonNegative");
    int32_t event_type = static_cast<int32_t>(key / 256);
    stage2_assert(event_type >= static_cast<int32_t>(EventType::OrderBuy) &&
                      event_type <= static_cast<int32_t>(EventType::TransferOutNonPoly),
                  AssertLevel::L0, "DB", "RestoreCacheEventTypeInRange");
    bump_event_counter(static_cast<EventType>(event_type), cnt);
    progress_.total_events += cnt;
  }

  if (meta.count("total_events") > 0) {
    stage2_assert(progress_.total_events == meta["total_events"],
                  AssertLevel::L0, "DB", "RestoreCacheTotalEventsMatch");
  }
  if (meta.count("total_users") > 0) {
    stage2_assert(progress_.total_users == meta["total_users"],
                  AssertLevel::L0, "DB", "RestoreCacheTotalUsersMatch");
  }
  stage2_log_info("Restore cache hit at cursor " + std::to_string(expected_cursor));
  return true;
}

void EventBuilder::load_from_rb() {
  TraceN("s2/restore_load_from_rb");
  const auto restore_started_at = std::chrono::steady_clock::now();
  auto conn = stage2_db_.create_connection();
  auto blob_hex = [](const duckdb::Value &v) {
    return blob_to_hex(v.GetValueUnsafe<std::string>());
  };
  auto blob_hex_lower = [&](const duckdb::Value &v) {
    return to_lower(blob_hex(v));
  };
  progress_ = BuildProgress{};
  committed_progress_ = BuildProgress{};

  conditions_.clear();
  cond_ids_.clear();
  cond_map_.clear();
  token_map_.clear();
  fpmm_map_.clear();
  cond_to_market_.clear();
  seen_users_.clear();
  fpmm_cond_idxs_.clear();
  negrisk_cond_idxs_.clear();
  cond_collateral_.clear();
  new_condition_pos_.clear();
  new_token_pos_.clear();

  collateral_addr_to_id_.clear();
  collateral_id_to_addr_.clear();
  next_collateral_id_ = static_cast<uint8_t>(Collateral::WrappedUSDCe) + 1;
  // 预置已知抵押品,保持固定 ID 和稳定编码
  collateral_addr_to_id_[ZERO_ADDR] = static_cast<uint8_t>(Collateral::Unknown);
  collateral_id_to_addr_[static_cast<uint8_t>(Collateral::Unknown)] = ZERO_ADDR;
  collateral_addr_to_id_[USDC_NATIVE] = static_cast<uint8_t>(Collateral::USDC);
  collateral_id_to_addr_[static_cast<uint8_t>(Collateral::USDC)] = USDC_NATIVE;
  collateral_addr_to_id_[USDC_E] = static_cast<uint8_t>(Collateral::USDCe);
  collateral_id_to_addr_[static_cast<uint8_t>(Collateral::USDCe)] = USDC_E;
  collateral_addr_to_id_[USDT] = static_cast<uint8_t>(Collateral::USDT);
  collateral_id_to_addr_[static_cast<uint8_t>(Collateral::USDT)] = USDT;
  collateral_addr_to_id_[WRAPPED_USDC_E] = static_cast<uint8_t>(Collateral::WrappedUSDCe);
  collateral_id_to_addr_[static_cast<uint8_t>(Collateral::WrappedUSDCe)] = WRAPPED_USDC_E;

  {
    TraceN("s2/restore/load_collateral");
    auto coll_r = conn->Query("SELECT coll_id, collateral_addr FROM rb_collateral");
    stage2_assert(coll_r && !coll_r->HasError(), AssertLevel::L0, "DB", "LoadCollateralSuccess");
    collateral_addr_to_id_.reserve(collateral_addr_to_id_.size() + static_cast<size_t>(coll_r->RowCount()));
    collateral_id_to_addr_.reserve(collateral_id_to_addr_.size() + static_cast<size_t>(coll_r->RowCount()));
    for (idx_t i = 0; i < coll_r->RowCount(); ++i) {
      uint8_t coll_id = static_cast<uint8_t>(coll_r->GetValue(0, i).GetValue<int32_t>());
      std::string addr = blob_hex_lower(coll_r->GetValue(1, i));
      collateral_addr_to_id_[addr] = coll_id;
      collateral_id_to_addr_[coll_id] = addr;
      if (coll_id >= next_collateral_id_) {
        next_collateral_id_ = coll_id + 1;
      }
    }
  }

  {
    TraceN("s2/restore/load_cursor");
    auto cur = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
    stage2_assert(cur && !cur->HasError(), AssertLevel::L0, "DB", "CursorLoadSuccess");
    progress_.cursor = cur->RowCount() > 0 ? cur->GetValue(0, 0).GetValue<int64_t>() : 0;
  }

  {
    TraceN("s2/restore/load_condition");
    auto cond_r = conn->Query("SELECT cond_idx, cond_id, outcome_cnt, "
                              "payout_0, payout_1, payout_2, payout_3, "
                              "payout_4, payout_5, payout_6, payout_7, question_id, source FROM rb_condition ORDER BY cond_idx");
    stage2_assert(cond_r && !cond_r->HasError(), AssertLevel::L0, "DB", "LoadConditionSuccess");
    conditions_.reserve(static_cast<size_t>(cond_r->RowCount()));
    cond_ids_.reserve(static_cast<size_t>(cond_r->RowCount()));
    for (idx_t i = 0; i < cond_r->RowCount(); ++i) {
      uint32_t idx = cond_r->GetValue(0, i).GetValue<uint32_t>();
      std::string cond_id = blob_hex(cond_r->GetValue(1, i));
      ConditionInfo info;
      info.outcome_count = cond_r->GetValue(2, i).GetValue<uint8_t>();
      info.payout_numerators.reserve(info.outcome_count);
      for (int j = 0; j < info.outcome_count; ++j) {
        auto v = cond_r->GetValue(3 + j, i);
        info.payout_numerators.push_back(v.IsNull() ? -1 : v.GetValue<int64_t>());
      }
      auto qid_v = cond_r->GetValue(11, i);
      if (!qid_v.IsNull()) {
        info.question_id = blob_hex_lower(qid_v);
      }
      info.source = static_cast<ConditionSource>(cond_r->GetValue(12, i).GetValue<int>());
      while (conditions_.size() <= idx) {
        conditions_.emplace_back();
        cond_ids_.emplace_back();
      }
      conditions_[idx] = info;
      cond_ids_[idx] = cond_id;
      cond_map_[to_lower(cond_id)] = idx;
    }
  }

  {
    TraceN("s2/restore/load_token");
    auto token_r = conn->Query("SELECT token_id, cond_idx, token_idx, source FROM rb_token");
    stage2_assert(token_r && !token_r->HasError(), AssertLevel::L0, "DB", "LoadTokenSuccess");
    token_map_.reserve(static_cast<size_t>(token_r->RowCount()));
    for (idx_t i = 0; i < token_r->RowCount(); ++i) {
      std::string tid = blob_hex(token_r->GetValue(0, i));
      TokenInfo info;
      int32_t db_cond_idx = token_r->GetValue(1, i).GetValue<int32_t>();
      info.cond_idx = (db_cond_idx == -1) ? UNKNOWN_COND_IDX : static_cast<uint32_t>(db_cond_idx);
      info.token_idx = token_r->GetValue(2, i).GetValue<uint8_t>();
      info.source = static_cast<TokenSource>(token_r->GetValue(3, i).GetValue<int>());
      token_map_[to_lower(tid)] = info;
    }
  }

  {
    TraceN("s2/restore/load_condition_collateral");
    auto cond_coll_r = conn->Query("SELECT cond_idx, coll_id FROM rb_cond_collateral");
    stage2_assert(cond_coll_r && !cond_coll_r->HasError(), AssertLevel::L0, "DB", "LoadConditionCollateralSuccess");
    cond_collateral_.reserve(static_cast<size_t>(cond_coll_r->RowCount()));
    for (idx_t i = 0; i < cond_coll_r->RowCount(); ++i) {
      uint32_t cond_idx = cond_coll_r->GetValue(0, i).GetValue<uint32_t>();
      uint8_t coll_id = static_cast<uint8_t>(cond_coll_r->GetValue(1, i).GetValue<int32_t>());
      cond_collateral_[cond_idx] = coll_id;
    }
  }

  {
    TraceN("s2/restore/load_fpmm");
    auto fpmm_r = conn->Query("SELECT fpmm_addr, cond_idx, collateral FROM rb_fpmm");
    stage2_assert(fpmm_r && !fpmm_r->HasError(), AssertLevel::L0, "DB", "LoadFPMMSuccess");
    fpmm_map_.reserve(static_cast<size_t>(fpmm_r->RowCount()));
    for (idx_t i = 0; i < fpmm_r->RowCount(); ++i) {
      std::string addr = blob_hex(fpmm_r->GetValue(0, i));
      FPMMInfo info;
      info.cond_idx = fpmm_r->GetValue(1, i).GetValue<uint32_t>();
      info.collateral = static_cast<uint8_t>(fpmm_r->GetValue(2, i).GetValue<int32_t>());
      fpmm_map_[to_lower(addr)] = info;
      fpmm_cond_idxs_.insert(info.cond_idx);
      if (!cond_collateral_.count(info.cond_idx)) {
        cond_collateral_[info.cond_idx] = info.collateral;
      }
    }
  }

  {
    TraceN("s2/restore/load_neg_risk_market");
    // 从 rb_neg_risk_market 表加载 question_id -> market_id 映射,并标记 NegRisk 条件
    auto nrm_r = conn->Query("SELECT question_id, market_id FROM rb_neg_risk_market");
    stage2_assert(nrm_r && !nrm_r->HasError(), AssertLevel::L0, "DB", "LoadNegRiskMarketSuccess");
    cond_to_market_.reserve(static_cast<size_t>(nrm_r->RowCount()));
    for (idx_t i = 0; i < nrm_r->RowCount(); ++i) {
      std::string question_id = blob_hex_lower(nrm_r->GetValue(0, i));
      std::string market_id = blob_hex_lower(nrm_r->GetValue(1, i));
      cond_to_market_[question_id] = market_id;

      // 计算 conditionId 并标记对应条件为 NegRisk
      auto oracle_bytes = hex_to_blob(NEG_RISK_ADAPTER);
      auto qid_bytes = hex_to_blob(question_id);
      std::string input(84, '\0');
      std::memcpy(input.data(), oracle_bytes.data(), std::min(size_t(20), oracle_bytes.size()));
      std::memcpy(input.data() + 20, qid_bytes.data(), std::min(size_t(32), qid_bytes.size()));
      input[83] = 2; // outcomeSlotCount = 2
      auto cond_hash = crypto::keccak256(input);
      std::string cond_id = to_lower(crypto::Keccak256::to_hex(cond_hash));

      auto it = cond_map_.find(cond_id);
      if (it != cond_map_.end()) {
        negrisk_cond_idxs_.insert(it->second);
      }
    }
  }

  progress_.total_conditions = conditions_.size();
  progress_.total_tokens = token_map_.size();
  update_cond_type_stats();

  bool loaded_user_stats_cache = false;
  {
    TraceN("s2/restore/load_user_event_stats_cache");
    loaded_user_stats_cache =
        load_users_and_event_stats_from_cache_if_cursor_match(progress_.cursor);
  }
  if (!loaded_user_stats_cache) {
    TraceN("s2/restore/rebuild_user_event_stats");
    // 从 user_event.rocks 重建用户集合与事件统计(并行分段扫描).
    restore_users_and_event_stats_parallel();
  }

  {
    TraceN("s2/restore/load_transfer_snapshot");
    // 恢复 TransferStats(包含 internal 类,无法从 user_event 反推)
    auto xfer_kv = conn->Query("SELECT key, value FROM stage2_cursor WHERE key LIKE 'xfer_%'");
    stage2_assert(xfer_kv && !xfer_kv->HasError(), AssertLevel::L0, "DB", "LoadTransferSnapshotSuccess");
    std::unordered_map<std::string, int64_t> xfer_saved;
    xfer_saved.reserve(static_cast<size_t>(xfer_kv->RowCount()));
    for (idx_t i = 0; i < xfer_kv->RowCount(); ++i) {
      std::string key = xfer_kv->GetValue(0, i).GetValueUnsafe<std::string>();
      int64_t value = xfer_kv->GetValue(1, i).GetValue<int64_t>();
      xfer_saved[key] = value;
    }
    stage2_assert(!(progress_.cursor > 0 && progress_.total_events > 0 &&
                    xfer_saved.count("xfer_total") == 0),
                  AssertLevel::L0, "DB", "TransferStatsSnapshotPersisted",
                  "stage2 db missing persisted xfer stats snapshot; rebuild stage2 once");
    if (xfer_saved.count("xfer_total") > 0) {
      auto load = [&](const char *key) -> int64_t {
        auto it = xfer_saved.find(key);
        return it == xfer_saved.end() ? 0 : it->second;
      };
      auto &xs = progress_.xfer_stats;
      static const std::pair<const char *, int64_t TransferStats::*> kXferFields[] = {
          {"xfer_total", &TransferStats::total},
          {"xfer_split_normal", &TransferStats::split_normal},
          {"xfer_split_negrisk", &TransferStats::split_negrisk},
          {"xfer_split_non_poly", &TransferStats::split_non_poly},
          {"xfer_merge_normal", &TransferStats::merge_normal},
          {"xfer_merge_negrisk", &TransferStats::merge_negrisk},
          {"xfer_merge_non_poly", &TransferStats::merge_non_poly},
          {"xfer_redemption", &TransferStats::redemption},
          {"xfer_redemption_non_poly", &TransferStats::redemption_non_poly},
          {"xfer_convert", &TransferStats::convert},
          {"xfer_order_buy", &TransferStats::order_buy},
          {"xfer_order_sell", &TransferStats::order_sell},
          {"xfer_fpmm_buy", &TransferStats::fpmm_buy},
          {"xfer_fpmm_sell", &TransferStats::fpmm_sell},
          {"xfer_lp_add", &TransferStats::fpmm_lp_add},
          {"xfer_lp_remove", &TransferStats::fpmm_lp_remove},
          {"xfer_lp_return", &TransferStats::fpmm_lp_return},
          {"xfer_transfer_in_negrisk", &TransferStats::transfer_in_negrisk},
          {"xfer_transfer_in_other", &TransferStats::transfer_in_other},
          {"xfer_transfer_in_non_poly", &TransferStats::transfer_in_non_poly},
          {"xfer_transfer_out_negrisk", &TransferStats::transfer_out_negrisk},
          {"xfer_transfer_out_other", &TransferStats::transfer_out_other},
          {"xfer_transfer_out_non_poly", &TransferStats::transfer_out_non_poly},
          {"xfer_internal_mint_negrisk", &TransferStats::internal_mint_negrisk},
          {"xfer_internal_mint_fpmm", &TransferStats::internal_mint_fpmm},
          {"xfer_internal_burn_negrisk", &TransferStats::internal_burn_negrisk},
          {"xfer_internal_burn_fpmm", &TransferStats::internal_burn_fpmm},
          {"xfer_internal_burn_convert", &TransferStats::internal_burn_convert},
          {"xfer_internal_transfer_zero", &TransferStats::internal_transfer_zero},
          {"xfer_internal_transfer_order", &TransferStats::internal_transfer_order},
          {"xfer_internal_transfer_negrisk", &TransferStats::internal_transfer_negrisk},
          {"xfer_internal_transfer_fpmm", &TransferStats::internal_transfer_fpmm},
          {"xfer_internal_transfer_other", &TransferStats::internal_transfer_other},
          {"xfer_unclassified", &TransferStats::unclassified},
      };
      for (const auto &[key, field] : kXferFields) {
        xs.*field = load(key);
      }
      xs.verify();
    }
  }

  {
    TraceN("s2/restore/load_semantic_snapshot");
    auto sem_kv = conn->Query("SELECT key, value FROM stage2_cursor WHERE key LIKE 'sem_%'");
    stage2_assert(sem_kv && !sem_kv->HasError(), AssertLevel::L0, "DB", "LoadSemanticSnapshotSuccess");
    std::unordered_map<std::string, int64_t> sem_saved;
    sem_saved.reserve(static_cast<size_t>(sem_kv->RowCount()));
    for (idx_t i = 0; i < sem_kv->RowCount(); ++i) {
      std::string key = sem_kv->GetValue(0, i).GetValueUnsafe<std::string>();
      int64_t value = sem_kv->GetValue(1, i).GetValue<int64_t>();
      sem_saved[key] = value;
    }
    auto load = [&](const char *key) -> int64_t {
      auto it = sem_saved.find(key);
      return it == sem_saved.end() ? 0 : it->second;
    };
    auto &sst = progress_.split_sem_tree;
    sst.total = load("sem_split_total");
    sst.amount_zero = load("sem_split_amount_zero");
    sst.amount_positive = load("sem_split_amount_positive");
    sst.parent_root = load("sem_split_parent_root");
    sst.parent_nested = load("sem_split_parent_nested");
    sst.partition_single = load("sem_split_partition_single");
    sst.partition_multi = load("sem_split_partition_multi");
    sst.observed_leg = load("sem_split_observed_leg");
    sst.consumed = load("sem_split_consumed");
    sst.covered_by_parent = load("sem_split_covered_by_parent");
    sst.unobserved_leg = load("sem_split_unobserved_leg");

    auto &mst = progress_.merge_sem_tree;
    mst.total = load("sem_merge_total");
    mst.amount_zero = load("sem_merge_amount_zero");
    mst.amount_positive = load("sem_merge_amount_positive");
    mst.parent_root = load("sem_merge_parent_root");
    mst.parent_nested = load("sem_merge_parent_nested");
    mst.partition_single = load("sem_merge_partition_single");
    mst.partition_multi = load("sem_merge_partition_multi");
    mst.observed_leg = load("sem_merge_observed_leg");
    mst.consumed = load("sem_merge_consumed");
    mst.covered_by_parent = load("sem_merge_covered_by_parent");
    mst.unobserved_leg = load("sem_merge_unobserved_leg");

    auto &cst = progress_.convert_sem_tree;
    cst.total = load("sem_convert_total");
    cst.amount_zero = load("sem_convert_amount_zero");
    cst.amount_positive = load("sem_convert_amount_positive");
    cst.by_question_count.clear();
    static const std::string kConvertQPrefix = "sem_convert_qcnt_";
    for (const auto &[key, value] : sem_saved) {
      if (key.rfind(kConvertQPrefix, 0) != 0) {
        continue;
      }
      std::string suffix = key.substr(kConvertQPrefix.size());
      if (suffix == "unknown") {
        cst.by_question_count[-1] += value;
        continue;
      }
      int64_t qcnt = std::stoll(suffix);
      cst.by_question_count[qcnt] += value;
    }
    cst.consumed = load("sem_convert_consumed");

    auto &ost = progress_.order_sem_tree;
    ost.total = load("sem_order_total");
    ost.maker_buy = load("sem_order_maker_buy");
    ost.maker_sell = load("sem_order_maker_sell");
    ost.token_zero = load("sem_order_token_zero");
    ost.token_positive = load("sem_order_token_positive");
    ost.quote_zero = load("sem_order_quote_zero");
    ost.quote_positive = load("sem_order_quote_positive");
    ost.observed_leg = load("sem_order_observed_leg");
    ost.consumed = load("sem_order_consumed");
    ost.unobserved_leg = load("sem_order_unobserved_leg");

    auto &cov = progress_.cond_tree.coverage;
    cov.raw_rows = load("sem_cond_cov_raw_rows");
    cov.raw_has_question_id = load("sem_cond_cov_raw_has_question_id");
    cov.raw_no_question_id = load("sem_cond_cov_raw_no_question_id");
    cov.raw_by_outcome_count.clear();
    static const std::string kRawOutcomePrefix = "sem_cond_cov_raw_outcome_";
    for (const auto &[key, value] : sem_saved) {
      if (key.rfind(kRawOutcomePrefix, 0) != 0) {
        continue;
      }
      int64_t k = std::stoll(key.substr(kRawOutcomePrefix.size()));
      cov.raw_by_outcome_count[k] += value;
    }
  }
  {
    TraceN("s2/restore/finalize_stats");
    update_cond_type_stats();

    if (progress_.cursor > 0)
      progress_.phase = 3;

    committed_progress_ = progress_;
    build_cursor_ = committed_progress_.cursor;
  }
  const auto restore_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - restore_started_at)
          .count() *
      0.001 / 60;

  stage2_log_info("Restored: " + std::to_string(conditions_.size()) + " conditions, " +
                  std::to_string(token_map_.size()) + " tokens, " +
                  std::to_string(fpmm_map_.size()) + " FPMMs, " +
                  std::to_string(restore_ms) + " min");
  refresh_memory_snapshot("load_from_rb_done");
}

void EventBuilder::reap_commit_result_locked() {
  stage2_assert(commit_result_.has_value(), AssertLevel::L0, "State", "CommitResultReady");
  committed_progress_ = *commit_result_;
  commit_result_.reset();
  commit_busy_ = false;
  commit_pending_.store(false, std::memory_order_release);
}

void EventBuilder::commit_worker_loop() {
  TraceThread("Stage2-Commit");
  for (;;) {
    CommitPayload payload;
    {
      std::unique_lock<std::mutex> lock(commit_mu_);
      commit_cv_.wait(lock, [this] { return commit_stop_ || commit_payload_.has_value(); });
      if (commit_stop_ && !commit_payload_.has_value()) {
        return;
      }
      stage2_assert(commit_payload_.has_value(), AssertLevel::L0, "State", "CommitPayloadReady");
      payload = std::move(*commit_payload_);
      commit_payload_.reset();
    }
    BuildProgress committed;
    {
      TraceN("s2/commit");
      committed = commit_chunk(std::move(payload));
    }
    {
      std::lock_guard<std::mutex> lock(commit_mu_);
      stage2_assert(!commit_result_.has_value(), AssertLevel::L0, "State", "CommitResultSlotEmpty");
      commit_result_ = std::move(committed);
      commit_reusable_payload_ = std::move(payload);
    }
    commit_cv_.notify_all();
  }
}

int64_t EventBuilder::cursor() const { return committed_progress_.cursor; }

const core::rocks::Stage2UserEventStore &EventBuilder::user_event_store() const {
  return *user_event_store_;
}

bool EventBuilder::is_building() const {
  return build_running_.load(std::memory_order_acquire);
}

bool EventBuilder::has_pending_commit() const {
  return commit_pending_.load(std::memory_order_acquire);
}

json EventBuilder::rocksdb_memory_breakdown() const {
  const core::rocks::MemoryStats stats = user_event_store_->memory_stats();
  return {
      {"name", "stage2_user_event"},
      {"engine", "rocksdb"},
      {"path", user_event_store_->db_path()},
      {"memtables_bytes", stats.memtables_bytes},
      {"table_readers_bytes", stats.table_readers_bytes},
      {"block_cache_bytes", stats.block_cache_bytes},
      {"block_cache_pinned_bytes", stats.block_cache_pinned_bytes},
      {"estimated_total_bytes", stats.estimated_total_bytes()},
  };
}

void EventBuilder::request_stop() { stop_requested_ = true; }

void EventBuilder::clear_stop() { stop_requested_ = false; }

void EventBuilder::persist_restore_cache_snapshot() {
  wait_for_pending_commit();

  auto main_conn = stage2_db_.create_connection();
  auto main_cursor_r = main_conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
  stage2_assert(main_cursor_r && !main_cursor_r->HasError(), AssertLevel::L0, "DB", "MainCursorReadForCacheSuccess");
  stage2_assert(main_cursor_r->RowCount() > 0, AssertLevel::L0, "DB", "MainCursorExistsForCache");
  int64_t main_cursor = main_cursor_r->GetValue(0, 0).GetValue<int64_t>();

  stage2_assert(main_cursor == committed_progress_.cursor,
                AssertLevel::L0, "State", "MainCursorMatchesCommittedCursor");
  stage2_assert(committed_progress_.total_users == static_cast<int64_t>(seen_users_.size()),
                AssertLevel::L0, "State", "CommittedTotalUsersMatchesSeenUsers");

  init_restore_cache_schema();
  auto conn = restore_cache_db_->create_connection();
  auto exec_sql = [&](const std::string &sql) {
    auto r = conn->Query(sql);
    stage2_assert(r && !r->HasError(), AssertLevel::L0, "DB", "PersistRestoreCacheSQLSuccess");
  };
  auto append_blob = [](duckdb::Appender &ap, const std::string &hex) {
    std::string b = hex_to_blob(hex);
    ap.Append(duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(b.data()), b.size()));
  };

  exec_sql("BEGIN TRANSACTION");
  clear_restore_cache_locked(*conn);
  {
    duckdb::Appender ap(*conn, "restore_cache_users");
    for (const auto &user_hex : seen_users_) {
      ap.BeginRow();
      append_blob(ap, user_hex);
      ap.EndRow();
    }
    ap.Close();
  }
  {
    duckdb::Appender ap(*conn, "restore_cache_event_by_collateral");
    for (const auto &[key, cnt] : committed_progress_.event_by_collateral) {
      stage2_assert(cnt >= 0, AssertLevel::L0, "DB", "PersistRestoreCacheCountNonNegative");
      int32_t event_type = static_cast<int32_t>(key / 256);
      int32_t collateral = static_cast<int32_t>(key % 256);
      stage2_assert(event_type >= static_cast<int32_t>(EventType::OrderBuy) &&
                        event_type <= static_cast<int32_t>(EventType::TransferOutNonPoly),
                    AssertLevel::L0, "DB", "PersistRestoreCacheEventTypeInRange");
      stage2_assert(collateral >= 0 &&
                        collateral <= static_cast<int32_t>(std::numeric_limits<uint8_t>::max()),
                    AssertLevel::L0, "DB", "PersistRestoreCacheCollateralInRange");
      ap.BeginRow();
      ap.Append(event_type);
      ap.Append(collateral);
      ap.Append(cnt);
      ap.EndRow();
    }
    ap.Close();
  }
  {
    duckdb::Appender ap(*conn, "restore_cache_meta");
    auto save_meta = [&](const char *key, int64_t value) {
      ap.BeginRow();
      ap.Append(duckdb::Value(std::string(key)));
      ap.Append(value);
      ap.EndRow();
    };
    save_meta("version", kRestoreCacheVersion);
    save_meta("ready", 1);
    save_meta("cursor", main_cursor);
    save_meta("total_events", committed_progress_.total_events);
    save_meta("total_users", committed_progress_.total_users);
    ap.Close();
  }
  exec_sql("COMMIT");
  exec_sql("CHECKPOINT");
  stage2_log_info("Persisted restore cache at cursor " + std::to_string(main_cursor));
}

void EventBuilder::wait_for_pending_commit() {
  std::unique_lock<std::mutex> lock(commit_mu_);
  while (commit_busy_) {
    if (commit_result_.has_value()) {
      reap_commit_result_locked();
      continue;
    }
    commit_cv_.wait(lock);
  }
  if (commit_result_.has_value()) {
    reap_commit_result_locked();
  }
}

bool EventBuilder::build_chunk(int64_t target_block) {
  if (stop_requested_) {
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(commit_mu_);
    if (commit_result_.has_value()) {
      reap_commit_result_locked();
    }
  }
  if (build_cursor_ >= target_block) {
    return false;
  }
  build_running_.store(true, std::memory_order_release);

  int64_t chunk_start = build_cursor_;
  int64_t chunk_end = target_block;
  progress_.target = target_block;
  progress_.chunk_start = chunk_start;
  progress_.chunk_end = chunk_end;
  progress_.running = true;

  new_conditions_.clear();
  new_tokens_.clear();
  new_condition_pos_.clear();
  new_token_pos_.clear();
  new_fpmms_.clear();
  new_collaterals_.clear();
  new_cond_collaterals_.clear();
  new_neg_risk_markets_.clear();
  new_events_.clear();

  tx_split_.clear();
  tx_merge_.clear();
  tx_redemption_.clear();
  tx_convert_.clear();
  tx_order_.clear();
  tx_convert_by_tx_.clear();
  tx_order_by_amount_.clear();
  tx_split_by_actor_amount_.clear();
  tx_merge_by_actor_amount_.clear();
  tx_redemption_by_actor_.clear();
  tx_fpmm_trade_by_leg_.clear();
  tx_fpmm_trade_.clear();
  tx_fpmm_funding_.clear();
  tx_op_bounds_.clear();
  chunk_token_known_visible_from_sort_.clear();
  chunk_fpmm_visible_from_sort_.clear();
  chunk_negrisk_visible_from_sort_.clear();
  chunk_xfer_stats_ = {};
  chunk_split_sem_tree_ = {};
  chunk_merge_sem_tree_ = {};
  chunk_convert_sem_tree_ = {};
  chunk_order_sem_tree_ = {};
  refresh_memory_snapshot("chunk_start_cleared");

  // 开始 chunk log
  chunk_log_.begin(log_dir_, chunk_start, chunk_end);

  progress_.phase = 1;
  phase1_update_mappings(chunk_start, chunk_end);
  if (stop_requested_) {
    progress_.running = false;
    build_running_.store(false, std::memory_order_release);
    chunk_log_.finish();
    return false;
  }

  // 写入 log header(phase1 之后,此时 token_map 已更新)
  chunk_log_.write_header(token_map_.size(), fpmm_map_.size(), cond_map_.size());
  if (!token_map_.empty()) {
    auto it = token_map_.begin();
    chunk_log_.write_token_sample(it->first, it->second.cond_idx, it->second.token_idx);
  }

  progress_.phase = 2;
  phase2_build_semantic_index(chunk_start, chunk_end);
  refresh_memory_snapshot("phase2_semantic_done");
  if (stop_requested_) {
    progress_.running = false;
    build_running_.store(false, std::memory_order_release);
    chunk_log_.finish();
    refresh_memory_snapshot("stopped_after_phase2");
    return false;
  }

  progress_.phase = 3;
  phase3_process_transfers(chunk_start, chunk_end);
  refresh_memory_snapshot("phase3_transfer_done");
  if (stop_requested_) {
    progress_.running = false;
    build_running_.store(false, std::memory_order_release);
    chunk_log_.finish();
    refresh_memory_snapshot("stopped_after_phase3");
    return false;
  }

  // 验证 transfer 分类完整性
  chunk_xfer_stats_.verify();
  progress_.xfer_stats += chunk_xfer_stats_;
  progress_.split_sem_tree += chunk_split_sem_tree_;
  progress_.merge_sem_tree += chunk_merge_sem_tree_;
  progress_.convert_sem_tree += chunk_convert_sem_tree_;
  progress_.order_sem_tree += chunk_order_sem_tree_;
  progress_.cursor = chunk_end;
  progress_.running = false;
  build_cursor_ = chunk_end;

  CommitPayload payload;
  {
    std::unique_lock<std::mutex> lock(commit_mu_);
    if (commit_reusable_payload_.has_value()) {
      payload = std::move(*commit_reusable_payload_);
      commit_reusable_payload_.reset();
    }
  }
  payload.new_conditions.clear();
  payload.new_tokens.clear();
  payload.new_fpmms.clear();
  payload.new_collaterals.clear();
  payload.new_cond_collaterals.clear();
  payload.new_neg_risk_markets.clear();
  payload.new_events.clear();
  payload.new_cursor = chunk_end;
  payload.progress = progress_;
  payload.new_conditions.swap(new_conditions_);
  payload.new_tokens.swap(new_tokens_);
  payload.new_fpmms.swap(new_fpmms_);
  payload.new_collaterals.swap(new_collaterals_);
  payload.new_cond_collaterals.swap(new_cond_collaterals_);
  payload.new_neg_risk_markets.swap(new_neg_risk_markets_);
  payload.new_events.swap(new_events_);
  {
    std::unique_lock<std::mutex> lock(commit_mu_);
    // Note: wait_for_pending_commit() should be called before build_chunk() to ensure
    // the previous commit is done. Here we just assert the slot is free.
    stage2_assert(!commit_busy_, AssertLevel::L0, "State", "CommitSlotFreeBeforeSubmit");
    stage2_assert(!commit_payload_.has_value(), AssertLevel::L0, "State", "CommitQueueEmpty");
    commit_payload_ = std::move(payload);
    commit_busy_ = true;
    commit_pending_.store(true, std::memory_order_release);
  }
  build_running_.store(false, std::memory_order_release);
  commit_cv_.notify_all();

  // 记录统计信息并结束 chunk log
  chunk_log_.set_xfer_stats(TransferStats::format_log(chunk_xfer_stats_, progress_.xfer_stats));
  chunk_log_.finish();
  refresh_memory_snapshot("chunk_committed");
  return true;
}

const BuildProgress &EventBuilder::progress() const { return progress_; }
const BuildProgress &EventBuilder::committed_progress() const { return committed_progress_; }

} // namespace stage2
