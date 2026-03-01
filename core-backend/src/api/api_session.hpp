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

struct Stage1Status {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
  double bytes_per_block = 0.0;
};

struct Stage2Status {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
};

struct Stage3Status {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t behind_chunks = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
  int64_t processed_events = 0;
  int64_t stage3_sort_key = -1;
};

class ApiSession : public std::enable_shared_from_this<ApiSession> {
public:
  using Stage1Getter = std::function<Stage1Status()>;
  using Stage2Getter = std::function<Stage2Status()>;
  using Stage3Getter = std::function<Stage3Status()>;

  ApiSession(tcp::socket socket, Database &stage1_db, Database &stage2_db, stage3::StageSync &stage3,
             Stage1Getter stage1_getter, Stage2Getter stage2_getter, Stage3Getter stage3_getter);

  void run();

private:
  void do_read();
  void handle_request();
  void handle_health();
  static int64_t feather_row_count(const std::string &path);
  void handle_tables();
  void handle_query();
  void handle_export_csv();
  void handle_table_sample();
  void handle_stage1_status();
  void handle_stage2_status();
  void handle_stage2_detail();
  void handle_stage3_status();
  void handle_stage3_users();
  void handle_stage3_data();
  void handle_stage3_positions();
  void handle_stage3_events();
  std::string get_param(const char *name);
  static std::string url_decode(const std::string &str);
  void do_write();

  tcp::socket socket_;
  Database &stage1_db_;
  Database &stage2_db_;
  stage3::StageSync &stage3_;
  Stage1Getter stage1_getter_;
  Stage2Getter stage2_getter_;
  Stage3Getter stage3_getter_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};
