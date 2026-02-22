#pragma once

#include "../core/database.hpp"
#include "types.hpp"
#include <algorithm>
#include <cassert>
#include <nlohmann/json.hpp>
#include <unordered_map>

namespace stage2 {

static constexpr const char *ZERO_ADDR = "0x0000000000000000000000000000000000000000";
static constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
static constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
static constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";

struct ScanStats {
  int64_t rows = 0;
  int64_t events = 0;
};

struct BuildProgress {
  int64_t cursor = 0;
  int64_t target = 0;
  int64_t chunk_start = 0;
  int64_t chunk_end = 0;
  int phase = 0;
  bool running = false;
  int64_t total_conditions = 0;
  int64_t total_tokens = 0;
  int64_t total_events = 0;
  int64_t cnt_split = 0;
  int64_t cnt_merge = 0;
  int64_t cnt_redemption = 0;
  int64_t cnt_convert = 0;
  int64_t cnt_order = 0;
  int64_t cnt_fpmm_trade = 0;
  int64_t cnt_fpmm_funding = 0;
  int64_t cnt_transfer = 0;
};

class EventBuilder {
public:
  EventBuilder(Database &stage1_db, Database &stage2_db, int chunk_size)
      : stage1_db_(stage1_db), stage2_db_(stage2_db), chunk_size_(chunk_size) {}

  void init_schema() {
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
        payout_7    BIGINT
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_token (
        token_id  BLOB PRIMARY KEY,
        cond_idx  INTEGER NOT NULL,
        is_yes    INTEGER NOT NULL
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS rb_fpmm (
        fpmm_addr BLOB PRIMARY KEY,
        cond_idx  INTEGER NOT NULL
      )
    )");

    stage2_db_.execute(R"(
      CREATE TABLE IF NOT EXISTS user_event (
        user_addr   BLOB NOT NULL,
        sort_key    BIGINT NOT NULL,
        cond_idx    INTEGER NOT NULL,
        event_type  INTEGER NOT NULL,
        token_idx   INTEGER NOT NULL,
        amount      BIGINT NOT NULL,
        price       BIGINT NOT NULL,
        PRIMARY KEY (user_addr, sort_key, cond_idx, event_type, token_idx)
      )
    )");

    auto conn = stage2_db_.create_connection();
    auto r = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
    if (r->RowCount() == 0) {
      conn->Query("INSERT INTO stage2_cursor VALUES ('last_block', 0)");
    }
  }

  void load_from_rb() {
    progress_.phase = 0;
    auto conn = stage2_db_.create_connection();

    auto cur = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
    progress_.cursor = cur->RowCount() > 0 ? cur->GetValue(0, 0).GetValue<int64_t>() : 0;

    auto cond_r = conn->Query("SELECT cond_idx, cond_id, outcome_cnt, "
                              "payout_0, payout_1, payout_2, payout_3, "
                              "payout_4, payout_5, payout_6, payout_7 FROM rb_condition ORDER BY cond_idx");
    for (idx_t i = 0; i < cond_r->RowCount(); ++i) {
      uint32_t idx = cond_r->GetValue(0, i).GetValue<uint32_t>();
      std::string cond_id = blob_to_hex(cond_r->GetValue(1, i).GetValueUnsafe<std::string>());
      ConditionInfo info;
      info.outcome_count = cond_r->GetValue(2, i).GetValue<uint8_t>();
      for (int j = 0; j < info.outcome_count; ++j) {
        auto v = cond_r->GetValue(3 + j, i);
        info.payout_numerators.push_back(v.IsNull() ? -1 : v.GetValue<int64_t>());
      }
      while (conditions_.size() <= idx) {
        conditions_.emplace_back();
        cond_ids_.emplace_back();
      }
      conditions_[idx] = info;
      cond_ids_[idx] = cond_id;
      cond_map_[to_lower(cond_id)] = idx;
    }

    auto token_r = conn->Query("SELECT token_id, cond_idx, is_yes FROM rb_token");
    for (idx_t i = 0; i < token_r->RowCount(); ++i) {
      std::string tid = blob_to_hex(token_r->GetValue(0, i).GetValueUnsafe<std::string>());
      TokenInfo info;
      info.cond_idx = token_r->GetValue(1, i).GetValue<uint32_t>();
      info.is_yes = token_r->GetValue(2, i).GetValue<uint8_t>();
      token_map_[to_lower(tid)] = info;
    }

    auto fpmm_r = conn->Query("SELECT fpmm_addr, cond_idx FROM rb_fpmm");
    for (idx_t i = 0; i < fpmm_r->RowCount(); ++i) {
      std::string addr = blob_to_hex(fpmm_r->GetValue(0, i).GetValueUnsafe<std::string>());
      FPMMInfo info;
      info.cond_idx = fpmm_r->GetValue(1, i).GetValue<uint32_t>();
      fpmm_map_[to_lower(addr)] = info;
    }

    progress_.total_conditions = conditions_.size();
    progress_.total_tokens = token_map_.size();
  }

  int64_t cursor() const { return progress_.cursor; }

  bool build_chunk(int64_t target_block) {
    if (progress_.cursor >= target_block)
      return false;

    int64_t chunk_start = progress_.cursor;
    int64_t chunk_end = std::min(progress_.cursor + chunk_size_, target_block);
    progress_.target = target_block;
    progress_.chunk_start = chunk_start;
    progress_.chunk_end = chunk_end;
    progress_.running = true;

    new_conditions_.clear();
    new_tokens_.clear();
    new_fpmms_.clear();
    new_events_.clear();

    tx_split_.clear();
    tx_merge_.clear();
    tx_redemption_.clear();
    tx_convert_.clear();
    tx_order_.clear();
    tx_fpmm_trade_.clear();
    tx_fpmm_funding_.clear();

    progress_.phase = 1;
    phase1_update_mappings(chunk_start, chunk_end);

    progress_.phase = 2;
    phase2_build_semantic_index(chunk_start, chunk_end);

    progress_.phase = 3;
    phase3_process_transfers(chunk_start, chunk_end);

    commit_chunk(chunk_end);

    progress_.cursor = chunk_end;
    progress_.running = false;
    return true;
  }

  const BuildProgress &progress() const { return progress_; }

private:
  Database &stage1_db_;
  Database &stage2_db_;
  int chunk_size_;
  BuildProgress progress_;

  std::vector<ConditionInfo> conditions_;
  std::vector<std::string> cond_ids_;
  std::unordered_map<std::string, uint32_t> cond_map_;
  std::unordered_map<std::string, TokenInfo> token_map_;
  std::unordered_map<std::string, FPMMInfo> fpmm_map_;

  std::unordered_map<TxCondKey, SplitInfo> tx_split_;
  std::unordered_map<TxCondKey, MergeInfo> tx_merge_;
  std::unordered_map<TxCondKey, RedemptionInfo> tx_redemption_;
  std::unordered_map<TxKey, ConvertInfo> tx_convert_;
  std::unordered_map<TxTokenKey, OrderInfo> tx_order_;
  std::unordered_map<TxKey, FPMMTradeInfo> tx_fpmm_trade_;
  std::unordered_map<TxKey, FPMMFundingInfo> tx_fpmm_funding_;

  struct NewCondition {
    uint32_t idx;
    std::string cond_id;
    ConditionInfo info;
  };
  struct NewToken {
    std::string token_id;
    uint32_t cond_idx;
    uint8_t is_yes;
  };
  struct NewFPMM {
    std::string addr;
    uint32_t cond_idx;
  };

  std::vector<NewCondition> new_conditions_;
  std::vector<NewToken> new_tokens_;
  std::vector<NewFPMM> new_fpmms_;
  std::vector<std::tuple<std::string, RawEvent>> new_events_;

  static std::string blob_to_hex(const std::string &blob) {
    if (blob.starts_with("0x"))
      return blob;
    static const char hex_chars[] = "0123456789abcdef";
    std::string result = "0x";
    result.reserve(2 + blob.size() * 2);
    for (unsigned char c : blob) {
      result.push_back(hex_chars[c >> 4]);
      result.push_back(hex_chars[c & 0x0f]);
    }
    return result;
  }

  static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
  }

  static std::array<uint8_t, 32> hex_to_bytes32(const std::string &hex) {
    std::array<uint8_t, 32> result{};
    std::string h = hex;
    if (h.starts_with("0x"))
      h = h.substr(2);
    for (size_t i = 0; i < 32 && i * 2 < h.size(); ++i) {
      result[i] = static_cast<uint8_t>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
    }
    return result;
  }

  static std::string hex_to_blob(const std::string &hex) {
    std::string h = hex;
    if (h.starts_with("0x"))
      h = h.substr(2);
    std::string result;
    result.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
      result.push_back(static_cast<char>(std::stoul(h.substr(i, 2), nullptr, 16)));
    }
    return result;
  }

  uint32_t intern_condition(const std::string &cond_id, uint8_t outcome_cnt) {
    std::string lower = to_lower(cond_id);
    auto it = cond_map_.find(lower);
    if (it != cond_map_.end())
      return it->second;

    uint32_t idx = static_cast<uint32_t>(conditions_.size());
    ConditionInfo info;
    info.outcome_count = outcome_cnt;
    conditions_.push_back(info);
    cond_ids_.push_back(lower);
    cond_map_[lower] = idx;

    new_conditions_.push_back({idx, lower, info});
    progress_.total_conditions = conditions_.size();
    return idx;
  }

  void update_condition_payout(uint32_t idx, const std::vector<int64_t> &payouts) {
    if (idx < conditions_.size()) {
      conditions_[idx].payout_numerators = payouts;
      for (auto &nc : new_conditions_) {
        if (nc.idx == idx) {
          nc.info.payout_numerators = payouts;
          return;
        }
      }
      new_conditions_.push_back({idx, cond_ids_[idx], conditions_[idx]});
    }
  }

  void intern_token(const std::string &token_id, uint32_t cond_idx, uint8_t is_yes) {
    std::string lower = to_lower(token_id);
    if (token_map_.count(lower))
      return;
    token_map_[lower] = {cond_idx, is_yes};
    new_tokens_.push_back({lower, cond_idx, is_yes});
    progress_.total_tokens = token_map_.size();
  }

  void intern_fpmm(const std::string &addr, uint32_t cond_idx) {
    std::string lower = to_lower(addr);
    if (fpmm_map_.count(lower))
      return;
    fpmm_map_[lower] = {cond_idx};
    new_fpmms_.push_back({lower, cond_idx});
  }

  void push_event(const std::string &user_addr, const RawEvent &evt) {
    new_events_.emplace_back(to_lower(user_addr), evt);
    progress_.total_events++;
  }

  void phase1_update_mappings(int64_t start, int64_t end) {
    auto conn = stage1_db_.create_connection();

    auto cp = conn->Query(
        "SELECT condition_id, outcome_slot_count FROM condition_preparation "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < cp->RowCount(); ++i) {
      std::string cid = blob_to_hex(cp->GetValue(0, i).GetValueUnsafe<std::string>());
      int cnt = cp->GetValue(1, i).GetValue<int>();
      intern_condition(cid, cnt);
    }

    auto cr = conn->Query(
        "SELECT condition_id, payout_numerators FROM condition_resolution "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
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
        "SELECT token0, condition_id FROM token_map "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < tm->RowCount(); ++i) {
      std::string tid = blob_to_hex(tm->GetValue(0, i).GetValueUnsafe<std::string>());
      std::string cid = blob_to_hex(tm->GetValue(1, i).GetValueUnsafe<std::string>());
      std::string lower_cid = to_lower(cid);
      auto it = cond_map_.find(lower_cid);
      if (it == cond_map_.end()) {
        uint32_t idx = intern_condition(cid, 2);
        intern_token(tid, idx, 1);
      } else {
        intern_token(tid, it->second, 1);
      }
    }

    auto fpmm = conn->Query(
        "SELECT fpmm_addr, condition_ids FROM fpmm "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < fpmm->RowCount(); ++i) {
      std::string addr = blob_to_hex(fpmm->GetValue(0, i).GetValueUnsafe<std::string>());
      std::string cids_json = fpmm->GetValue(1, i).GetValueUnsafe<std::string>();
      auto cids_arr = nlohmann::json::parse(cids_json);
      assert(!cids_arr.empty());
      std::string cid = cids_arr[0].get<std::string>();
      std::string lower_cid = to_lower(cid);
      auto it = cond_map_.find(lower_cid);
      if (it == cond_map_.end())
        continue;
      intern_fpmm(addr, it->second);
    }
  }

  void phase2_build_semantic_index(int64_t start, int64_t end) {
    auto conn = stage1_db_.create_connection();

    auto split = conn->Query(
        "SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM split "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < split->RowCount(); ++i) {
      TxCondKey key;
      key.block = split->GetValue(0, i).GetValue<int64_t>();
      key.tx_hash = hex_to_bytes32(blob_to_hex(split->GetValue(1, i).GetValueUnsafe<std::string>()));
      key.cond_id = to_lower(blob_to_hex(split->GetValue(2, i).GetValueUnsafe<std::string>()));
      SplitInfo info;
      info.amount = split->GetValue(3, i).GetValue<int64_t>();
      info.stakeholder = to_lower(blob_to_hex(split->GetValue(4, i).GetValueUnsafe<std::string>()));
      info.cond_id = key.cond_id;
      tx_split_[key] = info;
    }

    auto merge = conn->Query(
        "SELECT block_number, tx_hash, condition_id, amount, stakeholder FROM merge "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < merge->RowCount(); ++i) {
      TxCondKey key;
      key.block = merge->GetValue(0, i).GetValue<int64_t>();
      key.tx_hash = hex_to_bytes32(blob_to_hex(merge->GetValue(1, i).GetValueUnsafe<std::string>()));
      key.cond_id = to_lower(blob_to_hex(merge->GetValue(2, i).GetValueUnsafe<std::string>()));
      MergeInfo info;
      info.amount = merge->GetValue(3, i).GetValue<int64_t>();
      info.stakeholder = to_lower(blob_to_hex(merge->GetValue(4, i).GetValueUnsafe<std::string>()));
      info.cond_id = key.cond_id;
      tx_merge_[key] = info;
    }

    auto redemption = conn->Query(
        "SELECT block_number, tx_hash, condition_id, payout, redeemer FROM redemption "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < redemption->RowCount(); ++i) {
      TxCondKey key;
      key.block = redemption->GetValue(0, i).GetValue<int64_t>();
      key.tx_hash = hex_to_bytes32(blob_to_hex(redemption->GetValue(1, i).GetValueUnsafe<std::string>()));
      key.cond_id = to_lower(blob_to_hex(redemption->GetValue(2, i).GetValueUnsafe<std::string>()));
      RedemptionInfo info;
      info.payout = redemption->GetValue(3, i).GetValue<int64_t>();
      info.redeemer = to_lower(blob_to_hex(redemption->GetValue(4, i).GetValueUnsafe<std::string>()));
      info.cond_id = key.cond_id;
      tx_redemption_[key] = info;
    }

    auto convert = conn->Query(
        "SELECT block_number, tx_hash, market_id, index_set, amount, stakeholder FROM convert "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < convert->RowCount(); ++i) {
      TxKey key;
      key.block = convert->GetValue(0, i).GetValue<int64_t>();
      key.tx_hash = hex_to_bytes32(blob_to_hex(convert->GetValue(1, i).GetValueUnsafe<std::string>()));
      ConvertInfo info;
      info.market_id = to_lower(blob_to_hex(convert->GetValue(2, i).GetValueUnsafe<std::string>()));
      info.index_set = convert->GetValue(3, i).GetValue<int64_t>();
      info.amount = convert->GetValue(4, i).GetValue<int64_t>();
      info.stakeholder = to_lower(blob_to_hex(convert->GetValue(5, i).GetValueUnsafe<std::string>()));
      tx_convert_[key] = info;
    }

    auto order = conn->Query(
        "SELECT block_number, tx_hash, maker, taker, maker_asset_id, taker_asset_id, "
        "maker_amount, taker_amount, fee FROM order_filled "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
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
        "usdc_amount, token_amount FROM fpmm_trade "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < fpmm_trade->RowCount(); ++i) {
      TxKey key;
      key.block = fpmm_trade->GetValue(0, i).GetValue<int64_t>();
      key.tx_hash = hex_to_bytes32(blob_to_hex(fpmm_trade->GetValue(1, i).GetValueUnsafe<std::string>()));
      FPMMTradeInfo info;
      info.fpmm_addr = to_lower(blob_to_hex(fpmm_trade->GetValue(2, i).GetValueUnsafe<std::string>()));
      info.trader = to_lower(blob_to_hex(fpmm_trade->GetValue(3, i).GetValueUnsafe<std::string>()));
      info.side = fpmm_trade->GetValue(4, i).GetValue<int>();
      info.outcome_idx = fpmm_trade->GetValue(5, i).GetValue<int>();
      info.usdc = fpmm_trade->GetValue(6, i).GetValue<int64_t>();
      info.tokens = fpmm_trade->GetValue(7, i).GetValue<int64_t>();
      tx_fpmm_trade_[key] = info;
    }

    auto fpmm_funding = conn->Query(
        "SELECT block_number, tx_hash, fpmm_addr, funder, side, amounts FROM fpmm_funding "
        "WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < fpmm_funding->RowCount(); ++i) {
      TxKey key;
      key.block = fpmm_funding->GetValue(0, i).GetValue<int64_t>();
      key.tx_hash = hex_to_bytes32(blob_to_hex(fpmm_funding->GetValue(1, i).GetValueUnsafe<std::string>()));
      FPMMFundingInfo info;
      info.fpmm_addr = to_lower(blob_to_hex(fpmm_funding->GetValue(2, i).GetValueUnsafe<std::string>()));
      info.funder = to_lower(blob_to_hex(fpmm_funding->GetValue(3, i).GetValueUnsafe<std::string>()));
      info.side = fpmm_funding->GetValue(4, i).GetValue<int>();
      std::string amounts_json = fpmm_funding->GetValue(5, i).GetValueUnsafe<std::string>();
      auto amounts_arr = nlohmann::json::parse(amounts_json);
      info.amount0 = amounts_arr.size() > 0 ? amounts_arr[0].get<int64_t>() : 0;
      info.amount1 = amounts_arr.size() > 1 ? amounts_arr[1].get<int64_t>() : 0;
      tx_fpmm_funding_[key] = info;
    }

    progress_.cnt_split += tx_split_.size();
    progress_.cnt_merge += tx_merge_.size();
    progress_.cnt_redemption += tx_redemption_.size();
    progress_.cnt_convert += tx_convert_.size();
    progress_.cnt_order += tx_order_.size();
    progress_.cnt_fpmm_trade += tx_fpmm_trade_.size();
    progress_.cnt_fpmm_funding += tx_fpmm_funding_.size();
  }

  void phase3_process_transfers(int64_t start, int64_t end) {
    auto conn = stage1_db_.create_connection();
    auto transfers = conn->Query(
        "SELECT block_number, tx_hash, log_index, operator, from_addr, to_addr, token_id, amount "
        "FROM transfer WHERE block_number > " +
        std::to_string(start) +
        " AND block_number <= " + std::to_string(end) +
        " ORDER BY block_number, log_index");

    for (idx_t i = 0; i < transfers->RowCount(); ++i) {
      int64_t block = transfers->GetValue(0, i).GetValue<int64_t>();
      std::string tx_hash_hex = blob_to_hex(transfers->GetValue(1, i).GetValueUnsafe<std::string>());
      int64_t log_idx = transfers->GetValue(2, i).GetValue<int64_t>();
      std::string op = to_lower(blob_to_hex(transfers->GetValue(3, i).GetValueUnsafe<std::string>()));
      std::string from = to_lower(blob_to_hex(transfers->GetValue(4, i).GetValueUnsafe<std::string>()));
      std::string to = to_lower(blob_to_hex(transfers->GetValue(5, i).GetValueUnsafe<std::string>()));
      std::string token_id = to_lower(blob_to_hex(transfers->GetValue(6, i).GetValueUnsafe<std::string>()));
      int64_t amount = transfers->GetValue(7, i).GetValue<int64_t>();

      int64_t sort_key = block * 1000000000LL + log_idx;
      auto tx_hash = hex_to_bytes32(tx_hash_hex);

      auto tit = token_map_.find(token_id);
      if (tit == token_map_.end())
        continue;
      uint32_t cond_idx = tit->second.cond_idx;
      uint8_t token_idx = tit->second.is_yes ? 0 : 1;

      classify_and_emit(sort_key, tx_hash, block, op, from, to, token_id, amount, cond_idx, token_idx);
    }
    progress_.cnt_transfer += transfers->RowCount();
  }

  void classify_and_emit(int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
                         int64_t block, const std::string &op,
                         const std::string &from, const std::string &to,
                         const std::string &token_id, int64_t amount,
                         uint32_t cond_idx, uint8_t token_idx) {
    TxKey tx_key{block, tx_hash};
    std::string cond_id = cond_idx < cond_ids_.size() ? cond_ids_[cond_idx] : "";
    TxCondKey tx_cond_key{block, tx_hash, cond_id};
    TxTokenKey tx_token_key{block, tx_hash, token_id};

    uint8_t outcome_cnt = cond_idx < conditions_.size() ? conditions_[cond_idx].outcome_count : 2;
    int64_t split_price = 1000000 / outcome_cnt;

    if (from == ZERO_ADDR) {
      auto sit = tx_split_.find(tx_cond_key);
      if (sit != tx_split_.end() && sit->second.stakeholder == to) {
        RawEvent evt{sort_key, cond_idx, EventType::Split, token_idx, 0, amount, split_price};
        push_event(to, evt);
        return;
      }
      auto fit = tx_fpmm_funding_.find(tx_key);
      if (fit != tx_fpmm_funding_.end() && fit->second.side == 1) {
        int64_t price = split_price;
        RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, 0, amount, price};
        push_event(fit->second.funder, evt);
        return;
      }
      return;
    }

    if (to == ZERO_ADDR) {
      auto mit = tx_merge_.find(tx_cond_key);
      if (mit != tx_merge_.end() && mit->second.stakeholder == from) {
        RawEvent evt{sort_key, cond_idx, EventType::Merge, token_idx, 0, -amount, split_price};
        push_event(from, evt);
        return;
      }
      auto rit = tx_redemption_.find(tx_cond_key);
      if (rit != tx_redemption_.end() && rit->second.redeemer == from) {
        int64_t payout_price = 1000000;
        if (cond_idx < conditions_.size()) {
          auto &payouts = conditions_[cond_idx].payout_numerators;
          if (token_idx < payouts.size() && payouts[token_idx] >= 0) {
            payout_price = payouts[token_idx];
          }
        }
        RawEvent evt{sort_key, cond_idx, EventType::Redemption, token_idx, 0, -amount, payout_price};
        push_event(from, evt);
        return;
      }
      auto cit = tx_convert_.find(tx_key);
      if (cit != tx_convert_.end() && cit->second.stakeholder == from) {
        int M = __builtin_popcountll(cit->second.index_set);
        int64_t conv_price = M > 1 ? 1000000 * (M - 1) / M : 0;
        RawEvent evt{sort_key, cond_idx, EventType::Convert, token_idx, 0, -amount, conv_price};
        push_event(from, evt);
        return;
      }
      auto fit = tx_fpmm_funding_.find(tx_key);
      if (fit != tx_fpmm_funding_.end() && fit->second.side == 2) {
        int64_t price = split_price;
        RawEvent evt{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, 0, -amount, price};
        push_event(fit->second.funder, evt);
        return;
      }
      return;
    }

    if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
      auto oit = tx_order_.find(tx_token_key);
      if (oit != tx_order_.end()) {
        int64_t price = oit->second.tokens > 0 ? (oit->second.usdc * 1000000 / oit->second.tokens) : 0;
        bool maker_buys = (oit->second.maker_side == 1);

        if (maker_buys) {
          RawEvent buy_evt{sort_key, cond_idx, EventType::Buy, token_idx, 0, amount, price};
          push_event(oit->second.maker, buy_evt);
          RawEvent sell_evt{sort_key, cond_idx, EventType::Sell, token_idx, 0, -amount, price};
          push_event(oit->second.taker, sell_evt);
        } else {
          RawEvent sell_evt{sort_key, cond_idx, EventType::Sell, token_idx, 0, -amount, price};
          push_event(oit->second.maker, sell_evt);
          RawEvent buy_evt{sort_key, cond_idx, EventType::Buy, token_idx, 0, amount, price};
          push_event(oit->second.taker, buy_evt);
        }
        return;
      }
      RawEvent in_evt{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0};
      push_event(to, in_evt);
      RawEvent out_evt{sort_key, cond_idx, EventType::TransferOut, token_idx, 0, -amount, 0};
      push_event(from, out_evt);
      return;
    }

    if (op == NEG_RISK_ADAPTER) {
      return;
    }

    auto fpmm_it = fpmm_map_.find(op);
    if (fpmm_it != fpmm_map_.end()) {
      auto tit = tx_fpmm_trade_.find(tx_key);
      if (tit != tx_fpmm_trade_.end()) {
        int64_t price = tit->second.tokens > 0 ? (tit->second.usdc * 1000000 / tit->second.tokens) : 0;
        if (tit->second.side == 1) {
          RawEvent evt{sort_key, cond_idx, EventType::FPMMBuy, token_idx, 0, amount, price};
          push_event(tit->second.trader, evt);
        } else {
          RawEvent evt{sort_key, cond_idx, EventType::FPMMSell, token_idx, 0, -amount, price};
          push_event(tit->second.trader, evt);
        }
        return;
      }
      if (from == op) {
        RawEvent evt{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0};
        push_event(to, evt);
        return;
      }
      return;
    }

    RawEvent in_evt{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0};
    push_event(to, in_evt);
    RawEvent out_evt{sort_key, cond_idx, EventType::TransferOut, token_idx, 0, -amount, 0};
    push_event(from, out_evt);
  }

  void commit_chunk(int64_t new_cursor) {
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
      conn->Query("INSERT OR REPLACE INTO rb_condition VALUES (" +
                  std::to_string(nc.idx) + ", " +
                  "'" + escape_blob(blob) + "'::BLOB, " +
                  std::to_string(nc.info.outcome_count) + ", " + pvals + ")");
    }

    for (auto &nt : new_tokens_) {
      std::string blob = hex_to_blob(nt.token_id);
      conn->Query(std::string("INSERT OR IGNORE INTO rb_token VALUES ('") +
                  escape_blob(blob) + "'::BLOB, " +
                  std::to_string(nt.cond_idx) + ", " +
                  std::to_string(nt.is_yes) + ")");
    }

    for (auto &nf : new_fpmms_) {
      std::string blob = hex_to_blob(nf.addr);
      conn->Query(std::string("INSERT OR IGNORE INTO rb_fpmm VALUES ('") +
                  escape_blob(blob) + "'::BLOB, " +
                  std::to_string(nf.cond_idx) + ")");
    }

    for (auto &[user, evt] : new_events_) {
      std::string user_blob = hex_to_blob(user);
      conn->Query(std::string("INSERT OR IGNORE INTO user_event VALUES ('") +
                  escape_blob(user_blob) + "'::BLOB, " +
                  std::to_string(evt.sort_key) + ", " +
                  std::to_string(evt.cond_idx) + ", " +
                  std::to_string(evt.type) + ", " +
                  std::to_string(evt.token_idx) + ", " +
                  std::to_string(evt.amount) + ", " +
                  std::to_string(evt.price) + ")");
    }

    conn->Query("UPDATE stage2_cursor SET value = " + std::to_string(new_cursor) +
                " WHERE key = 'last_block'");

    conn->Query("COMMIT");
  }

  static std::string escape_blob(const std::string &blob) {
    std::string result;
    result.reserve(blob.size() * 2);
    for (unsigned char c : blob) {
      if (c == '\'') {
        result += "''";
      } else {
        result += c;
      }
    }
    return result;
  }
};

} // namespace stage2
