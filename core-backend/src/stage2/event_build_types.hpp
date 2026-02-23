#pragma once

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

namespace stage2 {

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
