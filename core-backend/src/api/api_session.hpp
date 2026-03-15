#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "../core/database.hpp"
#include "../stage3/stage3_sync.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

struct Stage0Status {
  bool syncing = false;
  int64_t last_block = 0;
  int64_t head_block = 0;
  int64_t behind_blocks = 0;
  int64_t condition_count = 0;
  int64_t ctf_condition_count = 0;
  int64_t negrisk_condition_count = 0;
  int64_t nonpoly_condition_count = 0;
  double blocks_per_second = 0.0;
  double eta_seconds = -1.0;
  // Tag status
  int64_t tag_last_block = 0;
  int64_t tagged_count = 0;
  int64_t untagged_count = 0;
  std::string tag_device;
  std::string tag_model_path;
  std::string tag_model_size_text;
};

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
  int64_t max_bucket = -1;
  int64_t bucket_user_count = 0;
};

class ApiSession : public std::enable_shared_from_this<ApiSession> {
public:
  using Stage0Getter = std::function<Stage0Status()>;
  using Stage0Retagger = std::function<void()>;
  using Stage1Getter = std::function<Stage1Status()>;
  using Stage2Getter = std::function<Stage2Status()>;
  using Stage3Getter = std::function<Stage3Status()>;
  using Stage0MemGetter = std::function<json()>;
  using Stage1MemGetter = std::function<json()>;
  using Stage2MemGetter = std::function<json()>;
  using Stage3MemGetter = std::function<json()>;

  ApiSession(tcp::socket socket, Database &stage0_db, Database &stage1_db, Database &stage2_db, Database &stage3_db,
             stage3::StageSync &stage3,
             Stage0Getter stage0_getter, Stage0Retagger stage0_retagger, Stage1Getter stage1_getter,
             Stage2Getter stage2_getter, Stage3Getter stage3_getter,
             Stage0MemGetter stage0_mem_getter, Stage1MemGetter stage1_mem_getter,
             Stage2MemGetter stage2_mem_getter, Stage3MemGetter stage3_mem_getter);

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
  void handle_stage0_status();
  void handle_stage0_retag();
  void handle_stage1_status();
  void handle_stage2_status();
  void handle_stage2_data();
  void handle_stage3_status();
  void handle_stage3_filter();
  void handle_stage3_pnl();
  void handle_stage3_positions();
  void handle_memory();
  std::string get_param(const char *name);
  static std::string url_decode(const std::string &str);
  void do_write();

  struct Stage3CondMeta {
    std::string tag = "unknown";
    std::string question = "unknown";
    std::string description = "";
  };
  std::unordered_map<uint32_t, Stage3CondMeta>
  load_stage3_cond_meta(const std::vector<uint32_t> &cond_idxs);
  static std::string normalize_stage3_user(const std::string &addr);
  static std::unordered_map<uint32_t, Stage3CondMeta> stage3_cond_meta_from_cache(const std::string &user);
  static void stage3_store_cond_meta_cache(const std::string &user,
                                           std::unordered_map<uint32_t, Stage3CondMeta> cond_meta);

  tcp::socket socket_;
  Database &stage0_db_;
  Database &stage1_db_;
  Database &stage2_db_;
  Database &stage3_db_;
  stage3::StageSync &stage3_;
  Stage0Getter stage0_getter_;
  Stage0Retagger stage0_retagger_;
  Stage1Getter stage1_getter_;
  Stage2Getter stage2_getter_;
  Stage3Getter stage3_getter_;
  Stage0MemGetter stage0_mem_getter_;
  Stage1MemGetter stage1_mem_getter_;
  Stage2MemGetter stage2_mem_getter_;
  Stage3MemGetter stage3_mem_getter_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;

  static std::mutex s3_meta_cache_mu_;
  static std::string s3_meta_cache_user_lower_;
  static std::unordered_map<uint32_t, Stage3CondMeta> s3_meta_cache_cond_meta_;
};
