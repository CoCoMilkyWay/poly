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
    intern_condition(cid, cnt, qid);
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
    std::string lower_cid = to_lower(cid);
    auto it = cond_map_.find(lower_cid);
    uint32_t cond_idx;
    if (it == cond_map_.end()) {
      cond_idx = intern_condition(cid, 2);
    } else {
      cond_idx = it->second;
    }
    intern_token(token0, cond_idx, 1);
    intern_token(token1, cond_idx, 0);
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
    auto it = cond_map_.find(lower_cid);
    uint32_t cond_idx;
    if (it == cond_map_.end()) {
      cond_idx = intern_condition(cid, 2);
    } else {
      cond_idx = it->second;
    }

    bool is_usdc = is_usdc_collateral(collateral);
    intern_fpmm(addr, cond_idx, is_usdc, collateral);

    // 为所有 FPMM 计算 token_id（包括非 USDC 的，以便识别用户的 merge/burn 操作）
    auto cond_bytes = hex_to_blob(lower_cid);
    auto collateral_bytes = hex_to_blob(collateral);
    for (int index_set = 1; index_set <= 2; ++index_set) {
      std::string collection_input(96, '\0');
      std::memcpy(collection_input.data() + 32, cond_bytes.data(), std::min(size_t(32), cond_bytes.size()));
      collection_input[95] = static_cast<char>(index_set);
      auto collection_hash = crypto::keccak256(collection_input);

      std::string position_input(52, '\0');
      std::memcpy(position_input.data(), collateral_bytes.data(), std::min(size_t(20), collateral_bytes.size()));
      std::memcpy(position_input.data() + 20, collection_hash.data(), 32);
      auto position_hash = crypto::keccak256(position_input);

      std::string token_id = crypto::Keccak256::to_hex(position_hash);
      intern_token(token_id, cond_idx, index_set == 1 ? 1 : 0);
    }
  }

  // 从 split 事件中提取 token_id（覆盖没有经过 FPMM 的 condition）
  auto split_for_tokens = conn->Query(
      "SELECT DISTINCT condition_id, collateral_token FROM " + stage1_db_.feather_table_range("split", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < split_for_tokens->RowCount(); ++i) {
    std::string cid = blob_to_hex(split_for_tokens->GetValue(0, i).GetValueUnsafe<std::string>());
    std::string lower_cid = to_lower(cid);
    std::string collateral = to_lower(blob_to_hex(split_for_tokens->GetValue(1, i).GetValueUnsafe<std::string>()));

    auto it = cond_map_.find(lower_cid);
    uint32_t cond_idx;
    if (it == cond_map_.end()) {
      cond_idx = intern_condition(cid, 2);
    } else {
      cond_idx = it->second;
    }

    // 如果是非 USDC collateral，记录到 non_usdc_cond_idxs_
    bool is_usdc = is_usdc_collateral(collateral);
    if (!is_usdc) {
      non_usdc_cond_idxs_.insert(cond_idx);
      non_usdc_collaterals_[cond_idx] = collateral;
    }

    // 计算 token_id（如果还没计算过）
    auto cond_bytes = hex_to_blob(lower_cid);
    auto collateral_bytes = hex_to_blob(collateral);
    for (int index_set = 1; index_set <= 2; ++index_set) {
      std::string collection_input(96, '\0');
      std::memcpy(collection_input.data() + 32, cond_bytes.data(), std::min(size_t(32), cond_bytes.size()));
      collection_input[95] = static_cast<char>(index_set);
      auto collection_hash = crypto::keccak256(collection_input);

      std::string position_input(52, '\0');
      std::memcpy(position_input.data(), collateral_bytes.data(), std::min(size_t(20), collateral_bytes.size()));
      std::memcpy(position_input.data() + 20, collection_hash.data(), 32);
      auto position_hash = crypto::keccak256(position_input);

      std::string token_id = crypto::Keccak256::to_hex(position_hash);
      intern_token(token_id, cond_idx, index_set == 1 ? 1 : 0);
    }
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
    TxCondKey key;
    key.block = split->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(split->GetValue(1, i).GetValueUnsafe<std::string>()));
    key.cond_id = to_lower(blob_to_hex(split->GetValue(2, i).GetValueUnsafe<std::string>()));
    SplitInfo info;
    info.amount = split->GetValue(3, i).GetValue<int64_t>();
    info.stakeholder = to_lower(blob_to_hex(split->GetValue(4, i).GetValueUnsafe<std::string>()));
    info.cond_id = key.cond_id;
    tx_split_[key].push_back(info);
  }

  auto merge = conn->Query(
      "SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM " +
      stage1_db_.feather_table_range("merge", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < merge->RowCount(); ++i) {
    TxCondKey key;
    key.block = merge->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(merge->GetValue(1, i).GetValueUnsafe<std::string>()));
    key.cond_id = to_lower(blob_to_hex(merge->GetValue(2, i).GetValueUnsafe<std::string>()));
    MergeInfo info;
    info.amount = merge->GetValue(3, i).GetValue<int64_t>();
    info.stakeholder = to_lower(blob_to_hex(merge->GetValue(4, i).GetValueUnsafe<std::string>()));
    info.cond_id = key.cond_id;
    tx_merge_[key].push_back(info);
  }

  auto redemption = conn->Query(
      "SELECT block_number, tx_hash, condition_id, payout, redeemer FROM " +
      stage1_db_.feather_table_range("redemption", start, end) +
      " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
  for (idx_t i = 0; i < redemption->RowCount(); ++i) {
    TxCondKey key;
    key.block = redemption->GetValue(0, i).GetValue<int64_t>();
    key.tx_hash = hex_to_bytes32(blob_to_hex(redemption->GetValue(1, i).GetValueUnsafe<std::string>()));
    key.cond_id = to_lower(blob_to_hex(redemption->GetValue(2, i).GetValueUnsafe<std::string>()));
    RedemptionInfo info;
    info.payout = redemption->GetValue(3, i).GetValue<int64_t>();
    info.redeemer = to_lower(blob_to_hex(redemption->GetValue(4, i).GetValueUnsafe<std::string>()));
    info.cond_id = key.cond_id;
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

  for (const auto &kv : tx_split_)
    progress_.cnt_split += kv.second.size();
  for (const auto &kv : tx_merge_)
    progress_.cnt_merge += kv.second.size();
  for (const auto &kv : tx_redemption_)
    progress_.cnt_redemption += kv.second.size();
  for (const auto &kv : tx_convert_)
    progress_.cnt_convert += kv.second.size();
  progress_.cnt_order += tx_order_.size();
  progress_.cnt_fpmm_trade += tx_fpmm_trade_.size();
  progress_.cnt_fpmm_funding += tx_fpmm_funding_.size();
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

    // 优先检查是否涉及 non-USDC FPMM（在 token lookup 之前）
    auto check_non_usdc_fpmm = [&](const std::string &addr) -> bool {
      auto it = fpmm_map_.find(addr);
      if (it != fpmm_map_.end() && !it->second.is_usdc) {
        non_usdc_by_collat_[it->second.collateral]++;
        chunk_xfer_stats_.add(TransferClass::NonUsdcFpmm);
        return true;
      }
      return false;
    };
    if (check_non_usdc_fpmm(op) || check_non_usdc_fpmm(from) || check_non_usdc_fpmm(to))
      continue;

    auto tit = token_map_.find(token_id);
    if (tit == token_map_.end()) {
      chunk_xfer_stats_.add(TransferClass::NonPolymarket);
      continue;
    }
    uint32_t cond_idx = tit->second.cond_idx;
    uint8_t token_idx = tit->second.is_yes ? 0 : 1;

    // 检查 token 是否属于非 USDC condition（用户直接操作如 merge/burn）
    if (non_usdc_cond_idxs_.count(cond_idx)) {
      // 从 non_usdc_collaterals_ 或 fpmm_map_ 找到 collateral 地址
      auto collat_it = non_usdc_collaterals_.find(cond_idx);
      if (collat_it != non_usdc_collaterals_.end()) {
        non_usdc_by_collat_[collat_it->second]++;
      } else {
        for (const auto &[addr, info] : fpmm_map_) {
          if (info.cond_idx == cond_idx && !info.is_usdc) {
            non_usdc_by_collat_[info.collateral]++;
            break;
          }
        }
      }
      chunk_xfer_stats_.add(TransferClass::NonUsdcFpmm);
      continue;
    }

    TransferClass cls = classify_and_emit(sort_key, tx_hash, r.block, op, from, to, token_id, r.amount, cond_idx, token_idx);
    chunk_xfer_stats_.add(cls);
  }
}

void EventBuilder::commit_chunk(int64_t new_cursor) {
  TraceN("s2/commit");
  auto conn = stage2_db_.create_connection();
  conn->Query("BEGIN TRANSACTION");

  for (auto &nc : new_conditions_) {
    std::string blob = hex_to_blob(nc.cond_id);
    std::string pvals;
    for (int i = 0; i < 8; ++i) {
      if (i > 0)
        pvals += ", ";
      if (i < static_cast<int>(nc.info.payout_numerators.size()) && nc.info.payout_numerators[i] >= 0) {
        pvals += std::to_string(nc.info.payout_numerators[i]);
      } else {
        pvals += "NULL";
      }
    }
    std::string qid_val = nc.info.question_id.empty() ? "NULL" : blob_to_hex_literal(hex_to_blob(nc.info.question_id));
    conn->Query("INSERT OR REPLACE INTO rb_condition VALUES (" +
                std::to_string(nc.idx) + ", " +
                blob_to_hex_literal(blob) + ", " +
                std::to_string(nc.info.outcome_count) + ", " + pvals + ", " + qid_val + ")");
  }

  for (auto &nt : new_tokens_) {
    std::string blob = hex_to_blob(nt.token_id);
    conn->Query("INSERT OR IGNORE INTO rb_token VALUES (" +
                blob_to_hex_literal(blob) + ", " +
                std::to_string(nt.cond_idx) + ", " +
                std::to_string(nt.is_yes) + ")");
  }

  for (auto &nf : new_fpmms_) {
    std::string blob = hex_to_blob(nf.addr);
    std::string collateral_val = nf.collateral.empty() ? "NULL" : blob_to_hex_literal(hex_to_blob(nf.collateral));
    conn->Query("INSERT OR IGNORE INTO rb_fpmm VALUES (" +
                blob_to_hex_literal(blob) + ", " +
                std::to_string(nf.cond_idx) + ", " +
                std::to_string(nf.is_usdc ? 1 : 0) + ", " +
                collateral_val + ")");
  }

  for (auto &nm : new_neg_risk_markets_) {
    std::string qid_blob = hex_to_blob(nm.question_id);
    std::string mid_blob = hex_to_blob(nm.market_id);
    conn->Query("INSERT OR IGNORE INTO rb_neg_risk_market VALUES (" +
                blob_to_hex_literal(qid_blob) + ", " +
                blob_to_hex_literal(mid_blob) + ")");
  }

  if (!new_events_.empty()) {
    conn->Query("CREATE TEMP TABLE IF NOT EXISTS tmp_user_event ("
                "user_addr BLOB, sort_key BIGINT, cond_idx INTEGER, "
                "event_type INTEGER, token_idx INTEGER, amount BIGINT, price BIGINT)");
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
