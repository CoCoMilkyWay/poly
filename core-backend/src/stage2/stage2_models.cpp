#include "stage2_models.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace stage2 {

void ChunkLog::begin(const std::string &log_dir, int64_t start, int64_t end) {
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

void ChunkLog::finish() {
  int64_t total_np = 0;
  for (const auto &[_, cnt] : non_poly_by_op_)
    total_np += cnt;

  if (total_np == 0)
    return;

  std::filesystem::create_directories(log_dir_);
  std::string path = log_dir_ + "/chunk_" + std::to_string(start_) + "_" + std::to_string(total_np) + "NP.log";
  std::ofstream ofs(path);
  if (!ofs)
    return;

  ofs << header_info_;
  if (!token_sample_.empty())
    ofs << token_sample_;

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

  if (!xfer_stats_str_.empty()) {
    ofs << "\n"
        << xfer_stats_str_;
  }
}

void ChunkLog::log_non_polymarket(int64_t block, const std::string &tx_hash,
                                  const std::string &op, const std::string &from,
                                  const std::string &to, const std::string &token_id,
                                  int64_t amount) {
  non_poly_by_op_[op]++;
  non_poly_token_ids_.insert(token_id);
  if (non_poly_samples_.size() < 20) {
    non_poly_samples_.push_back({block, tx_hash, op, from, to, token_id, amount});
  }
}

void ChunkLog::write_header(size_t token_map_size, size_t fpmm_map_size, size_t cond_map_size) {
  header_info_ = "=== Chunk [" + std::to_string(start_) + ", " + std::to_string(end_) + "] ===\n" +
                 "token_map.size=" + std::to_string(token_map_size) + "\n" +
                 "fpmm_map.size=" + std::to_string(fpmm_map_size) + "\n" +
                 "cond_map.size=" + std::to_string(cond_map_size) + "\n\n";
}

void ChunkLog::write_token_sample(const std::string &token_id, uint32_t cond_idx, bool is_yes) {
  token_sample_ = "[TOKEN_SAMPLE] len=" + std::to_string(token_id.size()) + " id=" + token_id +
                  " cond_idx=" + std::to_string(cond_idx) + " is_yes=" + std::to_string(is_yes) + "\n";
}

void ChunkLog::set_xfer_stats(const std::string &stats_str) { xfer_stats_str_ = stats_str; }

uint8_t addr_to_known_collateral_id(const std::string &addr) {
  if (addr == USDC_NATIVE)
    return static_cast<uint8_t>(Collateral::USDC);
  if (addr == USDC_E)
    return static_cast<uint8_t>(Collateral::USDCe);
  if (addr == WETH)
    return static_cast<uint8_t>(Collateral::WETH);
  if (addr == DAI)
    return static_cast<uint8_t>(Collateral::DAI);
  if (addr == WMATIC)
    return static_cast<uint8_t>(Collateral::WMATIC);
  if (addr == USDT)
    return static_cast<uint8_t>(Collateral::USDT);
  if (addr == WRAPPED_USDC_E)
    return static_cast<uint8_t>(Collateral::WrappedUSDCe);
  return static_cast<uint8_t>(Collateral::Unknown);
}

bool is_usdc_collateral(const std::string &addr) {
  return addr == USDC_E || addr == USDC_NATIVE;
}

bool is_usdc_collateral(Collateral c) {
  return c == Collateral::USDC || c == Collateral::USDCe || c == Collateral::WrappedUSDCe;
}

const char *collateral_name(Collateral c) {
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
  case Collateral::WrappedUSDCe:
    return "wUSDC.e";
  default:
    return "";
  }
}

const char *collateral_addr(Collateral c) {
  switch (c) {
  case Collateral::USDC:
    return USDC_NATIVE;
  case Collateral::USDCe:
    return USDC_E;
  case Collateral::WETH:
    return WETH;
  case Collateral::DAI:
    return DAI;
  case Collateral::WMATIC:
    return WMATIC;
  case Collateral::USDT:
    return USDT;
  case Collateral::WrappedUSDCe:
    return WRAPPED_USDC_E;
  default:
    return ZERO_ADDR;
  }
}

void TransferStats::add(TransferClass cls) {
  ++total;
  switch (cls) {
  case TransferClass::SplitNormal:
    ++split_normal;
    break;
  case TransferClass::SplitNegRisk:
    ++split_negrisk;
    break;
  case TransferClass::SplitNonPoly:
    ++split_non_poly;
    break;
  case TransferClass::MergeNormal:
    ++merge_normal;
    break;
  case TransferClass::MergeNegRisk:
    ++merge_negrisk;
    break;
  case TransferClass::MergeNonPoly:
    ++merge_non_poly;
    break;
  case TransferClass::Redemption:
    ++redemption;
    break;
  case TransferClass::RedemptionNonPoly:
    ++redemption_non_poly;
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
  case TransferClass::TransferInNonPoly:
    ++transfer_in_non_poly;
    break;
  case TransferClass::TransferOutNegRisk:
    ++transfer_out_negrisk;
    break;
  case TransferClass::TransferOutOther:
    ++transfer_out_other;
    break;
  case TransferClass::TransferOutNonPoly:
    ++transfer_out_non_poly;
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
  case TransferClass::Unclassified:
    ++unclassified;
    break;
  }
}

TransferStats &TransferStats::operator+=(const TransferStats &o) {
  total += o.total;
  split_normal += o.split_normal;
  split_negrisk += o.split_negrisk;
  split_non_poly += o.split_non_poly;
  merge_normal += o.merge_normal;
  merge_negrisk += o.merge_negrisk;
  merge_non_poly += o.merge_non_poly;
  redemption += o.redemption;
  redemption_non_poly += o.redemption_non_poly;
  convert += o.convert;
  order_buy += o.order_buy;
  order_sell += o.order_sell;
  fpmm_buy += o.fpmm_buy;
  fpmm_sell += o.fpmm_sell;
  fpmm_lp_add += o.fpmm_lp_add;
  fpmm_lp_remove += o.fpmm_lp_remove;
  fpmm_lp_return += o.fpmm_lp_return;
  transfer_in_negrisk += o.transfer_in_negrisk;
  transfer_in_other += o.transfer_in_other;
  transfer_in_non_poly += o.transfer_in_non_poly;
  transfer_out_negrisk += o.transfer_out_negrisk;
  transfer_out_other += o.transfer_out_other;
  transfer_out_non_poly += o.transfer_out_non_poly;
  internal_mint_negrisk += o.internal_mint_negrisk;
  internal_mint_fpmm += o.internal_mint_fpmm;
  internal_burn_negrisk += o.internal_burn_negrisk;
  internal_burn_fpmm += o.internal_burn_fpmm;
  internal_burn_convert += o.internal_burn_convert;
  internal_transfer_zero += o.internal_transfer_zero;
  internal_transfer_order += o.internal_transfer_order;
  internal_transfer_negrisk += o.internal_transfer_negrisk;
  internal_transfer_fpmm += o.internal_transfer_fpmm;
  internal_transfer_other += o.internal_transfer_other;
  unclassified += o.unclassified;
  return *this;
}

void TransferStats::verify() const {
  int64_t sum = user_events() + internal() + unclassified;
  assert(sum == total && "total = user_events + internal + unclassified");
  assert(unclassified == 0 && "unclassified must be 0");
}

void TransferStats::print_summary() const {
  std::cerr << "Transfer Stats Summary: total=" << total << std::endl;
  std::cerr << "  UserEvents: " << user_events() << std::endl;
  std::cerr << "    Split: " << split() << " (poly=" << split_poly() << ", non_poly=" << split_non_poly << ")" << std::endl;
  std::cerr << "    Merge: " << merge() << " (poly=" << merge_poly() << ", non_poly=" << merge_non_poly << ")" << std::endl;
  std::cerr << "    Redemption: " << redemption_all() << " (poly=" << redemption << ", non_poly=" << redemption_non_poly << ")" << std::endl;
  std::cerr << "    Convert: " << convert << std::endl;
  std::cerr << "    Order: " << order() << " (buy=" << order_buy << ", sell=" << order_sell << ")" << std::endl;
  std::cerr << "    FPMMTrade: " << fpmm_trade() << " (buy=" << fpmm_buy << ", sell=" << fpmm_sell << ")" << std::endl;
  std::cerr << "    FPMMLP: " << fpmm_lp() << " (add=" << fpmm_lp_add << ", remove=" << fpmm_lp_remove << ", return=" << fpmm_lp_return << ")" << std::endl;
  std::cerr << "    TransferIn: " << transfer_in() << " (poly=" << transfer_in_poly() << ", non_poly=" << transfer_in_non_poly << ")" << std::endl;
  std::cerr << "    TransferOut: " << transfer_out() << " (poly=" << transfer_out_poly() << ", non_poly=" << transfer_out_non_poly << ")" << std::endl;
  std::cerr << "  Internal: " << internal() << std::endl;
  std::cerr << "    Mint: " << internal_mint() << " (negrisk=" << internal_mint_negrisk << ", fpmm=" << internal_mint_fpmm << ")" << std::endl;
  std::cerr << "    Burn: " << internal_burn() << " (negrisk=" << internal_burn_negrisk << ", fpmm=" << internal_burn_fpmm << ", convert=" << internal_burn_convert << ")" << std::endl;
  std::cerr << "    Transfer: " << internal_transfer() << " (zero=" << internal_transfer_zero << ", order=" << internal_transfer_order << ", negrisk=" << internal_transfer_negrisk << ", fpmm=" << internal_transfer_fpmm << ", other=" << internal_transfer_other << ")" << std::endl;
  if (unclassified > 0)
    std::cerr << "  ERROR Unclassified: " << unclassified << std::endl;
}

std::string TransferStats::format_log(const TransferStats &chunk, const TransferStats &acc) {
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
  line("  非Polymarket", chunk.non_polymarket(), acc.non_polymarket());
  return oss.str();
}

} // namespace stage2
