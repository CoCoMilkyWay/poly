#include "event_build.hpp"
#include "misc/profiler.hpp"

namespace stage2 {
namespace {

inline std::string block_range_query(Database &db, const std::string &select_sql,
                                     const std::string &table_name,
                                     int64_t start, int64_t end) {
  return select_sql + db.feather_table_range(table_name, start, end) +
         " WHERE block_number > " + std::to_string(start) +
         " AND block_number <= " + std::to_string(end);
}

} // namespace

void EventBuilder::phase1_update_mappings(int64_t start, int64_t end) {
  TraceN("s2/phase1_map");
  auto conn = stage1_db_.create_connection();
  auto get_i32 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return tbl->GetValue(col, row).GetValue<int>();
  };
  auto get_hex = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return blob_to_hex(tbl->GetValue(col, row).GetValueUnsafe<std::string>());
  };
  auto get_hex_lower = [&](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return to_lower(get_hex(tbl, col, row));
  };

  auto cp = conn->Query(block_range_query(
      stage1_db_, "SELECT condition_id, outcome_slot_count, question_id FROM ",
      "condition_preparation", start, end));
  for (idx_t i = 0; i < cp->RowCount(); ++i) {
    std::string cid = get_hex(cp, 0, i);
    int cnt = get_i32(cp, 1, i);
    std::string qid = get_hex_lower(cp, 2, i);
    intern_condition(cid, cnt, ConditionSource::ConditionPrep, qid);
  }

  auto cr = conn->Query(block_range_query(
      stage1_db_, "SELECT condition_id, payout_numerators FROM ",
      "condition_resolution", start, end));
  for (idx_t i = 0; i < cr->RowCount(); ++i) {
    std::string cid = get_hex(cr, 0, i);
    std::string lower = to_lower(cid);
    auto it = cond_map_.find(lower);
    if (it == cond_map_.end())
      continue;
    std::string payout_str = cr->GetValue(1, i).GetValueUnsafe<std::string>();
    std::vector<int64_t> payouts;
    auto payout_arr = nlohmann::json::parse(payout_str);
    for (const auto &v : payout_arr) {
      payouts.push_back(v.get<int64_t>());
    }
    update_condition_payout(it->second, payouts);
  }

  auto tm = conn->Query(block_range_query(
      stage1_db_, "SELECT token0, token1, condition_id FROM ", "token_map", start, end));
  for (idx_t i = 0; i < tm->RowCount(); ++i) {
    std::string token0 = get_hex(tm, 0, i);
    std::string token1 = get_hex(tm, 1, i);
    std::string cid = get_hex(tm, 2, i);
    uint32_t cond_idx = intern_condition(cid, 2, ConditionSource::PolymarketTokenReg);
    intern_token(token0, cond_idx, 1, TokenSource::PolymarketTokenReg);
    intern_token(token1, cond_idx, 0, TokenSource::PolymarketTokenReg);
  }

  auto fpmm = conn->Query(block_range_query(
      stage1_db_, "SELECT fpmm_addr, condition_ids, collateral_token FROM ",
      "fpmm", start, end));
  for (idx_t i = 0; i < fpmm->RowCount(); ++i) {
    std::string addr = get_hex(fpmm, 0, i);
    std::string cids_json = fpmm->GetValue(1, i).GetValueUnsafe<std::string>();
    std::string collateral = get_hex_lower(fpmm, 2, i);

    auto cids_arr = nlohmann::json::parse(cids_json);
    assert(!cids_arr.empty());
    std::vector<std::string> cids;
    cids.reserve(cids_arr.size());
    uint32_t primary_cond_idx = 0;
    bool has_primary = false;
    for (const auto &v : cids_arr) {
      std::string cid = v.get<std::string>();
      cids.push_back(to_lower(cid));
      uint32_t idx = intern_condition(cid, 2, ConditionSource::PolymarketFPMM);
      if (!has_primary) {
        primary_cond_idx = idx;
        has_primary = true;
      }
    }
    assert(has_primary);

    uint8_t coll_id = intern_collateral(collateral);
    intern_fpmm(addr, primary_cond_idx, coll_id);
    // 为 FPMM 计算所有 atomic position token_id（覆盖多条件组合头寸）
    intern_fpmm_tokens(cids, collateral, primary_cond_idx);
  }

  // 从 split 事件中提取 token_id（覆盖没有经过 FPMM 的 condition）
  auto split_for_tokens = conn->Query(block_range_query(
      stage1_db_, "SELECT DISTINCT condition_id, collateral_token FROM ",
      "split", start, end));
  for (idx_t i = 0; i < split_for_tokens->RowCount(); ++i) {
    std::string cid = get_hex(split_for_tokens, 0, i);
    std::string lower_cid = to_lower(cid);
    std::string collateral = get_hex_lower(split_for_tokens, 1, i);

    uint32_t cond_idx = intern_condition(cid, 2, ConditionSource::SplitEvent);
    uint8_t coll_id = intern_collateral(collateral);
    set_cond_collateral(cond_idx, coll_id);
    intern_condition_tokens(lower_cid, collateral, cond_idx, TokenSource::SplitEvent);
  }

  auto nrq = conn->Query(block_range_query(
      stage1_db_, "SELECT market_id, question_id FROM ", "neg_risk_question",
      start, end));
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
  update_cond_type_stats();
}

void EventBuilder::phase2_build_semantic_index(int64_t start, int64_t end) {
  TraceN("s2/phase2_idx");
  auto conn = stage1_db_.create_connection();
  auto get_i64 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return tbl->GetValue(col, row).GetValue<int64_t>();
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

  auto split = conn->Query(block_range_query(
      stage1_db_,
      "SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM ",
      "split", start, end));
  for (idx_t i = 0; i < split->RowCount(); ++i) {
    TxKey key = build_tx_key(split, i);
    SplitInfo info;
    info.amount = get_i64(split, 3, i);
    info.stakeholder = get_hex_lower(split, 4, i);
    info.cond_id = get_hex_lower(split, 2, i);
    tx_split_[key].push_back(info);
  }

  auto merge = conn->Query(block_range_query(
      stage1_db_,
      "SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM ",
      "merge", start, end));
  for (idx_t i = 0; i < merge->RowCount(); ++i) {
    TxKey key = build_tx_key(merge, i);
    MergeInfo info;
    info.amount = get_i64(merge, 3, i);
    info.stakeholder = get_hex_lower(merge, 4, i);
    info.cond_id = get_hex_lower(merge, 2, i);
    tx_merge_[key].push_back(info);
  }

  auto redemption = conn->Query(block_range_query(
      stage1_db_,
      "SELECT block_number, tx_hash, condition_id, payout, redeemer FROM ",
      "redemption", start, end));
  for (idx_t i = 0; i < redemption->RowCount(); ++i) {
    TxKey key = build_tx_key(redemption, i);
    RedemptionInfo info;
    info.payout = get_i64(redemption, 3, i);
    info.redeemer = get_hex_lower(redemption, 4, i);
    info.cond_id = get_hex_lower(redemption, 2, i);
    tx_redemption_[key].push_back(info);
  }

  auto convert = conn->Query(block_range_query(
      stage1_db_,
      "SELECT block_number, tx_hash, market_id, index_set, amount, stakeholder FROM ",
      "convert", start, end));
  for (idx_t i = 0; i < convert->RowCount(); ++i) {
    std::string market_id = get_hex_lower(convert, 2, i);
    TxMarketKey key;
    key.block = get_i64(convert, 0, i);
    key.tx_hash = hex_to_bytes32(get_hex(convert, 1, i));
    key.market_id = market_id;
    ConvertInfo info;
    info.market_id = market_id;
    info.index_set = get_i64(convert, 3, i);
    info.amount = get_i64(convert, 4, i);
    info.stakeholder = get_hex_lower(convert, 5, i);
    tx_convert_[key].push_back(info);
  }

  auto order = conn->Query(block_range_query(
      stage1_db_,
      "SELECT block_number, tx_hash, maker, taker, maker_asset_id, taker_asset_id, "
      "maker_amount, taker_amount, fee FROM ",
      "order_filled", start, end));
  static const std::string ZERO_TOKEN_ID =
      "0x0000000000000000000000000000000000000000000000000000000000000000";
  for (idx_t i = 0; i < order->RowCount(); ++i) {
    int64_t block = get_i64(order, 0, i);
    auto tx_hash = hex_to_bytes32(get_hex(order, 1, i));
    std::string maker = get_hex_lower(order, 2, i);
    std::string taker = get_hex_lower(order, 3, i);
    std::string maker_asset = get_hex(order, 4, i);
    std::string taker_asset = get_hex(order, 5, i);
    int64_t maker_amt = get_i64(order, 6, i);
    int64_t taker_amt = get_i64(order, 7, i);
    int64_t fee = get_i64(order, 8, i);

    bool maker_is_usdc = maker_asset == ZERO_TOKEN_ID;
    std::string token_id = maker_is_usdc ? taker_asset : maker_asset;

    TxTokenKey key{block, tx_hash, to_lower(token_id)};
    assert(tx_order_.count(key) == 0 && "Duplicate order");
    OrderInfo info;
    info.maker = maker;
    info.taker = taker;
    info.maker_side = maker_is_usdc ? 1 : 2;
    info.usdc = maker_is_usdc ? maker_amt : taker_amt;
    info.tokens = maker_is_usdc ? taker_amt : maker_amt;
    info.fee = fee;
    tx_order_[key] = info;
  }

  auto fpmm_trade = conn->Query(block_range_query(
      stage1_db_,
      "SELECT block_number, tx_hash, fpmm_addr, trader, side, outcome_index, "
      "usdc_amount, token_amount FROM ",
      "fpmm_trade", start, end));
  for (idx_t i = 0; i < fpmm_trade->RowCount(); ++i) {
    std::string fpmm_addr = get_hex_lower(fpmm_trade, 2, i);
    TxFPMMKey key;
    key.block = get_i64(fpmm_trade, 0, i);
    key.tx_hash = hex_to_bytes32(get_hex(fpmm_trade, 1, i));
    key.fpmm_addr = fpmm_addr;
    assert(tx_fpmm_trade_.count(key) == 0 && "Duplicate FPMM trade");
    FPMMTradeInfo info;
    info.fpmm_addr = fpmm_addr;
    info.trader = get_hex_lower(fpmm_trade, 3, i);
    info.side = fpmm_trade->GetValue(4, i).GetValue<int>();
    info.outcome_idx = fpmm_trade->GetValue(5, i).GetValue<int>();
    info.usdc = get_i64(fpmm_trade, 6, i);
    info.tokens = get_i64(fpmm_trade, 7, i);
    tx_fpmm_trade_[key] = info;
  }

  auto fpmm_funding = conn->Query(block_range_query(
      stage1_db_,
      "SELECT block_number, tx_hash, fpmm_addr, funder, side, amounts FROM ",
      "fpmm_funding", start, end));
  for (idx_t i = 0; i < fpmm_funding->RowCount(); ++i) {
    std::string fpmm_addr = get_hex_lower(fpmm_funding, 2, i);
    TxFPMMKey key;
    key.block = get_i64(fpmm_funding, 0, i);
    key.tx_hash = hex_to_bytes32(get_hex(fpmm_funding, 1, i));
    key.fpmm_addr = fpmm_addr;
    assert(tx_fpmm_funding_.count(key) == 0 && "Duplicate FPMM funding");
    FPMMFundingInfo info;
    info.fpmm_addr = fpmm_addr;
    info.funder = get_hex_lower(fpmm_funding, 3, i);
    info.side = fpmm_funding->GetValue(4, i).GetValue<int>();
    std::string amounts_json = fpmm_funding->GetValue(5, i).GetValueUnsafe<std::string>();
    auto amounts_arr = nlohmann::json::parse(amounts_json);
    info.amounts_count = static_cast<int>(amounts_arr.size());
    info.amount0 = amounts_arr.size() > 0 ? amounts_arr[0].get<int64_t>() : 0;
    info.amount1 = amounts_arr.size() > 1 ? amounts_arr[1].get<int64_t>() : 0;
    tx_fpmm_funding_[key] = info;
  }
}

void EventBuilder::phase3_process_transfers(int64_t start, int64_t end) {
  TraceN("s2/phase3_xfer");
  auto conn = stage1_db_.create_connection();
  auto get_i64 = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return tbl->GetValue(col, row).GetValue<int64_t>();
  };
  auto get_hex = [](const duckdb::unique_ptr<duckdb::MaterializedQueryResult> &tbl, int col, idx_t row) {
    return blob_to_hex(tbl->GetValue(col, row).GetValueUnsafe<std::string>());
  };
  auto transfers = conn->Query(
      block_range_query(
          stage1_db_,
          "SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount FROM ",
          "transfer", start, end) +
      " ORDER BY block_number, log_index");

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

  for (idx_t i = 0; i < transfers->RowCount(); ++i) {
    int64_t block = get_i64(transfers, 0, i);
    int64_t log_idx = get_i64(transfers, 2, i);
    int64_t amount = get_i64(transfers, 7, i);
    std::string op = to_lower(get_hex(transfers, 3, i));
    std::string from = to_lower(get_hex(transfers, 4, i));
    std::string to = to_lower(get_hex(transfers, 5, i));
    std::string token_id = to_lower(get_hex(transfers, 6, i));

    int64_t sort_key = block * 1000000000LL + log_idx;
    auto tx_hash = hex_to_bytes32(get_hex(transfers, 1, i));

    auto tit = token_map_.find(token_id);

    if (tit == token_map_.end()) {
      // 未知token：加入 token_map_，使用特殊值表示未知 condition
      intern_token(token_id, UNKNOWN_COND_IDX, UNKNOWN_IS_YES, TokenSource::TransferInferred);
      tit = token_map_.find(token_id); // 重新获取迭代器
    }

    uint32_t cond_idx = tit->second.cond_idx;
    uint8_t token_idx = tit->second.is_yes ? 0 : 1;

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

    TransferClass cls = classify_and_emit(sort_key, tx_hash, block, op, from, to, token_id, amount, cond_idx, token_idx, collateral);
    chunk_xfer_stats_.add(cls);
  }
}

void EventBuilder::commit_chunk(int64_t new_cursor) {
  TraceN("s2/commit");
  auto conn = stage2_db_.create_connection();
  conn->Query("BEGIN TRANSACTION");

  auto append_blob = [](duckdb::Appender &ap, const std::string &hex) {
    std::string b = hex_to_blob(hex);
    ap.Append(duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(b.data()), b.size()));
  };

  if (!new_conditions_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_condition ("
                "cond_idx INTEGER, cond_id BLOB, outcome_cnt INTEGER, "
                "payout_0 BIGINT, payout_1 BIGINT, payout_2 BIGINT, payout_3 BIGINT, "
                "payout_4 BIGINT, payout_5 BIGINT, payout_6 BIGINT, payout_7 BIGINT, "
                "question_id BLOB, source INTEGER)");
    conn->Query("DELETE FROM tmp_rb_condition");
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
    conn->Query("INSERT OR REPLACE INTO rb_condition SELECT * FROM tmp_rb_condition");
  }

  if (!new_tokens_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_token ("
                "token_id BLOB, cond_idx INTEGER, is_yes INTEGER, source INTEGER)");
    conn->Query("DELETE FROM tmp_rb_token");
    {
      duckdb::Appender ap(*conn, "tmp_rb_token");
      for (auto &nt : new_tokens_) {
        int32_t db_cond_idx = (nt.cond_idx == UNKNOWN_COND_IDX) ? -1 : static_cast<int32_t>(nt.cond_idx);
        ap.BeginRow();
        append_blob(ap, nt.token_id);
        ap.Append(db_cond_idx);
        ap.Append(static_cast<int32_t>(nt.is_yes));
        ap.Append(static_cast<int32_t>(nt.source));
        ap.EndRow();
      }
      ap.Close();
    }
    conn->Query("INSERT OR IGNORE INTO rb_token SELECT * FROM tmp_rb_token");
  }

  if (!new_fpmms_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_fpmm ("
                "fpmm_addr BLOB, cond_idx INTEGER, collateral INTEGER)");
    conn->Query("DELETE FROM tmp_rb_fpmm");
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
    conn->Query("INSERT OR IGNORE INTO rb_fpmm SELECT * FROM tmp_rb_fpmm");
  }

  if (!new_collaterals_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_collateral ("
                "coll_id INTEGER, collateral_addr BLOB)");
    conn->Query("DELETE FROM tmp_rb_collateral");
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
    conn->Query("INSERT OR IGNORE INTO rb_collateral SELECT * FROM tmp_rb_collateral");
  }

  if (!new_cond_collaterals_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_cond_collateral ("
                "cond_idx INTEGER, coll_id INTEGER)");
    conn->Query("DELETE FROM tmp_rb_cond_collateral");
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
    conn->Query("INSERT OR REPLACE INTO rb_cond_collateral SELECT * FROM tmp_rb_cond_collateral");
  }

  if (!new_neg_risk_markets_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_rb_neg_risk_market ("
                "question_id BLOB, market_id BLOB)");
    conn->Query("DELETE FROM tmp_rb_neg_risk_market");
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
    conn->Query("INSERT OR IGNORE INTO rb_neg_risk_market SELECT * FROM tmp_rb_neg_risk_market");
  }

  if (!new_events_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_user_event ("
                "user_addr BLOB, sort_key BIGINT, cond_idx INTEGER, "
                "event_type INTEGER, token_idx INTEGER, collateral INTEGER, amount BIGINT, price BIGINT)");
    conn->Query("DELETE FROM tmp_user_event");

    {
      duckdb::Appender appender(*conn, "tmp_user_event");
      for (auto &[user, evt] : new_events_) {
        std::string user_blob = hex_to_blob(user);
        appender.BeginRow();
        appender.Append(duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(user_blob.data()), user_blob.size()));
        appender.Append(evt.sort_key);
        appender.Append(static_cast<int32_t>(evt.cond_idx));
        appender.Append(static_cast<int32_t>(evt.type));
        appender.Append(static_cast<int32_t>(evt.token_idx));
        appender.Append(static_cast<int32_t>(evt.collateral));
        appender.Append(evt.amount);
        appender.Append(evt.price);
        appender.EndRow();
      }
      appender.Close();
    }

    conn->Query("INSERT OR IGNORE INTO user_event "
                "SELECT * FROM tmp_user_event");
  }

  conn->Query("INSERT OR REPLACE INTO stage2_cursor VALUES ('last_block', " +
              std::to_string(new_cursor) + ")");

  auto save_cnt = [&](const char *key, int64_t val) {
    conn->Query("INSERT OR REPLACE INTO stage2_cursor VALUES ('" + std::string(key) +
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

  conn->Query("COMMIT");

  auto user_cnt = conn->Query("SELECT COUNT(DISTINCT user_addr) FROM user_event");
  progress_.total_users = user_cnt->RowCount() > 0 ? user_cnt->GetValue(0, 0).GetValue<int64_t>() : 0;
}

} // namespace stage2
