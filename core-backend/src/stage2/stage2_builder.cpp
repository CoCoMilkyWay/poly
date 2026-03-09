#include "stage2_builder.hpp"
#include "misc/profiler.hpp"

#include <algorithm>
#include <cstring>

namespace stage2 {

EventBuilder::EventBuilder(Database &stage1_db, Database &stage2_db)
    : stage1_db_(stage1_db), stage2_db_(stage2_db),
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

void EventBuilder::load_from_rb() {
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

  auto cur = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
  stage2_assert(cur && !cur->HasError(), AssertLevel::L0, "DB", "CursorLoadSuccess");
  progress_.cursor = cur->RowCount() > 0 ? cur->GetValue(0, 0).GetValue<int64_t>() : 0;

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

  auto cond_coll_r = conn->Query("SELECT cond_idx, coll_id FROM rb_cond_collateral");
  stage2_assert(cond_coll_r && !cond_coll_r->HasError(), AssertLevel::L0, "DB", "LoadConditionCollateralSuccess");
  cond_collateral_.reserve(static_cast<size_t>(cond_coll_r->RowCount()));
  for (idx_t i = 0; i < cond_coll_r->RowCount(); ++i) {
    uint32_t cond_idx = cond_coll_r->GetValue(0, i).GetValue<uint32_t>();
    uint8_t coll_id = static_cast<uint8_t>(cond_coll_r->GetValue(1, i).GetValue<int32_t>());
    cond_collateral_[cond_idx] = coll_id;
  }

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

  progress_.total_conditions = conditions_.size();
  progress_.total_tokens = token_map_.size();
  update_cond_type_stats();

  // 恢复已知用户与 user_event 统计(来自 RocksDB)
  auto user_blobs = user_event_store_->collect_distinct_users();
  seen_users_.reserve(user_blobs.size());
  for (const auto &user_blob : user_blobs) {
    seen_users_.insert(to_lower(blob_to_hex(user_blob)));
  }
  progress_.total_users = seen_users_.size();

  progress_.event_by_collateral.clear();
  user_event_store_->for_each_event([&](const core::rocks::Stage2UserEventRecord &row) {
    stage2_assert(row.event_type >= static_cast<int32_t>(EventType::OrderBuy) &&
                      row.event_type <= static_cast<int32_t>(EventType::TransferOutNonPoly),
                  AssertLevel::L0, "DB", "LoadEventTypeInRange");
    stage2_assert(row.collateral >= 0 &&
                      row.collateral <= static_cast<int32_t>(std::numeric_limits<uint8_t>::max()),
                  AssertLevel::L0, "DB", "LoadCollateralInRange");
    bump_event_counter(static_cast<EventType>(row.event_type), 1);
    progress_.total_events++;

    uint8_t effective_collateral = static_cast<uint8_t>(row.collateral);
    if (effective_collateral == static_cast<uint8_t>(Collateral::Unknown) && row.cond_idx >= 0) {
      uint32_t cond_idx = static_cast<uint32_t>(row.cond_idx);
      auto coll_it = cond_collateral_.find(cond_idx);
      if (coll_it != cond_collateral_.end()) {
        effective_collateral = coll_it->second;
      } else if (cond_idx < conditions_.size()) {
        const std::string &qid = conditions_[cond_idx].question_id;
        if (!qid.empty() && cond_to_market_.count(qid) > 0) {
          effective_collateral = static_cast<uint8_t>(Collateral::WrappedUSDCe);
        }
      }
    }

    uint16_t key = static_cast<uint16_t>(row.event_type) * 256 + effective_collateral;
    progress_.event_by_collateral[key]++;
  });

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

  {
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
  update_cond_type_stats();

  if (progress_.cursor > 0)
    progress_.phase = 3;

  committed_progress_ = progress_;
  build_cursor_ = committed_progress_.cursor;

  stage2_log_info("Restored: " + std::to_string(conditions_.size()) + " conditions, " +
                  std::to_string(token_map_.size()) + " tokens, " +
                  std::to_string(fpmm_map_.size()) + " FPMMs");
  refresh_memory_snapshot("load_from_rb_done");
}

void EventBuilder::reap_commit_result_locked() {
  stage2_assert(commit_result_.has_value(), AssertLevel::L0, "State", "CommitResultReady");
  committed_progress_ = *commit_result_;
  commit_result_.reset();
  commit_busy_ = false;
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

void EventBuilder::request_stop() { stop_requested_ = true; }

void EventBuilder::clear_stop() { stop_requested_ = false; }

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
    chunk_log_.finish();
    refresh_memory_snapshot("stopped_after_phase2");
    return false;
  }

  progress_.phase = 3;
  phase3_process_transfers(chunk_start, chunk_end);
  refresh_memory_snapshot("phase3_transfer_done");
  if (stop_requested_) {
    progress_.running = false;
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
  }
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
