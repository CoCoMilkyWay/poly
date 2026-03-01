#pragma once

#include <functional>
#include <memory>
#include <string>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "../core/database.hpp"
#include "../stage3/pnl_replay.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

struct Stage1SyncStatus {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
  double bytes_per_block = 0.0;
};

struct Stage2SyncStatus {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
};

struct Stage3SyncStatus {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
  int64_t stage3_sort_key = -1;
  int64_t processed_events = 0;
};

class ApiSession : public std::enable_shared_from_this<ApiSession> {
public:
  using Stage1SyncGetter = std::function<Stage1SyncStatus()>;
  using Stage2SyncGetter = std::function<Stage2SyncStatus()>;
  using Stage3SyncGetter = std::function<Stage3SyncStatus()>;

  ApiSession(tcp::socket socket, Database &stage1_db, Database &stage2_db, stage3::StageSync &stage3_sync,
             Stage1SyncGetter stage1_getter = nullptr, Stage2SyncGetter stage2_getter = nullptr,
             Stage3SyncGetter stage3_getter = nullptr);

  void run();

private:
  void do_read();
  void handle_request();
  void handle_health();
  static int64_t feather_row_count(const std::string &path);
  void handle_tables();
  void handle_sync_state();
  void handle_query();
  void handle_export_csv();
  void handle_table_sample();
  void handle_rebuild_status();
  void handle_user_pnl(const std::string &target);
  void handle_user_positions(const std::string &target);
  void handle_replay_users();
  void handle_replay();
  void handle_replay_positions();
  void handle_replay_trades();
  static std::string extract_user_addr(const std::string &target);
  std::string get_param(const char *name);
  static std::string url_decode(const std::string &str);
  void do_write();

  tcp::socket socket_;
  Database &stage1_db_;
  Database &stage2_db_;
  stage3::StageSync &stage3_sync_;
  Stage1SyncGetter sync_getter_;
  Stage2SyncGetter stage2_getter_;
  Stage3SyncGetter stage3_getter_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};
