#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "rocksdb/c.h"

namespace core::rocks {

namespace detail {

inline uint32_t encode_i32_lex(int32_t v) {
  return static_cast<uint32_t>(v) ^ 0x80000000u;
}

inline uint64_t encode_i64_lex(int64_t v) {
  return static_cast<uint64_t>(v) ^ 0x8000000000000000ULL;
}

inline int32_t decode_i32_lex(uint32_t v) {
  return static_cast<int32_t>(v ^ 0x80000000u);
}

inline int64_t decode_i64_lex(uint64_t v) {
  return static_cast<int64_t>(v ^ 0x8000000000000000ULL);
}

inline void append_u32_be(std::string &out, uint32_t v) {
  out.push_back(static_cast<char>((v >> 24) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>(v & 0xff));
}

inline void append_u64_be(std::string &out, uint64_t v) {
  out.push_back(static_cast<char>((v >> 56) & 0xff));
  out.push_back(static_cast<char>((v >> 48) & 0xff));
  out.push_back(static_cast<char>((v >> 40) & 0xff));
  out.push_back(static_cast<char>((v >> 32) & 0xff));
  out.push_back(static_cast<char>((v >> 24) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>(v & 0xff));
}

inline uint32_t read_u32_be(std::string_view in, size_t off) {
  assert(off + 4 <= in.size());
  return (static_cast<uint32_t>(static_cast<uint8_t>(in[off])) << 24) |
         (static_cast<uint32_t>(static_cast<uint8_t>(in[off + 1])) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(in[off + 2])) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(in[off + 3])));
}

inline uint64_t read_u64_be(std::string_view in, size_t off) {
  assert(off + 8 <= in.size());
  return (static_cast<uint64_t>(static_cast<uint8_t>(in[off])) << 56) |
         (static_cast<uint64_t>(static_cast<uint8_t>(in[off + 1])) << 48) |
         (static_cast<uint64_t>(static_cast<uint8_t>(in[off + 2])) << 40) |
         (static_cast<uint64_t>(static_cast<uint8_t>(in[off + 3])) << 32) |
         (static_cast<uint64_t>(static_cast<uint8_t>(in[off + 4])) << 24) |
         (static_cast<uint64_t>(static_cast<uint8_t>(in[off + 5])) << 16) |
         (static_cast<uint64_t>(static_cast<uint8_t>(in[off + 6])) << 8) |
         (static_cast<uint64_t>(static_cast<uint8_t>(in[off + 7])));
}

inline bool starts_with(std::string_view full, std::string_view prefix) {
  return full.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), full.begin());
}

inline void assert_no_err(char *err) {
  if (err != nullptr) {
    rocksdb_free(err);
    assert(false && "rocksdb call failed");
  }
}

inline int64_t property_int64(rocksdb_t *db, const char *prop_name) {
  uint64_t value = 0;
  const int ret = rocksdb_property_int(db, prop_name, &value);
  assert(ret == 0);
  return static_cast<int64_t>(value);
}

inline rocksdb_options_t *make_db_options() {
  rocksdb_options_t *opts = rocksdb_options_create();
  // Keep SST/WAL at sane granularity. Passing 0 to optimize_level_style_compaction
  // shrinks write_buffer_size to 64KB and explodes file count on large sync.
  constexpr size_t kWriteBufferSize = 128ull * 1024ull * 1024ull;
  constexpr uint64_t kTargetFileSizeBase = 128ull * 1024ull * 1024ull;
  constexpr uint64_t kMaxBytesForLevelBase = 1024ull * 1024ull * 1024ull;
  constexpr uint64_t kMaxTotalWalSize = 1024ull * 1024ull * 1024ull;
  rocksdb_options_set_create_if_missing(opts, 1);
  rocksdb_options_set_create_missing_column_families(opts, 1);
  rocksdb_options_increase_parallelism(opts, 4);
  rocksdb_options_set_max_open_files(opts, 4096);
  rocksdb_options_set_write_buffer_size(opts, kWriteBufferSize);
  rocksdb_options_set_max_write_buffer_number(opts, 6);
  rocksdb_options_set_min_write_buffer_number_to_merge(opts, 2);
  rocksdb_options_set_level0_file_num_compaction_trigger(opts, 4);
  rocksdb_options_set_target_file_size_base(opts, kTargetFileSizeBase);
  rocksdb_options_set_max_bytes_for_level_base(opts, kMaxBytesForLevelBase);
  rocksdb_options_set_max_total_wal_size(opts, kMaxTotalWalSize);
  rocksdb_options_set_keep_log_file_num(opts, 8);
  rocksdb_options_set_recycle_log_file_num(opts, 4);
  return opts;
}

} // namespace detail

struct MemoryStats {
  int64_t memtables_bytes = 0;
  int64_t table_readers_bytes = 0;
  int64_t block_cache_bytes = 0;
  int64_t block_cache_pinned_bytes = 0;

  int64_t estimated_total_bytes() const {
    return memtables_bytes + table_readers_bytes + block_cache_bytes + block_cache_pinned_bytes;
  }
};

struct Stage2UserEventRecord {
  std::string user_addr;
  int64_t sort_key = 0;
  int32_t cond_idx = 0;
  int32_t event_type = 0;
  int32_t token_idx = 0;
  int32_t collateral = 0;
  int64_t amount = 0;
  int64_t price = 0;
};

class Stage2UserEventStore {
public:
  static constexpr size_t kUserAddrBytes = 20;
  static constexpr const char *kCfSortData = "sort_data";
  static constexpr const char *kCfUserIndex = "user_index";

  explicit Stage2UserEventStore(const std::string &db_path) : db_path_(db_path) {
    open();
  }

  ~Stage2UserEventStore() {
    close();
  }

  Stage2UserEventStore(const Stage2UserEventStore &) = delete;
  Stage2UserEventStore &operator=(const Stage2UserEventStore &) = delete;

  void write_events(const std::vector<Stage2UserEventRecord> &rows) const {
    if (rows.empty()) {
      return;
    }
    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    for (const auto &row : rows) {
      const std::string sort_key = build_sort_key(row);
      const std::string user_key = build_user_key(row);
      const std::string value = build_value(row);
      rocksdb_writebatch_put_cf(batch, sort_cf_, sort_key.data(), sort_key.size(),
                                value.data(), value.size());
      rocksdb_writebatch_put_cf(batch, user_cf_, user_key.data(), user_key.size(), "", 0);
    }
    char *err = nullptr;
    rocksdb_write(db_, write_options_, batch, &err);
    rocksdb_writebatch_destroy(batch);
    detail::assert_no_err(err);
  }

  std::vector<Stage2UserEventRecord> scan_by_sort_key(int64_t sort_key_exclusive,
                                                      int64_t sort_key_inclusive,
                                                      size_t limit) const {
    std::vector<Stage2UserEventRecord> out;
    if (limit == 0 || sort_key_exclusive >= sort_key_inclusive) {
      return out;
    }
    const int64_t seek_sort_key = sort_key_exclusive + 1;
    const std::string seek_key = build_sort_seek_key(seek_sort_key);

    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, read_options_, sort_cf_);
    rocksdb_iter_seek(it, seek_key.data(), seek_key.size());
    while (rocksdb_iter_valid(it) != 0 && out.size() < limit) {
      size_t klen = 0;
      size_t vlen = 0;
      const char *k = rocksdb_iter_key(it, &klen);
      const char *v = rocksdb_iter_value(it, &vlen);
      Stage2UserEventRecord row = decode_sort_entry(std::string_view(k, klen), std::string_view(v, vlen));
      if (row.sort_key > sort_key_inclusive) {
        break;
      }
      out.push_back(std::move(row));
      rocksdb_iter_next(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
    return out;
  }

  std::vector<Stage2UserEventRecord> scan_by_user(const std::string &user_addr) const {
    assert(user_addr.size() == kUserAddrBytes);
    std::vector<Stage2UserEventRecord> out;
    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, read_options_, user_cf_);
    rocksdb_iter_seek(it, user_addr.data(), user_addr.size());
    while (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      const char *kptr = rocksdb_iter_key(it, &klen);
      std::string_view k(kptr, klen);
      if (!detail::starts_with(k, user_addr)) {
        break;
      }
      Stage2UserEventRecord row = decode_user_key_only(k);
      const std::string sort_key = build_sort_key(row);
      size_t value_len = 0;
      char *err = nullptr;
      char *value = rocksdb_get_cf(db_, read_options_, sort_cf_, sort_key.data(), sort_key.size(),
                                   &value_len, &err);
      detail::assert_no_err(err);
      assert(value != nullptr);
      decode_value_into(std::string_view(value, value_len), row);
      rocksdb_free(value);
      out.push_back(std::move(row));
      rocksdb_iter_next(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
    return out;
  }

  bool has_user(std::string_view user_addr) const {
    assert(user_addr.size() == kUserAddrBytes);
    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, read_options_, user_cf_);
    rocksdb_iter_seek(it, user_addr.data(), user_addr.size());
    bool found = false;
    if (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      const char *kptr = rocksdb_iter_key(it, &klen);
      found = detail::starts_with(std::string_view(kptr, klen), user_addr);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
    return found;
  }

  MemoryStats memory_stats() const {
    MemoryStats stats;
    stats.memtables_bytes = detail::property_int64(db_, "rocksdb.cur-size-all-mem-tables");
    stats.table_readers_bytes = detail::property_int64(db_, "rocksdb.estimate-table-readers-mem");
    stats.block_cache_bytes = detail::property_int64(db_, "rocksdb.block-cache-usage");
    stats.block_cache_pinned_bytes = detail::property_int64(db_, "rocksdb.block-cache-pinned-usage");
    return stats;
  }

  const std::string &db_path() const {
    return db_path_;
  }

  template <typename Fn>
  void for_each_event(Fn &&fn) const {
    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, scan_read_options_, sort_cf_);
    rocksdb_iter_seek_to_first(it);
    while (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      size_t vlen = 0;
      const char *k = rocksdb_iter_key(it, &klen);
      const char *v = rocksdb_iter_value(it, &vlen);
      fn(decode_sort_entry(std::string_view(k, klen), std::string_view(v, vlen)));
      rocksdb_iter_next(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
  }

  template <typename Fn>
  void for_each_event_brief(Fn &&fn) const {
    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, scan_read_options_, sort_cf_);
    rocksdb_iter_seek_to_first(it);
    while (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      size_t vlen = 0;
      const char *k = rocksdb_iter_key(it, &klen);
      const char *v = rocksdb_iter_value(it, &vlen);
      const std::string_view key(k, klen);
      const std::string_view value(v, vlen);
      fn(sort_entry_user_addr(key),
         decode_sort_entry_cond_idx(key),
         decode_sort_entry_event_type(key),
         decode_value_collateral(value));
      rocksdb_iter_next(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
  }

  int64_t count_distinct_users() const {
    int64_t count = 0;
    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, scan_read_options_, user_cf_);
    std::string last_user;
    rocksdb_iter_seek_to_first(it);
    while (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      const char *kptr = rocksdb_iter_key(it, &klen);
      std::string_view k(kptr, klen);
      assert(k.size() == 40);
      std::string_view user = k.substr(0, kUserAddrBytes);
      if (last_user.size() == user.size() &&
          std::equal(user.begin(), user.end(), last_user.begin())) {
        rocksdb_iter_next(it);
        continue;
      }
      last_user.assign(user.begin(), user.end());
      count++;
      rocksdb_iter_next(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
    return count;
  }

  int64_t count_events() const {
    int64_t cnt = 0;
    for_each_event([&](const Stage2UserEventRecord &) { cnt++; });
    return cnt;
  }

private:
  void open() {
    options_ = detail::make_db_options();
    read_options_ = rocksdb_readoptions_create();
    scan_read_options_ = rocksdb_readoptions_create();
    write_options_ = rocksdb_writeoptions_create();
    rocksdb_writeoptions_set_sync(write_options_, 1);
    rocksdb_readoptions_set_fill_cache(scan_read_options_, 0);
    rocksdb_readoptions_set_verify_checksums(scan_read_options_, 0);

    constexpr int kCfCount = 3;
    const char *names[kCfCount] = {"default", kCfSortData, kCfUserIndex};
    const rocksdb_options_t *cf_options[kCfCount] = {options_, options_, options_};
    rocksdb_column_family_handle_t *handles[kCfCount] = {nullptr, nullptr, nullptr};
    char *err = nullptr;
    db_ = rocksdb_open_column_families(options_, db_path_.c_str(), kCfCount, names, cf_options, handles, &err);
    detail::assert_no_err(err);
    assert(db_ != nullptr);
    default_cf_ = handles[0];
    sort_cf_ = handles[1];
    user_cf_ = handles[2];
    assert(default_cf_ != nullptr);
    assert(sort_cf_ != nullptr);
    assert(user_cf_ != nullptr);
  }

  void close() {
    if (user_cf_ != nullptr) {
      rocksdb_column_family_handle_destroy(user_cf_);
      user_cf_ = nullptr;
    }
    if (sort_cf_ != nullptr) {
      rocksdb_column_family_handle_destroy(sort_cf_);
      sort_cf_ = nullptr;
    }
    if (default_cf_ != nullptr) {
      rocksdb_column_family_handle_destroy(default_cf_);
      default_cf_ = nullptr;
    }
    if (db_ != nullptr) {
      rocksdb_close(db_);
      db_ = nullptr;
    }
    if (write_options_ != nullptr) {
      rocksdb_writeoptions_destroy(write_options_);
      write_options_ = nullptr;
    }
    if (read_options_ != nullptr) {
      rocksdb_readoptions_destroy(read_options_);
      read_options_ = nullptr;
    }
    if (scan_read_options_ != nullptr) {
      rocksdb_readoptions_destroy(scan_read_options_);
      scan_read_options_ = nullptr;
    }
    if (options_ != nullptr) {
      rocksdb_options_destroy(options_);
      options_ = nullptr;
    }
  }

  static std::string build_sort_key(const Stage2UserEventRecord &row) {
    assert(row.user_addr.size() == kUserAddrBytes);
    std::string out;
    out.reserve(40);
    detail::append_u64_be(out, detail::encode_i64_lex(row.sort_key));
    out.append(row.user_addr);
    detail::append_u32_be(out, detail::encode_i32_lex(row.cond_idx));
    detail::append_u32_be(out, detail::encode_i32_lex(row.event_type));
    detail::append_u32_be(out, detail::encode_i32_lex(row.token_idx));
    assert(out.size() == 40);
    return out;
  }

  static std::string build_user_key(const Stage2UserEventRecord &row) {
    assert(row.user_addr.size() == kUserAddrBytes);
    std::string out;
    out.reserve(40);
    out.append(row.user_addr);
    detail::append_u64_be(out, detail::encode_i64_lex(row.sort_key));
    detail::append_u32_be(out, detail::encode_i32_lex(row.cond_idx));
    detail::append_u32_be(out, detail::encode_i32_lex(row.event_type));
    detail::append_u32_be(out, detail::encode_i32_lex(row.token_idx));
    assert(out.size() == 40);
    return out;
  }

  static std::string build_sort_seek_key(int64_t sort_key) {
    std::string out;
    out.reserve(40);
    detail::append_u64_be(out, detail::encode_i64_lex(sort_key));
    out.append(32, '\0');
    assert(out.size() == 40);
    return out;
  }

  static std::string build_value(const Stage2UserEventRecord &row) {
    std::string out;
    out.reserve(20);
    detail::append_u32_be(out, detail::encode_i32_lex(row.collateral));
    detail::append_u64_be(out, detail::encode_i64_lex(row.amount));
    detail::append_u64_be(out, detail::encode_i64_lex(row.price));
    assert(out.size() == 20);
    return out;
  }

  static void decode_value_into(std::string_view v, Stage2UserEventRecord &row) {
    assert(v.size() == 20);
    row.collateral = detail::decode_i32_lex(detail::read_u32_be(v, 0));
    row.amount = detail::decode_i64_lex(detail::read_u64_be(v, 4));
    row.price = detail::decode_i64_lex(detail::read_u64_be(v, 12));
  }

  static int32_t decode_value_collateral(std::string_view v) {
    assert(v.size() == 20);
    return detail::decode_i32_lex(detail::read_u32_be(v, 0));
  }

  static Stage2UserEventRecord decode_user_key_only(std::string_view k) {
    assert(k.size() == 40);
    Stage2UserEventRecord row;
    row.user_addr.assign(k.substr(0, kUserAddrBytes));
    row.sort_key = detail::decode_i64_lex(detail::read_u64_be(k, 20));
    row.cond_idx = detail::decode_i32_lex(detail::read_u32_be(k, 28));
    row.event_type = detail::decode_i32_lex(detail::read_u32_be(k, 32));
    row.token_idx = detail::decode_i32_lex(detail::read_u32_be(k, 36));
    return row;
  }

  static Stage2UserEventRecord decode_sort_entry(std::string_view k,
                                                 std::string_view v) {
    assert(k.size() == 40);
    Stage2UserEventRecord row;
    row.sort_key = detail::decode_i64_lex(detail::read_u64_be(k, 0));
    row.user_addr.assign(k.substr(8, kUserAddrBytes));
    row.cond_idx = detail::decode_i32_lex(detail::read_u32_be(k, 28));
    row.event_type = detail::decode_i32_lex(detail::read_u32_be(k, 32));
    row.token_idx = detail::decode_i32_lex(detail::read_u32_be(k, 36));
    decode_value_into(v, row);
    return row;
  }

  static int64_t decode_sort_entry_sort_key(std::string_view k) {
    assert(k.size() == 40);
    return detail::decode_i64_lex(detail::read_u64_be(k, 0));
  }

  static std::string_view sort_entry_user_addr(std::string_view k) {
    assert(k.size() == 40);
    return k.substr(8, kUserAddrBytes);
  }

  static int32_t decode_sort_entry_cond_idx(std::string_view k) {
    assert(k.size() == 40);
    return detail::decode_i32_lex(detail::read_u32_be(k, 28));
  }

  static int32_t decode_sort_entry_event_type(std::string_view k) {
    assert(k.size() == 40);
    return detail::decode_i32_lex(detail::read_u32_be(k, 32));
  }

  std::string db_path_;
  rocksdb_t *db_ = nullptr;
  rocksdb_options_t *options_ = nullptr;
  rocksdb_readoptions_t *read_options_ = nullptr;
  rocksdb_readoptions_t *scan_read_options_ = nullptr;
  rocksdb_writeoptions_t *write_options_ = nullptr;
  rocksdb_column_family_handle_t *default_cf_ = nullptr;
  rocksdb_column_family_handle_t *sort_cf_ = nullptr;
  rocksdb_column_family_handle_t *user_cf_ = nullptr;
};

struct Stage3EventFactRecord {
  std::string user_addr;
  int64_t sort_key = 0;
  int32_t cond_idx = 0;
  int32_t event_type = 0;
  int32_t token_idx = 0;
  int64_t realized_delta = 0;
  int64_t realized_cum = 0;
  int64_t unrealized_pnl = 0;
  int32_t token_count = 0;
  int32_t tag_id = 13;
  int64_t exposure = 0;
  int64_t volume = 0;
  int64_t holding_period = 0;
};

class Stage3EventFactStore {
public:
  static constexpr size_t kUserAddrBytes = 20;
  static constexpr const char *kCfTimeline = "timeline";

  explicit Stage3EventFactStore(const std::string &db_path) : db_path_(db_path) {
    open();
  }

  ~Stage3EventFactStore() {
    close();
  }

  Stage3EventFactStore(const Stage3EventFactStore &) = delete;
  Stage3EventFactStore &operator=(const Stage3EventFactStore &) = delete;

  void write_events(const std::vector<Stage3EventFactRecord> &rows) const {
    if (rows.empty()) {
      return;
    }
    rocksdb_writebatch_t *batch = rocksdb_writebatch_create();
    for (const auto &row : rows) {
      const std::string key = build_key(row);
      const std::string value = build_value(row);
      rocksdb_writebatch_put_cf(batch, timeline_cf_, key.data(), key.size(),
                                value.data(), value.size());
    }
    char *err = nullptr;
    rocksdb_write(db_, write_options_, batch, &err);
    rocksdb_writebatch_destroy(batch);
    detail::assert_no_err(err);
  }

  std::vector<Stage3EventFactRecord> scan_by_user(const std::string &user_addr) const {
    assert(user_addr.size() == kUserAddrBytes);
    std::vector<Stage3EventFactRecord> out;
    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, read_options_, timeline_cf_);
    rocksdb_iter_seek(it, user_addr.data(), user_addr.size());
    while (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      size_t vlen = 0;
      const char *kptr = rocksdb_iter_key(it, &klen);
      std::string_view k(kptr, klen);
      if (!detail::starts_with(k, user_addr)) {
        break;
      }
      const char *vptr = rocksdb_iter_value(it, &vlen);
      out.push_back(decode_entry(std::string_view(kptr, klen), std::string_view(vptr, vlen)));
      rocksdb_iter_next(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
    return out;
  }

  std::vector<Stage3EventFactRecord> scan_by_user_range(const std::string &user_addr, int64_t sort_key_start, int64_t sort_key_end) const {
    assert(user_addr.size() == kUserAddrBytes);
    assert(sort_key_start <= sort_key_end);
    std::vector<Stage3EventFactRecord> out;
    std::string start_key;
    start_key.reserve(40);
    start_key.append(user_addr);
    detail::append_u64_be(start_key, detail::encode_i64_lex(sort_key_start));

    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, read_options_, timeline_cf_);
    rocksdb_iter_seek(it, start_key.data(), start_key.size());
    while (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      size_t vlen = 0;
      const char *kptr = rocksdb_iter_key(it, &klen);
      std::string_view k(kptr, klen);
      if (!detail::starts_with(k, user_addr)) {
        break;
      }
      const char *vptr = rocksdb_iter_value(it, &vlen);
      Stage3EventFactRecord rec = decode_entry(std::string_view(kptr, klen), std::string_view(vptr, vlen));
      if (rec.sort_key > sort_key_end) {
        break;
      }
      if (rec.sort_key >= sort_key_start && rec.sort_key <= sort_key_end) {
        out.push_back(std::move(rec));
      }
      rocksdb_iter_next(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
    return out;
  }

  bool find_last_before_sort_key(const std::string &user_addr, int64_t sort_key_exclusive, Stage3EventFactRecord &out) const {
    assert(user_addr.size() == kUserAddrBytes);
    std::string seek_key;
    seek_key.reserve(28);
    seek_key.append(user_addr);
    detail::append_u64_be(seek_key, detail::encode_i64_lex(sort_key_exclusive));

    rocksdb_iterator_t *it = rocksdb_create_iterator_cf(db_, read_options_, timeline_cf_);
    rocksdb_iter_seek(it, seek_key.data(), seek_key.size());
    if (rocksdb_iter_valid(it) == 0) {
      rocksdb_iter_seek_to_last(it);
    } else {
      rocksdb_iter_prev(it);
    }
    bool found = false;
    while (rocksdb_iter_valid(it) != 0) {
      size_t klen = 0;
      size_t vlen = 0;
      const char *kptr = rocksdb_iter_key(it, &klen);
      std::string_view k(kptr, klen);
      if (detail::starts_with(k, user_addr)) {
        const char *vptr = rocksdb_iter_value(it, &vlen);
        out = decode_entry(std::string_view(kptr, klen), std::string_view(vptr, vlen));
        found = true;
        break;
      }
      rocksdb_iter_prev(it);
    }
    char *err = nullptr;
    rocksdb_iter_get_error(it, &err);
    rocksdb_iter_destroy(it);
    detail::assert_no_err(err);
    return found;
  }

  MemoryStats memory_stats() const {
    MemoryStats stats;
    stats.memtables_bytes = detail::property_int64(db_, "rocksdb.cur-size-all-mem-tables");
    stats.table_readers_bytes = detail::property_int64(db_, "rocksdb.estimate-table-readers-mem");
    stats.block_cache_bytes = detail::property_int64(db_, "rocksdb.block-cache-usage");
    stats.block_cache_pinned_bytes = detail::property_int64(db_, "rocksdb.block-cache-pinned-usage");
    return stats;
  }

  const std::string &db_path() const {
    return db_path_;
  }

private:
  void open() {
    options_ = detail::make_db_options();
    read_options_ = rocksdb_readoptions_create();
    write_options_ = rocksdb_writeoptions_create();
    rocksdb_writeoptions_set_sync(write_options_, 1);

    constexpr int kCfCount = 2;
    const char *names[kCfCount] = {"default", kCfTimeline};
    const rocksdb_options_t *cf_options[kCfCount] = {options_, options_};
    rocksdb_column_family_handle_t *handles[kCfCount] = {nullptr, nullptr};
    char *err = nullptr;
    db_ = rocksdb_open_column_families(options_, db_path_.c_str(), kCfCount, names, cf_options, handles, &err);
    detail::assert_no_err(err);
    assert(db_ != nullptr);
    default_cf_ = handles[0];
    timeline_cf_ = handles[1];
    assert(default_cf_ != nullptr);
    assert(timeline_cf_ != nullptr);
  }

  void close() {
    if (timeline_cf_ != nullptr) {
      rocksdb_column_family_handle_destroy(timeline_cf_);
      timeline_cf_ = nullptr;
    }
    if (default_cf_ != nullptr) {
      rocksdb_column_family_handle_destroy(default_cf_);
      default_cf_ = nullptr;
    }
    if (db_ != nullptr) {
      rocksdb_close(db_);
      db_ = nullptr;
    }
    if (write_options_ != nullptr) {
      rocksdb_writeoptions_destroy(write_options_);
      write_options_ = nullptr;
    }
    if (read_options_ != nullptr) {
      rocksdb_readoptions_destroy(read_options_);
      read_options_ = nullptr;
    }
    if (options_ != nullptr) {
      rocksdb_options_destroy(options_);
      options_ = nullptr;
    }
  }

  static std::string build_key(const Stage3EventFactRecord &row) {
    assert(row.user_addr.size() == kUserAddrBytes);
    std::string out;
    out.reserve(40);
    out.append(row.user_addr);
    detail::append_u64_be(out, detail::encode_i64_lex(row.sort_key));
    detail::append_u32_be(out, detail::encode_i32_lex(row.cond_idx));
    detail::append_u32_be(out, detail::encode_i32_lex(row.event_type));
    detail::append_u32_be(out, detail::encode_i32_lex(row.token_idx));
    assert(out.size() == 40);
    return out;
  }

  static std::string build_value(const Stage3EventFactRecord &row) {
    std::string out;
    out.reserve(56);
    detail::append_u64_be(out, detail::encode_i64_lex(row.realized_delta));
    detail::append_u64_be(out, detail::encode_i64_lex(row.realized_cum));
    detail::append_u64_be(out, detail::encode_i64_lex(row.unrealized_pnl));
    detail::append_u32_be(out, detail::encode_i32_lex(row.token_count));
    detail::append_u32_be(out, detail::encode_i32_lex(row.tag_id));
    detail::append_u64_be(out, detail::encode_i64_lex(row.exposure));
    detail::append_u64_be(out, detail::encode_i64_lex(row.volume));
    detail::append_u64_be(out, detail::encode_i64_lex(row.holding_period));
    assert(out.size() == 56);
    return out;
  }

  static Stage3EventFactRecord decode_entry(std::string_view k,
                                            std::string_view v) {
    assert(k.size() == 40);
    assert(v.size() == 56);
    Stage3EventFactRecord row;
    row.user_addr.assign(k.substr(0, kUserAddrBytes));
    row.sort_key = detail::decode_i64_lex(detail::read_u64_be(k, 20));
    row.cond_idx = detail::decode_i32_lex(detail::read_u32_be(k, 28));
    row.event_type = detail::decode_i32_lex(detail::read_u32_be(k, 32));
    row.token_idx = detail::decode_i32_lex(detail::read_u32_be(k, 36));
    row.realized_delta = detail::decode_i64_lex(detail::read_u64_be(v, 0));
    row.realized_cum = detail::decode_i64_lex(detail::read_u64_be(v, 8));
    row.unrealized_pnl = detail::decode_i64_lex(detail::read_u64_be(v, 16));
    row.token_count = detail::decode_i32_lex(detail::read_u32_be(v, 24));
    row.tag_id = detail::decode_i32_lex(detail::read_u32_be(v, 28));
    row.exposure = detail::decode_i64_lex(detail::read_u64_be(v, 32));
    row.volume = detail::decode_i64_lex(detail::read_u64_be(v, 40));
    row.holding_period = detail::decode_i64_lex(detail::read_u64_be(v, 48));
    return row;
  }

  std::string db_path_;
  rocksdb_t *db_ = nullptr;
  rocksdb_options_t *options_ = nullptr;
  rocksdb_readoptions_t *read_options_ = nullptr;
  rocksdb_writeoptions_t *write_options_ = nullptr;
  rocksdb_column_family_handle_t *default_cf_ = nullptr;
  rocksdb_column_family_handle_t *timeline_cf_ = nullptr;
};

} // namespace core::rocks
