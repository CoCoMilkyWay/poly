#pragma once

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stage2 {

// Chunk 日志系统：只记录有问题的 chunk
// 文件名格式：chunk_{startblock}_{NP数量}NP_{NU数量}NU.log
class ChunkLog {
public:
  void begin(const std::string &log_dir, int64_t start, int64_t end) {
    log_dir_ = log_dir;
    start_ = start;
    end_ = end;
    non_poly_samples_.clear();
    non_poly_by_op_.clear();
    non_poly_token_ids_.clear();
    non_usdc_samples_.clear();
    non_usdc_by_collat_.clear();
    header_info_.clear();
    token_sample_.clear();
  }

  void finish() {
    int64_t total_np = 0, total_nu = 0;
    for (const auto &[_, cnt] : non_poly_by_op_)
      total_np += cnt;
    for (const auto &[_, cnt] : non_usdc_by_collat_)
      total_nu += cnt;

    // 没有异常，不写文件
    if (total_np == 0 && total_nu == 0)
      return;

    std::filesystem::create_directories(log_dir_);
    std::string path = log_dir_ + "/chunk_" + std::to_string(start_) + "_" +
                       std::to_string(total_np) + "NP_" + std::to_string(total_nu) + "NU.log";
    std::ofstream ofs(path);
    if (!ofs)
      return;

    // 写入 header
    ofs << header_info_;
    if (!token_sample_.empty())
      ofs << token_sample_;

    // NonPolymarket 汇总
    if (total_np > 0) {
      ofs << "\n=== NonPolymarket Summary ===\n";
      ofs << "total=" << total_np << ", unique_tokens=" << non_poly_token_ids_.size() << "\n";
      ofs << "by_operator:\n";
      for (const auto &[op, cnt] : non_poly_by_op_) {
        ofs << "  " << op << ": " << cnt << "\n";
      }
      ofs << "samples (first 20):\n";
      for (const auto &s : non_poly_samples_) {
        ofs << "  block=" << s.block << " tx=" << s.tx_hash.substr(0, 18) << "..."
            << " op=" << s.op.substr(0, 12) << "..."
            << " token_len=" << s.token_id.size()
            << " token=" << s.token_id.substr(0, 20) << "..."
            << " amt=" << s.amount << "\n";
      }
    }

    // NonUsdc 汇总
    if (total_nu > 0) {
      ofs << "\n=== NonUsdcFpmm Summary ===\n";
      ofs << "total=" << total_nu << "\n";
      ofs << "by_collateral:\n";
      for (const auto &[collat, cnt] : non_usdc_by_collat_) {
        ofs << "  " << collat << ": " << cnt << "\n";
      }
    }
  }

  void log_non_polymarket(int64_t block, const std::string &tx_hash,
                          const std::string &op, const std::string &from,
                          const std::string &to, const std::string &token_id,
                          int64_t amount) {
    non_poly_by_op_[op]++;
    non_poly_token_ids_.insert(token_id);
    if (non_poly_samples_.size() < 20) {
      non_poly_samples_.push_back({block, tx_hash, op, from, to, token_id, amount});
    }
  }

  void log_non_usdc_fpmm(int64_t block, const std::string &tx_hash,
                         const std::string &op, const std::string &from,
                         const std::string &to, const std::string &token_id,
                         int64_t amount, const std::string &collateral) {
    non_usdc_by_collat_[collateral]++;
    if (non_usdc_samples_.size() < 10) {
      non_usdc_samples_.push_back({block, tx_hash, op, from, to, token_id, amount, collateral});
    }
  }

  void write_header(size_t token_map_size, size_t fpmm_map_size, size_t cond_map_size) {
    header_info_ = "=== Chunk [" + std::to_string(start_) + ", " + std::to_string(end_) + "] ===\n" +
                   "token_map.size=" + std::to_string(token_map_size) + "\n" +
                   "fpmm_map.size=" + std::to_string(fpmm_map_size) + "\n" +
                   "cond_map.size=" + std::to_string(cond_map_size) + "\n\n";
  }

  void write_token_sample(const std::string &token_id, uint32_t cond_idx, bool is_yes) {
    token_sample_ = "[TOKEN_SAMPLE] len=" + std::to_string(token_id.size()) + " id=" + token_id +
                    " cond_idx=" + std::to_string(cond_idx) + " is_yes=" + std::to_string(is_yes) + "\n";
  }

private:
  std::string log_dir_;
  int64_t start_ = 0, end_ = 0;
  std::string header_info_, token_sample_;

  struct Sample {
    int64_t block;
    std::string tx_hash, op, from, to, token_id;
    int64_t amount;
    std::string collateral;
  };

  std::vector<Sample> non_poly_samples_;
  std::unordered_map<std::string, int64_t> non_poly_by_op_;
  std::unordered_set<std::string> non_poly_token_ids_;
  std::vector<Sample> non_usdc_samples_;
  std::unordered_map<std::string, int64_t> non_usdc_by_collat_;
};

static constexpr const char *ZERO_ADDR = "0x0000000000000000000000000000000000000000";
static constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
static constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
static constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";
static constexpr const char *USDC_E = "0x2791bca1f2de4661ed88a30c99a7a9449aa84174";      // bridged USDC
static constexpr const char *USDC_NATIVE = "0x3c499c542cef5e3811e1192ce70d8cc03d5c3359"; // native USDC
static constexpr const char *CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";

inline bool is_usdc_collateral(const std::string &addr) {
  return addr == USDC_E || addr == USDC_NATIVE;
}
static constexpr const char *NO_TOKEN_BURN_ADDRESS = "0x36a0e974a7083ea0ad4dea6a27b90fab22e93a32";

struct ScanStats {
  int64_t rows = 0;
  int64_t events = 0;
};

enum class TransferClass {
  Split,
  Merge,
  Redemption,
  Convert,
  OrderBuy,
  OrderSell,
  FPMMBuy,
  FPMMSell,
  FPMMLPAdd,
  FPMMLPRemove,
  FPMMLPReturn,
  TransferIn,
  TransferOut,
  InternalMint,
  InternalBurn,
  InternalTransfer,
  NonUsdcFpmm,   // 非 USDC 抵押品的 FPMM 操作
  NonPolymarket, // 非 Polymarket 的 token（如 Omen 等其他协议使用 ConditionalTokens）
  Unclassified,
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
  int64_t non_usdc_fpmm = 0;
  int64_t non_polymarket = 0; // 非 Polymarket token（其他协议如 Omen）
  int64_t unclassified = 0;

  void add(TransferClass cls) {
    ++total;
    switch (cls) {
    case TransferClass::Split:
      ++split;
      break;
    case TransferClass::Merge:
      ++merge;
      break;
    case TransferClass::Redemption:
      ++redemption;
      break;
    case TransferClass::Convert:
      ++convert;
      break;
    case TransferClass::OrderBuy:
      ++order_buy;
      break;
    case TransferClass::OrderSell:
      ++order_sell;
      break;
    case TransferClass::FPMMBuy:
      ++fpmm_buy;
      break;
    case TransferClass::FPMMSell:
      ++fpmm_sell;
      break;
    case TransferClass::FPMMLPAdd:
      ++fpmm_lp_add;
      break;
    case TransferClass::FPMMLPRemove:
      ++fpmm_lp_remove;
      break;
    case TransferClass::FPMMLPReturn:
      ++fpmm_lp_return;
      break;
    case TransferClass::TransferIn:
      ++transfer_in;
      break;
    case TransferClass::TransferOut:
      ++transfer_out;
      break;
    case TransferClass::InternalMint:
      ++internal_mint;
      break;
    case TransferClass::InternalBurn:
      ++internal_burn;
      break;
    case TransferClass::InternalTransfer:
      ++internal_transfer;
      break;
    case TransferClass::NonUsdcFpmm:
      ++non_usdc_fpmm;
      break;
    case TransferClass::NonPolymarket:
      ++non_polymarket;
      break;
    case TransferClass::Unclassified:
      ++unclassified;
      break;
    }
  }

  void verify() const {
    int64_t semantic = split + merge + redemption + convert +
                       order_buy + order_sell +
                       fpmm_buy + fpmm_sell + fpmm_lp_add + fpmm_lp_remove + fpmm_lp_return;
    int64_t user_xfer = transfer_in + transfer_out;
    int64_t internal = internal_mint + internal_burn + internal_transfer;
    int64_t skipped = non_usdc_fpmm + non_polymarket;
    int64_t bad = unclassified;

    int64_t sum = semantic + user_xfer + internal + skipped + bad;
    if (sum != total) {
      std::cerr << "[ERROR] Transfer stats don't add up: sum=" << sum << ", total=" << total << std::endl;
      assert(false);
    }
    if (bad > 0) {
      std::cerr << "[ERROR] Unclassified transfers: " << bad << std::endl;
      assert(false);
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
    std::cerr << "  Skipped (non-Polymarket):" << std::endl;
    std::cerr << "    NonUsdcFpmm: " << non_usdc_fpmm << ", NonPolymarket: " << non_polymarket << std::endl;
    if (unclassified > 0) {
      std::cerr << "  Errors:" << std::endl;
      std::cerr << "    Unclassified: " << unclassified << std::endl;
    }
  }
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
  int64_t total_markets = 0;
  int64_t cnt_cond_amm = 0;
  int64_t cnt_cond_normal = 0;
  int64_t cnt_cond_negrisk = 0;
  int64_t cnt_token_amm = 0;
  int64_t cnt_token_negrisk = 0;
  int64_t cnt_token_non_usdc = 0;
  int64_t cnt_token_norm = 0;
  int64_t cnt_split = 0;
  int64_t cnt_merge = 0;
  int64_t cnt_redemption = 0;
  int64_t cnt_convert = 0;
  int64_t cnt_order = 0;
  int64_t cnt_fpmm_trade = 0;
  int64_t cnt_fpmm_funding = 0;
  int64_t cnt_transfer = 0;
  TransferStats xfer_stats;
};

} // namespace stage2
