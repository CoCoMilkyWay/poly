#include "misc/profiler.hpp"
#include "stage2_builder.hpp"
#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>

namespace stage2 {
namespace {

constexpr std::string_view kEmptyRangeSql = "(SELECT 1 WHERE 1=0)";

inline std::optional<std::string> block_range_query(Database &db, const std::string &select_sql,
                                                    const std::string &table_name,
                                                    int64_t start, int64_t end) {
  std::string range_sql = db.feather_table_range(table_name, start, end);
  if (range_sql == kEmptyRangeSql) {
    return std::nullopt;
  }
  return select_sql + range_sql +
         " WHERE block_number > " + std::to_string(start) +
         " AND block_number <= " + std::to_string(end);
}

inline duckdb::unique_ptr<duckdb::MaterializedQueryResult>
query_block_range(duckdb::Connection &conn, Database &db,
                  const std::string &select_sql, const std::string &table_name,
                  int64_t start, int64_t end,
                  const std::string &suffix_sql = "") {
  auto sql = block_range_query(db, select_sql, table_name, start, end);
  if (!sql.has_value()) {
    return nullptr;
  }
  auto result = conn.Query(*sql + suffix_sql);
  stage2_assert(!result->HasError(), AssertLevel::L0, "DB", "RangeQuerySuccess");
  return result;
}

inline int64_t u256_blob_to_i64(const duckdb::Value &v) {
  std::string blob = v.GetValueUnsafe<std::string>();
  stage2_assert(blob.size() == 32, AssertLevel::L0, "Parse", "U256BlobSize32");
  for (size_t i = 0; i < 24; ++i) {
    stage2_assert(static_cast<unsigned char>(blob[i]) == 0, AssertLevel::L0, "Parse", "U256HighBitsZero");
  }
  uint64_t low = 0;
  for (size_t i = 24; i < 32; ++i) {
    low = (low << 8) | static_cast<unsigned char>(blob[i]);
  }
  stage2_assert(low <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()),
                AssertLevel::L0, "Parse", "U256FitsInt64");
  return static_cast<int64_t>(low);
}

inline std::vector<int64_t> parse_u256_list_i64(const duckdb::Value &v) {
  stage2_assert(v.type().id() == duckdb::LogicalTypeId::LIST, AssertLevel::L0, "Parse", "U256ListType");
  auto arr = duckdb::ListValue::GetChildren(v);
  std::vector<int64_t> out;
  out.reserve(arr.size());
  for (const auto &item : arr) {
    out.push_back(u256_blob_to_i64(item));
  }
  return out;
}

inline std::vector<std::string> parse_bytes32_list_hex_lower(const duckdb::Value &v) {
  stage2_assert(v.type().id() == duckdb::LogicalTypeId::LIST, AssertLevel::L0, "Parse", "Bytes32ListType");
  auto arr = duckdb::ListValue::GetChildren(v);
  std::vector<std::string> out;
  out.reserve(arr.size());
  for (const auto &item : arr) {
    std::string b = item.GetValueUnsafe<std::string>();
    stage2_assert(b.size() == 32, AssertLevel::L0, "Parse", "Bytes32ItemSize");
    out.push_back(to_lower(blob_to_hex(b)));
  }
  return out;
}

inline int64_t q_get_i64(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl,
                         int col, idx_t row) {
  return tbl->GetValue(col, row).GetValue<int64_t>();
}

inline int64_t q_get_u256_i64(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl,
                              int col, idx_t row) {
  return u256_blob_to_i64(tbl->GetValue(col, row));
}

inline std::string q_get_hex(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl,
                             int col, idx_t row) {
  return blob_to_hex(tbl->GetValue(col, row).GetValueUnsafe<std::string>());
}

inline std::string q_get_hex_lower(const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl,
                                   int col, idx_t row) {
  return to_lower(q_get_hex(tbl, col, row));
}

} // namespace

void EventBuilder::phase1_update_mappings(int64_t start, int64_t end) {
  TraceN("s2/phase1_map");
  if (stop_requested_) {
    return;
  }
  auto conn = stage1_db_.create_connection();
  std::unordered_set<uint32_t> tokenreg_cond_idxs_seen;
  auto get_u256_i32 = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    int64_t v = u256_blob_to_i64(tbl->GetValue(col, row));
    stage2_assert(v >= 0 && v <= std::numeric_limits<int>::max(),
                  AssertLevel::L0, "Parse", "U256FitsInt32");
    return static_cast<int>(v);
  };
  auto to_semantic_sort_key = [](int64_t block_number, int64_t semantic_log_index) {
    stage2_assert(semantic_log_index >= 0, AssertLevel::L0, "Input", "SemanticLogIndexNonNegative");
    return block_number * SORT_KEY_SCALE + semantic_log_index * TRANSFER_FLAT_LOG_SCALE;
  };

  {
    TraceN("condition_preparation");
    auto cp = query_block_range(
        *conn, stage1_db_,
        "SELECT condition_id, outcome_slot_count, question_id FROM ",
        "condition_preparation", start, end);
    if (cp) {
      for (idx_t i = 0; i < cp->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        std::string cid = q_get_hex(cp, 0, i);
        int cnt = get_u256_i32(cp, 1, i);
        std::string qid = q_get_hex_lower(cp, 2, i);
        progress_.cond_tree.coverage.raw_rows++;
        progress_.cond_tree.coverage.raw_by_outcome_count[cnt]++;
        if (!qid.empty() &&
            qid != "0x0000000000000000000000000000000000000000000000000000000000000000") {
          progress_.cond_tree.coverage.raw_has_question_id++;
        } else {
          progress_.cond_tree.coverage.raw_no_question_id++;
        }
        intern_condition(cid, cnt, ConditionSource::ConditionPrep, qid);
      }
    }
  }

  {
    TraceN("condition_resolution");
    auto cr = query_block_range(
        *conn, stage1_db_, "SELECT condition_id, payout_numerators FROM ",
        "condition_resolution", start, end);
    if (cr) {
      for (idx_t i = 0; i < cr->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        std::string cid = q_get_hex(cr, 0, i);
        std::string lower = to_lower(cid);
        auto it = cond_map_.find(lower);
        if (it == cond_map_.end())
          continue;
        std::vector<int64_t> payouts = parse_u256_list_i64(cr->GetValue(1, i));
        update_condition_payout(it->second, payouts);
      }
    }
  }

  {
    TraceN("token_map");
    auto tm = query_block_range(
        *conn, stage1_db_, "SELECT block_number, log_index, token0, token1, condition_id FROM ",
        "token_map", start, end);
    if (tm) {
      tokenreg_cond_idxs_seen.reserve(static_cast<size_t>(tm->RowCount()));
      for (idx_t i = 0; i < tm->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        int64_t block_number = q_get_i64(tm, 0, i);
        int64_t log_index = q_get_i64(tm, 1, i);
        int64_t evidence_sort_key = to_semantic_sort_key(block_number, log_index);
        std::string token0 = q_get_hex(tm, 2, i);
        std::string token1 = q_get_hex(tm, 3, i);
        std::string cid = q_get_hex(tm, 4, i);
        uint32_t cond_idx = intern_condition(cid, 2, ConditionSource::PolymarketTokenReg);
        tokenreg_cond_idxs_seen.insert(cond_idx);
        // TokenRegistered keeps a binary market ordering: token0 -> outcome 0, token1 -> outcome 1.
        intern_token(token0, cond_idx, 0, TokenSource::PolymarketTokenReg, evidence_sort_key);
        intern_token(token1, cond_idx, 1, TokenSource::PolymarketTokenReg, evidence_sort_key);
      }
    }
  }

  struct FPMMRow {
    int64_t evidence_sort_key = -1;
    std::string addr;
    std::string collateral;
    std::string conditional_tokens;
    std::vector<std::string> cids;
  };
  std::vector<FPMMRow> pending_fpmm_rows;
  auto process_fpmm_row = [&](const FPMMRow &row) {
    // Stage2 tracks one canonical ConditionalTokens domain.
    // FPMM rows from other CTF contracts are out-of-domain for mapping.
    if (row.conditional_tokens != CONDITIONAL_TOKENS) {
      return;
    }
    stage2_assert(!row.cids.empty(), AssertLevel::L1, "Mapping", "FPMMHasConditionIds");
    uint32_t primary_cond_idx = 0;
    bool has_primary = false;
    for (const auto &cid : row.cids) {
      std::string lower_cid = to_lower(cid);
      auto it = cond_map_.find(lower_cid);
      stage2_assert(it != cond_map_.end(), AssertLevel::L1, "Mapping", "FPMMConditionKnownInDomain");
      uint8_t outcome_cnt = conditions_[it->second].outcome_count;
      stage2_assert(outcome_cnt > 0 && outcome_cnt <= MAX_OUTCOMES,
                    AssertLevel::L1, "Mapping", "OutcomeCountRange");
      uint32_t idx = intern_condition(cid, outcome_cnt, ConditionSource::PolymarketFPMM);
      if (!has_primary) {
        primary_cond_idx = idx;
        has_primary = true;
      }
    }
    stage2_assert(has_primary, AssertLevel::L1, "Mapping", "FPMMHasPrimaryCondition");
    uint8_t coll_id = intern_collateral(row.collateral);
    intern_fpmm(row.addr, primary_cond_idx, coll_id, row.evidence_sort_key);
    // 为 FPMM 计算所有 atomic position token_id(覆盖多条件组合头寸)
    intern_fpmm_tokens(row.cids, row.collateral, primary_cond_idx, row.evidence_sort_key);
  };
  {
    TraceN("fpmm_source_rows");
    auto fpmm = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, log_index, fpmm_addr, condition_ids, collateral_token, conditional_tokens FROM ",
        "fpmm", start, end);
    if (fpmm) {
      pending_fpmm_rows.reserve(fpmm->RowCount());
      for (idx_t i = 0; i < fpmm->RowCount(); ++i) {
        int64_t block_number = q_get_i64(fpmm, 0, i);
        int64_t log_index = q_get_i64(fpmm, 1, i);
        pending_fpmm_rows.push_back(FPMMRow{
            .evidence_sort_key = to_semantic_sort_key(block_number, log_index),
            .addr = q_get_hex(fpmm, 2, i),
            .collateral = q_get_hex_lower(fpmm, 4, i),
            .conditional_tokens = q_get_hex_lower(fpmm, 5, i),
            .cids = parse_bytes32_list_hex_lower(fpmm->GetValue(3, i)),
        });
      }
    }
  }

  auto infer_outcome_count = [&](const std::string &lower_cid,
                                 const duckdb::Value &index_sets_value) -> uint8_t {
    stage2_assert(index_sets_value.type().id() == duckdb::LogicalTypeId::LIST,
                  AssertLevel::L0, "Parse", "U256ListType");
    auto arr = duckdb::ListValue::GetChildren(index_sets_value);
    uint8_t inferred = 0;
    for (const auto &item : arr) {
      int64_t index_set_i64 = u256_blob_to_i64(item);
      stage2_assert(index_set_i64 >= 0, AssertLevel::L0, "Parse", "IndexSetNonNegative");
      uint64_t index_set = static_cast<uint64_t>(index_set_i64);
      if (index_set == 0) {
        continue;
      }
      uint8_t bits = static_cast<uint8_t>(64 - __builtin_clzll(index_set));
      if (bits > inferred) {
        inferred = bits;
      }
    }
    if (inferred == 0) {
      auto it = cond_map_.find(lower_cid);
      stage2_assert(it != cond_map_.end(), AssertLevel::L1, "Mapping", "InferOutcomeCondKnown");
      inferred = conditions_[it->second].outcome_count;
    }
    stage2_assert(inferred > 0 && inferred <= MAX_OUTCOMES,
                  AssertLevel::L1, "Mapping", "InferOutcomeCountRange");
    return inferred;
  };

  // 从 split/merge/redemption 事件中提取 token_id(覆盖没有经过 FPMM 的 condition)
  auto update_from_condition_event = [&](const char *table_name,
                                         const char *index_sets_col,
                                         ConditionSource cond_source,
                                         TokenSource token_source) {
    auto rows = query_block_range(
        *conn, stage1_db_,
        std::string("SELECT block_number, log_index, condition_id, collateral_token, ") + index_sets_col + " FROM ",
        table_name, start, end);
    if (!rows) {
      return;
    }
    struct CondInference {
      std::string collateral;
      uint8_t outcome_count = 0;
      int64_t first_sort_key = -1;
    };
    std::unordered_map<std::string, CondInference> inferred_map;
    inferred_map.reserve(rows->RowCount());
    std::unordered_map<std::string, uint8_t> known_collateral_cache;
    known_collateral_cache.reserve(16);
    auto cached_known_collateral_id = [&](const std::string &addr) {
      auto it = known_collateral_cache.find(addr);
      if (it != known_collateral_cache.end()) {
        return it->second;
      }
      uint8_t coll_id = addr_to_known_collateral_id(addr);
      known_collateral_cache.emplace(addr, coll_id);
      return coll_id;
    };
    auto choose_canonical_collateral = [&](const std::string &lhs, const std::string &rhs) {
      if (lhs == rhs)
        return lhs;
      uint8_t lhs_id = cached_known_collateral_id(lhs);
      uint8_t rhs_id = cached_known_collateral_id(rhs);
      bool lhs_known = lhs_id != static_cast<uint8_t>(Collateral::Unknown);
      bool rhs_known = rhs_id != static_cast<uint8_t>(Collateral::Unknown);
      // Prefer known collateral over unknown dynamic addresses.
      if (lhs_known && !rhs_known)
        return lhs;
      if (!lhs_known && rhs_known)
        return rhs;
      // Deterministic tie-breaker keeps merge stable across runs.
      return (lhs < rhs) ? lhs : rhs;
    };
    for (idx_t i = 0; i < rows->RowCount(); ++i) {
      if (stop_requested_) {
        return;
      }
      int64_t block_number = q_get_i64(rows, 0, i);
      int64_t log_index = q_get_i64(rows, 1, i);
      int64_t evidence_sort_key = to_semantic_sort_key(block_number, log_index);
      std::string lower_cid = q_get_hex_lower(rows, 2, i);
      std::string collateral = q_get_hex_lower(rows, 3, i);
      uint8_t inferred_count = infer_outcome_count(lower_cid, rows->GetValue(4, i));
      auto it = inferred_map.find(lower_cid);
      if (it == inferred_map.end()) {
        inferred_map.emplace(lower_cid, CondInference{collateral, inferred_count, evidence_sort_key});
        continue;
      }
      it->second.collateral = choose_canonical_collateral(it->second.collateral, collateral);
      if (inferred_count > it->second.outcome_count) {
        it->second.outcome_count = inferred_count;
      }
      if (it->second.first_sort_key < 0 || evidence_sort_key < it->second.first_sort_key) {
        it->second.first_sort_key = evidence_sort_key;
      }
    }
    for (const auto &[lower_cid, inf] : inferred_map) {
      uint32_t cond_idx = intern_condition(lower_cid, inf.outcome_count, cond_source);
      uint8_t coll_id = intern_collateral(inf.collateral);
      set_cond_collateral(cond_idx, coll_id);
      intern_condition_tokens(lower_cid, inf.collateral, cond_idx, token_source, inf.first_sort_key);
    }
  };
  {
    TraceN("infer_condition_tokens");
    update_from_condition_event("split", "partition", ConditionSource::SplitEvent, TokenSource::SplitEvent);
    update_from_condition_event("merge", "partition", ConditionSource::MergeEvent, TokenSource::MergeEvent);
    update_from_condition_event("redemption", "index_sets", ConditionSource::RedemptionEvent, TokenSource::RedemptionEvent);
  }

  {
    TraceN("process_fpmm_rows");
    for (const auto &row : pending_fpmm_rows) {
      if (stop_requested_) {
        return;
      }
      process_fpmm_row(row);
    }
  }

  {
    TraceN("neg_risk_mapping");
    auto nrq = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, log_index, market_id, question_id FROM ", "neg_risk_question",
        start, end);
    if (nrq) {
      for (idx_t i = 0; i < nrq->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        int64_t block_number = q_get_i64(nrq, 0, i);
        int64_t log_index = q_get_i64(nrq, 1, i);
        int64_t evidence_sort_key = to_semantic_sort_key(block_number, log_index);
        std::string market_id = q_get_hex_lower(nrq, 2, i);
        std::string question_id = q_get_hex_lower(nrq, 3, i);

        auto [it_market, inserted] = cond_to_market_.emplace(question_id, market_id);
        if (inserted) {
          new_neg_risk_markets_.push_back({question_id, market_id});
        }

        auto oracle_bytes = hex_to_blob(NEG_RISK_ADAPTER);
        auto qid_bytes = hex_to_blob(question_id);
        std::string input(84, '\0');
        std::memcpy(input.data(), oracle_bytes.data(), std::min(size_t(20), oracle_bytes.size()));
        std::memcpy(input.data() + 20, qid_bytes.data(), std::min(size_t(32), qid_bytes.size()));
        input[83] = 2;
        auto cond_hash = crypto::keccak256(input);
        std::string cond_id = to_lower(crypto::Keccak256::to_hex(cond_hash));

        auto it = cond_map_.find(cond_id);
        if (it != cond_map_.end()) {
          auto [_, cond_inserted] = negrisk_cond_idxs_.insert(it->second);
          if (cond_inserted) {
            auto vis_it = chunk_negrisk_visible_from_sort_.find(it->second);
            if (vis_it == chunk_negrisk_visible_from_sort_.end() ||
                evidence_sort_key < vis_it->second) {
              chunk_negrisk_visible_from_sort_[it->second] = evidence_sort_key;
            }
          }
        }
      }
    }
  }

  // TokenRegistered is authoritative Polymarket evidence at condition level:
  // every touched condition must end this phase with source=PolymarketTokenReg.
  for (uint32_t cond_idx : tokenreg_cond_idxs_seen) {
    stage2_assert(cond_idx < conditions_.size(),
                  AssertLevel::L1, "Mapping", "TokenRegCondIdxInRange");
    stage2_assert(conditions_[cond_idx].source == ConditionSource::PolymarketTokenReg,
                  AssertLevel::L1, "Mapping", "TokenRegConditionSourceSticky");
  }
  update_cond_type_stats();
}

void EventBuilder::phase2_build_semantic_index(int64_t start, int64_t end) {
  TraceN("s2/phase2_idx");
  if (stop_requested_) {
    return;
  }
  auto conn = stage1_db_.create_connection();
  auto build_tx_key = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, idx_t row) {
    TxKey key;
    key.block = q_get_i64(tbl, 0, row);
    key.tx_hash = hex_to_bytes32(q_get_hex(tbl, 1, row));
    return key;
  };
  auto build_tx_fpmm_key = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl,
                               idx_t row, const std::string &fpmm_addr) {
    TxFPMMKey key;
    key.block = q_get_i64(tbl, 0, row);
    key.tx_hash = hex_to_bytes32(q_get_hex(tbl, 1, row));
    key.fpmm_addr = fpmm_addr;
    return key;
  };
  std::unordered_map<TxKey, std::vector<int64_t>> tx_op_logs;
  auto record_semantic = [&](const TxKey &key, int64_t log_index) {
    tx_op_logs[key].push_back(log_index);
  };
  int64_t src_split_rows = 0;
  int64_t src_merge_rows = 0;
  int64_t src_redemption_rows = 0;
  int64_t src_convert_rows = 0;
  int64_t src_order_rows = 0;
  int64_t src_fpmm_trade_rows = 0;
  int64_t src_fpmm_funding_rows = 0;

  int64_t idx_split_rows = 0;
  int64_t idx_merge_rows = 0;
  int64_t idx_redemption_rows = 0;
  int64_t idx_convert_rows = 0;
  int64_t idx_order_rows = 0;
  int64_t idx_fpmm_trade_rows = 0;
  int64_t idx_fpmm_funding_rows = 0;

  int64_t skipped_fpmm_trade_unknown_fpmm_rows = 0;
  int64_t skipped_fpmm_funding_unknown_fpmm_rows = 0;
  auto load_split_or_merge = [&](const char *table_name,
                                 auto &&build_info,
                                 auto &target_map,
                                 int64_t &src_rows,
                                 int64_t &idx_rows) {
    auto rows = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, tx_hash, log_index, condition_id, amount, stakeholder, collateral_token, parent_collection_id, partition FROM ",
        table_name, start, end);
    if (!rows) {
      return;
    }
    idx_t row_count = rows->RowCount();
    src_rows = static_cast<int64_t>(row_count);
    size_t reserve_hint = static_cast<size_t>(row_count);
    target_map.reserve(target_map.size() + reserve_hint);
    tx_op_logs.reserve(tx_op_logs.size() + reserve_hint);
    for (idx_t i = 0; i < row_count; ++i) {
      if (stop_requested_) {
        return;
      }
      TxKey key = build_tx_key(rows, i);
      auto info = build_info(rows, i);
      int64_t semantic_log = info.log_index;
      target_map[key].push_back(std::move(info));
      idx_rows++;
      record_semantic(key, semantic_log);
    }
  };

  {
    TraceN("load_split_merge");
    load_split_or_merge(
        "split",
        [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &rows, idx_t i) {
          SplitInfo info;
          info.log_index = q_get_i64(rows, 2, i);
          info.cond_id = q_get_hex_lower(rows, 3, i);
          info.amount = q_get_u256_i64(rows, 4, i);
          info.stakeholder = q_get_hex_lower(rows, 5, i);
          info.collateral_token = q_get_hex_lower(rows, 6, i);
          info.parent_collection_id = q_get_hex_lower(rows, 7, i);
          info.partition = parse_bytes32_list_hex_lower(rows->GetValue(8, i));
          return info;
        },
        tx_split_, src_split_rows, idx_split_rows);

    load_split_or_merge(
        "merge",
        [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &rows, idx_t i) {
          MergeInfo info;
          info.log_index = q_get_i64(rows, 2, i);
          info.cond_id = q_get_hex_lower(rows, 3, i);
          info.amount = q_get_u256_i64(rows, 4, i);
          info.stakeholder = q_get_hex_lower(rows, 5, i);
          info.collateral_token = q_get_hex_lower(rows, 6, i);
          info.parent_collection_id = q_get_hex_lower(rows, 7, i);
          info.partition = parse_bytes32_list_hex_lower(rows->GetValue(8, i));
          return info;
        },
        tx_merge_, src_merge_rows, idx_merge_rows);
  }

  {
    TraceN("load_redemption");
    auto redemption = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, tx_hash, log_index, condition_id, payout, redeemer, collateral_token, parent_collection_id, index_sets FROM ",
        "redemption", start, end);
    if (redemption) {
      src_redemption_rows = static_cast<int64_t>(redemption->RowCount());
      for (idx_t i = 0; i < redemption->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        TxKey key = build_tx_key(redemption, i);
        RedemptionInfo info;
        info.log_index = q_get_i64(redemption, 2, i);
        info.cond_id = q_get_hex_lower(redemption, 3, i);
        info.payout = q_get_u256_i64(redemption, 4, i);
        info.redeemer = q_get_hex_lower(redemption, 5, i);
        info.collateral_token = q_get_hex_lower(redemption, 6, i);
        info.parent_collection_id = q_get_hex_lower(redemption, 7, i);
        info.index_sets = parse_bytes32_list_hex_lower(redemption->GetValue(8, i));
        tx_redemption_[key].push_back(info);
        idx_redemption_rows++;
        record_semantic(key, info.log_index);
      }
    }
  }

  {
    TraceN("load_convert");
    auto convert = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, tx_hash, log_index, market_id, index_set, amount, stakeholder FROM ",
        "convert", start, end);
    if (convert) {
      src_convert_rows = static_cast<int64_t>(convert->RowCount());
      for (idx_t i = 0; i < convert->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        std::string market_id = q_get_hex_lower(convert, 3, i);
        TxMarketKey key;
        key.block = q_get_i64(convert, 0, i);
        key.tx_hash = hex_to_bytes32(q_get_hex(convert, 1, i));
        key.market_id = market_id;
        TxKey tx_key;
        tx_key.block = key.block;
        tx_key.tx_hash = key.tx_hash;
        ConvertInfo info;
        info.log_index = q_get_i64(convert, 2, i);
        info.market_id = market_id;
        info.index_set = q_get_hex_lower(convert, 4, i);
        info.amount = q_get_u256_i64(convert, 5, i);
        info.stakeholder = q_get_hex_lower(convert, 6, i);
        tx_convert_[key].push_back(info);
        idx_convert_rows++;
        record_semantic(tx_key, info.log_index);
      }
    }
  }

  {
    TraceN("load_order");
    auto order = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, tx_hash, log_index, maker, taker, maker_asset_id, taker_asset_id, "
        "maker_amount, taker_amount, fee FROM ",
        "order_filled", start, end);
    static const std::string ZERO_TOKEN_ID =
        "0x0000000000000000000000000000000000000000000000000000000000000000";
    if (order) {
      idx_t row_count = order->RowCount();
      src_order_rows = static_cast<int64_t>(row_count);
      size_t reserve_hint = static_cast<size_t>(row_count);
      tx_order_.reserve(tx_order_.size() + reserve_hint);
      tx_op_logs.reserve(tx_op_logs.size() + reserve_hint);
      for (idx_t i = 0; i < row_count; ++i) {
        if (stop_requested_) {
          return;
        }
        int64_t block = q_get_i64(order, 0, i);
        auto tx_hash = hex_to_bytes32(q_get_hex(order, 1, i));
        int64_t log_index = q_get_i64(order, 2, i);
        std::string maker = q_get_hex_lower(order, 3, i);
        std::string taker = q_get_hex_lower(order, 4, i);
        std::string maker_asset = q_get_hex_lower(order, 5, i);
        std::string taker_asset = q_get_hex_lower(order, 6, i);
        int64_t maker_amt = q_get_u256_i64(order, 7, i);
        int64_t taker_amt = q_get_u256_i64(order, 8, i);
        int64_t fee = q_get_u256_i64(order, 9, i);

        bool maker_is_quote = maker_asset == ZERO_TOKEN_ID;
        const std::string &token_id_lower = maker_is_quote ? taker_asset : maker_asset;

        TxTokenKey key{block, tx_hash, token_id_lower};
        TxKey tx_key{block, tx_hash};
        OrderInfo info;
        info.log_index = log_index;
        info.token_id = token_id_lower;
        info.maker = maker;
        info.taker = taker;
        info.maker_side = maker_is_quote ? 1 : 2;
        info.quote_amount = maker_is_quote ? maker_amt : taker_amt;
        info.tokens = maker_is_quote ? taker_amt : maker_amt;
        info.fee = fee;
        tx_order_[key].push_back(std::move(info));
        idx_order_rows++;
        record_semantic(tx_key, info.log_index);
      }
    }
  }

  {
    TraceN("load_fpmm_trade");
    auto fpmm_trade = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, tx_hash, log_index, fpmm_addr, trader, side, outcome_index, "
        "collateral_amount, token_amount FROM ",
        "fpmm_trade", start, end);
    if (fpmm_trade) {
      src_fpmm_trade_rows = static_cast<int64_t>(fpmm_trade->RowCount());
      for (idx_t i = 0; i < fpmm_trade->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        std::string fpmm_addr = q_get_hex_lower(fpmm_trade, 3, i);
        if (!is_known_fpmm(fpmm_addr)) {
          skipped_fpmm_trade_unknown_fpmm_rows++;
          continue;
        }
        TxFPMMKey key = build_tx_fpmm_key(fpmm_trade, i, fpmm_addr);
        TxKey tx_key{key.block, key.tx_hash};
        FPMMTradeInfo info;
        info.log_index = q_get_i64(fpmm_trade, 2, i);
        info.fpmm_addr = fpmm_addr;
        info.trader = q_get_hex_lower(fpmm_trade, 4, i);
        info.side = fpmm_trade->GetValue(5, i).GetValue<int>();
        stage2_assert(info.side == 1 || info.side == 2,
                      AssertLevel::L0, "Input", "FPMMTradeSideRange");
        int64_t outcome_idx = q_get_u256_i64(fpmm_trade, 6, i);
        stage2_assert(outcome_idx >= 0 && outcome_idx <= std::numeric_limits<int>::max(),
                      AssertLevel::L0, "Input", "OutcomeIdxFitsInt");
        info.outcome_idx = static_cast<int>(outcome_idx);
        info.collateral_amount = q_get_u256_i64(fpmm_trade, 7, i);
        info.tokens = q_get_u256_i64(fpmm_trade, 8, i);
        info.requires_erc1155_leg = (info.tokens > 0 && info.trader != info.fpmm_addr);
        tx_fpmm_trade_[key].push_back(info);
        idx_fpmm_trade_rows++;
        record_semantic(tx_key, info.log_index);
      }
    }
  }

  {
    TraceN("load_fpmm_funding");
    auto fpmm_funding = query_block_range(
        *conn, stage1_db_,
        "SELECT block_number, tx_hash, log_index, fpmm_addr, funder, side, amounts FROM ",
        "fpmm_funding", start, end);
    if (fpmm_funding) {
      src_fpmm_funding_rows = static_cast<int64_t>(fpmm_funding->RowCount());
      for (idx_t i = 0; i < fpmm_funding->RowCount(); ++i) {
        if (stop_requested_) {
          return;
        }
        std::string fpmm_addr = q_get_hex_lower(fpmm_funding, 3, i);
        if (!is_known_fpmm(fpmm_addr)) {
          skipped_fpmm_funding_unknown_fpmm_rows++;
          continue;
        }
        TxFPMMKey key = build_tx_fpmm_key(fpmm_funding, i, fpmm_addr);
        TxKey tx_key{key.block, key.tx_hash};
        FPMMFundingInfo info;
        info.log_index = q_get_i64(fpmm_funding, 2, i);
        info.fpmm_addr = fpmm_addr;
        info.funder = q_get_hex_lower(fpmm_funding, 4, i);
        info.side = fpmm_funding->GetValue(5, i).GetValue<int>();
        info.amounts = parse_u256_list_i64(fpmm_funding->GetValue(6, i));
        tx_fpmm_funding_[key].push_back(info);
        idx_fpmm_funding_rows++;
        record_semantic(tx_key, info.log_index);
      }
    }
  }

  {
    TraceN("build_op_bounds");
    for (auto &[tx_key, logs] : tx_op_logs) {
      std::sort(logs.begin(), logs.end());
      logs.erase(std::unique(logs.begin(), logs.end()), logs.end());
      std::vector<TxOpBounds> bounds;
      bounds.reserve(logs.size());
      int64_t prev = -1;
      for (int64_t log_index : logs) {
        stage2_assert(log_index >= 0, AssertLevel::L0, "Input", "SemanticLogIndexNonNegative");
        bounds.push_back(TxOpBounds{prev, log_index});
        prev = log_index;
      }
      tx_op_bounds_[tx_key] = std::move(bounds);
    }
  }
  {
    TraceN("build_fast_indexes");
    tx_convert_by_tx_.clear();
    tx_convert_by_tx_.reserve(tx_convert_.size());
    for (auto &[tx_market_key, rows] : tx_convert_) {
      TxKey tx_key{tx_market_key.block, tx_market_key.tx_hash};
      auto &bucket = tx_convert_by_tx_[tx_key];
      bucket.reserve(bucket.size() + rows.size());
      for (auto &info : rows) {
        bucket.push_back(&info);
      }
    }

    tx_order_by_amount_.clear();
    tx_order_by_amount_.reserve(tx_order_.size());
    for (auto &[tx_token_key, rows] : tx_order_) {
      auto &amount_map = tx_order_by_amount_[tx_token_key];
      for (auto &info : rows) {
        amount_map[info.tokens].push_back(&info);
      }
    }

    tx_split_by_actor_amount_.clear();
    tx_split_by_actor_amount_.reserve(tx_split_.size());
    for (auto &[tx_key, rows] : tx_split_) {
      auto &bucket = tx_split_by_actor_amount_[tx_key];
      for (auto &info : rows) {
        bucket[actor_amount_index_key(info.stakeholder, info.amount)].push_back(&info);
      }
    }

    tx_merge_by_actor_amount_.clear();
    tx_merge_by_actor_amount_.reserve(tx_merge_.size());
    for (auto &[tx_key, rows] : tx_merge_) {
      auto &bucket = tx_merge_by_actor_amount_[tx_key];
      for (auto &info : rows) {
        bucket[actor_amount_index_key(info.stakeholder, info.amount)].push_back(&info);
      }
    }

    tx_redemption_by_actor_.clear();
    tx_redemption_by_actor_.reserve(tx_redemption_.size());
    for (auto &[tx_key, rows] : tx_redemption_) {
      auto &bucket = tx_redemption_by_actor_[tx_key];
      for (auto &info : rows) {
        bucket[info.redeemer].push_back(&info);
      }
    }

    tx_fpmm_trade_by_leg_.clear();
    tx_fpmm_trade_by_leg_.reserve(tx_fpmm_trade_.size());
    for (auto &[tx_fpmm_key, rows] : tx_fpmm_trade_) {
      auto &bucket = tx_fpmm_trade_by_leg_[tx_fpmm_key];
      for (auto &info : rows) {
        bucket[fpmm_trade_leg_index_key(info.side, info.trader, info.tokens)].push_back(&info);
      }
    }
  }
  stage2_assert(idx_split_rows == src_split_rows, AssertLevel::L1, "Index", "SplitIndexCoverage");
  stage2_assert(idx_merge_rows == src_merge_rows, AssertLevel::L1, "Index", "MergeIndexCoverage");
  stage2_assert(idx_redemption_rows == src_redemption_rows, AssertLevel::L1, "Index", "RedemptionIndexCoverage");
  stage2_assert(idx_convert_rows == src_convert_rows, AssertLevel::L1, "Index", "ConvertIndexCoverage");
  stage2_assert(idx_order_rows == src_order_rows, AssertLevel::L1, "Index", "OrderIndexCoverage");
  stage2_assert(idx_fpmm_trade_rows + skipped_fpmm_trade_unknown_fpmm_rows == src_fpmm_trade_rows,
                AssertLevel::L1, "Index", "FPMMTradeIndexCoverage");
  stage2_assert(idx_fpmm_funding_rows + skipped_fpmm_funding_unknown_fpmm_rows == src_fpmm_funding_rows,
                AssertLevel::L1, "Index", "FPMMFundingIndexCoverage");
}

void EventBuilder::phase3_process_transfers(int64_t start, int64_t end) {
  TraceN("s2/phase3_xfer");
  if (stop_requested_) {
    return;
  }
  auto conn = stage1_db_.create_connection();
  auto transfers = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount FROM ",
      "transfer", start, end,
      " ORDER BY block_number, log_index");
  std::unordered_map<TxFPMMKey, uint8_t> observed_fpmm_trade_leg_mask;
  struct ObservedOrderLeg {
    std::string from;
    std::string to;
    int64_t amount = 0;
  };
  std::unordered_map<TxTokenKey, std::unordered_map<int64_t, std::vector<ObservedOrderLeg>>> observed_order_legs_by_amount;
  struct ObservedCondLeg {
    std::string from;
    std::string to;
    int64_t amount = 0;
    int64_t base_log_index = -1;
  };
  std::unordered_map<TxKey, std::unordered_map<uint32_t, std::vector<ObservedCondLeg>>> observed_cond_legs_by_cond;
  int64_t observed_convert_user_burn_rows = 0;
  int64_t observed_convert_adapter_burn_rows = 0;
  int64_t classified_convert_rows = 0;
  int64_t classified_internal_burn_convert_rows = 0;

  auto resolve_transfer_collateral = [&](uint32_t cid_idx, const std::string &operator_addr,
                                         int64_t transfer_sort_key) {
    auto coll_it = cond_collateral_.find(cid_idx);
    if (coll_it != cond_collateral_.end())
      return static_cast<Collateral>(coll_it->second);
    // 对 TransferInferred token,优先用 FPMM operator 的 collateral 回填
    // 避免在 FPMM Buy/Sell/LP 事件中出现无意义的 0x000...000
    auto fit = fpmm_map_.find(operator_addr);
    if (fit != fpmm_map_.end()) {
      if (!is_fpmm_visible_at(operator_addr, transfer_sort_key)) {
        return Collateral::Unknown;
      }
      return static_cast<Collateral>(fit->second.collateral);
    }
    return Collateral::Unknown;
  };

  if (!transfers) {
    return;
  }
  const size_t transfer_rows = static_cast<size_t>(transfers->RowCount());
  if (transfer_rows == 0) {
    return;
  }
  observed_fpmm_trade_leg_mask.reserve(transfer_rows);
  observed_order_legs_by_amount.reserve(transfer_rows);
  observed_cond_legs_by_cond.reserve(transfer_rows);
  new_events_.reserve(new_events_.size() + transfer_rows);

  {
    TraceN("classify_transfer_rows");
    for (idx_t i = 0; i < transfers->RowCount(); ++i) {
      if (stop_requested_) {
        return;
      }
      int64_t block = q_get_i64(transfers, 0, i);
      int64_t log_idx = q_get_i64(transfers, 2, i);
      stage2_assert(log_idx >= 0 && log_idx < SORT_KEY_SCALE,
                    AssertLevel::L0, "Input", "TransferLogIndexRange");
      int64_t amount = q_get_u256_i64(transfers, 7, i);
      std::string tx_hash_hex = q_get_hex(transfers, 1, i);
      auto tx_hash = hex_to_bytes32(tx_hash_hex);
      std::string op = q_get_hex_lower(transfers, 3, i);
      std::string from = q_get_hex_lower(transfers, 4, i);
      std::string to = q_get_hex_lower(transfers, 5, i);
      std::string token_id = q_get_hex_lower(transfers, 6, i);
      if (op == NEG_RISK_ADAPTER && to == NO_TOKEN_BURN_ADDRESS && amount > 0) {
        if (from == NEG_RISK_ADAPTER) {
          observed_convert_adapter_burn_rows++;
        } else {
          observed_convert_user_burn_rows++;
        }
      }

      int64_t sort_key = block * SORT_KEY_SCALE + log_idx;
      if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
        TxTokenKey order_key{block, tx_hash, token_id};
        ObservedOrderLeg leg{from, to, amount};
        observed_order_legs_by_amount[order_key][amount].push_back(std::move(leg));
      }
      bool op_is_fpmm_visible = is_fpmm_visible_at(op, sort_key);
      if (op_is_fpmm_visible) {
        TxFPMMKey key{block, tx_hash, op};
        uint8_t mask = observed_fpmm_trade_leg_mask[key];
        if (from == op && amount > 0) {
          // side=1(Buy) direct leg: FPMM -> trader
          mask |= 0x1;
        }
        if (to == op && amount > 0) {
          // side=2(Sell) direct leg: trader -> FPMM
          mask |= 0x2;
        }
        if (from == ZERO_ADDR && to == op && amount > 0) {
          // side=1(Buy) internal split mint leg: 0 -> FPMM
          mask |= 0x1;
        }
        if (from == op && to == ZERO_ADDR && amount > 0) {
          // side=2(Sell) internal merge burn leg: FPMM -> 0
          mask |= 0x2;
        }
        observed_fpmm_trade_leg_mask[key] = mask;
      }

      auto tit = token_map_.find(token_id);

      if (tit == token_map_.end()) {
        // 未知token：加入 token_map_,使用特殊值表示未知 condition
        intern_token(token_id, UNKNOWN_COND_IDX, UNKNOWN_TOKEN_IDX, TokenSource::TransferInferred);
        tit = token_map_.find(token_id); // 重新获取迭代器
      }

      bool token_known_visible = is_token_known_visible_at(token_id, sort_key);
      uint32_t cond_idx = token_known_visible ? tit->second.cond_idx : UNKNOWN_COND_IDX;
      uint8_t token_idx = token_known_visible ? tit->second.token_idx : UNKNOWN_TOKEN_IDX;
      if (amount > 0 && cond_idx != UNKNOWN_COND_IDX) {
        TxKey tx_key{block, tx_hash};
        observed_cond_legs_by_cond[tx_key][cond_idx].push_back(
            ObservedCondLeg{from, to, amount, log_idx / TRANSFER_FLAT_LOG_SCALE});
      }

      // 获取抵押品类型
      Collateral collateral = resolve_transfer_collateral(cond_idx, op, sort_key);

      {
        char ctx_buf[512];
        snprintf(ctx_buf, sizeof(ctx_buf),
                 "block=%ld tx=%s log_index=%ld op=%s from=%s to=%s token_id=%s amount=%ld cond_idx=%u token_idx=%d collateral=%d",
                 static_cast<long>(block), tx_hash_hex.c_str(), static_cast<long>(log_idx),
                 op.c_str(), from.c_str(), to.c_str(), token_id.c_str(),
                 static_cast<long>(amount), cond_idx, static_cast<int>(token_idx), static_cast<int>(collateral));
        current_transfer_context_ = ctx_buf;
      }

      Stage2AssertContextScope assert_scope(&current_transfer_context_);
      TransferClass cls = classify_and_emit(sort_key, tx_hash, block, op, from, to, token_id,
                                            amount, cond_idx, token_idx, collateral);
      if (cls == TransferClass::Convert) {
        classified_convert_rows++;
      } else if (cls == TransferClass::InternalBurnConvert) {
        classified_internal_burn_convert_rows++;
      }
      update_xfer_tree(cls);
    }
  }
  stage2_assert(classified_convert_rows == observed_convert_user_burn_rows,
                AssertLevel::L4, "Partition", "ConvertUserBurnRowsCovered");
  stage2_assert(classified_internal_burn_convert_rows == observed_convert_adapter_burn_rows,
                AssertLevel::L4, "Partition", "ConvertInternalBurnRowsCovered");

  // Semantic coverage assertions: every semantic op in this chunk must be consumed by at least one transfer leg.
  {
    TraceN("coverage_asserts");
    static const std::string ZERO_BYTES32 =
        "0x0000000000000000000000000000000000000000000000000000000000000000";
    std::unordered_map<std::string, int64_t> market_question_count;
    market_question_count.reserve(cond_to_market_.size());
    for (const auto &[_, market_id] : cond_to_market_) {
      market_question_count[market_id]++;
    }
    auto cond_idx_for = [&](const std::string &cond_id) -> uint32_t {
      auto it = cond_map_.find(cond_id);
      if (it == cond_map_.end()) {
        return UNKNOWN_COND_IDX;
      }
      return it->second;
    };
    std::unordered_map<TxKey, std::unordered_map<int64_t, int64_t>> semantic_window_left_idx;
    semantic_window_left_idx.reserve(tx_op_bounds_.size());
    for (const auto &[tx_key, bounds] : tx_op_bounds_) {
      auto &idx = semantic_window_left_idx[tx_key];
      idx.reserve(bounds.size());
      for (const auto &b : bounds) {
        idx.emplace(b.right_inclusive, b.left_exclusive);
      }
    }
    auto semantic_window_left = [&](const TxKey &tx_key, int64_t semantic_log_index) {
      auto tx_it = semantic_window_left_idx.find(tx_key);
      if (tx_it == semantic_window_left_idx.end()) {
        return int64_t{-1};
      }
      auto left_it = tx_it->second.find(semantic_log_index);
      if (left_it == tx_it->second.end()) {
        return int64_t{-1};
      }
      return left_it->second;
    };
    auto observed_cond_leg_in_window = [&](const TxKey &tx_key,
                                           uint32_t cond_idx,
                                           int64_t semantic_log_index,
                                           auto &&match_leg) {
      int64_t left = semantic_window_left(tx_key, semantic_log_index);
      auto tx_it = observed_cond_legs_by_cond.find(tx_key);
      if (tx_it == observed_cond_legs_by_cond.end()) {
        return false;
      }
      auto cond_it = tx_it->second.find(cond_idx);
      if (cond_it == tx_it->second.end()) {
        return false;
      }
      for (const auto &leg : cond_it->second) {
        if (leg.base_log_index <= left || leg.base_log_index > semantic_log_index) {
          continue;
        }
        if (match_leg(leg)) {
          return true;
        }
      }
      return false;
    };
    auto observed_cond_leg_for_actor = [&](const TxKey &tx_key,
                                           const std::string &leg_cond_id,
                                           int64_t expected_amount,
                                           int64_t expected_semantic_log_index,
                                           auto &&match_leg) {
      uint32_t cond_idx = cond_idx_for(leg_cond_id);
      if (cond_idx == UNKNOWN_COND_IDX || expected_amount <= 0) {
        return false;
      }
      return observed_cond_leg_in_window(
          tx_key, cond_idx, expected_semantic_log_index,
          [&](const ObservedCondLeg &leg) { return match_leg(leg); });
    };
    auto split_leg_observed = [&](const TxKey &tx_key, const std::string &cond_id,
                                  const std::string &stakeholder, int64_t amount,
                                  int64_t semantic_log_index) {
      return observed_cond_leg_for_actor(
          tx_key,
          cond_id, amount, semantic_log_index,
          [&](const ObservedCondLeg &leg) {
            // Split semantic certainty comes from child mint legs.
            return leg.amount == amount && leg.from == ZERO_ADDR && leg.to == stakeholder;
          });
    };
    auto merge_leg_observed = [&](const TxKey &tx_key, const std::string &cond_id,
                                  const std::string &stakeholder, int64_t amount,
                                  int64_t semantic_log_index) {
      return observed_cond_leg_for_actor(
          tx_key,
          cond_id, amount, semantic_log_index,
          [&](const ObservedCondLeg &leg) {
            // Merge semantic certainty comes from child burn legs.
            return leg.amount == amount && leg.from == stakeholder && leg.to == ZERO_ADDR;
          });
    };
    auto redeem_leg_observed = [&](const TxKey &tx_key, const std::string &cond_id,
                                   const std::string &redeemer,
                                   int64_t semantic_log_index) {
      uint32_t cond_idx = cond_idx_for(cond_id);
      if (cond_idx == UNKNOWN_COND_IDX) {
        return false;
      }
      return observed_cond_leg_in_window(
          tx_key, cond_idx, semantic_log_index,
          [&](const ObservedCondLeg &leg) {
            return leg.from == redeemer && leg.to == ZERO_ADDR && leg.amount > 0;
          });
    };
    auto apply_split_merge_semantic_coverage = [&](auto &tree,
                                                   const auto &row,
                                                   bool must_consume,
                                                   const char *assert_rule) {
      tree.total++;
      if (row.amount == 0) {
        tree.amount_zero++;
      } else {
        tree.amount_positive++;
      }
      if (row.parent_collection_id == ZERO_BYTES32) {
        tree.parent_root++;
      } else {
        tree.parent_nested++;
      }
      if (row.partition.size() <= 1) {
        tree.partition_single++;
      } else {
        tree.partition_multi++;
      }
      bool consumed = row.consumed_count > 0;
      bool covered = row.covered_by_parent;
      if (must_consume) {
        tree.observed_leg++;
      } else {
        tree.unobserved_leg++;
      }
      if (consumed) {
        tree.consumed++;
      }
      if (covered) {
        tree.covered_by_parent++;
      }
      bool zero_amount_row = (row.amount == 0);
      bool not_required = !must_consume;
      stage2_assert(consumed || covered || zero_amount_row || not_required,
                    AssertLevel::L4, "Consume", assert_rule);
    };
    auto bump_convert_question_bucket = [&](int64_t qcnt) {
      int64_t key = (qcnt <= 0) ? -1 : qcnt;
      chunk_convert_sem_tree_.by_question_count[key]++;
    };
    for (const auto &[tx_key, rows] : tx_split_) {
      for (const auto &row : rows) {
        bool must_consume = split_leg_observed(tx_key, row.cond_id, row.stakeholder,
                                               row.amount, row.log_index);
        apply_split_merge_semantic_coverage(
            chunk_split_sem_tree_, row, must_consume, "SplitConsumedOrCoveredByParent");
      }
    }

    for (const auto &[tx_key, rows] : tx_merge_) {
      for (const auto &row : rows) {
        bool must_consume = merge_leg_observed(tx_key, row.cond_id, row.stakeholder,
                                               row.amount, row.log_index);
        apply_split_merge_semantic_coverage(
            chunk_merge_sem_tree_, row, must_consume, "MergeConsumedOrCoveredByParent");
      }
    }

    for (const auto &[tx_key, rows] : tx_redemption_) {
      for (const auto &row : rows) {
        // redeem can emit multiple rows in one tx/cond/redeemer key, including zero-payout rows.
        // Only positive-payout rows require at-least-one matched burn leg when such leg is observable.
        bool must_consume = (row.payout > 0) &&
                            redeem_leg_observed(tx_key, row.cond_id, row.redeemer, row.log_index);
        bool consumed = row.consumed_count > 0;
        bool covered = row.covered_by_parent;
        bool not_required = !must_consume;
        stage2_assert(consumed || covered || not_required,
                      AssertLevel::L4, "Consume", "RedeemConsumedOrCoveredByParent");
      }
    }

    for (const auto &[key, rows] : tx_order_) {
      auto order_leg_observed = [&](const OrderInfo &row) {
        auto tx_it = observed_order_legs_by_amount.find(key);
        if (tx_it == observed_order_legs_by_amount.end()) {
          return false;
        }
        auto amt_it = tx_it->second.find(row.tokens);
        if (amt_it == tx_it->second.end()) {
          return false;
        }
        for (const auto &leg : amt_it->second) {
          if (row.maker_side == 1) {
            if (leg.to == row.maker &&
                (leg.from == row.taker || leg.from == CTF_EXCHANGE || leg.from == NEG_RISK_CTF_EXCHANGE)) {
              return true;
            }
            continue;
          }
          if (leg.from == row.maker &&
              (leg.to == row.taker || leg.to == CTF_EXCHANGE || leg.to == NEG_RISK_CTF_EXCHANGE)) {
            return true;
          }
        }
        return false;
      };
      for (const auto &row : rows) {
        chunk_order_sem_tree_.total++;
        if (row.maker_side == 1) {
          chunk_order_sem_tree_.maker_buy++;
        } else {
          chunk_order_sem_tree_.maker_sell++;
        }
        if (row.tokens == 0) {
          chunk_order_sem_tree_.token_zero++;
        } else {
          chunk_order_sem_tree_.token_positive++;
        }
        if (row.quote_amount == 0) {
          chunk_order_sem_tree_.quote_zero++;
        } else {
          chunk_order_sem_tree_.quote_positive++;
        }
        bool must_consume = order_leg_observed(row);
        bool consumed = row.consumed;
        if (must_consume) {
          chunk_order_sem_tree_.observed_leg++;
        } else {
          chunk_order_sem_tree_.unobserved_leg++;
        }
        if (consumed) {
          chunk_order_sem_tree_.consumed++;
        }
        bool unobserved = !must_consume;
        stage2_assert(consumed || unobserved, AssertLevel::L4, "Consume", "OrderConsumedIfLegObserved");
      }
    }

    for (const auto &[key, rows] : tx_fpmm_trade_) {
      for (const auto &row : rows) {
        uint8_t observed_mask = 0;
        auto mit = observed_fpmm_trade_leg_mask.find(key);
        if (mit != observed_fpmm_trade_leg_mask.end()) {
          observed_mask = mit->second;
        }
        bool has_observed_leg = (row.side == 1) ? ((observed_mask & 0x1) != 0)
                                                : ((observed_mask & 0x2) != 0);
        bool must_consume_or_explain = row.requires_erc1155_leg && has_observed_leg;
        bool consumed = row.consumed;
        bool explained = row.explained_without_direct_leg;
        bool not_required = !must_consume_or_explain;
        stage2_assert(not_required || consumed || explained,
                      AssertLevel::L4, "Consume", "FPMMTradeConsumedOrExplained");
      }
    }

    for (const auto &[_, rows] : tx_convert_) {
      for (const auto &row : rows) {
        chunk_convert_sem_tree_.total++;
        if (row.amount == 0) {
          chunk_convert_sem_tree_.amount_zero++;
        } else {
          chunk_convert_sem_tree_.amount_positive++;
        }
        auto mit = market_question_count.find(row.market_id);
        int64_t qcnt = (mit == market_question_count.end()) ? 0 : mit->second;
        bump_convert_question_bucket(qcnt);
        if (row.consumed_count > 0) {
          chunk_convert_sem_tree_.consumed++;
        }
        stage2_assert(row.consumed_count > 0, AssertLevel::L4, "Consume", "ConvertConsumed");
      }
    }

    for (const auto &[_, rows] : tx_fpmm_funding_) {
      for (const auto &row : rows) {
        // Some FPMMFundingRemoved rows have zero token legs (amounts=[0,0]):
        // they are valid semantic markers with no ERC1155 transfer to consume.
        bool zero_leg_funding_remove = row.side == 2;
        for (int64_t amount : row.amounts) {
          if (amount != 0) {
            zero_leg_funding_remove = false;
            break;
          }
        }
        bool consumed = row.consumed_count > 0;
        stage2_assert(consumed || zero_leg_funding_remove,
                      AssertLevel::L4, "Consume", "FPMMFundingConsumedOrZeroLegRemove");
      }
    }
  }
}

BuildProgress EventBuilder::commit_chunk(CommitPayload payload) {
  auto &new_conditions_ = payload.new_conditions;
  auto &new_tokens_ = payload.new_tokens;
  auto &new_fpmms_ = payload.new_fpmms;
  auto &new_collaterals_ = payload.new_collaterals;
  auto &new_cond_collaterals_ = payload.new_cond_collaterals;
  auto &new_neg_risk_markets_ = payload.new_neg_risk_markets;
  auto &new_events_ = payload.new_events;
  auto &progress_ = payload.progress;
  int64_t new_cursor = payload.new_cursor;
  auto conn = stage2_db_.create_connection();
  auto exec_sql = [&](const std::string &sql) {
    auto r = conn->Query(sql);
    stage2_assert(r && !r->HasError(), AssertLevel::L0, "DB", "CommitSQLSuccess");
  };
  exec_sql("BEGIN TRANSACTION");

  auto append_blob = [](duckdb::Appender &ap, const std::string &hex) {
    std::string b = hex_to_blob(hex);
    ap.Append(duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(b.data()), b.size()));
  };
  auto flush_temp_table = [&](const std::string &temp_table_sql,
                              const std::string &temp_table_name,
                              auto &&append_rows,
                              const std::string &merge_sql) {
    exec_sql(temp_table_sql);
    exec_sql("DELETE FROM " + temp_table_name);
    {
      duckdb::Appender ap(*conn, temp_table_name);
      append_rows(ap);
      ap.Close();
    }
    exec_sql(merge_sql);
  };

  if (!new_conditions_.empty()) {
    flush_temp_table(
        "CREATE TEMP TABLE IF NOT EXISTS tmp_rb_condition ("
        "cond_idx INTEGER, cond_id BLOB, outcome_cnt INTEGER, "
        "payout_0 BIGINT, payout_1 BIGINT, payout_2 BIGINT, payout_3 BIGINT, "
        "payout_4 BIGINT, payout_5 BIGINT, payout_6 BIGINT, payout_7 BIGINT, "
        "question_id BLOB, source INTEGER)",
        "tmp_rb_condition",
        [&](duckdb::Appender &ap) {
          for (auto &nc : new_conditions_) {
            ap.BeginRow();
            ap.Append(static_cast<int32_t>(nc.idx));
            append_blob(ap, nc.cond_id);
            ap.Append(static_cast<int32_t>(nc.info.outcome_count));
            for (int i = 0; i < 8; ++i) {
              if (i < static_cast<int>(nc.info.payout_numerators.size()) && nc.info.payout_numerators[i] >= 0)
                ap.Append(nc.info.payout_numerators[i]);
              else
                ap.Append(duckdb::Value());
            }
            if (nc.info.question_id.empty())
              ap.Append(duckdb::Value());
            else
              append_blob(ap, nc.info.question_id);
            ap.Append(static_cast<int32_t>(nc.info.source));
            ap.EndRow();
          }
        },
        "INSERT INTO rb_condition "
        "SELECT * FROM tmp_rb_condition "
        "ON CONFLICT(cond_idx) DO UPDATE SET "
        "cond_id=excluded.cond_id, "
        "outcome_cnt=excluded.outcome_cnt, "
        "payout_0=excluded.payout_0, "
        "payout_1=excluded.payout_1, "
        "payout_2=excluded.payout_2, "
        "payout_3=excluded.payout_3, "
        "payout_4=excluded.payout_4, "
        "payout_5=excluded.payout_5, "
        "payout_6=excluded.payout_6, "
        "payout_7=excluded.payout_7, "
        "question_id=excluded.question_id, "
        "source=excluded.source");
  }

  if (!new_tokens_.empty()) {
    flush_temp_table(
        "CREATE TEMP TABLE IF NOT EXISTS tmp_rb_token ("
        "token_id BLOB, cond_idx INTEGER, token_idx INTEGER, source INTEGER)",
        "tmp_rb_token",
        [&](duckdb::Appender &ap) {
          for (auto &nt : new_tokens_) {
            int32_t db_cond_idx = (nt.cond_idx == UNKNOWN_COND_IDX) ? -1 : static_cast<int32_t>(nt.cond_idx);
            ap.BeginRow();
            append_blob(ap, nt.token_id);
            ap.Append(db_cond_idx);
            ap.Append(static_cast<int32_t>(nt.token_idx));
            ap.Append(static_cast<int32_t>(nt.source));
            ap.EndRow();
          }
        },
        "INSERT INTO rb_token SELECT * FROM tmp_rb_token "
        "ON CONFLICT(token_id) DO UPDATE SET "
        "cond_idx=excluded.cond_idx, "
        "token_idx=excluded.token_idx, "
        "source=excluded.source");
  }

  if (!new_fpmms_.empty()) {
    flush_temp_table(
        "CREATE TEMP TABLE IF NOT EXISTS tmp_rb_fpmm ("
        "fpmm_addr BLOB, cond_idx INTEGER, collateral INTEGER)",
        "tmp_rb_fpmm",
        [&](duckdb::Appender &ap) {
          for (auto &nf : new_fpmms_) {
            ap.BeginRow();
            append_blob(ap, nf.addr);
            ap.Append(static_cast<int32_t>(nf.cond_idx));
            ap.Append(static_cast<int32_t>(nf.collateral));
            ap.EndRow();
          }
        },
        "INSERT OR IGNORE INTO rb_fpmm SELECT * FROM tmp_rb_fpmm");
  }

  if (!new_collaterals_.empty()) {
    flush_temp_table(
        "CREATE TEMP TABLE IF NOT EXISTS tmp_rb_collateral ("
        "coll_id INTEGER, collateral_addr BLOB)",
        "tmp_rb_collateral",
        [&](duckdb::Appender &ap) {
          for (auto &nc : new_collaterals_) {
            ap.BeginRow();
            ap.Append(static_cast<int32_t>(nc.coll_id));
            append_blob(ap, nc.addr);
            ap.EndRow();
          }
        },
        "INSERT OR IGNORE INTO rb_collateral SELECT * FROM tmp_rb_collateral");
  }

  if (!new_cond_collaterals_.empty()) {
    flush_temp_table(
        "CREATE TEMP TABLE IF NOT EXISTS tmp_rb_cond_collateral ("
        "cond_idx INTEGER, coll_id INTEGER)",
        "tmp_rb_cond_collateral",
        [&](duckdb::Appender &ap) {
          for (auto &nc : new_cond_collaterals_) {
            ap.BeginRow();
            ap.Append(static_cast<int32_t>(nc.cond_idx));
            ap.Append(static_cast<int32_t>(nc.coll_id));
            ap.EndRow();
          }
        },
        "INSERT OR REPLACE INTO rb_cond_collateral SELECT * FROM tmp_rb_cond_collateral");
  }

  if (!new_neg_risk_markets_.empty()) {
    flush_temp_table(
        "CREATE TEMP TABLE IF NOT EXISTS tmp_rb_neg_risk_market ("
        "question_id BLOB, market_id BLOB)",
        "tmp_rb_neg_risk_market",
        [&](duckdb::Appender &ap) {
          for (auto &nm : new_neg_risk_markets_) {
            ap.BeginRow();
            append_blob(ap, nm.question_id);
            append_blob(ap, nm.market_id);
            ap.EndRow();
          }
        },
        "INSERT OR IGNORE INTO rb_neg_risk_market SELECT * FROM tmp_rb_neg_risk_market");
  }

  if (!new_events_.empty()) {
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_user_event ("
             "user_addr BLOB, sort_key BIGINT, cond_idx INTEGER, "
             "event_type INTEGER, token_idx INTEGER, collateral INTEGER, amount BIGINT, price BIGINT)");
    exec_sql("DELETE FROM tmp_user_event");

    {
      duckdb::Appender appender(*conn, "tmp_user_event");
      std::unordered_map<std::string, std::string> user_blob_cache;
      user_blob_cache.reserve(std::min<size_t>(new_events_.size(), 4096));
      for (auto &[user, evt] : new_events_) {
        auto user_blob_it = user_blob_cache.find(user);
        if (user_blob_it == user_blob_cache.end()) {
          user_blob_it = user_blob_cache.emplace(user, hex_to_blob(user)).first;
        }
        const std::string &user_blob = user_blob_it->second;
        int32_t db_cond_idx = (evt.cond_idx == UNKNOWN_COND_IDX) ? -1 : static_cast<int32_t>(evt.cond_idx);
        appender.BeginRow();
        appender.Append(duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()), user_blob.size()));
        appender.Append(evt.sort_key);
        appender.Append(db_cond_idx);
        appender.Append(static_cast<int32_t>(evt.type));
        appender.Append(static_cast<int32_t>(evt.token_idx));
        appender.Append(static_cast<int32_t>(evt.collateral));
        appender.Append(evt.amount);
        appender.Append(evt.price);
        appender.EndRow();
      }
      appender.Close();
    }

    exec_sql("INSERT OR IGNORE INTO user_event "
             "SELECT * FROM tmp_user_event");
  }

  exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_stage2_cursor (key TEXT, value BIGINT)");
  exec_sql("DELETE FROM tmp_stage2_cursor");
  {
    duckdb::Appender ap(*conn, "tmp_stage2_cursor");
    auto save_cnt = [&](const char *key, int64_t val) {
      ap.BeginRow();
      ap.Append(duckdb::Value(std::string(key)));
      ap.Append(val);
      ap.EndRow();
    };
    save_cnt("last_block", new_cursor);
    save_cnt("cnt_split", progress_.cnt_split);
    save_cnt("cnt_merge", progress_.cnt_merge);
    save_cnt("cnt_redemption", progress_.cnt_redemption);
    save_cnt("cnt_convert", progress_.cnt_convert);
    save_cnt("cnt_order", progress_.cnt_order);
    save_cnt("cnt_fpmm_trade", progress_.cnt_fpmm_trade);
    save_cnt("cnt_fpmm_funding", progress_.cnt_fpmm_funding);
    save_cnt("cnt_transfer", progress_.cnt_transfer);
    save_cnt("total_events", progress_.total_events);

    const auto &xs = progress_.xfer_stats;
    save_cnt("xfer_total", xs.total);
    save_cnt("xfer_split_normal", xs.split_normal);
    save_cnt("xfer_split_negrisk", xs.split_negrisk);
    save_cnt("xfer_split_non_poly", xs.split_non_poly);
    save_cnt("xfer_merge_normal", xs.merge_normal);
    save_cnt("xfer_merge_negrisk", xs.merge_negrisk);
    save_cnt("xfer_merge_non_poly", xs.merge_non_poly);
    save_cnt("xfer_redemption", xs.redemption);
    save_cnt("xfer_redemption_non_poly", xs.redemption_non_poly);
    save_cnt("xfer_convert", xs.convert);
    save_cnt("xfer_order_buy", xs.order_buy);
    save_cnt("xfer_order_sell", xs.order_sell);
    save_cnt("xfer_fpmm_buy", xs.fpmm_buy);
    save_cnt("xfer_fpmm_sell", xs.fpmm_sell);
    save_cnt("xfer_lp_add", xs.fpmm_lp_add);
    save_cnt("xfer_lp_remove", xs.fpmm_lp_remove);
    save_cnt("xfer_lp_return", xs.fpmm_lp_return);
    save_cnt("xfer_transfer_in_negrisk", xs.transfer_in_negrisk);
    save_cnt("xfer_transfer_in_other", xs.transfer_in_other);
    save_cnt("xfer_transfer_in_non_poly", xs.transfer_in_non_poly);
    save_cnt("xfer_transfer_out_negrisk", xs.transfer_out_negrisk);
    save_cnt("xfer_transfer_out_other", xs.transfer_out_other);
    save_cnt("xfer_transfer_out_non_poly", xs.transfer_out_non_poly);
    save_cnt("xfer_internal_mint_negrisk", xs.internal_mint_negrisk);
    save_cnt("xfer_internal_mint_fpmm", xs.internal_mint_fpmm);
    save_cnt("xfer_internal_burn_negrisk", xs.internal_burn_negrisk);
    save_cnt("xfer_internal_burn_fpmm", xs.internal_burn_fpmm);
    save_cnt("xfer_internal_burn_convert", xs.internal_burn_convert);
    save_cnt("xfer_internal_transfer_zero", xs.internal_transfer_zero);
    save_cnt("xfer_internal_transfer_order", xs.internal_transfer_order);
    save_cnt("xfer_internal_transfer_negrisk", xs.internal_transfer_negrisk);
    save_cnt("xfer_internal_transfer_fpmm", xs.internal_transfer_fpmm);
    save_cnt("xfer_internal_transfer_other", xs.internal_transfer_other);
    save_cnt("xfer_unclassified", xs.unclassified);

    const auto &sst = progress_.split_sem_tree;
    save_cnt("sem_split_total", sst.total);
    save_cnt("sem_split_amount_zero", sst.amount_zero);
    save_cnt("sem_split_amount_positive", sst.amount_positive);
    save_cnt("sem_split_parent_root", sst.parent_root);
    save_cnt("sem_split_parent_nested", sst.parent_nested);
    save_cnt("sem_split_partition_single", sst.partition_single);
    save_cnt("sem_split_partition_multi", sst.partition_multi);
    save_cnt("sem_split_observed_leg", sst.observed_leg);
    save_cnt("sem_split_consumed", sst.consumed);
    save_cnt("sem_split_covered_by_parent", sst.covered_by_parent);
    save_cnt("sem_split_unobserved_leg", sst.unobserved_leg);

    const auto &mst = progress_.merge_sem_tree;
    save_cnt("sem_merge_total", mst.total);
    save_cnt("sem_merge_amount_zero", mst.amount_zero);
    save_cnt("sem_merge_amount_positive", mst.amount_positive);
    save_cnt("sem_merge_parent_root", mst.parent_root);
    save_cnt("sem_merge_parent_nested", mst.parent_nested);
    save_cnt("sem_merge_partition_single", mst.partition_single);
    save_cnt("sem_merge_partition_multi", mst.partition_multi);
    save_cnt("sem_merge_observed_leg", mst.observed_leg);
    save_cnt("sem_merge_consumed", mst.consumed);
    save_cnt("sem_merge_covered_by_parent", mst.covered_by_parent);
    save_cnt("sem_merge_unobserved_leg", mst.unobserved_leg);

    const auto &cst = progress_.convert_sem_tree;
    save_cnt("sem_convert_total", cst.total);
    save_cnt("sem_convert_amount_zero", cst.amount_zero);
    save_cnt("sem_convert_amount_positive", cst.amount_positive);
    for (const auto &[qcnt, cnt] : cst.by_question_count) {
      std::string key = (qcnt < 0) ? "sem_convert_qcnt_unknown"
                                   : ("sem_convert_qcnt_" + std::to_string(qcnt));
      save_cnt(key.c_str(), cnt);
    }
    save_cnt("sem_convert_consumed", cst.consumed);

    const auto &ost = progress_.order_sem_tree;
    save_cnt("sem_order_total", ost.total);
    save_cnt("sem_order_maker_buy", ost.maker_buy);
    save_cnt("sem_order_maker_sell", ost.maker_sell);
    save_cnt("sem_order_token_zero", ost.token_zero);
    save_cnt("sem_order_token_positive", ost.token_positive);
    save_cnt("sem_order_quote_zero", ost.quote_zero);
    save_cnt("sem_order_quote_positive", ost.quote_positive);
    save_cnt("sem_order_observed_leg", ost.observed_leg);
    save_cnt("sem_order_consumed", ost.consumed);
    save_cnt("sem_order_unobserved_leg", ost.unobserved_leg);

    const auto &ct = progress_.cond_tree;
    save_cnt("sem_cond_cov_raw_rows", ct.coverage.raw_rows);
    save_cnt("sem_cond_cov_raw_has_question_id", ct.coverage.raw_has_question_id);
    save_cnt("sem_cond_cov_raw_no_question_id", ct.coverage.raw_no_question_id);
    for (const auto &[k, v] : ct.coverage.raw_by_outcome_count) {
      std::string key = "sem_cond_cov_raw_outcome_" + std::to_string(k);
      save_cnt(key.c_str(), v);
    }
    ap.Close();
  }
  exec_sql("DELETE FROM stage2_cursor WHERE "
           "key IN ('sem_cond_resolved', 'sem_cond_unresolved', 'sem_cond_token_none', "
           "'sem_cond_token_partial', 'sem_cond_token_full', 'sem_cond_cov_observed', "
           "'sem_negrisk_market_count', 'sem_negrisk_question_count') OR "
           "key LIKE 'sem_market_%' OR "
           "key LIKE 'sem_convert_qcnt_%' OR "
           "key LIKE 'sem_cond_collateral_%' OR "
           "key LIKE 'sem_cond_cov_raw_outcome_%' OR "
           "key LIKE 'sem_negrisk_qpm_%' OR "
           "key LIKE 'sem_negrisk_cpm_%'");
  exec_sql("INSERT OR REPLACE INTO stage2_cursor SELECT * FROM tmp_stage2_cursor");

  exec_sql("COMMIT");
  return progress_;
}

} // namespace stage2
