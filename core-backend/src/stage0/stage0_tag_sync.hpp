#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <boost/asio.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "stage0_tag.hpp"

namespace asio = boost::asio;

namespace stage0 {

class TagSync {
public:
  struct Status {
    int64_t tag_last_block = 0;
    int64_t tagged_count = 0;
    int64_t untagged_count = 0;
    std::string tag_device;
    std::string tag_model_path;
    std::string tag_model_size_text;
  };

  TagSync(const Config &config, Database &stage0_db, int base_interval_seconds);

  void start(asio::io_context &ioc);
  void stop();
  Status status() const;
  void reset_progress();

private:
  void schedule_sync(int delay_seconds);
  void do_sync_tick();

  void init_schema();
  void init_tagger();
  void load_tag_counts();
  int64_t do_tag_sync();
  int64_t get_tag_cursor();
  void set_tag_cursor_in_txn(duckdb::Connection &conn, int64_t cursor);

  const Config &config_;
  Database &stage0_db_;
  asio::io_context *ioc_ = nullptr;
  int base_interval_seconds_ = 0;
  std::atomic<bool> stop_requested_{false};
  mutable std::mutex status_mutex_;
  Status sync_;

  std::unique_ptr<Tagger> tagger_;
  std::atomic<uint64_t> tag_reset_epoch_{0};
  int64_t tag_last_block_ = 0;
  int64_t tagged_count_ = 0;
  int64_t untagged_count_ = 0;
};

} // namespace stage0
