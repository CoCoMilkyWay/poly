#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "../stage1/event_decode.hpp"

namespace fs = std::filesystem;

class FeatherWriter {
public:
  static constexpr int64_t PARTITION_SIZE = 100000;

  explicit FeatherWriter(const std::string &data_dir) : stage1_dir_(data_dir) {}

  void write_partition(int64_t start_block, const stage1::DecodedEvents &events) {
    write_transfer(start_block, events.transfer);
    write_condition_preparation(start_block, events.condition_preparation);
    write_condition_resolution(start_block, events.condition_resolution);
    write_split(start_block, events.split);
    write_merge(start_block, events.merge);
    write_redemption(start_block, events.redemption);
    write_fpmm(start_block, events.fpmm);
    write_fpmm_trade(start_block, events.fpmm_trade);
    write_fpmm_funding(start_block, events.fpmm_funding);
    write_order_filled(start_block, events.order_filled);
    write_token_map(start_block, events.token_map);
    write_neg_risk_market(start_block, events.neg_risk_market);
    write_neg_risk_question(start_block, events.neg_risk_question);
    write_convert(start_block, events.convert);
  }

  static int64_t partition_start(int64_t block) { return (block / PARTITION_SIZE) * PARTITION_SIZE; }
  static int64_t partition_end(int64_t block) { return partition_start(block) + PARTITION_SIZE - 1; }

private:
  std::string stage1_dir_;

  static void fsync_file(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    assert(fd >= 0);
    int ret = fsync(fd);
    assert(ret == 0);
    int close_ret = close(fd);
    assert(close_ret == 0);
  }

  static void fsync_dir(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    int ret = fsync(fd);
    assert(ret == 0);
    int close_ret = close(fd);
    assert(close_ret == 0);
  }

  std::string table_dir(const std::string &table) {
    std::string dir = stage1_dir_ + "/" + table;
    fs::create_directories(dir);
    return dir;
  }

  std::string partition_path(const std::string &table, int64_t start_block) {
    return table_dir(table) + "/" + std::to_string(start_block) + ".feather";
  }

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

  static std::string uint256_hex_to_bytes32(const std::string &hex) {
    std::string b = hex_to_bytes(hex);
    assert(b.size() == 32);
    return b;
  }

  void atomic_write_feather(const std::string &path, std::shared_ptr<arrow::Table> table) {
    std::string tmp_path = path + ".tmp";
    {
      auto outfile = arrow::io::FileOutputStream::Open(tmp_path);
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
      auto file_close_status = (*outfile)->Close();
      assert(file_close_status.ok());
    }
    fsync_file(tmp_path);
    int ret = std::rename(tmp_path.c_str(), path.c_str());
    assert(ret == 0);
    std::string dir = fs::path(path).parent_path().string();
    if (dir.empty())
      dir = ".";
    fsync_dir(dir);
  }

  void write_transfer(int64_t start_block, const std::vector<stage1::TransferEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, op, from, to, token_id, amount;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(op.Append(hex_to_bytes(e.op)).ok());
      assert(from.Append(hex_to_bytes(e.from)).ok());
      assert(to.Append(hex_to_bytes(e.to)).ok());
      assert(token_id.Append(hex_to_bytes(e.token_id)).ok());
      assert(amount.Append(uint256_hex_to_bytes32(e.amount)).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("operator", arrow::binary()),
        arrow::field("from_addr", arrow::binary()),
        arrow::field("to_addr", arrow::binary()),
        arrow::field("token_id", arrow::binary()),
        arrow::field("amount", arrow::binary()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(op.Finish(&a4).ok());
    assert(from.Finish(&a5).ok());
    assert(to.Finish(&a6).ok());
    assert(token_id.Finish(&a7).ok());
    assert(amount.Finish(&a8).ok());

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8});
    atomic_write_feather(partition_path("transfer", start_block), table);
  }

  void write_condition_preparation(int64_t start_block, const std::vector<stage1::ConditionPrepEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, condition_id, oracle, question_id, outcome_slot_count;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(condition_id.Append(hex_to_bytes(e.condition_id)).ok());
      assert(oracle.Append(hex_to_bytes(e.oracle)).ok());
      assert(question_id.Append(hex_to_bytes(e.question_id)).ok());
      assert(outcome_slot_count.Append(uint256_hex_to_bytes32(e.outcome_slot_count)).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("condition_id", arrow::binary()),
        arrow::field("oracle", arrow::binary()),
        arrow::field("question_id", arrow::binary()),
        arrow::field("outcome_slot_count", arrow::binary()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(condition_id.Finish(&a4).ok());
    assert(oracle.Finish(&a5).ok());
    assert(question_id.Finish(&a6).ok());
    assert(outcome_slot_count.Finish(&a7).ok());

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    atomic_write_feather(partition_path("condition_preparation", start_block), table);
  }

  void write_condition_resolution(int64_t start_block, const std::vector<stage1::ConditionResolveEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, condition_id, oracle, question_id, outcome_slot_count;
    arrow::StringBuilder payout_numerators;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(condition_id.Append(hex_to_bytes(e.condition_id)).ok());
      assert(oracle.Append(hex_to_bytes(e.oracle)).ok());
      assert(question_id.Append(hex_to_bytes(e.question_id)).ok());
      assert(outcome_slot_count.Append(uint256_hex_to_bytes32(e.outcome_slot_count)).ok());
      assert(payout_numerators.Append(e.payout_numerators).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("condition_id", arrow::binary()),
        arrow::field("oracle", arrow::binary()),
        arrow::field("question_id", arrow::binary()),
        arrow::field("outcome_slot_count", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8});
    atomic_write_feather(partition_path("condition_resolution", start_block), table);
  }

  void write_split_merge_impl(int64_t start_block, const std::vector<stage1::SplitMergeEvent> &events, const std::string &table_name) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, stakeholder, collateral_token, parent_collection_id, condition_id, amount;
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
      assert(amount.Append(uint256_hex_to_bytes32(e.amount)).ok());
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
        arrow::field("amount", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9});
    atomic_write_feather(partition_path(table_name, start_block), table);
  }

  void write_split(int64_t start_block, const std::vector<stage1::SplitMergeEvent> &events) {
    write_split_merge_impl(start_block, events, "split");
  }

  void write_merge(int64_t start_block, const std::vector<stage1::SplitMergeEvent> &events) {
    write_split_merge_impl(start_block, events, "merge");
  }

  void write_redemption(int64_t start_block, const std::vector<stage1::RedemptionEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, redeemer, collateral_token, parent_collection_id, condition_id, payout;
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
      assert(payout.Append(uint256_hex_to_bytes32(e.payout)).ok());
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
        arrow::field("payout", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9});
    atomic_write_feather(partition_path("redemption", start_block), table);
  }

  void write_fpmm(int64_t start_block, const std::vector<stage1::FpmmEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, factory, creator, fpmm_addr, conditional_tokens, collateral_token, fee;
    arrow::StringBuilder condition_ids;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(factory.Append(hex_to_bytes(e.factory)).ok());
      assert(creator.Append(hex_to_bytes(e.creator)).ok());
      assert(fpmm_addr.Append(hex_to_bytes(e.fpmm_addr)).ok());
      assert(conditional_tokens.Append(hex_to_bytes(e.conditional_tokens)).ok());
      assert(collateral_token.Append(hex_to_bytes(e.collateral_token)).ok());
      assert(condition_ids.Append(e.condition_ids).ok());
      assert(fee.Append(uint256_hex_to_bytes32(e.fee)).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("factory", arrow::binary()),
        arrow::field("creator", arrow::binary()),
        arrow::field("fpmm_addr", arrow::binary()),
        arrow::field("conditional_tokens", arrow::binary()),
        arrow::field("collateral_token", arrow::binary()),
        arrow::field("condition_ids", arrow::utf8()),
        arrow::field("fee", arrow::binary()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9, a10;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(factory.Finish(&a4).ok());
    assert(creator.Finish(&a5).ok());
    assert(fpmm_addr.Finish(&a6).ok());
    assert(conditional_tokens.Finish(&a7).ok());
    assert(collateral_token.Finish(&a8).ok());
    assert(condition_ids.Finish(&a9).ok());
    assert(fee.Finish(&a10).ok());

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10});
    atomic_write_feather(partition_path("fpmm", start_block), table);
  }

  void write_fpmm_trade(int64_t start_block, const std::vector<stage1::FpmmTradeEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index, side;
    arrow::BinaryBuilder tx_hash, fpmm_addr, trader, outcome_index, usdc_amount, token_amount, fee;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(fpmm_addr.Append(hex_to_bytes(e.fpmm_addr)).ok());
      assert(trader.Append(hex_to_bytes(e.trader)).ok());
      assert(side.Append(e.side).ok());
      assert(outcome_index.Append(uint256_hex_to_bytes32(e.outcome_index)).ok());
      assert(usdc_amount.Append(uint256_hex_to_bytes32(e.usdc_amount)).ok());
      assert(token_amount.Append(uint256_hex_to_bytes32(e.token_amount)).ok());
      assert(fee.Append(uint256_hex_to_bytes32(e.fee)).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("fpmm_addr", arrow::binary()),
        arrow::field("trader", arrow::binary()),
        arrow::field("side", arrow::int64()),
        arrow::field("outcome_index", arrow::binary()),
        arrow::field("usdc_amount", arrow::binary()),
        arrow::field("token_amount", arrow::binary()),
        arrow::field("fee", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10});
    atomic_write_feather(partition_path("fpmm_trade", start_block), table);
  }

  void write_fpmm_funding(int64_t start_block, const std::vector<stage1::FpmmFundingEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index, side;
    arrow::BinaryBuilder tx_hash, fpmm_addr, funder, collateral_from_fee_pool, shares;
    arrow::StringBuilder amounts;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(fpmm_addr.Append(hex_to_bytes(e.fpmm_addr)).ok());
      assert(funder.Append(hex_to_bytes(e.funder)).ok());
      assert(side.Append(e.side).ok());
      assert(amounts.Append(e.amounts).ok());
      assert(collateral_from_fee_pool.Append(uint256_hex_to_bytes32(e.collateral_from_fee_pool)).ok());
      assert(shares.Append(uint256_hex_to_bytes32(e.shares)).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("fpmm_addr", arrow::binary()),
        arrow::field("funder", arrow::binary()),
        arrow::field("side", arrow::int64()),
        arrow::field("amounts", arrow::utf8()),
        arrow::field("collateral_from_fee_pool", arrow::binary()),
        arrow::field("shares", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9});
    atomic_write_feather(partition_path("fpmm_funding", start_block), table);
  }

  void write_order_filled(int64_t start_block, const std::vector<stage1::OrderFilledEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, order_hash, maker, taker, maker_asset_id, taker_asset_id, maker_amount, taker_amount, fee;
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
      assert(maker_amount.Append(uint256_hex_to_bytes32(e.maker_amount)).ok());
      assert(taker_amount.Append(uint256_hex_to_bytes32(e.taker_amount)).ok());
      assert(fee.Append(uint256_hex_to_bytes32(e.fee)).ok());
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
        arrow::field("maker_amount", arrow::binary()),
        arrow::field("taker_amount", arrow::binary()),
        arrow::field("fee", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12});
    atomic_write_feather(partition_path("order_filled", start_block), table);
  }

  void write_token_map(int64_t start_block, const std::vector<stage1::TokenMapEvent> &events) {
    if (events.empty())
      return;
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    atomic_write_feather(partition_path("token_map", start_block), table);
  }

  void write_neg_risk_market(int64_t start_block, const std::vector<stage1::NegRiskMarketEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, market_id, oracle, fee_bips, data;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(market_id.Append(hex_to_bytes(e.market_id)).ok());
      assert(oracle.Append(hex_to_bytes(e.oracle)).ok());
      assert(fee_bips.Append(uint256_hex_to_bytes32(e.fee_bips)).ok());
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
        arrow::field("fee_bips", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    atomic_write_feather(partition_path("neg_risk_market", start_block), table);
  }

  void write_neg_risk_question(int64_t start_block, const std::vector<stage1::NegRiskQuestionEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, market_id, question_id, question_index, data;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(market_id.Append(hex_to_bytes(e.market_id)).ok());
      assert(question_id.Append(hex_to_bytes(e.question_id)).ok());
      assert(question_index.Append(uint256_hex_to_bytes32(e.question_index)).ok());
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
        arrow::field("question_index", arrow::binary()),
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

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    atomic_write_feather(partition_path("neg_risk_question", start_block), table);
  }

  void write_convert(int64_t start_block, const std::vector<stage1::ConvertEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    arrow::BinaryBuilder tx_hash, stakeholder, market_id, index_set, amount;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(hex_to_bytes(e.tx_hash)).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(stakeholder.Append(hex_to_bytes(e.stakeholder)).ok());
      assert(market_id.Append(hex_to_bytes(e.market_id)).ok());
      assert(index_set.Append(uint256_hex_to_bytes32(e.index_set)).ok());
      assert(amount.Append(uint256_hex_to_bytes32(e.amount)).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::binary()),
        arrow::field("log_index", arrow::int64()),
        arrow::field("stakeholder", arrow::binary()),
        arrow::field("market_id", arrow::binary()),
        arrow::field("index_set", arrow::binary()),
        arrow::field("amount", arrow::binary()),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(stakeholder.Finish(&a4).ok());
    assert(market_id.Finish(&a5).ok());
    assert(index_set.Finish(&a6).ok());
    assert(amount.Finish(&a7).ok());

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7});
    atomic_write_feather(partition_path("convert", start_block), table);
  }
};
