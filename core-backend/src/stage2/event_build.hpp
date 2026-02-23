#pragma once

#include "../core/database.hpp"
#include "../core/keccak256.hpp"
#include "misc/profiler.hpp"
#include "types.hpp"
#include <algorithm>
#include <cassert>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace stage2 {

static constexpr const char *ZERO_ADDR = "0x0000000000000000000000000000000000000000";
static constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
static constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
static constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";
static constexpr const char *USDC_E = "0x2791bca1f2de4661ed88a30c99a7a9449aa84174";
static constexpr const char *CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";
// NegRisk Convert burn address: keccak256("NO_TOKEN_BURN_ADDRESS")[0:20]
static constexpr const char *NO_TOKEN_BURN_ADDRESS = "0x36a0e974a7083ea0ad4dea6a27b90fab22e93a32";

struct ScanStats {
  int64_t rows = 0;
  int64_t events = 0;
};

// Transfer 分类结果 - 用于验证每笔 transfer 都被正确处理
enum class TransferClass {
  // 语义事件 (有对应的语义事件匹配)
  Split,            // mint: 普通/NegRisk split
  Merge,            // burn: 普通/NegRisk merge
  Redemption,       // burn: 赎回
  Convert,          // to=Adapter: NegRisk convert
  OrderBuy,         // Exchange: 订单买方
  OrderSell,        // Exchange: 订单卖方
  FPMMBuy,          // FPMM: AMM买
  FPMMSell,         // FPMM: AMM卖
  FPMMLPAdd,        // mint to FPMM: LP添加
  FPMMLPRemove,     // burn from FPMM: LP移除
  FPMMLPReturn,     // FPMM→user: LP多余token返还

  // 用户转账 (无语义事件匹配)
  TransferIn,       // 用户直接转账入
  TransferOut,      // 用户直接转账出

  // 内部操作 (跳过，不记录给任何用户)
  InternalMint,     // 协议内部mint (NegRisk/FPMM)
  InternalBurn,     // 协议内部burn (NegRisk/FPMM)
  InternalTransfer, // 协议内部转账

  // 错误 (不应该发生)
  UnknownToken,     // token_id 不在 token_map 中
  Unclassified,     // 无法分类 (bug)
};

struct TransferStats {
  int64_t total = 0;
  int64_t split = 0;
  int64_t merge = 0;
  int64_t redemption = 0;
  int64_t convert = 0;
  int64_t order_buy = 0;
  int64_t order_sell = 0;
  int64_t fpmm_buy = 0;
  int64_t fpmm_sell = 0;
  int64_t fpmm_lp_add = 0;
  int64_t fpmm_lp_remove = 0;
  int64_t fpmm_lp_return = 0;
  int64_t transfer_in = 0;
  int64_t transfer_out = 0;
  int64_t internal_mint = 0;
  int64_t internal_burn = 0;
  int64_t internal_transfer = 0;
  int64_t unknown_token = 0;
  int64_t unclassified = 0;

  void add(TransferClass cls) {
    ++total;
    switch (cls) {
    case TransferClass::Split: ++split; break;
    case TransferClass::Merge: ++merge; break;
    case TransferClass::Redemption: ++redemption; break;
    case TransferClass::Convert: ++convert; break;
    case TransferClass::OrderBuy: ++order_buy; break;
    case TransferClass::OrderSell: ++order_sell; break;
    case TransferClass::FPMMBuy: ++fpmm_buy; break;
    case TransferClass::FPMMSell: ++fpmm_sell; break;
    case TransferClass::FPMMLPAdd: ++fpmm_lp_add; break;
    case TransferClass::FPMMLPRemove: ++fpmm_lp_remove; break;
    case TransferClass::FPMMLPReturn: ++fpmm_lp_return; break;
    case TransferClass::TransferIn: ++transfer_in; break;
    case TransferClass::TransferOut: ++transfer_out; break;
    case TransferClass::InternalMint: ++internal_mint; break;
    case TransferClass::InternalBurn: ++internal_burn; break;
    case TransferClass::InternalTransfer: ++internal_transfer; break;
    case TransferClass::UnknownToken: ++unknown_token; break;
    case TransferClass::Unclassified: ++unclassified; break;
    }
  }

  void verify() const {
    int64_t semantic = split + merge + redemption + convert +
                       order_buy + order_sell +
                       fpmm_buy + fpmm_sell + fpmm_lp_add + fpmm_lp_remove + fpmm_lp_return;
    int64_t user_xfer = transfer_in + transfer_out;
    int64_t internal = internal_mint + internal_burn + internal_transfer;
    int64_t unknown = unknown_token;
    int64_t bad = unclassified;

    int64_t sum = semantic + user_xfer + internal + unknown + bad;
    if (sum != total) {
      std::cerr << "[ERROR] Transfer stats don't add up: sum=" << sum << ", total=" << total << std::endl;
      assert(false);
    }
    if (bad > 0) {
      std::cerr << "[ERROR] Unclassified transfers: " << bad << std::endl;
      assert(false);
    }
    if (unknown > 0) {
      std::cerr << "[WARN] Unknown token transfers: " << unknown << std::endl;
    }
  }

  void print_summary() const {
    std::cerr << "Transfer Stats Summary:" << std::endl;
    std::cerr << "  Total: " << total << std::endl;
    std::cerr << "  Semantic Events:" << std::endl;
    std::cerr << "    Split: " << split << ", Merge: " << merge << ", Redemption: " << redemption << std::endl;
    std::cerr << "    Convert: " << convert << std::endl;
    std::cerr << "    OrderBuy: " << order_buy << ", OrderSell: " << order_sell << std::endl;
    std::cerr << "    FPMMBuy: " << fpmm_buy << ", FPMMSell: " << fpmm_sell << std::endl;
    std::cerr << "    FPMMLPAdd: " << fpmm_lp_add << ", FPMMLPRemove: " << fpmm_lp_remove << ", FPMMLPReturn: " << fpmm_lp_return << std::endl;
    std::cerr << "  User Transfers:" << std::endl;
    std::cerr << "    TransferIn: " << transfer_in << ", TransferOut: " << transfer_out << std::endl;
    std::cerr << "  Internal:" << std::endl;
    std::cerr << "    InternalMint: " << internal_mint << ", InternalBurn: " << internal_burn << ", InternalTransfer: " << internal_transfer << std::endl;
    std::cerr << "  Errors:" << std::endl;
    std::cerr << "    UnknownToken: " << unknown_token << ", Unclassified: " << unclassified << std::endl;
  }
};

struct UnknownTokenInfo {
  int64_t block;
  std::string op;
  std::string from;
  std::string to;
  std::string token_id;
  int64_t amount;
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
  int64_t total_users = 0;
  int64_t total_markets = 0;    // NegRisk 市场数
  int64_t cnt_cond_amm = 0;     // AMM 问题数
  int64_t cnt_cond_normal = 0;  // Normal 问题数
  int64_t cnt_cond_negrisk = 0; // NegRisk 问题数
  int64_t cnt_split = 0;
  int64_t cnt_merge = 0;
  int64_t cnt_redemption = 0;
  int64_t cnt_convert = 0;
  int64_t cnt_order = 0;
  int64_t cnt_fpmm_trade = 0;
  int64_t cnt_fpmm_funding = 0;
  int64_t cnt_transfer = 0;
  TransferStats xfer_stats;     // Transfer 分类统计
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
        payout_7    BIGINT,
        question_id BLOB
      )
    )");
    // 添加question_id列（如果旧表没有这个列）
    {
      auto conn = stage2_db_.create_connection();
      auto r = conn->Query("SELECT column_name FROM information_schema.columns "
                           "WHERE table_name='rb_condition' AND column_name='question_id'");
      if (r->RowCount() == 0) {
        stage2_db_.execute("ALTER TABLE rb_condition ADD COLUMN question_id BLOB");
      }
    }

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
      CREATE TABLE IF NOT EXISTS rb_neg_risk_market (
        question_id BLOB PRIMARY KEY,
        market_id   BLOB NOT NULL
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
    auto conn = stage2_db_.create_connection();

    auto cur = conn->Query("SELECT value FROM stage2_cursor WHERE key='last_block'");
    progress_.cursor = cur->RowCount() > 0 ? cur->GetValue(0, 0).GetValue<int64_t>() : 0;

    // 从 user_event 表重新计算事件计数（保证数据一致性）
    auto cnt_r = conn->Query(R"(
      SELECT event_type, COUNT(*) as cnt FROM user_event GROUP BY event_type
    )");
    for (idx_t i = 0; i < cnt_r->RowCount(); ++i) {
      int type = cnt_r->GetValue(0, i).GetValue<int>();
      int64_t cnt = cnt_r->GetValue(1, i).GetValue<int64_t>();
      switch (static_cast<EventType>(type)) {
      case EventType::Buy:
      case EventType::Sell:
        progress_.cnt_order += cnt;
        break;
      case EventType::Split:
        progress_.cnt_split += cnt;
        break;
      case EventType::Merge:
        progress_.cnt_merge += cnt;
        break;
      case EventType::Redemption:
        progress_.cnt_redemption += cnt;
        break;
      case EventType::FPMMBuy:
      case EventType::FPMMSell:
        progress_.cnt_fpmm_trade += cnt;
        break;
      case EventType::FPMMLPAdd:
      case EventType::FPMMLPRemove:
        progress_.cnt_fpmm_funding += cnt;
        break;
      case EventType::Convert:
        progress_.cnt_convert += cnt;
        break;
      case EventType::TransferIn:
      case EventType::TransferOut:
        progress_.cnt_transfer += cnt;
        break;
      }
      progress_.total_events += cnt;
    }

    auto cond_r = conn->Query("SELECT cond_idx, cond_id, outcome_cnt, "
                              "payout_0, payout_1, payout_2, payout_3, "
                              "payout_4, payout_5, payout_6, payout_7, question_id FROM rb_condition ORDER BY cond_idx");
    for (idx_t i = 0; i < cond_r->RowCount(); ++i) {
      uint32_t idx = cond_r->GetValue(0, i).GetValue<uint32_t>();
      std::string cond_id = blob_to_hex(cond_r->GetValue(1, i).GetValueUnsafe<std::string>());
      ConditionInfo info;
      info.outcome_count = cond_r->GetValue(2, i).GetValue<uint8_t>();
      for (int j = 0; j < info.outcome_count; ++j) {
        auto v = cond_r->GetValue(3 + j, i);
        info.payout_numerators.push_back(v.IsNull() ? -1 : v.GetValue<int64_t>());
      }
      auto qid_v = cond_r->GetValue(11, i);
      if (!qid_v.IsNull()) {
        info.question_id = to_lower(blob_to_hex(qid_v.GetValueUnsafe<std::string>()));
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
      fpmm_cond_idxs_.insert(info.cond_idx);
    }

    // 从 rb_neg_risk_market 表加载 question_id -> market_id 映射，并标记 NegRisk 条件
    auto nrm_r = conn->Query("SELECT question_id, market_id FROM rb_neg_risk_market");
    for (idx_t i = 0; i < nrm_r->RowCount(); ++i) {
      std::string question_id = to_lower(blob_to_hex(nrm_r->GetValue(0, i).GetValueUnsafe<std::string>()));
      std::string market_id = to_lower(blob_to_hex(nrm_r->GetValue(1, i).GetValueUnsafe<std::string>()));
      cond_to_market_[question_id] = market_id;
      seen_markets_.insert(market_id);

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
    progress_.total_markets = seen_markets_.size();
    update_cond_type_stats();

    // 加载已知用户（恢复时从数据库）
    auto user_r = conn->Query("SELECT DISTINCT user_addr FROM user_event");
    for (idx_t i = 0; i < user_r->RowCount(); ++i) {
      std::string addr = blob_to_hex(user_r->GetValue(0, i).GetValueUnsafe<std::string>());
      seen_users_.insert(to_lower(addr));
    }
    progress_.total_users = seen_users_.size();

    if (progress_.cursor > 0)
      progress_.phase = 3;
  }

  int64_t cursor() const { return progress_.cursor; }

  bool build_chunk(int64_t target_block) {
    if (progress_.cursor >= target_block)
      return false;

    int64_t chunk_start = progress_.cursor;
    int64_t chunk_end = std::min(progress_.cursor + chunk_size_, target_block);
    // std::cerr << "[Stage2] Processing chunk: " << chunk_start << " -> " << chunk_end << std::endl;
    progress_.target = target_block;
    progress_.chunk_start = chunk_start;
    progress_.chunk_end = chunk_end;
    progress_.running = true;

    new_conditions_.clear();
    new_tokens_.clear();
    new_fpmms_.clear();
    new_neg_risk_markets_.clear();
    new_events_.clear();

    tx_split_.clear();
    tx_merge_.clear();
    tx_redemption_.clear();
    tx_convert_.clear();
    tx_order_.clear();
    tx_fpmm_trade_.clear();
    tx_fpmm_funding_.clear();
    chunk_xfer_stats_ = {};

    progress_.phase = 1;
    phase1_update_mappings(chunk_start, chunk_end);

    progress_.phase = 2;
    phase2_build_semantic_index(chunk_start, chunk_end);

    progress_.phase = 3;
    phase3_process_transfers(chunk_start, chunk_end);

    // 验证 transfer 分类完整性
    chunk_xfer_stats_.verify();
    // 打印 unknown token 详情（如果有）
    print_unknown_tokens();
    progress_.xfer_stats.total += chunk_xfer_stats_.total;
    progress_.xfer_stats.split += chunk_xfer_stats_.split;
    progress_.xfer_stats.merge += chunk_xfer_stats_.merge;
    progress_.xfer_stats.redemption += chunk_xfer_stats_.redemption;
    progress_.xfer_stats.convert += chunk_xfer_stats_.convert;
    progress_.xfer_stats.order_buy += chunk_xfer_stats_.order_buy;
    progress_.xfer_stats.order_sell += chunk_xfer_stats_.order_sell;
    progress_.xfer_stats.fpmm_buy += chunk_xfer_stats_.fpmm_buy;
    progress_.xfer_stats.fpmm_sell += chunk_xfer_stats_.fpmm_sell;
    progress_.xfer_stats.fpmm_lp_add += chunk_xfer_stats_.fpmm_lp_add;
    progress_.xfer_stats.fpmm_lp_remove += chunk_xfer_stats_.fpmm_lp_remove;
    progress_.xfer_stats.fpmm_lp_return += chunk_xfer_stats_.fpmm_lp_return;
    progress_.xfer_stats.transfer_in += chunk_xfer_stats_.transfer_in;
    progress_.xfer_stats.transfer_out += chunk_xfer_stats_.transfer_out;
    progress_.xfer_stats.internal_mint += chunk_xfer_stats_.internal_mint;
    progress_.xfer_stats.internal_burn += chunk_xfer_stats_.internal_burn;
    progress_.xfer_stats.internal_transfer += chunk_xfer_stats_.internal_transfer;
    progress_.xfer_stats.unknown_token += chunk_xfer_stats_.unknown_token;

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
  std::unordered_map<std::string, std::string> cond_to_market_; // condition_id -> market_id
  std::unordered_set<std::string> seen_users_;                  // 实时统计唯一用户
  std::unordered_set<std::string> seen_markets_;                // 统计唯一 NegRisk 市场
  std::unordered_set<uint32_t> fpmm_cond_idxs_;                 // AMM 对应的 cond_idx
  std::unordered_set<uint32_t> negrisk_cond_idxs_;              // NegRisk 对应的 cond_idx

  std::unordered_map<TxCondKey, std::vector<SplitInfo>> tx_split_;
  std::unordered_map<TxCondKey, std::vector<MergeInfo>> tx_merge_;
  std::unordered_map<TxCondKey, std::vector<RedemptionInfo>> tx_redemption_;
  std::unordered_map<TxMarketKey, std::vector<ConvertInfo>> tx_convert_;
  std::unordered_map<TxTokenKey, OrderInfo> tx_order_;
  std::unordered_map<TxFPMMKey, FPMMTradeInfo> tx_fpmm_trade_;
  std::unordered_map<TxFPMMKey, FPMMFundingInfo> tx_fpmm_funding_;
  TransferStats chunk_xfer_stats_;  // 当前 chunk 的 transfer 统计
  std::vector<UnknownTokenInfo> unknown_tokens_;  // 未知 token 详情

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

  struct NewNegRiskMarket {
    std::string question_id;
    std::string market_id;
  };

  std::vector<NewCondition> new_conditions_;
  std::vector<NewToken> new_tokens_;
  std::vector<NewFPMM> new_fpmms_;
  std::vector<NewNegRiskMarket> new_neg_risk_markets_;
  std::vector<std::tuple<std::string, RawEvent>> new_events_;

  static std::string blob_to_hex(const std::string &blob) {
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
      try {
        result[i] = static_cast<uint8_t>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] hex_to_bytes32 failed at i=" << i << ", hex='" << hex << "', substr='" << h.substr(i * 2, 2) << "'" << std::endl;
        throw;
      }
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
      try {
        result.push_back(static_cast<char>(std::stoul(h.substr(i, 2), nullptr, 16)));
      } catch (const std::exception &e) {
        std::cerr << "[ERROR] hex_to_blob failed at i=" << i << ", hex='" << hex << "', substr='" << h.substr(i, 2) << "'" << std::endl;
        throw;
      }
    }
    return result;
  }

  uint32_t intern_condition(const std::string &cond_id, uint8_t outcome_cnt, const std::string &question_id = "") {
    std::string lower = to_lower(cond_id);
    auto it = cond_map_.find(lower);
    if (it != cond_map_.end()) {
      // 更新question_id（如果之前没有）
      if (!question_id.empty() && conditions_[it->second].question_id.empty()) {
        conditions_[it->second].question_id = question_id;
        // 确保更新被持久化
        bool found = false;
        for (auto &nc : new_conditions_) {
          if (nc.idx == it->second) {
            nc.info.question_id = question_id;
            found = true;
            break;
          }
        }
        if (!found) {
          // 条件在之前 chunk 创建，需要添加到 new_conditions_ 以持久化更新
          new_conditions_.push_back({it->second, cond_ids_[it->second], conditions_[it->second]});
        }
      }
      return it->second;
    }

    uint32_t idx = static_cast<uint32_t>(conditions_.size());
    ConditionInfo info;
    info.outcome_count = outcome_cnt;
    info.question_id = question_id;
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
    fpmm_cond_idxs_.insert(cond_idx);
  }

  void update_cond_type_stats() {
    int64_t amm = 0, negrisk = 0, normal = 0;
    for (size_t i = 0; i < conditions_.size(); ++i) {
      uint32_t idx = static_cast<uint32_t>(i);
      if (fpmm_cond_idxs_.count(idx)) {
        ++amm;
      } else if (negrisk_cond_idxs_.count(idx)) {
        ++negrisk;
      } else {
        ++normal;
      }
    }
    progress_.cnt_cond_amm = amm;
    progress_.cnt_cond_negrisk = negrisk;
    progress_.cnt_cond_normal = normal;
  }

  void push_event(const std::string &user_addr, const RawEvent &evt) {
    std::string lower = to_lower(user_addr);
    new_events_.emplace_back(lower, evt);
    progress_.total_events++;

    // 实时更新用户数
    if (seen_users_.insert(lower).second) {
      progress_.total_users = seen_users_.size();
    }

    // 更新事件计数
    switch (evt.type) {
    case EventType::Buy:
    case EventType::Sell:
      progress_.cnt_order++;
      break;
    case EventType::Split:
      progress_.cnt_split++;
      break;
    case EventType::Merge:
      progress_.cnt_merge++;
      break;
    case EventType::Redemption:
      progress_.cnt_redemption++;
      break;
    case EventType::FPMMBuy:
    case EventType::FPMMSell:
      progress_.cnt_fpmm_trade++;
      break;
    case EventType::FPMMLPAdd:
    case EventType::FPMMLPRemove:
      progress_.cnt_fpmm_funding++;
      break;
    case EventType::Convert:
      progress_.cnt_convert++;
      break;
    case EventType::TransferIn:
    case EventType::TransferOut:
      progress_.cnt_transfer++;
      break;
    }
  }

  void phase1_update_mappings(int64_t start, int64_t end) {
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
      intern_token(token0, cond_idx, 1); // YES
      intern_token(token1, cond_idx, 0); // NO
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
      intern_fpmm(addr, cond_idx);

      // 计算FPMM的YES和NO token_id
      // collectionId = keccak256(parentCollectionId[32] + conditionId[32] + indexSet[32])
      // positionId = keccak256(collateralToken[20] + collectionId[32])
      auto cond_bytes = hex_to_blob(lower_cid);
      auto collateral_bytes = hex_to_blob(collateral);
      for (int index_set = 1; index_set <= 2; ++index_set) {
        // collectionId = keccak256(0x00..00[32] + conditionId[32] + indexSet[32])
        std::string collection_input(96, '\0');
        // parentCollectionId = 0x0 (前32字节已经是0)
        std::memcpy(collection_input.data() + 32, cond_bytes.data(), std::min(size_t(32), cond_bytes.size()));
        collection_input[95] = static_cast<char>(index_set); // indexSet在最后一个字节(uint256大端)
        auto collection_hash = crypto::keccak256(collection_input);

        // positionId = keccak256(collateralToken[20] + collectionId[32])
        std::string position_input(52, '\0');
        std::memcpy(position_input.data(), collateral_bytes.data(), std::min(size_t(20), collateral_bytes.size()));
        std::memcpy(position_input.data() + 20, collection_hash.data(), 32);
        auto position_hash = crypto::keccak256(position_input);

        std::string token_id = crypto::Keccak256::to_hex(position_hash);
        intern_token(token_id, cond_idx, index_set == 1 ? 1 : 0); // 1=YES, 2=NO
      }
    }

    // 建立condition -> market映射 (用于NegRisk convert)
    // conditionId = keccak256(oracle[20] + questionId[32] + outcomeSlotCount[32])
    auto nrq = conn->Query(
        "SELECT market_id, question_id FROM " + stage1_db_.feather_table_range("neg_risk_question", start, end) +
        " WHERE block_number > " + std::to_string(start) + " AND block_number <= " + std::to_string(end));
    for (idx_t i = 0; i < nrq->RowCount(); ++i) {
      std::string market_id = to_lower(blob_to_hex(nrq->GetValue(0, i).GetValueUnsafe<std::string>()));
      std::string question_id = to_lower(blob_to_hex(nrq->GetValue(1, i).GetValueUnsafe<std::string>()));

      // 只记录新的映射
      if (!cond_to_market_.count(question_id)) {
        cond_to_market_[question_id] = market_id;
        seen_markets_.insert(market_id);
        new_neg_risk_markets_.push_back({question_id, market_id});
        progress_.total_markets = seen_markets_.size();
      }

      // 计算 conditionId 并标记对应条件为 NegRisk
      // conditionId = keccak256(oracle[20] + questionId[32] + outcomeSlotCount[32])
      auto oracle_bytes = hex_to_blob(NEG_RISK_ADAPTER);
      auto qid_bytes = hex_to_blob(question_id);
      std::string input(84, '\0');
      std::memcpy(input.data(), oracle_bytes.data(), std::min(size_t(20), oracle_bytes.size()));
      std::memcpy(input.data() + 20, qid_bytes.data(), std::min(size_t(32), qid_bytes.size()));
      input[83] = 2; // outcomeSlotCount = 2 (uint256 大端)
      auto cond_hash = crypto::keccak256(input);
      std::string cond_id = to_lower(crypto::Keccak256::to_hex(cond_hash));
      
      auto it = cond_map_.find(cond_id);
      if (it != cond_map_.end()) {
        negrisk_cond_idxs_.insert(it->second);
      }
    }
    update_cond_type_stats();
  }

  void phase2_build_semantic_index(int64_t start, int64_t end) {
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
      // 同一 tx 同一 token 不应有多个 order (每个 OrderFilled 对应一笔 Transfer)
      assert(tx_order_.count(key) == 0 && "Duplicate order for same token in same tx");
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
      // 同一 tx 同一 FPMM 不应有多个 trade 事件
      assert(tx_fpmm_trade_.count(key) == 0 && "Duplicate FPMM trade in same tx");
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
      // 同一 tx 同一 FPMM 不应有多个 funding 事件
      assert(tx_fpmm_funding_.count(key) == 0 && "Duplicate FPMM funding in same tx");
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

  void phase3_process_transfers(int64_t start, int64_t end) {
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
        chunk_xfer_stats_.add(TransferClass::UnknownToken);
        // 记录 unknown token 详情用于诊断
        unknown_tokens_.push_back({r.block, op, from, to, token_id, r.amount});
        continue;
      }
      uint32_t cond_idx = tit->second.cond_idx;
      uint8_t token_idx = tit->second.is_yes ? 0 : 1;

      TransferClass cls = classify_and_emit(sort_key, tx_hash, r.block, op, from, to, token_id, r.amount, cond_idx, token_idx);
      chunk_xfer_stats_.add(cls);
    }
  }

  // 检查地址是否是协议合约（不应该记录事件给协议合约）
  bool is_protocol_contract(const std::string &addr) const {
    return addr == ZERO_ADDR || addr == CTF_EXCHANGE || addr == NEG_RISK_CTF_EXCHANGE ||
           addr == NEG_RISK_ADAPTER || addr == CONDITIONAL_TOKENS ||
           addr == NO_TOKEN_BURN_ADDRESS || fpmm_map_.count(addr) > 0;
  }

  // 打印 unknown token 详情
  void print_unknown_tokens() {
    if (unknown_tokens_.empty()) return;
    
    // 按 operator 和 token_id 统计
    std::unordered_map<std::string, int> by_op;
    std::unordered_set<std::string> unique_tokens;
    bool is_mint = false, is_burn = false, is_transfer = false;
    for (const auto &t : unknown_tokens_) {
      by_op[t.op]++;
      unique_tokens.insert(t.token_id);
      if (t.from == ZERO_ADDR) is_mint = true;
      else if (t.to == ZERO_ADDR) is_burn = true;
      else is_transfer = true;
    }
    
    std::cerr << "[DEBUG] Unknown tokens breakdown (" << unknown_tokens_.size() << " transfers, "
              << unique_tokens.size() << " unique tokens):" << std::endl;
    
    // 类型分布
    std::cerr << "  Transfer types: ";
    if (is_mint) std::cerr << "MINT ";
    if (is_burn) std::cerr << "BURN ";
    if (is_transfer) std::cerr << "TRANSFER ";
    std::cerr << std::endl;
    
    std::cerr << "  By operator:" << std::endl;
    for (const auto &[addr, cnt] : by_op) {
      std::string label = addr;
      if (addr == CTF_EXCHANGE) label = "CTF_EXCHANGE";
      else if (addr == NEG_RISK_CTF_EXCHANGE) label = "NEG_RISK_CTF_EXCHANGE";
      else if (addr == NEG_RISK_ADAPTER) label = "NEG_RISK_ADAPTER";
      else if (addr == CONDITIONAL_TOKENS) label = "CONDITIONAL_TOKENS";
      else if (fpmm_map_.count(addr)) label = "FPMM(" + addr.substr(0, 10) + "...)";
      std::cerr << "    " << label << ": " << cnt << std::endl;
    }
    
    // 打印前 3 个样本
    std::cerr << "  Sample unknown tokens (first 3):" << std::endl;
    for (size_t i = 0; i < std::min(unknown_tokens_.size(), size_t(3)); ++i) {
      const auto &t = unknown_tokens_[i];
      std::cerr << "    block=" << t.block 
                << ", op=" << t.op.substr(0, 10) << "..."
                << ", from=" << (t.from == ZERO_ADDR ? "0x0(mint)" : t.from.substr(0, 10) + "...")
                << ", to=" << (t.to == ZERO_ADDR ? "0x0(burn)" : t.to.substr(0, 10) + "...")
                << ", token=" << t.token_id.substr(0, 20) << "..."
                << ", amt=" << t.amount << std::endl;
    }
    
    // 清空以便下一个 chunk
    unknown_tokens_.clear();
  }

  TransferClass classify_and_emit(int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
                         int64_t block, const std::string &op,
                         const std::string &from, const std::string &to,
                         const std::string &token_id, int64_t amount,
                         uint32_t cond_idx, uint8_t token_idx) {
    // ===== 基础验证 =====
    assert(amount >= 0 && "Transfer amount must be non-negative");
    if (amount == 0)
      return TransferClass::InternalTransfer; // 0-amount 视为内部操作
    assert(cond_idx < conditions_.size() && "Invalid cond_idx");
    assert(token_idx < 2 && "Invalid token_idx");
    assert(from != to && "from and to must be different");

    std::string cond_id = cond_ids_[cond_idx];
    TxCondKey tx_cond_key{block, tx_hash, cond_id};
    TxTokenKey tx_token_key{block, tx_hash, token_id};

    uint8_t outcome_cnt = conditions_[cond_idx].outcome_count;
    int64_t split_price = 1000000 / outcome_cnt;

    // ========== mint 分支 (from == 0x0) ==========
    if (from == ZERO_ADDR) {
      // NegRisk 内部 mint → 跳过（用户通过后续 transfer 获取）
      if (to == NEG_RISK_ADAPTER)
        return TransferClass::InternalMint;

      // FPMM 内部 mint → 检查是否是 LP Add（必须先于 Split 检查！）
      if (fpmm_map_.count(to) > 0) {
        TxFPMMKey tx_fpmm_key{block, tx_hash, to};
        auto fit = tx_fpmm_funding_.find(tx_fpmm_key);
        if (fit != tx_fpmm_funding_.end() && fit->second.side == 1) {
          // LP Add: mint 的量是 max(amount0, amount1)，因为 split 生成等量 YES+NO
          int64_t split_amt = std::max(fit->second.amount0, fit->second.amount1);
          assert(amount == split_amt && "LP Add split amount mismatch");
          // 记录进入池子的量（不是 mint 的量）
          if (!is_protocol_contract(fit->second.funder)) {
            int64_t pool_amt = (token_idx == 0) ? fit->second.amount0 : fit->second.amount1;
            RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, 0, pool_amt, split_price};
            push_event(fit->second.funder, evt);
          }
          return TransferClass::FPMMLPAdd;
        }
        // FPMM 内部 split (无 funding 事件)
        return TransferClass::InternalMint;
      }

      // 普通 Split: stakeholder == to (用户直接 split)
      auto sit = tx_split_.find(tx_cond_key);
      if (sit != tx_split_.end()) {
        for (const auto &info : sit->second) {
          if (info.stakeholder == to) {
            // 验证 amount 与 split 事件一致
            assert(amount == info.amount && "Split amount mismatch");
            if (!is_protocol_contract(to)) {
              RawEvent evt{sort_key, cond_idx, EventType::Split, token_idx, 0, amount, split_price};
              push_event(to, evt);
            }
            return TransferClass::Split;
          }
        }
      }
      // 无法匹配的 mint → 不应该发生
      std::cerr << "[ERROR] Unmatched mint transfer: block=" << block 
                << ", to=" << to << ", token_id=" << token_id 
                << ", amount=" << amount << ", cond_idx=" << cond_idx << std::endl;
      assert(false && "Unmatched mint transfer");
      return TransferClass::Unclassified;
    }

    // ========== burn 分支 (to == 0x0) ==========
    if (to == ZERO_ADDR) {
      // NegRisk 内部 burn → 跳过（用户已通过 transfer 记录）
      if (from == NEG_RISK_ADAPTER)
        return TransferClass::InternalBurn;

      // FPMM 内部 burn：只会在 FPMM sell 时发生（内部 merge）
      // 注意：LP Remove 不涉及 burn，而是 transfer（见 FPMM operator 分支）
      if (fpmm_map_.count(from) > 0) {
        // FPMM 内部 merge for sell
        return TransferClass::InternalBurn;
      }

      // 普通 Merge: stakeholder == from (用户直接 merge)
      auto mit = tx_merge_.find(tx_cond_key);
      if (mit != tx_merge_.end()) {
        for (const auto &info : mit->second) {
          if (info.stakeholder == from) {
            // 验证 amount 与 merge 事件一致
            assert(amount == info.amount && "Merge amount mismatch");
            if (!is_protocol_contract(from)) {
              RawEvent evt{sort_key, cond_idx, EventType::Merge, token_idx, 0, -amount, split_price};
              push_event(from, evt);
            }
            return TransferClass::Merge;
          }
        }
      }

      // Redemption: redeemer == from
      auto rit = tx_redemption_.find(tx_cond_key);
      if (rit != tx_redemption_.end()) {
        for (const auto &info : rit->second) {
          if (info.redeemer == from) {
            if (!is_protocol_contract(from)) {
              auto &payouts = conditions_[cond_idx].payout_numerators;
              int64_t payout_price = (token_idx < payouts.size() && payouts[token_idx] >= 0)
                                         ? payouts[token_idx]
                                         : 1000000;
              assert(payout_price >= 0 && payout_price <= 1000000 && "Invalid payout price");
              RawEvent evt{sort_key, cond_idx, EventType::Redemption, token_idx, 0, -amount, payout_price};
              push_event(from, evt);
            }
            return TransferClass::Redemption;
          }
        }
      }
      // 无法匹配的 burn → 不应该发生
      std::cerr << "[ERROR] Unmatched burn transfer: block=" << block 
                << ", from=" << from << ", token_id=" << token_id 
                << ", amount=" << amount << ", cond_idx=" << cond_idx << std::endl;
      assert(false && "Unmatched burn transfer");
      return TransferClass::Unclassified;
    }

    // ========== Exchange operator ==========
    if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
      auto oit = tx_order_.find(tx_token_key);
      if (oit != tx_order_.end()) {
        // 验证订单数据
        assert(oit->second.tokens > 0 && "Order tokens must be positive");
        // 验证 amount 与订单一致 (1 transfer = 1 order_filled，应该完全匹配)
        assert(amount == oit->second.tokens && "Order amount mismatch: transfer amount != order tokens");
        // 验证 from/to 与 maker/taker 一致
        // maker_side=1 表示 maker 买入(出 USDC 收 token), 则 to=maker, from=taker
        // maker_side=2 表示 maker 卖出(出 token 收 USDC), 则 from=maker, to=taker
        if (oit->second.maker_side == 1) {
          assert(to == oit->second.maker && "Order buyer mismatch: to != maker");
          assert(from == oit->second.taker && "Order seller mismatch: from != taker");
        } else {
          assert(from == oit->second.maker && "Order seller mismatch: from != maker");
          assert(to == oit->second.taker && "Order buyer mismatch: to != taker");
        }

        int64_t price = oit->second.usdc * 1000000 / oit->second.tokens;
        assert(price >= 0 && price <= 1000000 && "Price out of range [0,1]");
        // 只给非协议方记录事件
        if (!is_protocol_contract(to))
          push_event(to, RawEvent{sort_key, cond_idx, EventType::Buy, token_idx, 0, amount, price});
        if (!is_protocol_contract(from))
          push_event(from, RawEvent{sort_key, cond_idx, EventType::Sell, token_idx, 0, -amount, price});

        // 一笔 transfer 只统计一次
        bool to_user = !is_protocol_contract(to);
        bool from_user = !is_protocol_contract(from);
        if (to_user) {
          return TransferClass::OrderBuy;
        } else if (from_user) {
          return TransferClass::OrderSell;
        } else {
          return TransferClass::InternalTransfer;
        }
      }
      // 无对应 order → 不应该发生（Exchange 只会在有 order 时操作）
      std::cerr << "[ERROR] Exchange transfer without order event: block=" << block 
                << ", op=" << op << ", from=" << from << ", to=" << to 
                << ", token_id=" << token_id << ", amount=" << amount << std::endl;
      assert(false && "Exchange transfer without order event");
      return TransferClass::Unclassified;
    }

    // ========== NegRisk Adapter operator ==========
    if (op == NEG_RISK_ADAPTER) {
      // Adapter → 用户: NegRisk Split (或 Convert 的 YES 输出)
      if (from == NEG_RISK_ADAPTER) {
        auto sit = tx_split_.find(tx_cond_key);
        if (sit != tx_split_.end()) {
          for (const auto &info : sit->second) {
            if (info.stakeholder == NEG_RISK_ADAPTER) {
              // 验证 amount: 对于 Convert 操作，transfer_amount <= split_amount (因为有手续费)
              // 检查是否是 Convert 的一部分（YES token，即 token_idx=0）
              bool is_convert_output = false;
              if (token_idx == 0 && !conditions_[cond_idx].question_id.empty()) {
                auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
                if (market_it != cond_to_market_.end()) {
                  TxMarketKey tx_market_key{block, tx_hash, market_it->second};
                  is_convert_output = (tx_convert_.count(tx_market_key) > 0);
                }
              }
              if (is_convert_output) {
                // Convert 的 YES 输出：允许 amount <= info.amount (扣除手续费)
                assert(amount <= info.amount && "Convert YES output amount exceeds split amount");
              } else {
                // 普通 NegRisk Split：amount 必须精确匹配
                assert(amount == info.amount && "NegRisk Split amount mismatch");
              }
              if (!is_protocol_contract(to)) {
                push_event(to, RawEvent{sort_key, cond_idx, EventType::Split, token_idx, 0, amount, split_price});
              }
              return TransferClass::Split;
            }
          }
        }
        // 无 split 事件 → TransferIn（罕见）
        if (!is_protocol_contract(to))
          push_event(to, RawEvent{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0});
        return TransferClass::TransferIn;
      }
      // 用户 → Adapter: NegRisk Merge
      if (to == NEG_RISK_ADAPTER) {
        auto mit = tx_merge_.find(tx_cond_key);
        if (mit != tx_merge_.end()) {
          for (const auto &info : mit->second) {
            if (info.stakeholder == NEG_RISK_ADAPTER) {
              assert(amount == info.amount && "NegRisk Merge amount mismatch");
              if (!is_protocol_contract(from)) {
                push_event(from, RawEvent{sort_key, cond_idx, EventType::Merge, token_idx, 0, -amount, split_price});
              }
              return TransferClass::Merge;
            }
          }
        }
        if (!is_protocol_contract(from))
          push_event(from, RawEvent{sort_key, cond_idx, EventType::TransferOut, token_idx, 0, -amount, 0});
        return TransferClass::TransferOut;
      }
      // 用户/Adapter → BurnAddr: NegRisk Convert (NO tokens 销毁)
      // convertPositions: user's NO → burn, adapter's accumulated NO → burn
      if (to == NO_TOKEN_BURN_ADDRESS) {
        // 只有 NO token (token_idx=1) 会被发送到 burn address
        assert(token_idx == 1 && "Convert should only burn NO tokens");
        // 如果是 adapter → burn，是内部操作，跳过
        if (from == NEG_RISK_ADAPTER) {
          return TransferClass::InternalBurn;
        }
        // 用户 → burn: 查找 Convert 事件
        if (!conditions_[cond_idx].question_id.empty()) {
          auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
          if (market_it != cond_to_market_.end()) {
            TxMarketKey tx_market_key{block, tx_hash, market_it->second};
            auto cit = tx_convert_.find(tx_market_key);
            if (cit != tx_convert_.end()) {
              for (const auto &info : cit->second) {
                if (info.stakeholder == from) {
                  assert(amount == info.amount && "Convert amount mismatch");
                  int M = __builtin_popcountll(info.index_set);
                  assert(M >= 2 && "Convert requires at least 2 positions");
                  if (!is_protocol_contract(from)) {
                    int64_t conv_price = 1000000 * (M - 1) / M;
                    push_event(from, RawEvent{sort_key, cond_idx, EventType::Convert, token_idx, 0, -amount, conv_price});
                  }
                  return TransferClass::Convert;
                }
              }
            }
          }
        }
        // 没有匹配的 convert 事件，但发送到 burn addr → 错误
        std::cerr << "[ERROR] Transfer to NO_TOKEN_BURN_ADDRESS without convert event: block=" << block 
                  << ", from=" << from << ", token_id=" << token_id 
                  << ", amount=" << amount << std::endl;
        assert(false && "Transfer to NO_TOKEN_BURN_ADDRESS without convert event");
        return TransferClass::Unclassified;
      }
      // Adapter 内部 → 跳过
      return TransferClass::InternalTransfer;
    }

    // ========== FPMM operator ==========
    auto fpmm_it = fpmm_map_.find(op);
    if (fpmm_it != fpmm_map_.end()) {
      TxFPMMKey tx_fpmm_key{block, tx_hash, op};
      auto tit = tx_fpmm_trade_.find(tx_fpmm_key);
      auto fit = tx_fpmm_funding_.find(tx_fpmm_key);

      // FPMM buy/sell (有 trade 事件)
      if (tit != tx_fpmm_trade_.end()) {
        // 验证 amount 与 trade 事件一致
        assert(amount == tit->second.tokens && "FPMM trade amount mismatch");
        // 验证 outcome_index 与 token_idx 一致 (0=YES, 1=NO)
        assert(tit->second.outcome_idx == token_idx && "FPMM trade outcome mismatch");
        // 验证方向：buy 时 from=FPMM, sell 时 to=FPMM
        if (tit->second.side == 1) {
          assert(from == op && "FPMM buy should transfer from FPMM");
          assert(to == tit->second.trader && "FPMM buy recipient mismatch");
        } else {
          assert(to == op && "FPMM sell should transfer to FPMM");
          assert(from == tit->second.trader && "FPMM sell sender mismatch");
        }

        if (!is_protocol_contract(tit->second.trader)) {
          assert(tit->second.tokens > 0 && "FPMM trade tokens must be positive");
          int64_t price = tit->second.usdc * 1000000 / tit->second.tokens;
          assert(price >= 0 && price <= 1000000 && "FPMM price out of range");
          if (tit->second.side == 1) {
            push_event(tit->second.trader, RawEvent{sort_key, cond_idx, EventType::FPMMBuy, token_idx, 0, amount, price});
            return TransferClass::FPMMBuy;
          } else {
            push_event(tit->second.trader, RawEvent{sort_key, cond_idx, EventType::FPMMSell, token_idx, 0, -amount, price});
            return TransferClass::FPMMSell;
          }
        }
        return (tit->second.side == 1) ? TransferClass::FPMMBuy : TransferClass::FPMMSell;
      }

      // LP 操作 (有 funding 事件)
      if (fit != tx_fpmm_funding_.end()) {
        // FPMM → 用户 的 transfer
        if (from == op && !is_protocol_contract(to)) {
          if (fit->second.side == 2) {
            // LP Remove: FPMM 将 YES+NO token 转给用户（这是 LP 撤出！）
            // FPMMFundingRemoved 事件的 amountsRemoved 就是转给用户的量
            int64_t expected_amt = (token_idx == 0) ? fit->second.amount0 : fit->second.amount1;
            assert(amount == expected_amt && "LP Remove amount mismatch");
            push_event(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, 0, amount, split_price});
            return TransferClass::FPMMLPRemove;
          } else {
            // LP Add 时返还多余 token 给用户（这只是返还，不影响 LP 头寸）
            // 这些返还的 token 不应该记录为 TransferIn，因为它们是 split 的一部分
            // 用户的净 delta 应该通过 FPMMLPAdd 事件的 pool_amt 来计算
            return TransferClass::FPMMLPReturn;
          }
        }
        // 其他 funding 相关的内部转账
        return TransferClass::InternalTransfer;
      }

      // 无 trade/funding 事件，但 operator 是 FPMM → 应该不发生
      std::cerr << "[ERROR] FPMM transfer without trade/funding event: block=" << block 
                << ", op=" << op << ", from=" << from << ", to=" << to 
                << ", token_id=" << token_id << ", amount=" << amount << std::endl;
      assert(false && "FPMM transfer without trade/funding event");
      return TransferClass::Unclassified;
    }

    // ========== 其他：用户间直接转账 ==========
    // 只为非协议合约记录事件（可能是未知的 FPMM 等）
    if (!is_protocol_contract(to))
      push_event(to, RawEvent{sort_key, cond_idx, EventType::TransferIn, token_idx, 0, amount, 0});
    if (!is_protocol_contract(from))
      push_event(from, RawEvent{sort_key, cond_idx, EventType::TransferOut, token_idx, 0, -amount, 0});

    // 一笔转账产生 TransferIn 和 TransferOut 各一个事件，但统计只算一次
    if (!is_protocol_contract(to) && !is_protocol_contract(from)) {
      // 用户间转账，两边都记录，统计算 TransferIn+TransferOut 的组合
      // 这里返回 TransferIn，TransferOut 的数量会少一半，但这是设计如此
      return TransferClass::TransferIn;
    } else if (!is_protocol_contract(to)) {
      return TransferClass::TransferIn;
    } else if (!is_protocol_contract(from)) {
      return TransferClass::TransferOut;
    } else {
      return TransferClass::InternalTransfer;
    }
  }

  void commit_chunk(int64_t new_cursor) {
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
      conn->Query("INSERT OR IGNORE INTO rb_fpmm VALUES (" +
                  blob_to_hex_literal(blob) + ", " +
                  std::to_string(nf.cond_idx) + ")");
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

  static std::string blob_to_hex_literal(const std::string &blob) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string result = "X'";
    result.reserve(3 + blob.size() * 2);
    for (unsigned char c : blob) {
      result.push_back(hex_chars[c >> 4]);
      result.push_back(hex_chars[c & 0x0f]);
    }
    result.push_back('\'');
    return result;
  }
};

} // namespace stage2
