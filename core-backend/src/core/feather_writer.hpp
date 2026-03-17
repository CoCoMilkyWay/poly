#pragma once

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/util/compression.h>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "../stage1/event_decode.hpp"
#include "misc/profiler.hpp"

namespace fs = std::filesystem;

class FeatherWriter {
public:
  explicit FeatherWriter(const std::string &data_dir) : stage1_dir_(data_dir) {}

  void write_partition(int64_t start_block, int64_t end_block, stage1::DecodedEvents &events) {
    begin_partition(start_block, end_block);
    append_partition_batch(events);
    finalize_partition();
    release_events(events);
  }

  void begin_partition(int64_t start_block, int64_t end_block) {
    assert(start_block <= end_block);
    assert(!streaming_session_.has_value());
    active_end_block_ = end_block;
    streaming_session_ = StreamingSession{};
    streaming_session_->start_block = start_block;
    streaming_session_->end_block = end_block;
  }

  void append_partition_batch(const stage1::DecodedEvents &events) {
    assert(streaming_session_.has_value());
    const int64_t start_block = streaming_session_->start_block;
    {
      TraceN("s1/transfer");
      write_transfer(start_block, events.transfer);
    }
    {
      TraceN("s1/condition_preparation");
      write_condition_preparation(start_block, events.condition_preparation);
    }
    {
      TraceN("s1/condition_resolution");
      write_condition_resolution(start_block, events.condition_resolution);
    }
    {
      TraceN("s1/split");
      write_split(start_block, events.split);
    }
    {
      TraceN("s1/merge");
      write_merge(start_block, events.merge);
    }
    {
      TraceN("s1/redemption");
      write_redemption(start_block, events.redemption);
    }
    {
      TraceN("s1/fpmm");
      write_fpmm(start_block, events.fpmm);
    }
    {
      TraceN("s1/fpmm_trade");
      write_fpmm_trade(start_block, events.fpmm_trade);
    }
    {
      TraceN("s1/fpmm_funding");
      write_fpmm_funding(start_block, events.fpmm_funding);
    }
    {
      TraceN("s1/order_filled");
      write_order_filled(start_block, events.order_filled);
    }
    {
      TraceN("s1/token_map");
      write_token_map(start_block, events.token_map);
    }
    {
      TraceN("s1/neg_risk_market");
      write_neg_risk_market(start_block, events.neg_risk_market);
    }
    {
      TraceN("s1/neg_risk_question");
      write_neg_risk_question(start_block, events.neg_risk_question);
    }
    {
      TraceN("s1/convert");
      write_convert(start_block, events.convert);
    }
  }

  void finalize_partition() {
    assert(streaming_session_.has_value());
    for (auto &[table, stream_writer] : streaming_session_->writers) {
      (void)table;
      auto close_status = stream_writer.writer->Close();
      assert(close_status.ok());
      auto file_close_status = stream_writer.outfile->Close();
      assert(file_close_status.ok());
      fsync_file(stream_writer.tmp_path);
    }
    for (auto &[table, stream_writer] : streaming_session_->writers) {
      (void)table;
      int ret = std::rename(stream_writer.tmp_path.c_str(), stream_writer.final_path.c_str());
      assert(ret == 0);
      std::string dir = fs::path(stream_writer.final_path).parent_path().string();
      if (dir.empty()) {
        dir = ".";
      }
      fsync_dir(dir);
    }
    streaming_session_.reset();
    active_end_block_ = -1;
  }

  uintmax_t partition_total_size_bytes(int64_t start_block, int64_t end_block) const {
    assert(start_block <= end_block);
    const fs::path root(stage1_dir_);
    assert(fs::exists(root));
    assert(fs::is_directory(root));
    uintmax_t total = 0;
    const std::string filename = std::to_string(start_block) + "-" + std::to_string(end_block) + ".feather";
    for (const auto &entry : fs::directory_iterator(root)) {
      if (!entry.is_directory()) {
        continue;
      }
      const fs::path file = entry.path() / filename;
      if (fs::exists(file) && fs::is_regular_file(file)) {
        total += fs::file_size(file);
      }
    }
    return total;
  }

private:
  struct StreamingTableWriter {
    std::string tmp_path;
    std::string final_path;
    std::shared_ptr<arrow::Schema> schema;
    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    std::shared_ptr<arrow::ipc::RecordBatchWriter> writer;
  };

  struct StreamingSession {
    int64_t start_block = 0;
    int64_t end_block = 0;
    std::map<std::string, StreamingTableWriter> writers;
  };

  std::string stage1_dir_;
  int64_t active_end_block_ = -1;
  std::optional<StreamingSession> streaming_session_;

  template <typename T>
  static void release_vector(std::vector<T> &v) {
    std::vector<T>().swap(v);
  }

  static void release_events(stage1::DecodedEvents &events) {
    release_vector(events.transfer);
    release_vector(events.condition_preparation);
    release_vector(events.condition_resolution);
    release_vector(events.split);
    release_vector(events.merge);
    release_vector(events.redemption);
    release_vector(events.fpmm);
    release_vector(events.fpmm_trade);
    release_vector(events.fpmm_funding);
    release_vector(events.order_filled);
    release_vector(events.token_map);
    release_vector(events.neg_risk_market);
    release_vector(events.neg_risk_question);
    release_vector(events.convert);
  }

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
    assert(active_end_block_ >= start_block);
    return table_dir(table) + "/" + std::to_string(start_block) + "-" + std::to_string(active_end_block_) + ".feather";
  }

  template <size_t N>
  static std::string_view bytes_view(const std::array<uint8_t, N> &value) {
    return std::string_view(reinterpret_cast<const char *>(value.data()), value.size());
  }

  static void append_bytes32_list(arrow::ListBuilder &list_builder,
                                  arrow::FixedSizeBinaryBuilder *value_builder,
                                  const std::vector<stage1::Bytes32> &values) {
    assert(list_builder.Append().ok());
    for (const auto &value : values) {
      assert(value_builder->Append(value).ok());
    }
  }

  static arrow::ipc::IpcWriteOptions ipc_write_options() {
    auto options = arrow::ipc::IpcWriteOptions::Defaults();
    auto codec = arrow::util::Codec::Create(arrow::Compression::ZSTD);
    assert(codec.ok());
    options.codec = std::shared_ptr<arrow::util::Codec>(std::move(*codec));
    return options;
  }

  void atomic_write_feather(const std::string &path, std::shared_ptr<arrow::Table> table) {
    std::string tmp_path = path + ".tmp";
    {
      auto outfile = arrow::io::FileOutputStream::Open(tmp_path);
      assert(outfile.ok());
      auto writer = arrow::ipc::MakeFileWriter(outfile->get(), table->schema(), ipc_write_options());
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

  void stream_write_feather(const std::string &table, int64_t start_block, std::shared_ptr<arrow::Table> table_data) {
    assert(streaming_session_.has_value());
    assert(streaming_session_->start_block == start_block);
    assert(streaming_session_->end_block == active_end_block_);
    assert(table_data->num_rows() > 0);

    auto it = streaming_session_->writers.find(table);
    if (it == streaming_session_->writers.end()) {
      StreamingTableWriter writer_slot;
      writer_slot.final_path = partition_path(table, start_block);
      writer_slot.tmp_path = writer_slot.final_path + ".tmp";
      if (fs::exists(writer_slot.tmp_path)) {
        fs::remove(writer_slot.tmp_path);
      }
      assert(!fs::exists(writer_slot.final_path));
      auto outfile = arrow::io::FileOutputStream::Open(writer_slot.tmp_path);
      assert(outfile.ok());
      auto writer = arrow::ipc::MakeFileWriter(outfile->get(), table_data->schema(), ipc_write_options());
      assert(writer.ok());
      writer_slot.schema = table_data->schema();
      writer_slot.outfile = *outfile;
      writer_slot.writer = *writer;
      it = streaming_session_->writers.emplace(table, std::move(writer_slot)).first;
    }

    assert(it->second.schema->Equals(*table_data->schema(), false));
    auto reader = arrow::TableBatchReader(*table_data);
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      auto status = reader.ReadNext(&batch);
      assert(status.ok());
      if (!batch) {
        break;
      }
      auto write_status = it->second.writer->WriteRecordBatch(*batch);
      assert(write_status.ok());
    }
  }

  void write_partition_table(const std::string &table, int64_t start_block, std::shared_ptr<arrow::Table> table_data) {
    if (table_data->num_rows() == 0) {
      return;
    }
    if (streaming_session_.has_value()) {
      stream_write_feather(table, start_block, std::move(table_data));
      return;
    }
    atomic_write_feather(partition_path(table, start_block), std::move(table_data));
  }

  void write_transfer(int64_t start_block, const std::vector<stage1::TransferEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), token_id(u256), op(addr20), from(addr20), to(addr20), amount(u256);

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(op.Append(e.op).ok());
      assert(from.Append(e.from).ok());
      assert(to.Append(e.to).ok());
      assert(token_id.Append(e.token_id).ok());
      assert(amount.Append(e.amount).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("operator", arrow::fixed_size_binary(20)),
        arrow::field("from_addr", arrow::fixed_size_binary(20)),
        arrow::field("to_addr", arrow::fixed_size_binary(20)),
        arrow::field("token_id", arrow::fixed_size_binary(32)),
        arrow::field("amount", arrow::fixed_size_binary(32)),
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
    write_partition_table("transfer", start_block, table);
  }

  void write_condition_preparation(int64_t start_block, const std::vector<stage1::ConditionPrepEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), condition_id(u256), question_id(u256),
        oracle(addr20), outcome_slot_count(u256);

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(condition_id.Append(e.condition_id).ok());
      assert(oracle.Append(e.oracle).ok());
      assert(question_id.Append(e.question_id).ok());
      assert(outcome_slot_count.Append(e.outcome_slot_count).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("condition_id", arrow::fixed_size_binary(32)),
        arrow::field("oracle", arrow::fixed_size_binary(20)),
        arrow::field("question_id", arrow::fixed_size_binary(32)),
        arrow::field("outcome_slot_count", arrow::fixed_size_binary(32)),
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
    write_partition_table("condition_preparation", start_block, table);
  }

  void write_condition_resolution(int64_t start_block, const std::vector<stage1::ConditionResolveEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), condition_id(u256), question_id(u256),
        oracle(addr20), outcome_slot_count(u256);
    auto payout_values_ptr = std::make_shared<arrow::FixedSizeBinaryBuilder>(u256);
    arrow::ListBuilder payout_numerators(arrow::default_memory_pool(), payout_values_ptr);
    auto *payout_values = static_cast<arrow::FixedSizeBinaryBuilder *>(payout_numerators.value_builder());

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(condition_id.Append(e.condition_id).ok());
      assert(oracle.Append(e.oracle).ok());
      assert(question_id.Append(e.question_id).ok());
      assert(outcome_slot_count.Append(e.outcome_slot_count).ok());
      append_bytes32_list(payout_numerators, payout_values, e.payout_numerators);
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("condition_id", arrow::fixed_size_binary(32)),
        arrow::field("oracle", arrow::fixed_size_binary(20)),
        arrow::field("question_id", arrow::fixed_size_binary(32)),
        arrow::field("outcome_slot_count", arrow::fixed_size_binary(32)),
        arrow::field("payout_numerators", arrow::list(arrow::fixed_size_binary(32))),
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
    write_partition_table("condition_resolution", start_block, table);
  }

  void write_split_merge_impl(int64_t start_block, const std::vector<stage1::SplitMergeEvent> &events, const std::string &table_name) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), parent_collection_id(u256), condition_id(u256),
        stakeholder(addr20), collateral_token(addr20), amount(u256);
    auto partition_values_ptr = std::make_shared<arrow::FixedSizeBinaryBuilder>(u256);
    arrow::ListBuilder partition(arrow::default_memory_pool(), partition_values_ptr);
    auto *partition_values = static_cast<arrow::FixedSizeBinaryBuilder *>(partition.value_builder());

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(stakeholder.Append(e.stakeholder).ok());
      assert(collateral_token.Append(e.collateral_token).ok());
      assert(parent_collection_id.Append(e.parent_collection_id).ok());
      assert(condition_id.Append(e.condition_id).ok());
      append_bytes32_list(partition, partition_values, e.partition);
      assert(amount.Append(e.amount).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("stakeholder", arrow::fixed_size_binary(20)),
        arrow::field("collateral_token", arrow::fixed_size_binary(20)),
        arrow::field("parent_collection_id", arrow::fixed_size_binary(32)),
        arrow::field("condition_id", arrow::fixed_size_binary(32)),
        arrow::field("partition", arrow::list(arrow::fixed_size_binary(32))),
        arrow::field("amount", arrow::fixed_size_binary(32)),
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
    write_partition_table(table_name, start_block, table);
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
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), parent_collection_id(u256), condition_id(u256),
        redeemer(addr20), collateral_token(addr20), payout(u256);
    auto index_set_values_ptr = std::make_shared<arrow::FixedSizeBinaryBuilder>(u256);
    arrow::ListBuilder index_sets(arrow::default_memory_pool(), index_set_values_ptr);
    auto *index_set_values = static_cast<arrow::FixedSizeBinaryBuilder *>(index_sets.value_builder());

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(redeemer.Append(e.redeemer).ok());
      assert(collateral_token.Append(e.collateral_token).ok());
      assert(parent_collection_id.Append(e.parent_collection_id).ok());
      assert(condition_id.Append(e.condition_id).ok());
      append_bytes32_list(index_sets, index_set_values, e.index_sets);
      assert(payout.Append(e.payout).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("redeemer", arrow::fixed_size_binary(20)),
        arrow::field("collateral_token", arrow::fixed_size_binary(20)),
        arrow::field("parent_collection_id", arrow::fixed_size_binary(32)),
        arrow::field("condition_id", arrow::fixed_size_binary(32)),
        arrow::field("index_sets", arrow::list(arrow::fixed_size_binary(32))),
        arrow::field("payout", arrow::fixed_size_binary(32)),
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
    write_partition_table("redemption", start_block, table);
  }

  void write_fpmm(int64_t start_block, const std::vector<stage1::FpmmEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index, creation_topics_count;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), factory(addr20), creator(addr20), fpmm_addr(addr20),
        conditional_tokens(addr20), collateral_token(addr20), fee(u256);
    auto condition_id_values_ptr = std::make_shared<arrow::FixedSizeBinaryBuilder>(u256);
    arrow::ListBuilder condition_ids(arrow::default_memory_pool(), condition_id_values_ptr);
    auto *condition_id_values = static_cast<arrow::FixedSizeBinaryBuilder *>(condition_ids.value_builder());
    arrow::StringBuilder creation_layout;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(factory.Append(e.factory).ok());
      assert(creation_topics_count.Append(e.creation_topics_count).ok());
      assert(creation_layout.Append(e.creation_layout).ok());
      assert(creator.Append(e.creator).ok());
      assert(fpmm_addr.Append(e.fpmm_addr).ok());
      assert(conditional_tokens.Append(e.conditional_tokens).ok());
      assert(collateral_token.Append(e.collateral_token).ok());
      append_bytes32_list(condition_ids, condition_id_values, e.condition_ids);
      assert(fee.Append(e.fee).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("factory", arrow::fixed_size_binary(20)),
        arrow::field("creation_topics_count", arrow::int64()),
        arrow::field("creation_layout", arrow::utf8()),
        arrow::field("creator", arrow::fixed_size_binary(20)),
        arrow::field("fpmm_addr", arrow::fixed_size_binary(20)),
        arrow::field("conditional_tokens", arrow::fixed_size_binary(20)),
        arrow::field("collateral_token", arrow::fixed_size_binary(20)),
        arrow::field("condition_ids", arrow::list(arrow::fixed_size_binary(32))),
        arrow::field("fee", arrow::fixed_size_binary(32)),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(factory.Finish(&a4).ok());
    assert(creation_topics_count.Finish(&a5).ok());
    assert(creation_layout.Finish(&a6).ok());
    assert(creator.Finish(&a7).ok());
    assert(fpmm_addr.Finish(&a8).ok());
    assert(conditional_tokens.Finish(&a9).ok());
    assert(collateral_token.Finish(&a10).ok());
    assert(condition_ids.Finish(&a11).ok());
    assert(fee.Finish(&a12).ok());

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12});
    write_partition_table("fpmm", start_block, table);
  }

  void write_fpmm_trade(int64_t start_block, const std::vector<stage1::FpmmTradeEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index, side;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), fpmm_addr(addr20), trader(addr20), outcome_index(u256),
        collateral_amount(u256), token_amount(u256), fee(u256);

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(fpmm_addr.Append(e.fpmm_addr).ok());
      assert(trader.Append(e.trader).ok());
      assert(side.Append(e.side).ok());
      assert(outcome_index.Append(e.outcome_index).ok());
      assert(collateral_amount.Append(e.collateral_amount).ok());
      assert(token_amount.Append(e.token_amount).ok());
      assert(fee.Append(e.fee).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("fpmm_addr", arrow::fixed_size_binary(20)),
        arrow::field("trader", arrow::fixed_size_binary(20)),
        arrow::field("side", arrow::int64()),
        arrow::field("outcome_index", arrow::fixed_size_binary(32)),
        arrow::field("collateral_amount", arrow::fixed_size_binary(32)),
        arrow::field("token_amount", arrow::fixed_size_binary(32)),
        arrow::field("fee", arrow::fixed_size_binary(32)),
    });

    std::shared_ptr<arrow::Array> a1, a2, a3, a4, a5, a6, a7, a8, a9, a10;
    assert(block_number.Finish(&a1).ok());
    assert(tx_hash.Finish(&a2).ok());
    assert(log_index.Finish(&a3).ok());
    assert(fpmm_addr.Finish(&a4).ok());
    assert(trader.Finish(&a5).ok());
    assert(side.Finish(&a6).ok());
    assert(outcome_index.Finish(&a7).ok());
    assert(collateral_amount.Finish(&a8).ok());
    assert(token_amount.Finish(&a9).ok());
    assert(fee.Finish(&a10).ok());

    auto table = arrow::Table::Make(schema, {a1, a2, a3, a4, a5, a6, a7, a8, a9, a10});
    write_partition_table("fpmm_trade", start_block, table);
  }

  void write_fpmm_funding(int64_t start_block, const std::vector<stage1::FpmmFundingEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index, side;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), fpmm_addr(addr20), funder(addr20),
        collateral_from_fee_pool(u256), shares(u256);
    auto amount_values_ptr = std::make_shared<arrow::FixedSizeBinaryBuilder>(u256);
    arrow::ListBuilder amounts(arrow::default_memory_pool(), amount_values_ptr);
    auto *amount_values = static_cast<arrow::FixedSizeBinaryBuilder *>(amounts.value_builder());

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(fpmm_addr.Append(e.fpmm_addr).ok());
      assert(funder.Append(e.funder).ok());
      assert(side.Append(e.side).ok());
      append_bytes32_list(amounts, amount_values, e.amounts);
      assert(collateral_from_fee_pool.Append(e.collateral_from_fee_pool).ok());
      assert(shares.Append(e.shares).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("fpmm_addr", arrow::fixed_size_binary(20)),
        arrow::field("funder", arrow::fixed_size_binary(20)),
        arrow::field("side", arrow::int64()),
        arrow::field("amounts", arrow::list(arrow::fixed_size_binary(32))),
        arrow::field("collateral_from_fee_pool", arrow::fixed_size_binary(32)),
        arrow::field("shares", arrow::fixed_size_binary(32)),
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
    write_partition_table("fpmm_funding", start_block, table);
  }

  void write_order_filled(int64_t start_block, const std::vector<stage1::OrderFilledEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), order_hash(u256), maker_asset_id(u256), taker_asset_id(u256),
        maker(addr20), taker(addr20), maker_amount(u256), taker_amount(u256), fee(u256);
    arrow::StringBuilder exchange;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(exchange.Append(e.exchange).ok());
      assert(order_hash.Append(e.order_hash).ok());
      assert(maker.Append(e.maker).ok());
      assert(taker.Append(e.taker).ok());
      assert(maker_asset_id.Append(e.maker_asset_id).ok());
      assert(taker_asset_id.Append(e.taker_asset_id).ok());
      assert(maker_amount.Append(e.maker_amount).ok());
      assert(taker_amount.Append(e.taker_amount).ok());
      assert(fee.Append(e.fee).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("exchange", arrow::utf8()),
        arrow::field("order_hash", arrow::fixed_size_binary(32)),
        arrow::field("maker", arrow::fixed_size_binary(20)),
        arrow::field("taker", arrow::fixed_size_binary(20)),
        arrow::field("maker_asset_id", arrow::fixed_size_binary(32)),
        arrow::field("taker_asset_id", arrow::fixed_size_binary(32)),
        arrow::field("maker_amount", arrow::fixed_size_binary(32)),
        arrow::field("taker_amount", arrow::fixed_size_binary(32)),
        arrow::field("fee", arrow::fixed_size_binary(32)),
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
    write_partition_table("order_filled", start_block, table);
  }

  void write_token_map(int64_t start_block, const std::vector<stage1::TokenMapEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), condition_id(u256), token0(u256), token1(u256);
    arrow::StringBuilder exchange;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(exchange.Append(e.exchange).ok());
      assert(token0.Append(e.token0).ok());
      assert(token1.Append(e.token1).ok());
      assert(condition_id.Append(e.condition_id).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("exchange", arrow::utf8()),
        arrow::field("token0", arrow::fixed_size_binary(32)),
        arrow::field("token1", arrow::fixed_size_binary(32)),
        arrow::field("condition_id", arrow::fixed_size_binary(32)),
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
    write_partition_table("token_map", start_block, table);
  }

  void write_neg_risk_market(int64_t start_block, const std::vector<stage1::NegRiskMarketEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), market_id(u256), oracle(addr20), fee_bips(u256);
    arrow::BinaryBuilder data;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(market_id.Append(e.market_id).ok());
      assert(oracle.Append(e.oracle).ok());
      assert(fee_bips.Append(e.fee_bips).ok());
      if (e.data) {
        assert(data.Append(*e.data).ok());
      } else {
        assert(data.AppendNull().ok());
      }
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("market_id", arrow::fixed_size_binary(32)),
        arrow::field("oracle", arrow::fixed_size_binary(20)),
        arrow::field("fee_bips", arrow::fixed_size_binary(32)),
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
    write_partition_table("neg_risk_market", start_block, table);
  }

  void write_neg_risk_question(int64_t start_block, const std::vector<stage1::NegRiskQuestionEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), market_id(u256), question_id(u256), question_index(u256);
    arrow::BinaryBuilder data;

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(market_id.Append(e.market_id).ok());
      assert(question_id.Append(e.question_id).ok());
      assert(question_index.Append(e.question_index).ok());
      if (e.data) {
        assert(data.Append(*e.data).ok());
      } else {
        assert(data.AppendNull().ok());
      }
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("market_id", arrow::fixed_size_binary(32)),
        arrow::field("question_id", arrow::fixed_size_binary(32)),
        arrow::field("question_index", arrow::fixed_size_binary(32)),
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
    write_partition_table("neg_risk_question", start_block, table);
  }

  void write_convert(int64_t start_block, const std::vector<stage1::ConvertEvent> &events) {
    if (events.empty())
      return;
    arrow::Int64Builder block_number, log_index;
    auto addr20 = arrow::fixed_size_binary(20);
    auto u256 = arrow::fixed_size_binary(32);
    arrow::FixedSizeBinaryBuilder tx_hash(u256), market_id(u256), stakeholder(addr20), index_set(u256), amount(u256);

    for (const auto &e : events) {
      assert(block_number.Append(e.block_number).ok());
      assert(tx_hash.Append(e.tx_hash).ok());
      assert(log_index.Append(e.log_index).ok());
      assert(stakeholder.Append(e.stakeholder).ok());
      assert(market_id.Append(e.market_id).ok());
      assert(index_set.Append(e.index_set).ok());
      assert(amount.Append(e.amount).ok());
    }

    auto schema = arrow::schema({
        arrow::field("block_number", arrow::int64()),
        arrow::field("tx_hash", arrow::fixed_size_binary(32)),
        arrow::field("log_index", arrow::int64()),
        arrow::field("stakeholder", arrow::fixed_size_binary(20)),
        arrow::field("market_id", arrow::fixed_size_binary(32)),
        arrow::field("index_set", arrow::fixed_size_binary(32)),
        arrow::field("amount", arrow::fixed_size_binary(32)),
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
    write_partition_table("convert", start_block, table);
  }
};
