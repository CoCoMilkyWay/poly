#pragma once

#include "types.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
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
    header_info_.clear();
    token_sample_.clear();
    xfer_stats_str_.clear();
  }

  void finish() {
    int64_t total_np = 0;
    for (const auto &[_, cnt] : non_poly_by_op_)
      total_np += cnt;

    // 没有异常，不写文件
    if (total_np == 0)
      return;

    std::filesystem::create_directories(log_dir_);
    std::string path = log_dir_ + "/chunk_" + std::to_string(start_) + "_" + std::to_string(total_np) + "NP.log";
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

    // Transfer 统计
    if (!xfer_stats_str_.empty()) {
      ofs << "\n"
          << xfer_stats_str_;
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

  void set_xfer_stats(const std::string &stats_str) {
    xfer_stats_str_ = stats_str;
  }

private:
  std::string log_dir_;
  int64_t start_ = 0, end_ = 0;
  std::string header_info_, token_sample_, xfer_stats_str_;

  struct Sample {
    int64_t block;
    std::string tx_hash, op, from, to, token_id;
    int64_t amount;
  };

  std::vector<Sample> non_poly_samples_;
  std::unordered_map<std::string, int64_t> non_poly_by_op_;
  std::unordered_set<std::string> non_poly_token_ids_;
};

static constexpr const char *ZERO_ADDR = "0x0000000000000000000000000000000000000000";
static constexpr const char *CTF_EXCHANGE = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
static constexpr const char *NEG_RISK_CTF_EXCHANGE = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
static constexpr const char *NEG_RISK_ADAPTER = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";
static constexpr const char *USDC_E = "0x2791bca1f2de4661ed88a30c99a7a9449aa84174";      // bridged USDC
static constexpr const char *USDC_NATIVE = "0x3c499c542cef5e3811e1192ce70d8cc03d5c3359"; // native USDC
static constexpr const char *WETH = "0x7ceb23fd6bc0add59e62ac25578270cff1b9f619";
static constexpr const char *DAI = "0x8f3cf7ad23cd3cadbd9735aff958023239c6a063";
static constexpr const char *WMATIC = "0x0d500b1d8e8ef31e21c99d1db9a6444d3adf1270";
static constexpr const char *USDT = "0xc2132d05d31c914a87c6611c10748aeb04b58e8f";
static constexpr const char *CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";
static constexpr const char *NO_TOKEN_BURN_ADDRESS = "0x36a0e974a7083ea0ad4dea6a27b90fab22e93a32";

inline Collateral addr_to_collateral(const std::string &addr) {
  if (addr == USDC_E)
    return Collateral::USDC;
  if (addr == USDC_NATIVE)
    return Collateral::USDCe;
  if (addr == WETH)
    return Collateral::WETH;
  if (addr == DAI)
    return Collateral::DAI;
  if (addr == WMATIC)
    return Collateral::WMATIC;
  if (addr == USDT)
    return Collateral::USDT;
  return Collateral::Unknown;
}

inline bool is_usdc_collateral(const std::string &addr) {
  return addr == USDC_E || addr == USDC_NATIVE;
}

inline bool is_usdc_collateral(Collateral c) {
  return c == Collateral::USDC || c == Collateral::USDCe;
}

inline const char *collateral_name(Collateral c) {
  switch (c) {
  case Collateral::USDC:
    return "USDC";
  case Collateral::USDCe:
    return "USDC.e";
  case Collateral::WETH:
    return "WETH";
  case Collateral::DAI:
    return "DAI";
  case Collateral::WMATIC:
    return "WMATIC";
  case Collateral::USDT:
    return "USDT";
  default:
    return "Unknown";
  }
}

inline const char *collateral_addr(Collateral c) {
  switch (c) {
  case Collateral::USDC:
    return USDC_E;
  case Collateral::USDCe:
    return USDC_NATIVE;
  case Collateral::WETH:
    return WETH;
  case Collateral::DAI:
    return DAI;
  case Collateral::WMATIC:
    return WMATIC;
  case Collateral::USDT:
    return USDT;
  default:
    return "";
  }
}

struct ScanStats {
  int64_t rows = 0;
  int64_t events = 0;
};

enum class TransferClass {
  // === Split 铸造 (2) ===
  SplitNormal,  // mint→用户, stakeholder==to (普通市场)
  SplitNegRisk, // Adapter→用户, stakeholder==Adapter (NegRisk)

  // === Merge 合并 (2) ===
  MergeNormal,  // 用户→burn, stakeholder==from (普通市场)
  MergeNegRisk, // 用户→Adapter, stakeholder==Adapter (NegRisk)

  // === 其他用户事件 (7) ===
  Redemption,   // 用户→burn with PayoutRedemption
  Convert,      // 用户→BurnAddr with PositionsConverted
  OrderBuy,     // Exchange to用户
  OrderSell,    // Exchange from用户
  FPMMBuy,      // FPMM→用户
  FPMMSell,     // 用户→FPMM
  FPMMLPAdd,    // mint→FPMM with FundingAdded
  FPMMLPRemove, // FPMM→用户 with FundingRemoved
  FPMMLPReturn, // FPMM→用户 with FundingAdded (返还多余)

  // === Transfer 转账 (4) ===
  TransferInNegRisk,  // Adapter→用户 无Split
  TransferInOther,    // 其他→用户
  TransferOutNegRisk, // 用户→Adapter 无Merge
  TransferOutOther,   // 用户→其他

  // === InternalMint 内铸 (2) ===
  InternalMintNegRisk, // mint→Adapter
  InternalMintFPMM,    // mint→FPMM 无Funding

  // === InternalBurn 内燃 (3) ===
  InternalBurnNegRisk, // Adapter→burn
  InternalBurnFPMM,    // FPMM→burn (USDC)
  InternalBurnConvert, // Adapter→BurnAddr (Convert NO)

  // === InternalTransfer 内转 (5) ===
  InternalTransferZero,    // amount==0
  InternalTransferOrder,   // Exchange双方协议
  InternalTransferNegRisk, // NegRisk其他
  InternalTransferFPMM,    // FPMM Funding其他
  InternalTransferOther,   // 其他协议间

  // === 其他 (2) ===
  NonPolymarket, // 非Polymarket token
  Unclassified,  // ERROR
};

struct TransferStats {
  int64_t total = 0;

  // === Split 铸造 (叶子节点) ===
  int64_t split_normal = 0;
  int64_t split_negrisk = 0;

  // === Merge 合并 (叶子节点) ===
  int64_t merge_normal = 0;
  int64_t merge_negrisk = 0;

  // === 其他用户事件 (叶子节点) ===
  int64_t redemption = 0;
  int64_t convert = 0;
  int64_t order_buy = 0;
  int64_t order_sell = 0;
  int64_t fpmm_buy = 0;
  int64_t fpmm_sell = 0;
  int64_t fpmm_lp_add = 0;
  int64_t fpmm_lp_remove = 0;
  int64_t fpmm_lp_return = 0;

  // === Transfer 转账 (叶子节点) ===
  int64_t transfer_in_negrisk = 0;
  int64_t transfer_in_other = 0;
  int64_t transfer_out_negrisk = 0;
  int64_t transfer_out_other = 0;

  // === InternalMint 内铸 (叶子节点) ===
  int64_t internal_mint_negrisk = 0;
  int64_t internal_mint_fpmm = 0;

  // === InternalBurn 内燃 (叶子节点) ===
  int64_t internal_burn_negrisk = 0;
  int64_t internal_burn_fpmm = 0;
  int64_t internal_burn_convert = 0;

  // === InternalTransfer 内转 (叶子节点) ===
  int64_t internal_transfer_zero = 0;
  int64_t internal_transfer_order = 0;
  int64_t internal_transfer_negrisk = 0;
  int64_t internal_transfer_fpmm = 0;
  int64_t internal_transfer_other = 0;

  // === 其他 (叶子节点) ===
  int64_t non_polymarket = 0;
  int64_t unclassified = 0;

  // non_polymarket按operator->token细分（累积统计）
  // key: operator地址, value: map<token_id, count>
  std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> non_poly_by_op_token;

  // === 汇总字段 (由叶子节点计算) ===
  int64_t split() const { return split_normal + split_negrisk; }
  int64_t merge() const { return merge_normal + merge_negrisk; }
  int64_t order() const { return order_buy + order_sell; }
  int64_t fpmm_trade() const { return fpmm_buy + fpmm_sell; }
  int64_t fpmm_lp() const { return fpmm_lp_add + fpmm_lp_remove + fpmm_lp_return; }
  int64_t transfer_in() const { return transfer_in_negrisk + transfer_in_other; }
  int64_t transfer_out() const { return transfer_out_negrisk + transfer_out_other; }
  int64_t transfer() const { return transfer_in() + transfer_out(); }
  int64_t internal_mint() const { return internal_mint_negrisk + internal_mint_fpmm; }
  int64_t internal_burn() const { return internal_burn_negrisk + internal_burn_fpmm + internal_burn_convert; }
  int64_t internal_transfer() const { return internal_transfer_zero + internal_transfer_order + internal_transfer_negrisk + internal_transfer_fpmm + internal_transfer_other; }

  // === 一级汇总 ===
  int64_t user_events() const {
    return split() + merge() + redemption + convert + order() + fpmm_trade() + fpmm_lp() + transfer();
  }
  int64_t internal() const {
    return internal_mint() + internal_burn() + internal_transfer();
  }
  int64_t skipped() const {
    return non_polymarket;
  }

  void add(TransferClass cls) {
    ++total;
    switch (cls) {
    case TransferClass::SplitNormal:
      ++split_normal;
      break;
    case TransferClass::SplitNegRisk:
      ++split_negrisk;
      break;
    case TransferClass::MergeNormal:
      ++merge_normal;
      break;
    case TransferClass::MergeNegRisk:
      ++merge_negrisk;
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
    case TransferClass::TransferInNegRisk:
      ++transfer_in_negrisk;
      break;
    case TransferClass::TransferInOther:
      ++transfer_in_other;
      break;
    case TransferClass::TransferOutNegRisk:
      ++transfer_out_negrisk;
      break;
    case TransferClass::TransferOutOther:
      ++transfer_out_other;
      break;
    case TransferClass::InternalMintNegRisk:
      ++internal_mint_negrisk;
      break;
    case TransferClass::InternalMintFPMM:
      ++internal_mint_fpmm;
      break;
    case TransferClass::InternalBurnNegRisk:
      ++internal_burn_negrisk;
      break;
    case TransferClass::InternalBurnFPMM:
      ++internal_burn_fpmm;
      break;
    case TransferClass::InternalBurnConvert:
      ++internal_burn_convert;
      break;
    case TransferClass::InternalTransferZero:
      ++internal_transfer_zero;
      break;
    case TransferClass::InternalTransferOrder:
      ++internal_transfer_order;
      break;
    case TransferClass::InternalTransferNegRisk:
      ++internal_transfer_negrisk;
      break;
    case TransferClass::InternalTransferFPMM:
      ++internal_transfer_fpmm;
      break;
    case TransferClass::InternalTransferOther:
      ++internal_transfer_other;
      break;
    case TransferClass::NonPolymarket:
      ++non_polymarket;
      break;
    case TransferClass::Unclassified:
      ++unclassified;
      break;
    }
  }

  void add_non_poly_op(const std::string &op, const std::string &token_id) {
    non_poly_by_op_token[op][token_id]++;
  }

  void verify() const {
    int64_t sum = user_events() + internal() + skipped() + unclassified;
    assert(sum == total && "total = user_events + internal + skipped + unclassified");
    assert(unclassified == 0 && "unclassified must be 0");
  }

  void print_summary() const {
    std::cerr << "Transfer Stats Summary: total=" << total << std::endl;
    std::cerr << "  UserEvents: " << user_events() << std::endl;
    std::cerr << "    Split: " << split() << " (normal=" << split_normal << ", negrisk=" << split_negrisk << ")" << std::endl;
    std::cerr << "    Merge: " << merge() << " (normal=" << merge_normal << ", negrisk=" << merge_negrisk << ")" << std::endl;
    std::cerr << "    Redemption: " << redemption << ", Convert: " << convert << std::endl;
    std::cerr << "    Order: " << order() << " (buy=" << order_buy << ", sell=" << order_sell << ")" << std::endl;
    std::cerr << "    FPMMTrade: " << fpmm_trade() << " (buy=" << fpmm_buy << ", sell=" << fpmm_sell << ")" << std::endl;
    std::cerr << "    FPMMLP: " << fpmm_lp() << " (add=" << fpmm_lp_add << ", remove=" << fpmm_lp_remove << ", return=" << fpmm_lp_return << ")" << std::endl;
    std::cerr << "    Transfer: " << transfer() << " (in=" << transfer_in() << "[nr=" << transfer_in_negrisk << ",oth=" << transfer_in_other << "], out=" << transfer_out() << "[nr=" << transfer_out_negrisk << ",oth=" << transfer_out_other << "])" << std::endl;
    std::cerr << "  Internal: " << internal() << std::endl;
    std::cerr << "    Mint: " << internal_mint() << " (negrisk=" << internal_mint_negrisk << ", fpmm=" << internal_mint_fpmm << ")" << std::endl;
    std::cerr << "    Burn: " << internal_burn() << " (negrisk=" << internal_burn_negrisk << ", fpmm=" << internal_burn_fpmm << ", convert=" << internal_burn_convert << ")" << std::endl;
    std::cerr << "    Transfer: " << internal_transfer() << " (zero=" << internal_transfer_zero << ", order=" << internal_transfer_order << ", negrisk=" << internal_transfer_negrisk << ", fpmm=" << internal_transfer_fpmm << ", other=" << internal_transfer_other << ")" << std::endl;
    std::cerr << "  Skipped: " << skipped() << std::endl;
    std::cerr << "    NonPolymarket: " << non_polymarket << std::endl;
    if (unclassified > 0)
      std::cerr << "  ERROR Unclassified: " << unclassified << std::endl;
  }

  static std::string format_log(const TransferStats &chunk, const TransferStats &acc) {
    std::ostringstream oss;
    auto line = [&](const char *name, int64_t c, int64_t a) {
      if (c > 0 || a > 0)
        oss << "  " << name << ": +" << c << " (=" << a << ")\n";
    };
    oss << "=== Transfer Stats (chunk / accumulated) ===\n";
    oss << "Total: +" << chunk.total << " (=" << acc.total << ")\n";
    oss << "UserEvents: +" << chunk.user_events() << " (=" << acc.user_events() << ")\n";
    line("  铸造.普通", chunk.split_normal, acc.split_normal);
    line("  铸造.NegRisk", chunk.split_negrisk, acc.split_negrisk);
    line("  合并.普通", chunk.merge_normal, acc.merge_normal);
    line("  合并.NegRisk", chunk.merge_negrisk, acc.merge_negrisk);
    line("  赎回", chunk.redemption, acc.redemption);
    line("  转换", chunk.convert, acc.convert);
    line("  订单.买", chunk.order_buy, acc.order_buy);
    line("  订单.卖", chunk.order_sell, acc.order_sell);
    line("  AMM.买", chunk.fpmm_buy, acc.fpmm_buy);
    line("  AMM.卖", chunk.fpmm_sell, acc.fpmm_sell);
    line("  LP.加", chunk.fpmm_lp_add, acc.fpmm_lp_add);
    line("  LP.减", chunk.fpmm_lp_remove, acc.fpmm_lp_remove);
    line("  LP.返", chunk.fpmm_lp_return, acc.fpmm_lp_return);
    line("  转入.Adapter", chunk.transfer_in_negrisk, acc.transfer_in_negrisk);
    line("  转入.其他", chunk.transfer_in_other, acc.transfer_in_other);
    line("  转出.Adapter", chunk.transfer_out_negrisk, acc.transfer_out_negrisk);
    line("  转出.其他", chunk.transfer_out_other, acc.transfer_out_other);
    oss << "Internal: +" << chunk.internal() << " (=" << acc.internal() << ")\n";
    line("  内铸.Adapter", chunk.internal_mint_negrisk, acc.internal_mint_negrisk);
    line("  内铸.FPMM", chunk.internal_mint_fpmm, acc.internal_mint_fpmm);
    line("  内燃.Adapter", chunk.internal_burn_negrisk, acc.internal_burn_negrisk);
    line("  内燃.FPMM", chunk.internal_burn_fpmm, acc.internal_burn_fpmm);
    line("  内燃.Convert", chunk.internal_burn_convert, acc.internal_burn_convert);
    line("  内转.零值", chunk.internal_transfer_zero, acc.internal_transfer_zero);
    line("  内转.订单", chunk.internal_transfer_order, acc.internal_transfer_order);
    line("  内转.Adapter", chunk.internal_transfer_negrisk, acc.internal_transfer_negrisk);
    line("  内转.FPMM", chunk.internal_transfer_fpmm, acc.internal_transfer_fpmm);
    line("  内转.其他", chunk.internal_transfer_other, acc.internal_transfer_other);
    oss << "Skipped: +" << chunk.skipped() << " (=" << acc.skipped() << ")\n";
    line("  非Polymarket", chunk.non_polymarket, acc.non_polymarket);
    return oss.str();
  }
};

// 问题树状partition: total = polymarket + other
struct ConditionTree {
  int64_t total = 0;
  struct Polymarket {
    int64_t total = 0;
    struct TokenReg {
      int64_t total = 0;
      int64_t amm = 0;     // 后来创建了FPMM
      int64_t negrisk = 0; // 负风险市场
      int64_t normal = 0;  // 无FPMM，非NegRisk
    } token_reg;
    int64_t fpmm_only = 0; // 只从FPMM推断(source=PolyFPMM)
  } polymarket;
  struct Other {
    int64_t total = 0;
    int64_t prep = 0;       // source=ConditionPrep
    int64_t other_fpmm = 0; // source=OtherFPMM
    int64_t split = 0;      // source=SplitEvent
  } other;
};

// 代币树状partition: total = polymarket + other
struct TokenTree {
  int64_t total = 0;
  struct Polymarket {
    int64_t total = 0;
    struct TokenReg {
      int64_t total = 0;
      int64_t amm = 0;     // 后来创建了FPMM
      int64_t negrisk = 0; // 负风险市场
      int64_t normal = 0;  // 无FPMM，非NegRisk
    } token_reg;
    struct FpmmOnly {
      int64_t total = 0;
      int64_t usdc = 0;     // USDC抵押品
      int64_t non_usdc = 0; // WETH等其他抵押品
    } fpmm_only;
  } polymarket;
  struct Other {
    int64_t total = 0;
    int64_t other_fpmm = 0; // source=OtherFPMM
    int64_t split = 0;      // source=SplitEvent
  } other;
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
  // 树状partition统计
  ConditionTree cond_tree;
  TokenTree token_tree;
  // 事件统计
  int64_t cnt_split = 0;
  int64_t cnt_merge = 0;
  int64_t cnt_redemption = 0;
  int64_t cnt_convert = 0;
  int64_t cnt_order = 0;
  int64_t cnt_fpmm_trade = 0;
  int64_t cnt_fpmm_funding = 0;
  int64_t cnt_transfer = 0;
  TransferStats xfer_stats;
  // 按(EventType, Collateral)分组统计 user_event
  // key: EventType * 16 + Collateral
  std::unordered_map<uint16_t, int64_t> event_by_collateral;
};

} // namespace stage2
