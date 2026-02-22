#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <cassert>
#include <filesystem>
#include <string>
#include <vector>

#include "../stage1/event_decode.hpp"

namespace fs = std::filesystem;

class FeatherWriter {
public:
  explicit FeatherWriter(const std::string &data_dir) : data_dir_(data_dir) {
    stage1_dir_ = data_dir + "/stage1";
    fs::create_directories(stage1_dir_);
  }

  void append_events(const stage1::DecodedEvents &events) {
    if (!events.transfer.empty())
      append_transfer(events.transfer);
    if (!events.condition_preparation.empty())
      append_condition_preparation(events.condition_preparation);
    if (!events.condition_resolution.empty())
      append_condition_resolution(events.condition_resolution);
    if (!events.split.empty())
      append_split(events.split);
    if (!events.merge.empty())
      append_merge(events.merge);
    if (!events.redemption.empty())
      append_redemption(events.redemption);
    if (!events.fpmm.empty())
      append_fpmm(events.fpmm);
    if (!events.fpmm_trade.empty())
      append_fpmm_trade(events.fpmm_trade);
    if (!events.fpmm_funding.empty())
      append_fpmm_funding(events.fpmm_funding);
    if (!events.order_filled.empty())
      append_order_filled(events.order_filled);
    if (!events.token_map.empty())
      append_token_map(events.token_map);
    if (!events.neg_risk_market.empty())
      append_neg_risk_market(events.neg_risk_market);
    if (!events.neg_risk_question.empty())
      append_neg_risk_question(events.neg_risk_question);
    if (!events.convert.empty())
      append_convert(events.convert);
  }

private:
  std::string data_dir_;
  std::string stage1_dir_;

  static std::string hex_to_bytes(const std::string &hex) {
    std::string h = hex;
    if (h.size() >= 2 && h[0] == '0' && (h[1] == 'x' || h[1] == 'X'))
      h = h.substr(2);
    std::string bytes;
    bytes.reserve(h.size() / 2);
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
      unsigned char c = 0;
      for (int j = 0; j < 2; ++j) {
        char ch = h[i + j];
        c <<= 4;
        if (ch >= '0' && ch <= '9')
          c |= ch - '0';
        else if (ch >= 'a' && ch <= 'f')
          c |= ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
          c |= ch - 'A' + 10;
      }
      bytes.push_back(static_cast<char>(c));
    }
    return bytes;
  }

  void write_feather(const std::string &path, std::shared_ptr<arrow::Table> table) {
    auto outfile = arrow::io::FileOutputStream::Open(path);
    assert(outfile.ok());
    auto writer = arrow::ipc::MakeFileWriter(outfile->get(), table->schema());
    assert(writer.ok());
    auto reader = arrow::TableBatchReader(*table);
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      auto status = reader.ReadNext(&batch);
      assert(status.ok());
      if (!batch)
        break;
      auto write_status = (*writer)->WriteRecordBatch(*batch);
      assert(write_status.ok());
    }
    auto close_status = (*writer)->Close();
    assert(close_status.ok());
  }

  std::shared_ptr<arrow::Table> read_feather(const std::string &path) {
    if (!fs::exists(path))
      return nullptr;
    auto infile = arrow::io::ReadableFile::Open(path);
    assert(infile.ok());
    auto reader = arrow::ipc::RecordBatchFileReader::Open(infile->get());
    assert(reader.ok());
    std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
    for (int i = 0; i < (*reader)->num_record_batches(); ++i) {
      auto batch = (*reader)->ReadRecordBatch(i);
      assert(batch.ok());
      batches.push_back(*batch);
    }
    if (batches.empty())
      return nullptr;
    auto table = arrow::Table::FromRecordBatches(batches);
    assert(table.ok());
    return *table;
  }

  std::shared_ptr<arrow::Table> concat_tables(std::shared_ptr<arrow::Table> existing,
                                              std::shared_ptr<arrow::Table> new_data) {
    if (!existing)
      return new_data;
    auto result = arrow::ConcatenateTables({existing, new_data});
    assert(result.ok());
    return *result;
  }

  void append_transfer(const std::vector<stage1::TransferEvent> &events) {
    arrow::Int64Builder block_number, log_index, amount;
    arrow::BinaryBuilder tx_hash, op, from, to, token_id;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(op.Append(hex_to_bytes(e.op)).ok());
      assert(from.Append(hex_to_bytes(e.from)).ok());
      assert(to.Append(hex_to_bytes(e.to)).ok());
      assert(token_id.Append(hex_to_bytes(e.token_id)).ok());
      assert(amount.Append(e.amount).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("operator", arrow::binary()),
        arrow::field("from_addr", arrow::binary()),
        arrow::field("to_addr", arrow::binary()),
        arrow::field("token_id", arrow::binary()),
        arrow::field("amount", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> arr_block, arr_tx, arr_log, arr_op, arr_from, arr_to, arr_token, arr_amt;
    assert(block_number.Finish(&arr_block).ok());
    assert(tx_hash.Finish(&arr_tx).ok());
    assert(log_index.Finish(&arr_log).ok());
    assert(op.Finish(&arr_op).ok());
    assert(from.Finish(&arr_from).ok());
    assert(to.Finish(&arr_to).ok());
    assert(token_id.Finish(&arr_token).ok());
    assert(amount.Finish(&arr_amt).ok());

    auto new_table = arrow::Table::Make(schema, {arr_block, arr_tx, arr_log, arr_op, arr_from, arr_to, arr_token, arr_amt});
    std::string path = stage1_dir_ + "/transfer.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_condition_preparation(const std::vector<stage1::ConditionPrepEvent> &events) {
    arrow::Int64Builder block_number, log_index, outcome_slot_count;
    arrow::BinaryBuilder tx_hash, condition_id, oracle, question_id;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(condition_id.Append(hex_to_bytes(e.condition_id)).ok());
      assert(oracle.Append(hex_to_bytes(e.oracle)).ok());
      assert(question_id.Append(hex_to_bytes(e.question_id)).ok());
      assert(outcome_slot_count.Append(e.outcome_slot_count).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("condition_id", arrow::binary()),
        arrow::field("oracle", arrow::binary()),
        arrow::field("question_id", arrow::binary()),
        arrow::field("outcome_slot_count", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(condition_id.Finish(&a4).ok());
    assert(oracle.Finish(&a5).ok());
    assert(question_id.Finish(&a6).ok());
    assert(outcome_slot_count.Finish(&a7).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    std::string path = stage1_dir_ + "/condition_preparation.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_condition_resolution(const std::vector<stage1::ConditionResolveEvent> &events) {
    arrow::Int64Builder block_number, log_index, outcome_slot_count;
    arrow::BinaryBuilder tx_hash, condition_id, oracle, question_id;
    arrow::StringBuilder payout_numerators;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(condition_id.Append(hex_to_bytes(e.condition_id)).ok());
      assert(oracle.Append(hex_to_bytes(e.oracle)).ok());
      assert(question_id.Append(hex_to_bytes(e.question_id)).ok());
      assert(outcome_slot_count.Append(e.outcome_slot_count).ok());
      assert(payout_numerators.Append(e.payout_numerators).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("condition_id", arrow::binary()),
        arrow::field("oracle", arrow::binary()),
        arrow::field("question_id", arrow::binary()),
        arrow::field("outcome_slot_count", arrow::int64()),
        arrow::field("payout_numerators", arrow::utf8()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(condition_id.Finish(&a4).ok());
    assert(oracle.Finish(&a5).ok());
    assert(question_id.Finish(&a6).ok());
    assert(outcome_slot_count.Finish(&a7).ok());
    assert(payout_numerators.Finish(&a8).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8});
    std::string path = stage1_dir_ + "/condition_resolution.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_split_merge_impl(const std::vector<stage1::SplitMergeEvent> &events, const std::string &table_name) {
    arrow::Int64Builder block_number, log_index, amount;
    arrow::BinaryBuilder tx_hash, stakeholder, collateral_token, parent_collection_id, condition_id;
    arrow::StringBuilder partition;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(stakeholder.Append(hex_to_bytes(e.stakeholder)).ok());
      assert(collateral_token.Append(hex_to_bytes(e.collateral_token)).ok());
      assert(parent_collection_id.Append(hex_to_bytes(e.parent_collection_id)).ok());
      assert(condition_id.Append(hex_to_bytes(e.condition_id)).ok());
      assert(partition.Append(e.partition).ok());
      assert(amount.Append(e.amount).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("stakeholder", arrow::binary()),
        arrow::field("collateral_token", arrow::binary()),
        arrow::field("parent_collection_id", arrow::binary()),
        arrow::field("condition_id", arrow::binary()),
        arrow::field("partition", arrow::utf8()),
        arrow::field("amount", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(stakeholder.Finish(&a4).ok());
    assert(collateral_token.Finish(&a5).ok());
    assert(parent_collection_id.Finish(&a6).ok());
    assert(condition_id.Finish(&a7).ok());
    assert(partition.Finish(&a8).ok());
    assert(amount.Finish(&a9).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9});
    std::string path = stage1_dir_ + "/" + table_name + ".feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_split(const std::vector<stage1::SplitMergeEvent> &events) {
    append_split_merge_impl(events, "split");
  }

  void append_merge(const std::vector<stage1::SplitMergeEvent> &events) {
    append_split_merge_impl(events, "merge");
  }

  void append_redemption(const std::vector<stage1::RedemptionEvent> &events) {
    arrow::Int64Builder block_number, log_index, payout;
    arrow::BinaryBuilder tx_hash, redeemer, collateral_token, parent_collection_id, condition_id;
    arrow::StringBuilder index_sets;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(redeemer.Append(hex_to_bytes(e.redeemer)).ok());
      assert(collateral_token.Append(hex_to_bytes(e.collateral_token)).ok());
      assert(parent_collection_id.Append(hex_to_bytes(e.parent_collection_id)).ok());
      assert(condition_id.Append(hex_to_bytes(e.condition_id)).ok());
      assert(index_sets.Append(e.index_sets).ok());
      assert(payout.Append(e.payout).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("redeemer", arrow::binary()),
        arrow::field("collateral_token", arrow::binary()),
        arrow::field("parent_collection_id", arrow::binary()),
        arrow::field("condition_id", arrow::binary()),
        arrow::field("index_sets", arrow::utf8()),
        arrow::field("payout", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(redeemer.Finish(&a4).ok());
    assert(collateral_token.Finish(&a5).ok());
    assert(parent_collection_id.Finish(&a6).ok());
    assert(condition_id.Finish(&a7).ok());
    assert(index_sets.Finish(&a8).ok());
    assert(payout.Finish(&a9).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9});
    std::string path = stage1_dir_ + "/redemption.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_fpmm(const std::vector<stage1::FpmmEvent> &events) {
    arrow::Int64Builder block_number, log_index, fee;
    arrow::BinaryBuilder tx_hash, creator, fpmm_addr, conditional_tokens, collateral_token;
    arrow::StringBuilder condition_ids;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(creator.Append(hex_to_bytes(e.creator)).ok());
      assert(fpmm_addr.Append(hex_to_bytes(e.fpmm_addr)).ok());
      assert(conditional_tokens.Append(hex_to_bytes(e.conditional_tokens)).ok());
      assert(collateral_token.Append(hex_to_bytes(e.collateral_token)).ok());
      assert(condition_ids.Append(e.condition_ids).ok());
      assert(fee.Append(e.fee).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("creator", arrow::binary()),
        arrow::field("fpmm_addr", arrow::binary()),
        arrow::field("conditional_tokens", arrow::binary()),
        arrow::field("collateral_token", arrow::binary()),
        arrow::field("condition_ids", arrow::utf8()),
        arrow::field("fee", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(creator.Finish(&a4).ok());
    assert(fpmm_addr.Finish(&a5).ok());
    assert(conditional_tokens.Finish(&a6).ok());
    assert(collateral_token.Finish(&a7).ok());
    assert(condition_ids.Finish(&a8).ok());
    assert(fee.Finish(&a9).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9});
    std::string path = stage1_dir_ + "/fpmm.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_fpmm_trade(const std::vector<stage1::FpmmTradeEvent> &events) {
    arrow::Int64Builder block_number, log_index, side, outcome_index, usdc_amount, token_amount, fee;
    arrow::BinaryBuilder tx_hash, fpmm_addr, trader;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(fpmm_addr.Append(hex_to_bytes(e.fpmm_addr)).ok());
      assert(trader.Append(hex_to_bytes(e.trader)).ok());
      assert(side.Append(e.side).ok());
      assert(outcome_index.Append(e.outcome_index).ok());
      assert(usdc_amount.Append(e.usdc_amount).ok());
      assert(token_amount.Append(e.token_amount).ok());
      assert(fee.Append(e.fee).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("fpmm_addr", arrow::binary()),
        arrow::field("trader", arrow::binary()),
        arrow::field("side", arrow::int64()),
        arrow::field("outcome_index", arrow::int64()),
        arrow::field("usdc_amount", arrow::int64()),
        arrow::field("token_amount", arrow::int64()),
        arrow::field("fee", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9, a10;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(fpmm_addr.Finish(&a4).ok());
    assert(trader.Finish(&a5).ok());
    assert(side.Finish(&a6).ok());
    assert(outcome_index.Finish(&a7).ok());
    assert(usdc_amount.Finish(&a8).ok());
    assert(token_amount.Finish(&a9).ok());
    assert(fee.Finish(&a10).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10});
    std::string path = stage1_dir_ + "/fpmm_trade.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_fpmm_funding(const std::vector<stage1::FpmmFundingEvent> &events) {
    arrow::Int64Builder block_number, log_index, side, collateral_from_fee_pool, shares;
    arrow::BinaryBuilder tx_hash, fpmm_addr, funder;
    arrow::StringBuilder amounts;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(fpmm_addr.Append(hex_to_bytes(e.fpmm_addr)).ok());
      assert(funder.Append(hex_to_bytes(e.funder)).ok());
      assert(side.Append(e.side).ok());
      assert(amounts.Append(e.amounts).ok());
      assert(collateral_from_fee_pool.Append(e.collateral_from_fee_pool).ok());
      assert(shares.Append(e.shares).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("fpmm_addr", arrow::binary()),
        arrow::field("funder", arrow::binary()),
        arrow::field("side", arrow::int64()),
        arrow::field("amounts", arrow::utf8()),
        arrow::field("collateral_from_fee_pool", arrow::int64()),
        arrow::field("shares", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(fpmm_addr.Finish(&a4).ok());
    assert(funder.Finish(&a5).ok());
    assert(side.Finish(&a6).ok());
    assert(amounts.Finish(&a7).ok());
    assert(collateral_from_fee_pool.Finish(&a8).ok());
    assert(shares.Finish(&a9).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9});
    std::string path = stage1_dir_ + "/fpmm_funding.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_order_filled(const std::vector<stage1::OrderFilledEvent> &events) {
    arrow::Int64Builder block_number, log_index, maker_amount, taker_amount, fee;
    arrow::BinaryBuilder tx_hash, order_hash, maker, taker, maker_asset_id, taker_asset_id;
    arrow::StringBuilder exchange;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(exchange.Append(e.exchange).ok());
      assert(order_hash.Append(hex_to_bytes(e.order_hash)).ok());
      assert(maker.Append(hex_to_bytes(e.maker)).ok());
      assert(taker.Append(hex_to_bytes(e.taker)).ok());
      assert(maker_asset_id.Append(hex_to_bytes(e.maker_asset_id)).ok());
      assert(taker_asset_id.Append(hex_to_bytes(e.taker_asset_id)).ok());
      assert(maker_amount.Append(e.maker_amount).ok());
      assert(taker_amount.Append(e.taker_amount).ok());
      assert(fee.Append(e.fee).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("exchange", arrow::utf8()),
        arrow::field("order_hash", arrow::binary()),
        arrow::field("maker", arrow::binary()),
        arrow::field("taker", arrow::binary()),
        arrow::field("maker_asset_id", arrow::binary()),
        arrow::field("taker_asset_id", arrow::binary()),
        arrow::field("maker_amount", arrow::int64()),
        arrow::field("taker_amount", arrow::int64()),
        arrow::field("fee", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(exchange.Finish(&a4).ok());
    assert(order_hash.Finish(&a5).ok());
    assert(maker.Finish(&a6).ok());
    assert(taker.Finish(&a7).ok());
    assert(maker_asset_id.Finish(&a8).ok());
    assert(taker_asset_id.Finish(&a9).ok());
    assert(maker_amount.Finish(&a10).ok());
    assert(taker_amount.Finish(&a11).ok());
    assert(fee.Finish(&a12).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12});
    std::string path = stage1_dir_ + "/order_filled.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_token_map(const std::vector<stage1::TokenMapEvent> &events) {
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, token0, token1, condition_id;
    arrow::StringBuilder exchange;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(exchange.Append(e.exchange).ok());
      assert(token0.Append(hex_to_bytes(e.token0)).ok());
      assert(token1.Append(hex_to_bytes(e.token1)).ok());
      assert(condition_id.Append(hex_to_bytes(e.condition_id)).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("exchange", arrow::utf8()),
        arrow::field("token0", arrow::binary()),
        arrow::field("token1", arrow::binary()),
        arrow::field("condition_id", arrow::binary()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(exchange.Finish(&a4).ok());
    assert(token0.Finish(&a5).ok());
    assert(token1.Finish(&a6).ok());
    assert(condition_id.Finish(&a7).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    std::string path = stage1_dir_ + "/token_map.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_neg_risk_market(const std::vector<stage1::NegRiskMarketEvent> &events) {
    arrow::Int64Builder block_number, log_index, fee_bips;
    arrow::BinaryBuilder tx_hash, market_id, oracle, data;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(market_id.Append(hex_to_bytes(e.market_id)).ok());
      assert(oracle.Append(hex_to_bytes(e.oracle)).ok());
      assert(fee_bips.Append(e.fee_bips).ok());
      if (e.data) {
        assert(data.Append(hex_to_bytes(*e.data)).ok());
      } else {
        assert(data.AppendNull().ok());
      }
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("market_id", arrow::binary()),
        arrow::field("oracle", arrow::binary()),
        arrow::field("fee_bips", arrow::int64()),
        arrow::field("data", arrow::binary()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(market_id.Finish(&a4).ok());
    assert(oracle.Finish(&a5).ok());
    assert(fee_bips.Finish(&a6).ok());
    assert(data.Finish(&a7).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    std::string path = stage1_dir_ + "/neg_risk_market.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_neg_risk_question(const std::vector<stage1::NegRiskQuestionEvent> &events) {
    arrow::Int64Builder block_number, log_index, question_index;
    arrow::BinaryBuilder tx_hash, market_id, question_id, data;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(market_id.Append(hex_to_bytes(e.market_id)).ok());
      assert(question_id.Append(hex_to_bytes(e.question_id)).ok());
      assert(question_index.Append(e.question_index).ok());
      if (e.data) {
        assert(data.Append(hex_to_bytes(*e.data)).ok());
      } else {
        assert(data.AppendNull().ok());
      }
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("market_id", arrow::binary()),
        arrow::field("question_id", arrow::binary()),
        arrow::field("question_index", arrow::int64()),
        arrow::field("data", arrow::binary()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(market_id.Finish(&a4).ok());
    assert(question_id.Finish(&a5).ok());
    assert(question_index.Finish(&a6).ok());
    assert(data.Finish(&a7).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    std::string path = stage1_dir_ + "/neg_risk_question.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }

  void append_convert(const std::vector<stage1::ConvertEvent> &events) {
    arrow::Int64Builder block_number, log_index, index_set, amount;
    arrow::BinaryBuilder tx_hash, stakeholder, market_id;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(stakeholder.Append(hex_to_bytes(e.stakeholder)).ok());
      assert(market_id.Append(hex_to_bytes(e.market_id)).ok());
      assert(index_set.Append(e.index_set).ok());
      assert(amount.Append(e.amount).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("stakeholder", arrow::binary()),
        arrow::field("market_id", arrow::binary()),
        arrow::field("index_set", arrow::int64()),
        arrow::field("amount", arrow::int64()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(stakeholder.Finish(&a4).ok());
    assert(market_id.Finish(&a5).ok());
    assert(index_set.Finish(&a6).ok());
    assert(amount.Finish(&a7).ok());

    auto new_table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    std::string path = stage1_dir_ + "/convert.feather";
    auto existing = read_feather(path);
    auto merged = concat_tables(existing, new_table);
    write_feather(path, merged);
  }
};
