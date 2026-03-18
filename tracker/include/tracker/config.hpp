#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tracker {

struct UrlParts {
  std::string scheme;
  std::string host;
  std::string port;
  std::string target;

  [[nodiscard]] bool secure() const {
    return scheme == "https" || scheme == "wss";
  }

  [[nodiscard]] bool websocket() const {
    return scheme == "ws" || scheme == "wss";
  }
};

struct AppConfig {
  std::filesystem::path tracker_dir;
  std::filesystem::path address_file;
  std::filesystem::path meta_file;
  std::filesystem::path aggregate_file;
  std::filesystem::path history_file;
  std::filesystem::path seed_rebuild_file;

  std::string rpc_name;
  std::string rpc_http_url;
  std::string rpc_ws_url;
  std::string graph_api_key;
  std::string backend_host;
  uint16_t backend_port = 8871;
  uint16_t frontend_port = 8870;
  uint32_t resync_interval_sec = 300;
  uint32_t topic_group_size = 50;
  uint32_t get_logs_block_span = 400;
  size_t recent_event_limit = 512;
  size_t user_query_batch_limit = 50;
  size_t graph_id_batch_limit = 100;
  size_t graph_page_limit = 1000;

  static AppConfig load(const std::filesystem::path &tracker_dir);
};

UrlParts parse_url(const std::string &url);
std::string derive_ws_url(const std::string &http_url);

} // namespace tracker
