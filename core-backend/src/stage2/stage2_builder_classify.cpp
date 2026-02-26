#include "stage2_builder.hpp"

namespace stage2 {

TransferClass EventBuilder::classify_and_emit(
    int64_t sort_key, const std::array<uint8_t, 32> &tx_hash,
    int64_t block, const std::string &op,
    const std::string &from, const std::string &to,
    const std::string &token_id, int64_t amount,
    uint32_t cond_idx, uint8_t token_idx, Collateral collateral) {

  assert_transfer(amount >= 0, "Transfer amount must be non-negative");
  if (amount == 0)
    return TransferClass::InternalTransferZero;

  // 对于未知 token，只做基本检查
  bool known_token = (cond_idx != UNKNOWN_COND_IDX);
  if (known_token) {
    assert_transfer(cond_idx < conditions_.size(), "Invalid cond_idx");
    assert_transfer(token_idx < 2, "Invalid token_idx");
  }
  assert_transfer(from != to, "from and to must be different");

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
  auto is_user_addr = [&](const std::string &addr) { return !is_protocol_contract(addr); };
  auto emit_if_user = [&](const std::string &addr, const RawEvent &evt) {
    if (is_user_addr(addr))
      push_event(addr, evt);
  };
  auto usdc_price = [&](int64_t usdc_amount, int64_t token_amount) -> int64_t {
    return is_usdc_collateral(collateral) ? (usdc_amount * 1000000 / token_amount) : 0;
  };
  auto classify_transfer_by_counterparty = [&](TransferClass in_cls, TransferClass out_cls,
                                               TransferClass internal_cls) {
    if (is_user_addr(to))
      return in_cls;
    if (is_user_addr(from))
      return out_cls;
    return internal_cls;
  };
  auto find_split_info = [&](const std::string &stakeholder, int64_t amt) -> const SplitInfo * {
    auto it = tx_split_.find(tx_key);
    if (it == tx_split_.end())
      return nullptr;
    for (const auto &info : it->second) {
      if (info.stakeholder == stakeholder && info.amount == amt && cond_matches(info.cond_id))
        return &info;
    }
    return nullptr;
  };
  auto find_merge_info = [&](const std::string &stakeholder, int64_t amt) -> const MergeInfo * {
    auto it = tx_merge_.find(tx_key);
    if (it == tx_merge_.end())
      return nullptr;
    for (const auto &info : it->second) {
      if (info.stakeholder == stakeholder && info.amount == amt && cond_matches(info.cond_id))
        return &info;
    }
    return nullptr;
  };
  auto has_redemption_info = [&](const std::string &redeemer) -> bool {
    auto it = tx_redemption_.find(tx_key);
    if (it == tx_redemption_.end())
      return false;
    for (const auto &info : it->second) {
      if (info.redeemer == redeemer && cond_matches(info.cond_id))
        return true;
    }
    return false;
  };
  auto find_fpmm_trade_info = [&](const TxFPMMKey &key, int side, int64_t token_amount) -> const FPMMTradeInfo * {
    auto it = tx_fpmm_trade_.find(key);
    if (it == tx_fpmm_trade_.end())
      return nullptr;
    for (const auto &info : it->second) {
      if (info.side == side && info.tokens == token_amount)
        return &info;
    }
    return nullptr;
  };
  auto find_fpmm_funding_info = [&](const TxFPMMKey &key, int64_t transfer_amount,
                                    bool expect_refund) -> const FPMMFundingInfo * {
    auto it = tx_fpmm_funding_.find(key);
    if (it == tx_fpmm_funding_.end())
      return nullptr;
    for (const auto &info : it->second) {
      if (info.side != 1 || info.amounts_count != 2)
        continue;
      int64_t expected_amount = expect_refund
                                    ? std::abs(info.amount0 - info.amount1)
                                    : std::max(info.amount0, info.amount1);
      if (expected_amount == transfer_amount)
        return &info;
    }
    return nullptr;
  };

  // ========== mint 分支 (from == 0x0) ==========
  if (from == ZERO_ADDR) {
    if (to == NEG_RISK_ADAPTER)
      return TransferClass::InternalMintNegRisk;

    auto fpmm_mint_it = fpmm_map_.find(to);
    if (fpmm_mint_it != fpmm_map_.end()) {
      TxFPMMKey tx_fpmm_key{block, tx_hash, to};
      const FPMMFundingInfo *fit = find_fpmm_funding_info(tx_fpmm_key, amount, false);
      if (fit != nullptr && known_token) {
        int64_t split_amt = std::max(fit->amount0, fit->amount1);
        assert_transfer(amount == split_amt, "LP Add split amount mismatch");
        if (is_user_addr(fit->funder)) {
          int64_t pool_amt = (token_idx == 0) ? fit->amount0 : fit->amount1;
          RawEvent evt{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, coll, 0, pool_amt, split_price};
          push_event(fit->funder, evt);
        }
        return TransferClass::FPMMLPAdd;
      }
      return TransferClass::InternalMintFPMM;
    }

    if (find_split_info(to, amount) != nullptr) {
      if (known_token) {
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::Split, token_idx, coll, 0, amount, split_price});
        return TransferClass::SplitNormal;
      } else {
        return TransferClass::SplitNonPoly;
      }
    }

    std::cerr << "[ERROR] Unmatched mint transfer: block=" << block
              << ", to=" << to << ", token_id=" << token_id
              << ", amount=" << amount << ", cond_idx=" << cond_idx
              << ", known=" << known_token << std::endl;
    fail_transfer_assert("Unmatched mint transfer");
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

    if (find_merge_info(from, amount) != nullptr) {
      if (known_token) {
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Merge, token_idx, coll, 0, -amount, split_price});
        return TransferClass::MergeNormal;
      } else {
        return TransferClass::MergeNonPoly;
      }
    }

    if (has_redemption_info(from)) {
      if (known_token) {
        if (is_user_addr(from)) {
          auto &payouts = conditions_[cond_idx].payout_numerators;
          int64_t payout_price = is_usdc_collateral(collateral)
                                     ? ((token_idx < payouts.size() && payouts[token_idx] >= 0)
                                            ? payouts[token_idx]
                                            : 1000000)
                                     : 0;
          emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Redemption, token_idx, coll, 0, -amount, payout_price});
        }
        return TransferClass::Redemption;
      } else {
        return TransferClass::RedemptionNonPoly;
      }
    }

    std::cerr << "[ERROR] Unmatched burn transfer: block=" << block
              << ", from=" << from << ", token_id=" << token_id
              << ", amount=" << amount << ", cond_idx=" << cond_idx
              << ", known=" << known_token << std::endl;
    fail_transfer_assert("Unmatched burn transfer");
    return TransferClass::Unclassified;
  }

  // ========== Exchange operator ==========
  if (op == CTF_EXCHANGE || op == NEG_RISK_CTF_EXCHANGE) {
    auto oit = tx_order_.find(tx_token_key);
    if (oit != tx_order_.end()) {
      assert_transfer(oit->second.tokens > 0, "Order tokens must be positive");
      assert_transfer(amount == oit->second.tokens, "Order amount mismatch");
      if (oit->second.maker_side == 1) {
        assert_transfer(to == oit->second.maker, "Order buyer mismatch");
        assert_transfer(from == oit->second.taker, "Order seller mismatch");
      } else {
        assert_transfer(from == oit->second.maker, "Order seller mismatch");
        assert_transfer(to == oit->second.taker, "Order buyer mismatch");
      }

      int64_t price = usdc_price(oit->second.usdc, oit->second.tokens);
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::Buy, token_idx, coll, 0, amount, price});
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Sell, token_idx, coll, 0, -amount, price});
      return classify_transfer_by_counterparty(TransferClass::OrderBuy, TransferClass::OrderSell,
                                               TransferClass::InternalTransferOrder);
    }

    // 未知 token 的 Exchange 操作，归类为 NonPolymarket（不应该发生）
    if (!known_token)
      return TransferClass::Unclassified;

    std::cerr << "[ERROR] Exchange transfer without order: block=" << block
              << ", op=" << op << ", from=" << from << ", to=" << to << std::endl;
    fail_transfer_assert("Exchange transfer without order");
    return TransferClass::Unclassified;
  }

  // ========== NegRisk Adapter operator ==========
  if (op == NEG_RISK_ADAPTER) {
    if (from == NEG_RISK_ADAPTER) {
      const SplitInfo *split_info = find_split_info(NEG_RISK_ADAPTER, amount);
      if (split_info == nullptr) {
        auto sit = tx_split_.find(tx_key);
        if (sit != tx_split_.end()) {
          for (const auto &info : sit->second) {
            if (info.stakeholder == NEG_RISK_ADAPTER && cond_matches(info.cond_id)) {
              split_info = &info;
              break;
            }
          }
        }
      }
      if (split_info != nullptr) {
        bool is_convert_output = false;
        if (known_token && token_idx == 0 && !conditions_[cond_idx].question_id.empty()) {
          auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
          if (market_it != cond_to_market_.end()) {
            TxMarketKey tx_market_key{block, tx_hash, market_it->second};
            is_convert_output = (tx_convert_.count(tx_market_key) > 0);
          }
        }
        if (is_convert_output) {
          assert_transfer(amount <= split_info->amount, "Convert YES output exceeds split");
        } else {
          assert_transfer(amount == split_info->amount, "NegRisk Split mismatch");
        }
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::Split, token_idx, coll, 0, amount, split_price});
        return TransferClass::SplitNegRisk;
      }
      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferIn, token_idx, coll, 0, amount, 0});
      return TransferClass::TransferInNegRisk;
    }

    if (to == NEG_RISK_ADAPTER) {
      if (const MergeInfo *merge_info = find_merge_info(NEG_RISK_ADAPTER, amount); merge_info != nullptr) {
        (void)merge_info;
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Merge, token_idx, coll, 0, -amount, split_price});
        return TransferClass::MergeNegRisk;
      }
      emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOut, token_idx, coll, 0, -amount, 0});
      return TransferClass::TransferOutNegRisk;
    }

    if (to == NO_TOKEN_BURN_ADDRESS) {
      if (from == NEG_RISK_ADAPTER)
        return TransferClass::InternalBurnConvert;

      assert_transfer(known_token && token_idx == 1, "Convert should only burn NO tokens");
      if (!conditions_[cond_idx].question_id.empty()) {
        auto market_it = cond_to_market_.find(conditions_[cond_idx].question_id);
        if (market_it != cond_to_market_.end()) {
          TxMarketKey tx_market_key{block, tx_hash, market_it->second};
          auto cit = tx_convert_.find(tx_market_key);
          if (cit != tx_convert_.end()) {
            for (const auto &ci : cit->second) {
              if (ci.stakeholder == from && ci.amount == amount) {
                emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::Convert, token_idx, coll, 0, -amount, 0});
                return TransferClass::Convert;
              }
            }
          }
        }
      }
      std::cerr << "[ERROR] Convert burn without event: block=" << block
                << ", from=" << from << ", amount=" << amount << std::endl;
      fail_transfer_assert("Convert burn without event");
      return TransferClass::Unclassified;
    }
    fail_transfer_assert("Unexpected NegRisk adapter transfer pattern");
    return TransferClass::Unclassified;
  }

  // ========== FPMM operator ==========
  auto fpmm_it = fpmm_map_.find(op);
  if (fpmm_it != fpmm_map_.end()) {
    TxFPMMKey tx_fpmm_key{block, tx_hash, op};

    if (from == op) {
      const FPMMFundingInfo *fit = find_fpmm_funding_info(tx_fpmm_key, amount, true);
      if (fit != nullptr && known_token) {
        assert_transfer(fit->amount0 != fit->amount1, "LP Add should have asymmetric amounts");
        int64_t refund_amt = std::abs(fit->amount0 - fit->amount1);
        assert_transfer(amount == refund_amt, "LP Add refund amount mismatch");
        int64_t refund_idx = (fit->amount0 < fit->amount1) ? 0 : 1;
        if (token_idx == refund_idx) {
          emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPAdd, token_idx, coll, 0, -amount, split_price});
          return TransferClass::FPMMLPReturn;
        }
        // 多 outcome / 组合 token 无法用二元 token_idx 映射，降级到后续通用分支
      }

      const FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 1, amount);
      if (tit != nullptr) {
        assert_transfer(amount == tit->tokens, "FPMM Buy amount mismatch");
        int64_t price = usdc_price(tit->usdc, tit->tokens);
        emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMBuy, token_idx, coll, 0, amount, price});
        return TransferClass::FPMMBuy;
      }

      emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::FPMMLPRemove, token_idx, coll, 0, amount, split_price});
      return TransferClass::FPMMLPRemove;
    }

    if (to == op) {
      const FPMMTradeInfo *tit = find_fpmm_trade_info(tx_fpmm_key, 2, amount);
      if (tit != nullptr) {
        assert_transfer(amount == tit->tokens, "FPMM Sell amount mismatch");
        int64_t price = usdc_price(tit->usdc, tit->tokens);
        emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::FPMMSell, token_idx, coll, 0, -amount, price});
        return TransferClass::FPMMSell;
      }
    }
    fail_transfer_assert("Unexpected FPMM transfer pattern");
    return TransferClass::Unclassified;
  }

  // ========== 普通用户转账 ==========
  if (known_token) {
    emit_if_user(to, RawEvent{sort_key, cond_idx, EventType::TransferIn, token_idx, coll, 0, amount, 0});
    emit_if_user(from, RawEvent{sort_key, cond_idx, EventType::TransferOut, token_idx, coll, 0, -amount, 0});
    return classify_transfer_by_counterparty(TransferClass::TransferInOther,
                                             TransferClass::TransferOutOther,
                                             TransferClass::InternalTransferOther);
  } else {
    // NonPoly token 不记录 user_event，只统计
    return classify_transfer_by_counterparty(TransferClass::TransferInNonPoly,
                                             TransferClass::TransferOutNonPoly,
                                             TransferClass::InternalTransferOther);
  }
}

} // namespace stage2
