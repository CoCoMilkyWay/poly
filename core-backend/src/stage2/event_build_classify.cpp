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

  // 对于未知 token，只做基本检查
  bool known_token = (cond_idx != UNKNOWN_COND_IDX);
  if (known_token) {
    assert(cond_idx < conditions_.size() && "Invalid cond_idx");
    assert(token_idx < 2 && "Invalid token_idx");
  }
  assert(from != to && "from and to must be different");

  uint8_t coll = static_cast<uint8_t>(collateral);
  std::string cond_id = known_token ? cond_ids_[cond_idx] : "";
  TxKey tx_key{block, tx_hash};
  TxTokenKey tx_token_key{block, tx_hash, token_id};

  uint8_t outcome_cnt = known_token ? conditions_[cond_idx].outcome_count : 2;
  int64_t split_price = (known_token && is_usdc_collateral(collateral)) ? (1000000 / outcome_cnt) : 0;

  // Lambda: 匹配 Split/Merge/Redemption 时，已知 token 需要检查 cond_id
  auto cond_matches = [&](const std::string &info_cond_id) {
    return !known_token || cond_id == info_cond_id;
  };

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

    auto sit = tx_split_.find(tx_key);
    if (sit != tx_split_.end()) {
      for (const auto &info : sit->second) {
        if (info.stakeholder == to && info.amount == amount && cond_matches(info.cond_id)) {
          if (known_token) {
            if (!is_protocol_contract(to)) {
              RawEvent evt{sort_key, cond_idx, EventType::Split, token_idx, coll, 0, amount, split_price};
              push_event(to, evt);
            }
            return TransferClass::SplitNormal;
          } else {
            return TransferClass::SplitNonPoly;
          }
        }
      }
    }

    std::cerr << "[ERROR] Unmatched mint transfer: block=" << block
              << ", to=" << to << ", token_id=" << token_id
              << ", amount=" << amount << ", cond_idx=" << cond_idx
              << ", known=" << known_token << std::endl;
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

    auto mit = tx_merge_.find(tx_key);
    if (mit != tx_merge_.end()) {
      for (const auto &info : mit->second) {
        if (info.stakeholder == from && info.amount == amount && cond_matches(info.cond_id)) {
          if (known_token) {
            if (!is_protocol_contract(from)) {
              RawEvent evt{sort_key, cond_idx, EventType::Merge, token_idx, coll, 0, -amount, split_price};
              push_event(from, evt);
            }
            return TransferClass::MergeNormal;
          } else {
            return TransferClass::MergeNonPoly;
          }
        }
      }
    }

    auto rit = tx_redemption_.find(tx_key);
    if (rit != tx_redemption_.end()) {
      for (const auto &info : rit->second) {
        if (info.redeemer == from && cond_matches(info.cond_id)) {
          if (known_token) {
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
          } else {
            return TransferClass::RedemptionNonPoly;
          }
        }
      }
    }

    std::cerr << "[ERROR] Unmatched burn transfer: block=" << block
              << ", from=" << from << ", token_id=" << token_id
              << ", amount=" << amount << ", cond_idx=" << cond_idx
              << ", known=" << known_token << std::endl;
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

    // 未知 token 的 Exchange 操作，归类为 NonPolymarket（不应该发生）
    if (!known_token)
      return TransferClass::Unclassified;

    std::cerr << "[ERROR] Exchange transfer without order: block=" << block
              << ", op=" << op << ", from=" << from << ", to=" << to << std::endl;
    assert(false && "Exchange transfer without order");
    return TransferClass::Unclassified;
  }

  // ========== NegRisk Adapter operator ==========
  if (op == NEG_RISK_ADAPTER) {
    if (from == NEG_RISK_ADAPTER) {
      auto sit = tx_split_.find(tx_key);
      if (sit != tx_split_.end()) {
        for (const auto &info : sit->second) {
          if (info.stakeholder == NEG_RISK_ADAPTER && cond_matches(info.cond_id)) {
            bool is_convert_output = false;
            if (known_token && token_idx == 0 && !conditions_[cond_idx].question_id.empty()) {
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
      auto mit = tx_merge_.find(tx_key);
      if (mit != tx_merge_.end()) {
        for (const auto &info : mit->second) {
          if (info.stakeholder == NEG_RISK_ADAPTER && cond_matches(info.cond_id)) {
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
      assert(known_token && token_idx == 1 && "Convert should only burn NO tokens");
      if (from == NEG_RISK_ADAPTER)
        return TransferClass::InternalBurnConvert;

      if (!conditions_[cond_idx].question_id.empty()) {
        auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
        if (market_it != cond_to_market_.end()) {
          TxMarketKey tx_market_key{block, tx_hash, market_it->second};
          auto cit = tx_convert_.find(tx_market_key);
          if (cit != tx_convert_.end()) {
            for (const auto &ci : cit->second) {
              if (ci.stakeholder == from && ci.amount == amount) {
                if (!is_protocol_contract(from))
                  push_event(from, RawEvent{sort_key, cond_idx, EventType::Convert, token_idx, coll, 0, -amount, 0});
                return TransferClass::Convert;
              }
            }
          }
        }
      }
      std::cerr << "[ERROR] Convert burn without event: block=" << block
                << ", from=" << from << ", amount=" << amount << std::endl;
      assert(false && "Convert burn without event");
      return TransferClass::Unclassified;
    }
  }

  // ========== FPMM operator ==========
  auto fpmm_it = fpmm_map_.find(op);
  if (fpmm_it != fpmm_map_.end()) {
    TxFPMMKey tx_fpmm_key{block, tx_hash, op};

    if (from == op) {
      auto fit = tx_fpmm_funding_.find(tx_fpmm_key);
      if (fit != tx_fpmm_funding_.end() && fit->second.side == 1) {
        assert(fit->second.amount0 != fit->second.amount1 && "LP Add should have asymmetric amounts");
        int64_t refund_amt = std::abs(fit->second.amount0 - fit->second.amount1);
        assert(amount == refund_amt && "LP Add refund amount mismatch");
        int64_t refund_idx = (fit->second.amount0 < fit->second.amount1) ? 0 : 1;
        assert(token_idx == refund_idx && "LP Add refund token mismatch");
        if (!is_protocol_contract(to)) {
          push_event(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, coll, 0, -amount, split_price});
        }
        return TransferClass::FPMMLPReturn;
      }

      auto tit = tx_fpmm_trade_.find(tx_fpmm_key);
      if (tit != tx_fpmm_trade_.end() && tit->second.side == 1) {
        assert(amount == tit->second.tokens && "FPMM Buy amount mismatch");
        int64_t price = is_usdc_collateral(collateral) ? (tit->second.usdc * 1000000 / tit->second.tokens) : 0;
        if (!is_protocol_contract(to)) {
          push_event(to, RawEvent{sort_key, cond_idx, EventType::FPMMBuy, token_idx, coll, 0, amount, price});
        }
        return TransferClass::FPMMBuy;
      }

      if (!is_protocol_contract(to)) {
        push_event(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, coll, 0, amount, split_price});
      }
      return TransferClass::FPMMLPRemove;
    }

    if (to == op) {
      auto tit = tx_fpmm_trade_.find(tx_fpmm_key);
      if (tit != tx_fpmm_trade_.end() && tit->second.side == 2) {
        assert(amount == tit->second.tokens && "FPMM Sell amount mismatch");
        int64_t price = is_usdc_collateral(collateral) ? (tit->second.usdc * 1000000 / tit->second.tokens) : 0;
        if (!is_protocol_contract(from)) {
          push_event(from, RawEvent{sort_key, cond_idx, EventType::FPMMSell, token_idx, coll, 0, -amount, price});
        }
        return TransferClass::FPMMSell;
      }
    }
  }

  // ========== 普通用户转账 ==========
  if (known_token) {
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
  } else {
    // NonPoly token 不记录 user_event，只统计
    if (!is_protocol_contract(to) && !is_protocol_contract(from))
      return TransferClass::TransferInNonPoly;
    else if (!is_protocol_contract(to))
      return TransferClass::TransferInNonPoly;
    else if (!is_protocol_contract(from))
      return TransferClass::TransferOutNonPoly;
    else
      return TransferClass::InternalTransferOther;
  }
}

} // namespace stage2
