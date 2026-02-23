#include "event_build.hpp"
#include "misc/profiler.hpp"

namespace stage2 {

void EventBuilder::phase1_update_mappings(int64_t start, int64_t end) {
  TraceN("s2/phase1_map");
  auto conn = stage1_db_.create_connection();

  auto cp = conn->Query(
      "SELECT condition_id, outcome_slot_count, question_id FROM " + stage1_db_.feather_table_range("condition_preparation", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < cp->RowCount(); ++i) {
    std::string cid = blob_to_hex(cp->GetValue(0, i).GetValueUnsafe<std::string>());
    int cnt = cp->GetValue(1, i).GetValue<int>();
    std::string qid = to_lower(blob_to_hex(cp->GetValue(2, i).GetValueUnsafe<std::string>()));
    intern_condition(cid, cnt, ConditionSource::ConditionPrep, qid);
  }

  auto cr = conn->Query(
      "SELECT condition_id, payout_numerators FROM " + stage1_db_.feather_table_range("condition_resolution", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < cr->RowCount(); ++i) {
    std::string cid = blob_to_hex(cr->GetValue(0, i).GetValueUnsafe<std::string>());
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

  auto tm = conn->Query(
      "SELECT token0, token1, condition_id FROM " + stage1_db_.feather_table_range("token_map", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < tm->RowCount(); ++i) {
    std::string token0 = blob_to_hex(tm->GetValue(0, i).GetValueUnsafe<std::string>());
    std::string token1 = blob_to_hex(tm->GetValue(1, i).GetValueUnsafe<std::string>());
    std::string cid = blob_to_hex(tm->GetValue(2, i).GetValueUnsafe<std::string>());
    uint32_t cond_idx = intern_condition(cid, 2, ConditionSource::PolymarketTokenReg);
    intern_token(token0, cond_idx, 1, TokenSource::PolymarketTokenReg);
    intern_token(token1, cond_idx, 0, TokenSource::PolymarketTokenReg);
  }

  auto fpmm = conn->Query(
      "SELECT fpmm_addr, condition_ids, collateral_token FROM " + stage1_db_.feather_table_range("fpmm", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < fpmm->RowCount(); ++i) {
    std::string addr = blob_to_hex(fpmm->GetValue(0, i).GetValueUnsafe<std::string>());
    std::string cids_json = fpmm->GetValue(1, i).GetValueUnsafe<std::string>();
    std::string collateral = to_lower(blob_to_hex(fpmm->GetValue(2, i).GetValueUnsafe<std::string>()));

    auto cids_arr = nlohmann::json::parse(cids_json);
    assert(!cids_arr.empty());
    std::string cid = cids_arr[0].get<std::string>();
    std::string lower_cid = to_lower(cid);
    uint32_t cond_idx = intern_condition(cid, 2, ConditionSource::PolymarketFPMM);

    intern_fpmm(addr, cond_idx, addr_to_collateral(collateral));
    // 为所有 FPMM 计算 token_id（包括非 USDC 的，以便识别用户的 merge/burn 操作）
    intern_condition_tokens(lower_cid, collateral, cond_idx, TokenSource::PolymarketFPMM);
  }

  // 从 split 事件中提取 token_id（覆盖没有经过 FPMM 的 condition）
  auto split_for_tokens = conn->Query(
      "SELECT DISTINCT condition_id, collateral_token FROM " + stage1_db_.feather_table_range("split", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < split_for_tokens->RowCount(); ++i) {
    std::string cid = blob_to_hex(split_for_tokens->GetValue(0, i).GetValueUnsafe<std::string>());
    std::string lower_cid = to_lower(cid);
    std::string collateral = to_lower(blob_to_hex(split_for_tokens->GetValue(1, i).GetValueUnsafe<std::string>()));

    uint32_t cond_idx = intern_condition(cid, 2, ConditionSource::SplitEvent);
    cond_collateral_[cond_idx] = addr_to_collateral(collateral);
    intern_condition_tokens(lower_cid, collateral, cond_idx, TokenSource::SplitEvent);
  }

  auto nrq = conn->Query(
      "SELECT market_id, question_id FROM " + stage1_db_.feather_table_range("neg_risk_question", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < nrq->RowCount(); ++i) {
    std::string market_id = to_lower(blob_to_hex(nrq->GetValue(0, i).GetValueUnsafe<std::string>()));
    std::string question_id = to_lower(blob_to_hex(nrq->GetValue(1, i).GetValueUnsafe<std::string>()));

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

  auto split = conn->Query(
      "SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM " +
      stage1_db_.feather_table_range("split", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < split->RowCount(); ++i) {
    TxKey key;
    key.block = split->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(split->GetValue(1, i).GetValueUnsafe<std::string>()));
    SplitInfo info;
    info.amount = split->GetValue(3, i).GetValue<int64_t>();
    info.stakeholder = to_lower(blob_to_hex(split->GetValue(4, i).GetValueUnsafe<std::string>()));
    info.cond_id = to_lower(blob_to_hex(split->GetValue(2, i).GetValueUnsafe<std::string>()));
    tx_split_[key].push_back(info);
  }

  auto merge = conn->Query(
      "SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM " +
      stage1_db_.feather_table_range("merge", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < merge->RowCount(); ++i) {
    TxKey key;
    key.block = merge->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(merge->GetValue(1, i).GetValueUnsafe<std::string>()));
    MergeInfo info;
    info.amount = merge->GetValue(3, i).GetValue<int64_t>();
    info.stakeholder = to_lower(blob_to_hex(merge->GetValue(4, i).GetValueUnsafe<std::string>()));
    info.cond_id = to_lower(blob_to_hex(merge->GetValue(2, i).GetValueUnsafe<std::string>()));
    tx_merge_[key].push_back(info);
  }

  auto redemption = conn->Query(
      "SELECT block_number, tx_hash, condition_id, payout, redeemer FROM " +
      stage1_db_.feather_table_range("redemption", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < redemption->RowCount(); ++i) {
    TxKey key;
    key.block = redemption->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(redemption->GetValue(1, i).GetValueUnsafe<std::string>()));
    RedemptionInfo info;
    info.payout = redemption->GetValue(3, i).GetValue<int64_t>();
    info.redeemer = to_lower(blob_to_hex(redemption->GetValue(4, i).GetValueUnsafe<std::string>()));
    info.cond_id = to_lower(blob_to_hex(redemption->GetValue(2, i).GetValueUnsafe<std::string>()));
    tx_redemption_[key].push_back(info);
  }

  auto convert = conn->Query(
      "SELECT block_number, tx_hash, market_id, index_set, amount, stakeholder FROM " +
      stage1_db_.feather_table_range("convert", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < convert->RowCount(); ++i) {
    std::string market_id = to_lower(blob_to_hex(convert->GetValue(2, i).GetValueUnsafe<std::string>()));
    TxMarketKey key;
    key.block = convert->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(convert->GetValue(1, i).GetValueUnsafe<std::string>()));
    key.market_id = market_id;
    ConvertInfo info;
    info.market_id = market_id;
    info.index_set = convert->GetValue(3, i).GetValue<int64_t>();
    info.amount = convert->GetValue(4, i).GetValue<int64_t>();
    info.stakeholder = to_lower(blob_to_hex(convert->GetValue(5, i).GetValueUnsafe<std::string>()));
    tx_convert_[key].push_back(info);
  }

  auto order = conn->Query(
      "SELECT block_number, tx_hash, maker, taker, maker_asset_id, taker_asset_id, "
      "maker_amount, taker_amount, fee FROM " +
      stage1_db_.feather_table_range("order_filled", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < order->RowCount(); ++i) {
    int64_t block = order->GetValue(0, i).GetValue<int64_t>();
    auto tx_hash = hex_to_bytes32(blob_to_hex(order->GetValue(1, i).GetValueUnsafe<std::string>()));
    std::string maker = to_lower(blob_to_hex(order->GetValue(2, i).GetValueUnsafe<std::string>()));
    std::string taker = to_lower(blob_to_hex(order->GetValue(3, i).GetValueUnsafe<std::string>()));
    std::string maker_asset = blob_to_hex(order->GetValue(4, i).GetValueUnsafe<std::string>());
    std::string taker_asset = blob_to_hex(order->GetValue(5, i).GetValueUnsafe<std::string>());
    int64_t maker_amt = order->GetValue(6, i).GetValue<int64_t>();
    int64_t taker_amt = order->GetValue(7, i).GetValue<int64_t>();
    int64_t fee = order->GetValue(8, i).GetValue<int64_t>();

    bool maker_is_usdc = maker_asset == "0x0000000000000000000000000000000000000000000000000000000000000000";
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

  auto fpmm_trade = conn->Query(
      "SELECT block_number, tx_hash, fpmm_addr, trader, side, outcome_index, "
      "usdc_amount, token_amount FROM " +
      stage1_db_.feather_table_range("fpmm_trade", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < fpmm_trade->RowCount(); ++i) {
    std::string fpmm_addr = to_lower(blob_to_hex(fpmm_trade->GetValue(2, i).GetValueUnsafe<std::string>()));
    TxFPMMKey key;
    key.block = fpmm_trade->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(fpmm_trade->GetValue(1, i).GetValueUnsafe<std::string>()));
    key.fpmm_addr = fpmm_addr;
    assert(tx_fpmm_trade_.count(key) == 0 && "Duplicate FPMM trade");
    FPMMTradeInfo info;
    info.fpmm_addr = fpmm_addr;
    info.trader = to_lower(blob_to_hex(fpmm_trade->GetValue(3, i).GetValueUnsafe<std::string>()));
    info.side = fpmm_trade->GetValue(4, i).GetValue<int>();
    info.outcome_idx = fpmm_trade->GetValue(5, i).GetValue<int>();
    info.usdc = fpmm_trade->GetValue(6, i).GetValue<int64_t>();
    info.tokens = fpmm_trade->GetValue(7, i).GetValue<int64_t>();
    tx_fpmm_trade_[key] = info;
  }

  auto fpmm_funding = conn->Query(
      "SELECT block_number, tx_hash, fpmm_addr, funder, side, amounts FROM " +
      stage1_db_.feather_table_range("fpmm_funding", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < fpmm_funding->RowCount(); ++i) {
    std::string fpmm_addr = to_lower(blob_to_hex(fpmm_funding->GetValue(2, i).GetValueUnsafe<std::string>()));
    TxFPMMKey key;
    key.block = fpmm_funding->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(fpmm_funding->GetValue(1, i).GetValueUnsafe<std::string>()));
    key.fpmm_addr = fpmm_addr;
    assert(tx_fpmm_funding_.count(key) == 0 && "Duplicate FPMM funding");
    FPMMFundingInfo info;
    info.fpmm_addr = fpmm_addr;
    info.funder = to_lower(blob_to_hex(fpmm_funding->GetValue(3, i).GetValueUnsafe<std::string>()));
    info.side = fpmm_funding->GetValue(4, i).GetValue<int>();
    std::string amounts_json = fpmm_funding->GetValue(5, i).GetValueUnsafe<std::string>();
    auto amounts_arr = nlohmann::json::parse(amounts_json);
    info.amount0 = amounts_arr.size() > 0 ? amounts_arr[0].get<int64_t>() : 0;
    info.amount1 = amounts_arr.size() > 1 ? amounts_arr[1].get<int64_t>() : 0;
    tx_fpmm_funding_[key] = info;
  }
}

void EventBuilder::phase3_process_transfers(int64_t start, int64_t end) {
  TraceN("s2/phase3_xfer");
  auto conn = stage1_db_.create_connection();
  auto transfers = conn->Query(
      "SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount FROM " +
      stage1_db_.feather_table_range("transfer", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));

  struct TransferRow {
    int64_t block, log_idx, amount;
    std::string tx_hash, op, from, to, token_id;
  };
  std::vector<TransferRow> rows;
  rows.reserve(transfers->RowCount());
  for (idx_t i = 0; i < transfers->RowCount(); ++i) {
    rows.push_back({
        transfers->GetValue(0, i).GetValue<int64_t>(),
        transfers->GetValue(2, i).GetValue<int64_t>(),
        transfers->GetValue(7, i).GetValue<int64_t>(),
        blob_to_hex(transfers->GetValue(1, i).GetValueUnsafe<std::string>()),
        blob_to_hex(transfers->GetValue(3, i).GetValueUnsafe<std::string>()),
        blob_to_hex(transfers->GetValue(4, i).GetValueUnsafe<std::string>()),
        blob_to_hex(transfers->GetValue(5, i).GetValueUnsafe<std::string>()),
        blob_to_hex(transfers->GetValue(6, i).GetValueUnsafe<std::string>()),
    });
  }
  std::sort(rows.begin(), rows.end(), [](const TransferRow &a, const TransferRow &b) {
    return a.block != b.block ? a.block < b.block : a.log_idx < b.log_idx;
  });

  for (const auto &r : rows) {
    std::string op = to_lower(r.op);
    std::string from = to_lower(r.from);
    std::string to = to_lower(r.to);
    std::string token_id = to_lower(r.token_id);

    int64_t sort_key = r.block * 1000000000LL + r.log_idx;
    auto tx_hash = hex_to_bytes32(r.tx_hash);

    auto tit = token_map_.find(token_id);

    if (tit == token_map_.end()) {
      // 未知token：加入 token_map_，使用特殊值表示未知 condition
      intern_token(token_id, UNKNOWN_COND_IDX, UNKNOWN_IS_YES, TokenSource::TransferInferred);
      tit = token_map_.find(token_id); // 重新获取迭代器
    }

    uint32_t cond_idx = tit->second.cond_idx;
    uint8_t token_idx = tit->second.is_yes ? 0 : 1;

    // 获取抵押品类型
    Collateral collateral = Collateral::Unknown;
    auto coll_it = cond_collateral_.find(cond_idx);
    if (coll_it != cond_collateral_.end()) {
      collateral = coll_it->second;
    }

    TransferClass cls = classify_and_emit(sort_key, tx_hash, r.block, op, from, to, token_id, r.amount, cond_idx, token_idx, collateral);
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
