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

} // namespace

void EventBuilder::phase1_update_mappings(int64_t start, int64_t end) {
  TraceN("s2/phase1_map");
  auto conn = stage1_db_.create_connection();
  auto get_i32 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return tbl->GetValue(col, row).GetValue<int>();
  };
  auto get_u256_i32 = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    int64_t v = u256_blob_to_i64(tbl->GetValue(col, row));
    stage2_assert(v >= 0 && v <= std::numeric_limits<int>::max(),
                    AssertLevel::L0, "Parse", "U256FitsInt32");
    return static_cast<int>(v);
  };
  auto get_hex = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return blob_to_hex(tbl->GetValue(col, row).GetValueUnsafe<std::string>());
  };
  auto get_hex_lower = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return to_lower(get_hex(tbl, col, row));
  };

  auto cp = query_block_range(
      *conn, stage1_db_,
      "SELECT condition_id, outcome_slot_count, question_id FROM ",
      "condition_preparation", start, end);
  if (cp) {
    for (idx_t i = 0; i < cp->RowCount(); ++i) {
      std::string cid = get_hex(cp, 0, i);
      int cnt = get_u256_i32(cp, 1, i);
      std::string qid = get_hex_lower(cp, 2, i);
      intern_condition(cid, cnt, ConditionSource::ConditionPrep, qid);
    }
  }

  auto cr = query_block_range(
      *conn, stage1_db_, "SELECT condition_id, payout_numerators FROM ",
      "condition_resolution", start, end);
  if (cr) {
    for (idx_t i = 0; i < cr->RowCount(); ++i) {
      std::string cid = get_hex(cr, 0, i);
      std::string lower = to_lower(cid);
      auto it = cond_map_.find(lower);
      if (it == cond_map_.end())
        continue;
      std::vector<int64_t> payouts = parse_u256_list_i64(cr->GetValue(1, i));
      update_condition_payout(it->second, payouts);
    }
  }

  auto tm = query_block_range(
      *conn, stage1_db_, "SELECT token0, token1, condition_id FROM ",
      "token_map", start, end);
  if (tm) {
    for (idx_t i = 0; i < tm->RowCount(); ++i) {
      std::string token0 = get_hex(tm, 0, i);
      std::string token1 = get_hex(tm, 1, i);
      std::string cid = get_hex(tm, 2, i);
      uint32_t cond_idx = intern_condition(cid, 2, ConditionSource::PolymarketTokenReg);
      // TokenRegistered keeps a binary market ordering: token0 -> outcome 0, token1 -> outcome 1.
      intern_token(token0, cond_idx, 0, TokenSource::PolymarketTokenReg);
      intern_token(token1, cond_idx, 1, TokenSource::PolymarketTokenReg);
    }
  }

  struct FPMMRow {
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
    intern_fpmm(row.addr, primary_cond_idx, coll_id);
    // 为 FPMM 计算所有 atomic position token_id（覆盖多条件组合头寸）
    intern_fpmm_tokens(row.cids, row.collateral, primary_cond_idx);
  };
  auto fpmm = query_block_range(
      *conn, stage1_db_,
      "SELECT fpmm_addr, condition_ids, collateral_token, conditional_tokens FROM ",
      "fpmm", start, end);
  if (fpmm) {
    pending_fpmm_rows.reserve(fpmm->RowCount());
    for (idx_t i = 0; i < fpmm->RowCount(); ++i) {
      pending_fpmm_rows.push_back(FPMMRow{
          .addr = get_hex(fpmm, 0, i),
          .collateral = get_hex_lower(fpmm, 2, i),
          .conditional_tokens = get_hex_lower(fpmm, 3, i),
          .cids = parse_bytes32_list_hex_lower(fpmm->GetValue(1, i)),
      });
    }
  }

  auto infer_outcome_count = [&](const std::string &lower_cid, const std::vector<int64_t> &index_sets) -> uint8_t {
    uint8_t inferred = 0;
    for (int64_t index_set_i64 : index_sets) {
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

  // 从 split/merge/redemption 事件中提取 token_id（覆盖没有经过 FPMM 的 condition）
  auto update_from_condition_event = [&](const char *table_name,
                                         const char *index_sets_col,
                                         ConditionSource cond_source,
                                         TokenSource token_source) {
    auto rows = query_block_range(
        *conn, stage1_db_,
        std::string("SELECT condition_id, collateral_token, ") + index_sets_col + " FROM ",
        table_name, start, end);
    if (!rows) {
      return;
    }
    struct CondInference {
      std::string collateral;
      uint8_t outcome_count = 0;
    };
    std::unordered_map<std::string, CondInference> inferred_map;
    auto choose_canonical_collateral = [&](const std::string &lhs, const std::string &rhs) {
      if (lhs == rhs)
        return lhs;
      uint8_t lhs_id = addr_to_known_collateral_id(lhs);
      uint8_t rhs_id = addr_to_known_collateral_id(rhs);
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
      std::string lower_cid = get_hex_lower(rows, 0, i);
      std::string collateral = get_hex_lower(rows, 1, i);
      std::vector<int64_t> index_sets = parse_u256_list_i64(rows->GetValue(2, i));
      uint8_t inferred_count = infer_outcome_count(lower_cid, index_sets);
      auto it = inferred_map.find(lower_cid);
      if (it == inferred_map.end()) {
        inferred_map.emplace(lower_cid, CondInference{collateral, inferred_count});
        continue;
      }
      it->second.collateral = choose_canonical_collateral(it->second.collateral, collateral);
      if (inferred_count > it->second.outcome_count) {
        it->second.outcome_count = inferred_count;
      }
    }
    for (const auto &[lower_cid, inf] : inferred_map) {
      uint32_t cond_idx = intern_condition(lower_cid, inf.outcome_count, cond_source);
      uint8_t coll_id = intern_collateral(inf.collateral);
      set_cond_collateral(cond_idx, coll_id);
      intern_condition_tokens(lower_cid, inf.collateral, cond_idx, token_source);
    }
  };
  update_from_condition_event("split", "partition", ConditionSource::SplitEvent, TokenSource::SplitEvent);
  update_from_condition_event("merge", "partition", ConditionSource::MergeEvent, TokenSource::MergeEvent);
  update_from_condition_event("redemption", "index_sets", ConditionSource::RedemptionEvent, TokenSource::RedemptionEvent);

  for (const auto &row : pending_fpmm_rows) {
    process_fpmm_row(row);
  }

  auto nrq = query_block_range(
      *conn, stage1_db_,
      "SELECT market_id, question_id FROM ", "neg_risk_question",
      start, end);
  if (nrq) {
    for (idx_t i = 0; i < nrq->RowCount(); ++i) {
      std::string market_id = get_hex_lower(nrq, 0, i);
      std::string question_id = get_hex_lower(nrq, 1, i);

      if (!cond_to_market_.count(question_id)) {
        cond_to_market_[question_id] = market_id;
        seen_markets_.insert(market_id);
        new_neg_risk_markets_.push_back({question_id, market_id});
        progress_.total_markets = seen_markets_.size();
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
        negrisk_cond_idxs_.insert(it->second);
      }
    }
  }
  update_cond_type_stats();
}

void EventBuilder::phase2_build_semantic_index(int64_t start, int64_t end) {
  TraceN("s2/phase2_idx");
  auto conn = stage1_db_.create_connection();
  auto get_i64 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return tbl->GetValue(col, row).GetValue<int64_t>();
  };
  auto get_u256_i64 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return u256_blob_to_i64(tbl->GetValue(col, row));
  };
  auto get_hex = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return blob_to_hex(tbl->GetValue(col, row).GetValueUnsafe<std::string>());
  };
  auto get_hex_lower = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return to_lower(get_hex(tbl, col, row));
  };
  auto build_tx_key = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, idx_t row) {
    TxKey key;
    key.block = get_i64(tbl, 0, row);
    key.tx_hash = hex_to_bytes32(get_hex(tbl, 1, row));
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

  auto split = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, condition_id, amount, stakeholder, collateral_token, parent_collection_id, partition FROM ",
      "split", start, end);
  if (split) {
    src_split_rows = static_cast<int64_t>(split->RowCount());
    for (idx_t i = 0; i < split->RowCount(); ++i) {
      TxKey key = build_tx_key(split, i);
      SplitInfo info;
      info.log_index = get_i64(split, 2, i);
      info.cond_id = get_hex_lower(split, 3, i);
      info.amount = get_u256_i64(split, 4, i);
      info.stakeholder = get_hex_lower(split, 5, i);
      info.collateral_token = get_hex_lower(split, 6, i);
      info.parent_collection_id = get_hex_lower(split, 7, i);
      info.partition = parse_bytes32_list_hex_lower(split->GetValue(8, i));
      tx_split_[key].push_back(info);
      idx_split_rows++;
      record_semantic(key, info.log_index);
    }
  }

  auto merge = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, condition_id, amount, stakeholder, collateral_token, parent_collection_id, partition FROM ",
      "merge", start, end);
  if (merge) {
    src_merge_rows = static_cast<int64_t>(merge->RowCount());
    for (idx_t i = 0; i < merge->RowCount(); ++i) {
      TxKey key = build_tx_key(merge, i);
      MergeInfo info;
      info.log_index = get_i64(merge, 2, i);
      info.cond_id = get_hex_lower(merge, 3, i);
      info.amount = get_u256_i64(merge, 4, i);
      info.stakeholder = get_hex_lower(merge, 5, i);
      info.collateral_token = get_hex_lower(merge, 6, i);
      info.parent_collection_id = get_hex_lower(merge, 7, i);
      info.partition = parse_bytes32_list_hex_lower(merge->GetValue(8, i));
      tx_merge_[key].push_back(info);
      idx_merge_rows++;
      record_semantic(key, info.log_index);
    }
  }

  auto redemption = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, condition_id, payout, redeemer, collateral_token, parent_collection_id, index_sets FROM ",
      "redemption", start, end);
  if (redemption) {
    src_redemption_rows = static_cast<int64_t>(redemption->RowCount());
    for (idx_t i = 0; i < redemption->RowCount(); ++i) {
      TxKey key = build_tx_key(redemption, i);
      RedemptionInfo info;
      info.log_index = get_i64(redemption, 2, i);
      info.cond_id = get_hex_lower(redemption, 3, i);
      info.payout = get_u256_i64(redemption, 4, i);
      info.redeemer = get_hex_lower(redemption, 5, i);
      info.collateral_token = get_hex_lower(redemption, 6, i);
      info.parent_collection_id = get_hex_lower(redemption, 7, i);
      info.index_sets = parse_bytes32_list_hex_lower(redemption->GetValue(8, i));
      tx_redemption_[key].push_back(info);
      idx_redemption_rows++;
      record_semantic(key, info.log_index);
    }
  }

  auto convert = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, market_id, index_set, amount, stakeholder FROM ",
      "convert", start, end);
  if (convert) {
    src_convert_rows = static_cast<int64_t>(convert->RowCount());
    for (idx_t i = 0; i < convert->RowCount(); ++i) {
      std::string market_id = get_hex_lower(convert, 3, i);
      TxMarketKey key;
      key.block = get_i64(convert, 0, i);
      key.tx_hash = hex_to_bytes32(get_hex(convert, 1, i));
      key.market_id = market_id;
      TxKey tx_key;
      tx_key.block = key.block;
      tx_key.tx_hash = key.tx_hash;
      ConvertInfo info;
      info.log_index = get_i64(convert, 2, i);
      info.market_id = market_id;
      info.index_set = get_u256_i64(convert, 4, i);
      info.amount = get_u256_i64(convert, 5, i);
      info.stakeholder = get_hex_lower(convert, 6, i);
      tx_convert_[key].push_back(info);
      idx_convert_rows++;
      record_semantic(tx_key, info.log_index);
    }
  }

  auto order = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, maker, taker, maker_asset_id, taker_asset_id, "
      "maker_amount, taker_amount, fee FROM ",
      "order_filled", start, end);
  static const std::string ZERO_TOKEN_ID =
      "0x0000000000000000000000000000000000000000000000000000000000000000";
  if (order) {
    src_order_rows = static_cast<int64_t>(order->RowCount());
    for (idx_t i = 0; i < order->RowCount(); ++i) {
      int64_t block = get_i64(order, 0, i);
      auto tx_hash = hex_to_bytes32(get_hex(order, 1, i));
      int64_t log_index = get_i64(order, 2, i);
      std::string maker = get_hex_lower(order, 3, i);
      std::string taker = get_hex_lower(order, 4, i);
      std::string maker_asset = get_hex(order, 5, i);
      std::string taker_asset = get_hex(order, 6, i);
      int64_t maker_amt = get_u256_i64(order, 7, i);
      int64_t taker_amt = get_u256_i64(order, 8, i);
      int64_t fee = get_u256_i64(order, 9, i);

      bool maker_is_usdc = maker_asset == ZERO_TOKEN_ID;
      std::string token_id = maker_is_usdc ? taker_asset : maker_asset;
      std::string token_id_lower = to_lower(token_id);

      TxTokenKey key{block, tx_hash, token_id_lower};
      TxKey tx_key{block, tx_hash};
      OrderInfo info;
      info.log_index = log_index;
      info.token_id = token_id_lower;
      info.maker = maker;
      info.taker = taker;
      info.maker_side = maker_is_usdc ? 1 : 2;
      info.usdc = maker_is_usdc ? maker_amt : taker_amt;
      info.tokens = maker_is_usdc ? taker_amt : maker_amt;
      info.fee = fee;
      tx_order_[key].push_back(info);
      idx_order_rows++;
      record_semantic(tx_key, info.log_index);
    }
  }

  auto fpmm_trade = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, fpmm_addr, trader, side, outcome_index, "
      "usdc_amount, token_amount FROM ",
      "fpmm_trade", start, end);
  if (fpmm_trade) {
    src_fpmm_trade_rows = static_cast<int64_t>(fpmm_trade->RowCount());
    for (idx_t i = 0; i < fpmm_trade->RowCount(); ++i) {
      std::string fpmm_addr = get_hex_lower(fpmm_trade, 3, i);
      if (fpmm_map_.find(fpmm_addr) == fpmm_map_.end()) {
        skipped_fpmm_trade_unknown_fpmm_rows++;
        continue;
      }
      TxFPMMKey key;
      key.block = get_i64(fpmm_trade, 0, i);
      key.tx_hash = hex_to_bytes32(get_hex(fpmm_trade, 1, i));
      key.fpmm_addr = fpmm_addr;
      TxKey tx_key{key.block, key.tx_hash};
      FPMMTradeInfo info;
      info.log_index = get_i64(fpmm_trade, 2, i);
      info.fpmm_addr = fpmm_addr;
      info.trader = get_hex_lower(fpmm_trade, 4, i);
      info.side = fpmm_trade->GetValue(5, i).GetValue<int>();
      stage2_assert(info.side == 1 || info.side == 2,
                    AssertLevel::L0, "Input", "FPMMTradeSideRange");
      int64_t outcome_idx = get_u256_i64(fpmm_trade, 6, i);
      stage2_assert(outcome_idx >= 0 && outcome_idx <= std::numeric_limits<int>::max(),
                      AssertLevel::L0, "Input", "OutcomeIdxFitsInt");
      info.outcome_idx = static_cast<int>(outcome_idx);
      info.usdc = get_u256_i64(fpmm_trade, 7, i);
      info.tokens = get_u256_i64(fpmm_trade, 8, i);
      info.requires_erc1155_leg = (info.tokens > 0 && info.trader != info.fpmm_addr);
      tx_fpmm_trade_[key].push_back(info);
      idx_fpmm_trade_rows++;
      record_semantic(tx_key, info.log_index);
    }
  }

  auto fpmm_funding = query_block_range(
      *conn, stage1_db_,
      "SELECT block_number, tx_hash, log_index, fpmm_addr, funder, side, amounts FROM ",
      "fpmm_funding", start, end);
  if (fpmm_funding) {
    src_fpmm_funding_rows = static_cast<int64_t>(fpmm_funding->RowCount());
    for (idx_t i = 0; i < fpmm_funding->RowCount(); ++i) {
      std::string fpmm_addr = get_hex_lower(fpmm_funding, 3, i);
      if (fpmm_map_.find(fpmm_addr) == fpmm_map_.end()) {
        skipped_fpmm_funding_unknown_fpmm_rows++;
        continue;
      }
      TxFPMMKey key;
      key.block = get_i64(fpmm_funding, 0, i);
      key.tx_hash = hex_to_bytes32(get_hex(fpmm_funding, 1, i));
      key.fpmm_addr = fpmm_addr;
      TxKey tx_key{key.block, key.tx_hash};
      FPMMFundingInfo info;
      info.log_index = get_i64(fpmm_funding, 2, i);
      info.fpmm_addr = fpmm_addr;
      info.funder = get_hex_lower(fpmm_funding, 4, i);
      info.side = fpmm_funding->GetValue(5, i).GetValue<int>();
      info.amounts = parse_u256_list_i64(fpmm_funding->GetValue(6, i));
      tx_fpmm_funding_[key].push_back(info);
      idx_fpmm_funding_rows++;
      record_semantic(tx_key, info.log_index);
    }
  }

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
  auto conn = stage1_db_.create_connection();
  auto get_i64 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return tbl->GetValue(col, row).GetValue<int64_t>();
  };
  auto get_u256_i64 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return u256_blob_to_i64(tbl->GetValue(col, row));
  };
  auto get_hex = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return blob_to_hex(tbl->GetValue(col, row).GetValueUnsafe<std::string>());
  };
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
  std::unordered_map<TxTokenKey, std::vector<ObservedOrderLeg>> observed_order_legs;
  struct ObservedCondLeg {
    std::string from;
    std::string to;
    int64_t amount = 0;
    uint32_t cond_idx = UNKNOWN_COND_IDX;
    int64_t base_log_index = -1;
  };
  std::unordered_map<TxKey, std::vector<ObservedCondLeg>> observed_cond_legs;
  int64_t observed_convert_user_burn_rows = 0;
  int64_t observed_convert_adapter_burn_rows = 0;
  int64_t classified_convert_rows = 0;
  int64_t classified_internal_burn_convert_rows = 0;

  auto resolve_transfer_collateral = [&](uint32_t cid_idx, const std::string &operator_addr) {
    auto coll_it = cond_collateral_.find(cid_idx);
    if (coll_it != cond_collateral_.end())
      return static_cast<Collateral>(coll_it->second);
    // 对 TransferInferred token，优先用 FPMM operator 的 collateral 回填
    // 避免在 FPMM Buy/Sell/LP 事件中出现无意义的 0x000...000
    auto fit = fpmm_map_.find(operator_addr);
    if (fit != fpmm_map_.end())
      return static_cast<Collateral>(fit->second.collateral);
    return Collateral::Unknown;
  };

  if (!transfers) {
    return;
  }

  for (idx_t i = 0; i < transfers->RowCount(); ++i) {
    int64_t block = get_i64(transfers, 0, i);
    int64_t log_idx = get_i64(transfers, 2, i);
    stage2_assert(log_idx >= 0 && log_idx < SORT_KEY_SCALE,
                    AssertLevel::L0, "Input", "TransferLogIndexRange");
    int64_t amount = get_u256_i64(transfers, 7, i);
    std::string op = to_lower(get_hex(transfers, 3, i));
    std::string from = to_lower(get_hex(transfers, 4, i));
    std::string to = to_lower(get_hex(transfers, 5, i));
    std::string token_id = to_lower(get_hex(transfers, 6, i));
    if (op == NEG_RISK_ADAPTER && to == NO_TOKEN_BURN_ADDRESS && amount > 0) {
      if (from == NEG_RISK_ADAPTER) {
        observed_convert_adapter_burn_rows++;
      } else {
        observed_convert_user_burn_rows++;
      }
    }

    int64_t sort_key = block * SORT_KEY_SCALE + log_idx;
    auto tx_hash = hex_to_bytes32(get_hex(transfers, 1, i));
    if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
      TxTokenKey order_key{block, tx_hash, token_id};
      observed_order_legs[order_key].push_back(ObservedOrderLeg{from, to, amount});
    }
    if (fpmm_map_.find(op) != fpmm_map_.end()) {
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
      // 未知token：加入 token_map_，使用特殊值表示未知 condition
      intern_token(token_id, UNKNOWN_COND_IDX, UNKNOWN_TOKEN_IDX, TokenSource::TransferInferred);
      tit = token_map_.find(token_id); // 重新获取迭代器
    }

    uint32_t cond_idx = tit->second.cond_idx;
    uint8_t token_idx = UNKNOWN_TOKEN_IDX;
    if (tit->second.token_idx != UNKNOWN_TOKEN_IDX) {
      token_idx = tit->second.token_idx;
    }
    if (amount > 0 && cond_idx != UNKNOWN_COND_IDX) {
      TxKey tx_key{block, tx_hash};
      observed_cond_legs[tx_key].push_back(
          ObservedCondLeg{from, to, amount, cond_idx, log_idx / TRANSFER_FLAT_LOG_SCALE});
    }

    // 获取抵押品类型
    Collateral collateral = resolve_transfer_collateral(cond_idx, op);

    {
      std::ostringstream oss;
      oss << "block=" << block
          << " tx=" << get_hex(transfers, 1, i)
          << " log_index=" << log_idx
          << " op=" << op
          << " from=" << from
          << " to=" << to
          << " token_id=" << token_id
          << " amount=" << amount
          << " cond_idx=" << cond_idx
          << " token_idx=" << static_cast<int>(token_idx)
          << " collateral=" << static_cast<int>(collateral);
      current_transfer_context_ = oss.str();
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
  stage2_assert(classified_convert_rows == observed_convert_user_burn_rows,
                AssertLevel::L4, "Partition", "ConvertUserBurnRowsCovered");
  stage2_assert(classified_internal_burn_convert_rows == observed_convert_adapter_burn_rows,
                AssertLevel::L4, "Partition", "ConvertInternalBurnRowsCovered");

  // Semantic coverage assertions: every semantic op in this chunk must be consumed by at least one transfer leg.
  auto cond_idx_for = [&](const std::string &cond_id) -> uint32_t {
    auto it = cond_map_.find(cond_id);
    if (it == cond_map_.end()) {
      return UNKNOWN_COND_IDX;
    }
    return it->second;
  };
  auto semantic_window_left = [&](const TxKey &tx_key, int64_t semantic_log_index) {
    auto bit = tx_op_bounds_.find(tx_key);
    if (bit == tx_op_bounds_.end()) {
      return int64_t{-1};
    }
    for (const auto &b : bit->second) {
      if (b.right_inclusive == semantic_log_index) {
        return b.left_exclusive;
      }
    }
    return int64_t{-1};
  };
  auto split_leg_observed = [&](const TxKey &tx_key, const std::string &cond_id,
                                const std::string &stakeholder, int64_t amount,
                                int64_t semantic_log_index) {
    uint32_t cond_idx = cond_idx_for(cond_id);
    if (cond_idx == UNKNOWN_COND_IDX || amount <= 0) {
      return false;
    }
    int64_t left = semantic_window_left(tx_key, semantic_log_index);
    auto it = observed_cond_legs.find(tx_key);
    if (it == observed_cond_legs.end()) {
      return false;
    }
    for (const auto &leg : it->second) {
      if (leg.base_log_index <= left || leg.base_log_index > semantic_log_index) {
        continue;
      }
      if (leg.cond_idx != cond_idx || leg.amount != amount) {
        continue;
      }
      // Split semantic certainty comes from child mint legs.
      if (leg.from == ZERO_ADDR && leg.to == stakeholder) {
        return true;
      }
    }
    return false;
  };
  auto merge_leg_observed = [&](const TxKey &tx_key, const std::string &cond_id,
                                const std::string &stakeholder, int64_t amount,
                                int64_t semantic_log_index) {
    uint32_t cond_idx = cond_idx_for(cond_id);
    if (cond_idx == UNKNOWN_COND_IDX || amount <= 0) {
      return false;
    }
    int64_t left = semantic_window_left(tx_key, semantic_log_index);
    auto it = observed_cond_legs.find(tx_key);
    if (it == observed_cond_legs.end()) {
      return false;
    }
    for (const auto &leg : it->second) {
      if (leg.base_log_index <= left || leg.base_log_index > semantic_log_index) {
        continue;
      }
      if (leg.cond_idx != cond_idx || leg.amount != amount) {
        continue;
      }
      // Merge semantic certainty comes from child burn legs.
      if (leg.from == stakeholder && leg.to == ZERO_ADDR) {
        return true;
      }
    }
    return false;
  };
  auto redeem_leg_observed = [&](const TxKey &tx_key, const std::string &cond_id,
                                 const std::string &redeemer,
                                 int64_t semantic_log_index) {
    uint32_t cond_idx = cond_idx_for(cond_id);
    if (cond_idx == UNKNOWN_COND_IDX) {
      return false;
    }
    int64_t left = semantic_window_left(tx_key, semantic_log_index);
    auto it = observed_cond_legs.find(tx_key);
    if (it == observed_cond_legs.end()) {
      return false;
    }
    for (const auto &leg : it->second) {
      if (leg.base_log_index <= left || leg.base_log_index > semantic_log_index) {
        continue;
      }
      if (leg.cond_idx != cond_idx) {
        continue;
      }
      if (leg.from == redeemer && leg.to == ZERO_ADDR && leg.amount > 0) {
        return true;
      }
    }
    return false;
  };
  int64_t split_rows_total = 0;
  int64_t split_rows_consumed = 0;
  int64_t split_rows_covered = 0;
  int64_t split_rows_zero = 0;
  int64_t split_rows_not_required = 0;
  for (const auto &[tx_key, rows] : tx_split_) {
    for (const auto &row : rows) {
      // split amount==0 is a valid semantic marker with no effective ERC1155 leg.
      bool zero_amount_split = (row.amount == 0);
      bool must_consume = split_leg_observed(tx_key, row.cond_id, row.stakeholder,
                                             row.amount, row.log_index);
      bool consumed = row.consumed_count > 0;
      bool covered = row.covered_by_parent;
      bool not_required = !must_consume;
      stage2_assert(consumed || covered || zero_amount_split || not_required,
                    AssertLevel::L4, "Consume", "SplitConsumedOrCoveredByParent");
      split_rows_total++;
      if (consumed) {
        split_rows_consumed++;
      } else if (covered) {
        split_rows_covered++;
      } else if (zero_amount_split) {
        split_rows_zero++;
      } else {
        split_rows_not_required++;
      }
    }
  }
  stage2_assert(split_rows_total == split_rows_consumed + split_rows_covered + split_rows_zero + split_rows_not_required,
                AssertLevel::L4, "Partition", "SplitRowPartitionTotal");

  int64_t merge_rows_total = 0;
  int64_t merge_rows_consumed = 0;
  int64_t merge_rows_covered = 0;
  int64_t merge_rows_zero = 0;
  int64_t merge_rows_not_required = 0;
  for (const auto &[tx_key, rows] : tx_merge_) {
    for (const auto &row : rows) {
      // merge amount==0 is a valid semantic marker with no effective ERC1155 leg.
      bool zero_amount_merge = (row.amount == 0);
      bool must_consume = merge_leg_observed(tx_key, row.cond_id, row.stakeholder,
                                             row.amount, row.log_index);
      bool consumed = row.consumed_count > 0;
      bool covered = row.covered_by_parent;
      bool not_required = !must_consume;
      stage2_assert(consumed || covered || zero_amount_merge || not_required,
                    AssertLevel::L4, "Consume", "MergeConsumedOrCoveredByParent");
      merge_rows_total++;
      if (consumed) {
        merge_rows_consumed++;
      } else if (covered) {
        merge_rows_covered++;
      } else if (zero_amount_merge) {
        merge_rows_zero++;
      } else {
        merge_rows_not_required++;
      }
    }
  }
  stage2_assert(merge_rows_total == merge_rows_consumed + merge_rows_covered + merge_rows_zero + merge_rows_not_required,
                AssertLevel::L4, "Partition", "MergeRowPartitionTotal");

  int64_t redeem_rows_total = 0;
  int64_t redeem_rows_consumed = 0;
  int64_t redeem_rows_covered = 0;
  int64_t redeem_rows_not_required = 0;
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
      redeem_rows_total++;
      if (consumed) {
        redeem_rows_consumed++;
      } else if (covered) {
        redeem_rows_covered++;
      } else {
        redeem_rows_not_required++;
      }
    }
  }
  stage2_assert(redeem_rows_total == redeem_rows_consumed + redeem_rows_covered + redeem_rows_not_required,
                AssertLevel::L4, "Partition", "RedeemRowPartitionTotal");

  int64_t order_rows_total = 0;
  int64_t order_rows_consumed = 0;
  int64_t order_rows_unobserved = 0;
  for (const auto &[key, rows] : tx_order_) {
    auto order_leg_observed = [&](const OrderInfo &row) {
      auto oit = observed_order_legs.find(key);
      if (oit == observed_order_legs.end()) {
        return false;
      }
      for (const auto &leg : oit->second) {
        if (leg.amount != row.tokens) {
          continue;
        }
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
      bool must_consume = order_leg_observed(row);
      bool consumed = row.consumed;
      bool unobserved = !must_consume;
      stage2_assert(consumed || unobserved, AssertLevel::L4, "Consume", "OrderConsumedIfLegObserved");
      order_rows_total++;
      if (consumed) {
        order_rows_consumed++;
      } else {
        order_rows_unobserved++;
      }
    }
  }
  stage2_assert(order_rows_total == order_rows_consumed + order_rows_unobserved,
                AssertLevel::L4, "Partition", "OrderRowPartitionTotal");

  int64_t trade_rows_total = 0;
  int64_t trade_rows_consumed = 0;
  int64_t trade_rows_explained = 0;
  int64_t trade_rows_not_required = 0;
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
      trade_rows_total++;
      if (consumed) {
        trade_rows_consumed++;
      } else if (explained) {
        trade_rows_explained++;
      } else {
        trade_rows_not_required++;
      }
    }
  }
  stage2_assert(trade_rows_total == trade_rows_consumed + trade_rows_explained + trade_rows_not_required,
                AssertLevel::L4, "Partition", "FPMMTradeRowPartitionTotal");

  int64_t convert_rows_total = 0;
  int64_t convert_rows_consumed = 0;
  for (const auto &[_, rows] : tx_convert_) {
    for (const auto &row : rows) {
      convert_rows_total++;
      stage2_assert(row.consumed_count > 0, AssertLevel::L4, "Consume", "ConvertConsumed");
      if (row.consumed_count > 0) {
        convert_rows_consumed++;
      }
    }
  }
  stage2_assert(convert_rows_total == convert_rows_consumed,
                AssertLevel::L4, "Partition", "ConvertRowPartitionTotal");

  int64_t funding_rows_total = 0;
  int64_t funding_rows_consumed = 0;
  int64_t funding_rows_zero_leg_remove = 0;
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
      funding_rows_total++;
      if (consumed) {
        funding_rows_consumed++;
      } else {
        funding_rows_zero_leg_remove++;
      }
    }
  }
  stage2_assert(funding_rows_total == funding_rows_consumed + funding_rows_zero_leg_remove,
                AssertLevel::L4, "Partition", "FPMMFundingRowPartitionTotal");
}

void EventBuilder::commit_chunk(int64_t new_cursor) {
  TraceN("s2/commit");
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

  if (!new_conditions_.empty()) {
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_condition ("
             "cond_idx INTEGER, cond_id BLOB, outcome_cnt INTEGER, "
             "payout_0 BIGINT, payout_1 BIGINT, payout_2 BIGINT, payout_3 BIGINT, "
             "payout_4 BIGINT, payout_5 BIGINT, payout_6 BIGINT, payout_7 BIGINT, "
             "question_id BLOB, source INTEGER)");
    exec_sql("DELETE FROM tmp_rb_condition");
    {
      duckdb::Appender ap(*conn, "tmp_rb_condition");
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
      ap.Close();
    }
    exec_sql(
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
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_token ("
             "token_id BLOB, cond_idx INTEGER, token_idx INTEGER, source INTEGER)");
    exec_sql("DELETE FROM tmp_rb_token");
    {
      duckdb::Appender ap(*conn, "tmp_rb_token");
      for (auto &nt : new_tokens_) {
        int32_t db_cond_idx = (nt.cond_idx == UNKNOWN_COND_IDX) ? -1 : static_cast<int32_t>(nt.cond_idx);
        ap.BeginRow();
        append_blob(ap, nt.token_id);
        ap.Append(db_cond_idx);
        ap.Append(static_cast<int32_t>(nt.token_idx));
        ap.Append(static_cast<int32_t>(nt.source));
        ap.EndRow();
      }
      ap.Close();
    }
    exec_sql(
        "INSERT INTO rb_token SELECT * FROM tmp_rb_token "
        "ON CONFLICT(token_id) DO UPDATE SET "
        "cond_idx=excluded.cond_idx, "
        "token_idx=excluded.token_idx, "
        "source=excluded.source");
  }

  if (!new_fpmms_.empty()) {
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_fpmm ("
             "fpmm_addr BLOB, cond_idx INTEGER, collateral INTEGER)");
    exec_sql("DELETE FROM tmp_rb_fpmm");
    {
      duckdb::Appender ap(*conn, "tmp_rb_fpmm");
      for (auto &nf : new_fpmms_) {
        ap.BeginRow();
        append_blob(ap, nf.addr);
        ap.Append(static_cast<int32_t>(nf.cond_idx));
        ap.Append(static_cast<int32_t>(nf.collateral));
        ap.EndRow();
      }
      ap.Close();
    }
    exec_sql("INSERT OR IGNORE INTO rb_fpmm SELECT * FROM tmp_rb_fpmm");
  }

  if (!new_collaterals_.empty()) {
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_collateral ("
             "coll_id INTEGER, collateral_addr BLOB)");
    exec_sql("DELETE FROM tmp_rb_collateral");
    {
      duckdb::Appender ap(*conn, "tmp_rb_collateral");
      for (auto &nc : new_collaterals_) {
        ap.BeginRow();
        ap.Append(static_cast<int32_t>(nc.coll_id));
        append_blob(ap, nc.addr);
        ap.EndRow();
      }
      ap.Close();
    }
    exec_sql("INSERT OR IGNORE INTO rb_collateral SELECT * FROM tmp_rb_collateral");
  }

  if (!new_cond_collaterals_.empty()) {
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_cond_collateral ("
             "cond_idx INTEGER, coll_id INTEGER)");
    exec_sql("DELETE FROM tmp_rb_cond_collateral");
    {
      duckdb::Appender ap(*conn, "tmp_rb_cond_collateral");
      for (auto &nc : new_cond_collaterals_) {
        ap.BeginRow();
        ap.Append(static_cast<int32_t>(nc.cond_idx));
        ap.Append(static_cast<int32_t>(nc.coll_id));
        ap.EndRow();
      }
      ap.Close();
    }
    exec_sql("INSERT OR REPLACE INTO rb_cond_collateral SELECT * FROM tmp_rb_cond_collateral");
  }

  if (!new_neg_risk_markets_.empty()) {
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_neg_risk_market ("
             "question_id BLOB, market_id BLOB)");
    exec_sql("DELETE FROM tmp_rb_neg_risk_market");
    {
      duckdb::Appender ap(*conn, "tmp_rb_neg_risk_market");
      for (auto &nm : new_neg_risk_markets_) {
        ap.BeginRow();
        append_blob(ap, nm.question_id);
        append_blob(ap, nm.market_id);
        ap.EndRow();
      }
      ap.Close();
    }
    exec_sql("INSERT OR IGNORE INTO rb_neg_risk_market SELECT * FROM tmp_rb_neg_risk_market");
  }

  if (!new_events_.empty()) {
    exec_sql("CREATE TEMP TABLE IF NOT EXISTS tmp_user_event ("
             "user_addr BLOB, sort_key BIGINT, cond_idx INTEGER, "
             "event_type INTEGER, token_idx INTEGER, collateral INTEGER, amount BIGINT, price BIGINT)");
    exec_sql("DELETE FROM tmp_user_event");

    {
      duckdb::Appender appender(*conn, "tmp_user_event");
      for (auto &[user, evt] : new_events_) {
        std::string user_blob = hex_to_blob(user);
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

  exec_sql("INSERT OR REPLACE INTO stage2_cursor(key, value) VALUES ('last_block', " +
           std::to_string(new_cursor) + ")");

  auto save_cnt = [&](const char *key, int64_t val) {
    exec_sql("INSERT OR REPLACE INTO stage2_cursor(key, value) VALUES ('" + std::string(key) +
             "', " + std::to_string(val) + ")");
  };
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

  exec_sql("COMMIT");

  auto user_cnt = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM user_event");
  stage2_assert(user_cnt && !user_cnt->HasError(), AssertLevel::L0, "DB", "UserCountQuerySuccess");
  progress_.total_users = user_cnt->RowCount() > 0 ? user_cnt->GetValue(0, 0).GetValue<int64_t>() : 0;
}

} // namespace stage2
