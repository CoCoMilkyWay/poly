#include "event_build.hpp"

namespace stage2 {

TransferClass EventBuilder::classify_and_emit(
    int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
    int64_t block, const std::string &op,
    const std::string &from, const std::string &to,
    const std::string &token_id, int64_t amount,
    uint32_t cond_idx, uint8_t token_idx, Collateral collateral) {

  assert(amount >= 0 && "Transfer amount must be non-negative");
  if (amount == 0)
    return TransferClass::InternalTransferZero;
  assert(cond_idx < conditions_.size() && "Invalid cond_idx");
  assert(token_idx < 2 && "Invalid token_idx");
  assert(from != to && "from and to must be different");

  uint8_t coll = static_cast<uint8_t>(collateral);

  // 对于推断的token（没有Split/Merge等事件），直接分类为TransferIn/TransferOut
  if (conditions_[cond_idx].source == ConditionSource::TransferInferred) {
    if (!is_protocol_contract(to))
      push_event(to, RawEvent{sort_key, cond_idx, EventType::TransferIn, token_idx, coll, 0, amount, 0});
    if (!is_protocol_contract(from))
      push_event(from, RawEvent{sort_key, cond_idx, EventType::TransferOut, token_idx, coll, 0, -amount, 0});
    
    if (!is_protocol_contract(to))
      return TransferClass::TransferInOther;
    else if (!is_protocol_contract(from))
      return TransferClass::TransferOutOther;
    else
      return TransferClass::InternalTransferOther;
  }

  std::string cond_id = cond_ids_[cond_idx];
  TxCondKey tx_cond_key{block, tx_hash, cond_id};
  TxTokenKey tx_token_key{block, tx_hash, token_id};

  uint8_t outcome_cnt = conditions_[cond_idx].outcome_count;
  // 非USDC的price设为0
  int64_t split_price = is_usdc_collateral(collateral) ? (1000000 / outcome_cnt) : 0;

  // ========== mint 分支 (from == 0x0) ==========
  if (from == ZERO_ADDR) {
    if (to == NEG_RISK_ADAPTER)
      return TransferClass::InternalMintNegRisk;

    auto fpmm_mint_it = fpmm_map_.find(to);
    if (fpmm_mint_it != fpmm_map_.end()) {
      TxFPMMKey tx_fpmm_key{block, tx_hash, to};
      auto fit = tx_fpmm_funding_.find(tx_fpmm_key);
      if (fit != tx_fpmm_funding_.end() && fit->second.side == 1) {
        int64_t split_amt = std::max(fit->second.amount0, fit->second.amount1);
        assert(amount == split_amt && "LP Add split amount mismatch");
        if (!is_protocol_contract(fit->second.funder)) {
          int64_t pool_amt = (token_idx == 0) ? fit->second.amount0 : fit->second.amount1;
          RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, coll, 0, pool_amt, split_price};
          push_event(fit->second.funder, evt);
        }
        return TransferClass::FPMMLPAdd;
      }
      return TransferClass::InternalMintFPMM;
    }

    auto sit = tx_split_.find(tx_cond_key);
    if (sit != tx_split_.end()) {
      for (const auto &info : sit->second) {
        if (info.stakeholder == to) {
          assert(amount == info.amount && "Split amount mismatch");
          if (!is_protocol_contract(to)) {
            RawEvent evt{sort_key, cond_idx, EventType::Split, token_idx, coll, 0, amount, split_price};
            push_event(to, evt);
          }
          return TransferClass::SplitNormal;
        }
      }
    }
    std::cerr << "[ERROR] Unmatched mint transfer: block=" << block
              << ", to=" << to << ", token_id=" << token_id
              << ", amount=" << amount << ", cond_idx=" << cond_idx << std::endl;
    assert(false && "Unmatched mint transfer");
    return TransferClass::Unclassified;
  }

  // ========== burn 分支 (to == 0x0) ==========
  if (to == ZERO_ADDR) {
    if (from == NEG_RISK_ADAPTER)
      return TransferClass::InternalBurnNegRisk;

    auto fpmm_burn_it = fpmm_map_.find(from);
    if (fpmm_burn_it != fpmm_map_.end()) {
      return TransferClass::InternalBurnFPMM;
    }

    auto mit = tx_merge_.find(tx_cond_key);
    if (mit != tx_merge_.end()) {
      for (const auto &info : mit->second) {
        if (info.stakeholder == from) {
          assert(amount == info.amount && "Merge amount mismatch");
          if (!is_protocol_contract(from)) {
            RawEvent evt{sort_key, cond_idx, EventType::Merge, token_idx, coll, 0, -amount, split_price};
            push_event(from, evt);
          }
          return TransferClass::MergeNormal;
        }
      }
    }

    auto rit = tx_redemption_.find(tx_cond_key);
    if (rit != tx_redemption_.end()) {
      for (const auto &info : rit->second) {
        if (info.redeemer == from) {
          if (!is_protocol_contract(from)) {
            auto &payouts = conditions_[cond_idx].payout_numerators;
            int64_t payout_price = is_usdc_collateral(collateral)
                                       ? ((token_idx < payouts.size() && payouts[token_idx] >= 0)
                                              ? payouts[token_idx]
                                              : 1000000)
                                       : 0;
            RawEvent evt{sort_key, cond_idx, EventType::Redemption, token_idx, coll, 0, -amount, payout_price};
            push_event(from, evt);
          }
          return TransferClass::Redemption;
        }
      }
    }
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
      assert(oit->second.tokens > 0 && "Order tokens must be positive");
      assert(amount == oit->second.tokens && "Order amount mismatch");
      if (oit->second.maker_side == 1) {
        assert(to == oit->second.maker && "Order buyer mismatch");
        assert(from == oit->second.taker && "Order seller mismatch");
      } else {
        assert(from == oit->second.maker && "Order seller mismatch");
        assert(to == oit->second.taker && "Order buyer mismatch");
      }

      int64_t price = is_usdc_collateral(collateral) ? (oit->second.usdc * 1000000 / oit->second.tokens) : 0;
      if (!is_protocol_contract(to))
        push_event(to, RawEvent{sort_key, cond_idx, EventType::Buy, token_idx, coll, 0, amount, price});
      if (!is_protocol_contract(from))
        push_event(from, RawEvent{sort_key, cond_idx, EventType::Sell, token_idx, coll, 0, -amount, price});

      if (!is_protocol_contract(to))
        return TransferClass::OrderBuy;
      else if (!is_protocol_contract(from))
        return TransferClass::OrderSell;
      else
        return TransferClass::InternalTransferOrder;
    }
    std::cerr << "[ERROR] Exchange transfer without order: block=" << block
              << ", op=" << op << ", from=" << from << ", to=" << to << std::endl;
    assert(false && "Exchange transfer without order");
    return TransferClass::Unclassified;
  }

  // ========== NegRisk Adapter operator ==========
  if (op == NEG_RISK_ADAPTER) {
    if (from == NEG_RISK_ADAPTER) {
      auto sit = tx_split_.find(tx_cond_key);
      if (sit != tx_split_.end()) {
        for (const auto &info : sit->second) {
          if (info.stakeholder == NEG_RISK_ADAPTER) {
            bool is_convert_output = false;
            if (token_idx == 0 && !conditions_[cond_idx].question_id.empty()) {
              auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
              if (market_it != cond_to_market_.end()) {
                TxMarketKey tx_market_key{block, tx_hash, market_it->second};
                is_convert_output = (tx_convert_.count(tx_market_key) > 0);
              }
            }
            if (is_convert_output) {
              assert(amount <= info.amount && "Convert YES output exceeds split");
            } else {
              assert(amount == info.amount && "NegRisk Split mismatch");
            }
            if (!is_protocol_contract(to)) {
              push_event(to, RawEvent{sort_key, cond_idx, EventType::Split, token_idx, coll, 0, amount, split_price});
            }
            return TransferClass::SplitNegRisk;
          }
        }
      }
      if (!is_protocol_contract(to))
        push_event(to, RawEvent{sort_key, cond_idx, EventType::TransferIn, token_idx, coll, 0, amount, 0});
      return TransferClass::TransferInNegRisk;
    }

    if (to == NEG_RISK_ADAPTER) {
      auto mit = tx_merge_.find(tx_cond_key);
      if (mit != tx_merge_.end()) {
        for (const auto &info : mit->second) {
          if (info.stakeholder == NEG_RISK_ADAPTER) {
            assert(amount == info.amount && "NegRisk Merge mismatch");
            if (!is_protocol_contract(from)) {
              push_event(from, RawEvent{sort_key, cond_idx, EventType::Merge, token_idx, coll, 0, -amount, split_price});
            }
            return TransferClass::MergeNegRisk;
          }
        }
      }
      if (!is_protocol_contract(from))
        push_event(from, RawEvent{sort_key, cond_idx, EventType::TransferOut, token_idx, coll, 0, -amount, 0});
      return TransferClass::TransferOutNegRisk;
    }

    if (to == NO_TOKEN_BURN_ADDRESS) {
      assert(token_idx == 1 && "Convert should only burn NO tokens");
      if (from == NEG_RISK_ADAPTER)
        return TransferClass::InternalBurnConvert;

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
                  int64_t conv_price = is_usdc_collateral(collateral) ? (1000000 * (M - 1) / M) : 0;
                  push_event(from, RawEvent{sort_key, cond_idx, EventType::Convert, token_idx, coll, 0, -amount, conv_price});
                }
                return TransferClass::Convert;
              }
            }
          }
        }
      }
      std::cerr << "[ERROR] Transfer to burn addr without convert: block=" << block
                << ", from=" << from << std::endl;
      assert(false && "Transfer to burn addr without convert");
      return TransferClass::Unclassified;
    }
    return TransferClass::InternalTransferNegRisk;
  }

  // ========== FPMM operator ==========
  auto fpmm_it = fpmm_map_.find(op);
  if (fpmm_it != fpmm_map_.end()) {
    TxFPMMKey tx_fpmm_key{block, tx_hash, op};
    auto tit = tx_fpmm_trade_.find(tx_fpmm_key);
    auto fit = tx_fpmm_funding_.find(tx_fpmm_key);

    if (tit != tx_fpmm_trade_.end()) {
      assert(amount == tit->second.tokens && "FPMM trade amount mismatch");
      assert(tit->second.outcome_idx == token_idx && "FPMM trade outcome mismatch");
      if (tit->second.side == 1) {
        assert(from == op && "FPMM buy should transfer from FPMM");
        assert(to == tit->second.trader && "FPMM buy recipient mismatch");
      } else {
        assert(to == op && "FPMM sell should transfer to FPMM");
        assert(from == tit->second.trader && "FPMM sell sender mismatch");
      }

      if (!is_protocol_contract(tit->second.trader)) {
        assert(tit->second.tokens > 0 && "FPMM trade tokens must be positive");
        int64_t price = is_usdc_collateral(collateral) ? (tit->second.usdc * 1000000 / tit->second.tokens) : 0;
        if (tit->second.side == 1) {
          push_event(tit->second.trader, RawEvent{sort_key, cond_idx, EventType::FPMMBuy, token_idx, coll, 0, amount, price});
          return TransferClass::FPMMBuy;
        } else {
          push_event(tit->second.trader, RawEvent{sort_key, cond_idx, EventType::FPMMSell, token_idx, coll, 0, -amount, price});
          return TransferClass::FPMMSell;
        }
      }
      return (tit->second.side == 1) ? TransferClass::FPMMBuy : TransferClass::FPMMSell;
    }

    if (fit != tx_fpmm_funding_.end()) {
      if (from == op && !is_protocol_contract(to)) {
        if (fit->second.side == 2) {
          int64_t expected_amt = (token_idx == 0) ? fit->second.amount0 : fit->second.amount1;
          assert(amount == expected_amt && "LP Remove amount mismatch");
          push_event(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, coll, 0, amount, split_price});
          return TransferClass::FPMMLPRemove;
        } else {
          return TransferClass::FPMMLPReturn;
        }
      }
      return TransferClass::InternalTransferFPMM;
    }

    std::cerr << "[ERROR] FPMM transfer without trade/funding: block=" << block
              << ", op=" << op << ", from=" << from << ", to=" << to << std::endl;
    assert(false && "FPMM transfer without trade/funding");
    return TransferClass::Unclassified;
  }

  // ========== 其他：用户间直接转账 ==========
  if (!is_protocol_contract(to))
    push_event(to, RawEvent{sort_key, cond_idx, EventType::TransferIn, token_idx, coll, 0, amount, 0});
  if (!is_protocol_contract(from))
    push_event(from, RawEvent{sort_key, cond_idx, EventType::TransferOut, token_idx, coll, 0, -amount, 0});

  if (!is_protocol_contract(to) && !is_protocol_contract(from))
    return TransferClass::TransferInOther;
  else if (!is_protocol_contract(to))
    return TransferClass::TransferInOther;
  else if (!is_protocol_contract(from))
    return TransferClass::TransferOutOther;
  else
    return TransferClass::InternalTransferOther;
}

} // namespace stage2
