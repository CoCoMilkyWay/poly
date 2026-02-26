#pragma once

#include "stage2_types.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace stage2 {

// Chunk 日志系统：只记录有问题的 chunk
// 文件名格式：chunk_{startblock}_{NP数量}NP_{NU数量}NU.log
class ChunkLog {
public:
  void begin(const std::string &log_dir, int64_t start, int64_t end);
  void finish();
  void log_non_polymarket(int64_t block, const std::string &tx_hash,
                          const std::string &op, const std::string &from,
                          const std::string &to, const std::string &token_id,
                          int64_t amount);
  void write_header(size_t token_map_size, size_t fpmm_map_size, size_t cond_map_size);
  void write_token_sample(const std::string &token_id, uint32_t cond_idx, bool is_yes);
  void set_xfer_stats(const std::string &stats_str);

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
static constexpr const char *WRAPPED_USDC_E = "0x3a3bd7bb9528e159577f7c2e685cc81a765002e2";
static constexpr const char *CONDITIONAL_TOKENS = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";
static constexpr const char *NO_TOKEN_BURN_ADDRESS = "0x36a0e974a7083ea0ad4dea6a27b90fab22e93a32";

uint8_t addr_to_known_collateral_id(const std::string &addr);

bool is_usdc_collateral(const std::string &addr);

bool is_usdc_collateral(Collateral c);

const char *collateral_name(Collateral c);

const char *collateral_addr(Collateral c);

struct ScanStats {
  int64_t rows = 0;
  int64_t events = 0;
};

enum class TransferClass {
  // === Split 铸造 (3) ===
  SplitNormal,  // mint→用户, stakeholder==to (普通市场)
  SplitNegRisk, // Adapter→用户, stakeholder==Adapter (NegRisk)
  SplitNonPoly, // 非Polymarket condition的Split

  // === Merge 合并 (3) ===
  MergeNormal,  // 用户→burn, stakeholder==from (普通市场)
  MergeNegRisk, // 用户→Adapter, stakeholder==Adapter (NegRisk)
  MergeNonPoly, // 非Polymarket condition的Merge

  // === Redemption 赎回 (2) ===
  Redemption,        // Polymarket condition的Redemption
  RedemptionNonPoly, // 非Polymarket condition的Redemption

  // === Polymarket专属操作 (8) ===
  Convert,      // 用户→BurnAddr with PositionsConverted
  OrderBuy,     // Exchange to用户
  OrderSell,    // Exchange from用户
  FPMMBuy,      // FPMM→用户
  FPMMSell,     // 用户→FPMM
  FPMMLPAdd,    // mint→FPMM with FundingAdded
  FPMMLPRemove, // FPMM→用户 with FundingRemoved
  FPMMLPReturn, // FPMM→用户 with FundingAdded (返还多余)

  // === Transfer 转账 (6) ===
  TransferInNegRisk,  // Adapter→用户 无Split (Poly)
  TransferInOther,    // 其他→用户 (Poly)
  TransferInNonPoly,  // 非Polymarket token转入
  TransferOutNegRisk, // 用户→Adapter 无Merge (Poly)
  TransferOutOther,   // 用户→其他 (Poly)
  TransferOutNonPoly, // 非Polymarket token转出

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

  // === 其他 (1) ===
  Unclassified, // ERROR
};

struct TransferStats {
  int64_t total = 0;

  // === Split 铸造 ===
  int64_t split_normal = 0;
  int64_t split_negrisk = 0;
  int64_t split_non_poly = 0;

  // === Merge 合并 ===
  int64_t merge_normal = 0;
  int64_t merge_negrisk = 0;
  int64_t merge_non_poly = 0;

  // === Redemption 赎回 ===
  int64_t redemption = 0;
  int64_t redemption_non_poly = 0;

  // === Polymarket专属操作 ===
  int64_t convert = 0;
  int64_t order_buy = 0;
  int64_t order_sell = 0;
  int64_t fpmm_buy = 0;
  int64_t fpmm_sell = 0;
  int64_t fpmm_lp_add = 0;
  int64_t fpmm_lp_remove = 0;
  int64_t fpmm_lp_return = 0;

  // === Transfer 转账 ===
  int64_t transfer_in_negrisk = 0;
  int64_t transfer_in_other = 0;
  int64_t transfer_in_non_poly = 0;
  int64_t transfer_out_negrisk = 0;
  int64_t transfer_out_other = 0;
  int64_t transfer_out_non_poly = 0;

  // === InternalMint 内铸 ===
  int64_t internal_mint_negrisk = 0;
  int64_t internal_mint_fpmm = 0;

  // === InternalBurn 内燃 ===
  int64_t internal_burn_negrisk = 0;
  int64_t internal_burn_fpmm = 0;
  int64_t internal_burn_convert = 0;

  // === InternalTransfer 内转 ===
  int64_t internal_transfer_zero = 0;
  int64_t internal_transfer_order = 0;
  int64_t internal_transfer_negrisk = 0;
  int64_t internal_transfer_fpmm = 0;
  int64_t internal_transfer_other = 0;

  // === 其他 ===
  int64_t unclassified = 0;

  // === 汇总字段 (由叶子节点计算) ===
  int64_t split() const { return split_normal + split_negrisk + split_non_poly; }
  int64_t split_poly() const { return split_normal + split_negrisk; }
  int64_t merge() const { return merge_normal + merge_negrisk + merge_non_poly; }
  int64_t merge_poly() const { return merge_normal + merge_negrisk; }
  int64_t redemption_all() const { return redemption + redemption_non_poly; }
  int64_t order() const { return order_buy + order_sell; }
  int64_t fpmm_trade() const { return fpmm_buy + fpmm_sell; }
  int64_t fpmm_lp() const { return fpmm_lp_add + fpmm_lp_remove + fpmm_lp_return; }
  int64_t transfer_in() const { return transfer_in_negrisk + transfer_in_other + transfer_in_non_poly; }
  int64_t transfer_in_poly() const { return transfer_in_negrisk + transfer_in_other; }
  int64_t transfer_out() const { return transfer_out_negrisk + transfer_out_other + transfer_out_non_poly; }
  int64_t transfer_out_poly() const { return transfer_out_negrisk + transfer_out_other; }
  int64_t transfer() const { return transfer_in() + transfer_out(); }
  int64_t internal_mint() const { return internal_mint_negrisk + internal_mint_fpmm; }
  int64_t internal_burn() const { return internal_burn_negrisk + internal_burn_fpmm + internal_burn_convert; }
  int64_t internal_transfer() const { return internal_transfer_zero + internal_transfer_order + internal_transfer_negrisk + internal_transfer_fpmm + internal_transfer_other; }

  // === 一级汇总 ===
  int64_t user_events() const {
    return split() + merge() + redemption_all() + convert + order() + fpmm_trade() + fpmm_lp() + transfer();
  }
  int64_t internal() const {
    return internal_mint() + internal_burn() + internal_transfer();
  }
  int64_t non_polymarket() const {
    return split_non_poly + merge_non_poly + redemption_non_poly + transfer_in_non_poly + transfer_out_non_poly;
  }
  int64_t skipped() const { return non_polymarket(); }

  void add(TransferClass cls);
  TransferStats &operator+=(const TransferStats &o);
  void verify() const;
  void print_summary() const;
  static std::string format_log(const TransferStats &chunk, const TransferStats &acc);
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
    int64_t other_fpmm = 0; // source=OtherFPMM (预期=0)
    int64_t split = 0;      // source=SplitEvent (预期=0)
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
      std::unordered_map<uint8_t, int64_t> by_collateral; // Collateral enum → token count
    } fpmm_only;
  } polymarket;
  struct Other {
    int64_t total = 0;
    int64_t other_fpmm = 0;        // source=OtherFPMM (预期=0)
    int64_t split = 0;             // source=SplitEvent (预期=0)
    int64_t transfer_inferred = 0; // 从Transfer中发现的未知token（无condition信息）
    std::unordered_map<std::string, int64_t> transfer_inferred_by_token;
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
  // 按(EventType, TokenId)分组统计 user_event（token_id 目前为 token_idx: 0=YES,1=NO）
  // key: EventType * 256 + TokenId
  std::unordered_map<uint16_t, int64_t> event_by_token;
  // 按(EventType, CollateralId)分组统计 user_event
  // key: EventType * 256 + CollateralId
  std::unordered_map<uint16_t, int64_t> event_by_collateral;
};

} // namespace stage2
