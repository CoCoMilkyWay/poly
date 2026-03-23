#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace tracker {

// ============================================================================
// RPC Nodes
// ============================================================================

struct RpcNode {
  std::string_view name;
  std::string_view url;
};

inline constexpr RpcNode kRpcAlchemy = {
    "Alchemy",
    "https://polygon-mainnet.g.alchemy.com/v2/BkfGdYfzIqosg_dYeRWop"};
inline constexpr RpcNode kRpcDrpc = {
    "dRPC",
    "https://lb.drpc.org/"
    "ogrpc?network=polygon&dkey=Aj9Y8zpk1EVEkcL8w3Z1mGcHAZE3DdUR8biw-uF7NYYO"};
inline constexpr RpcNode kDefaultRpc = kRpcAlchemy;

// ============================================================================
// External APIs & Server Defaults
// ============================================================================

inline constexpr const char *kSnapshotApiUrl =
    "https://polygon-mainnet.g.alchemy.com/nft/v3/BkfGdYfzIqosg_dYeRWop";
inline constexpr const char *kBackendHost = "0.0.0.0";
inline constexpr uint16_t kBackendPort = 8871;
inline constexpr uint16_t kFrontendPort = 8870;

// ============================================================================
// Sync Settings
// ============================================================================

inline constexpr uint32_t kResyncIntervalSec = 3600;
inline constexpr uint32_t kTopicGroupSize = 128;
inline constexpr uint32_t kGetLogsBlockSpan = 2000;

// ============================================================================
// Query Limits
// ============================================================================

inline constexpr size_t kRecentEventLimit = 512;
inline constexpr size_t kGammaBatchLimit = 50;     // Gamma: condition_ids[] URL 长度限制 ~4KB
inline constexpr size_t kClobBatchLimit = 500;     // CLOB: prices[] URL 长度限制 500
inline constexpr size_t kHttpConcurrency = 10;

// ============================================================================
// Filter Thresholds
// ============================================================================
inline constexpr long double kUserTokenRatioThreshold = 0.5L;   // 有效用户: 用户token净值占比 >= x%
inline constexpr long double kTokenValueThreshold = 0.001L;     // 有效token: token价值 >=x% 账户净值
inline constexpr long double kAggregateValueThreshold = 0.001L; // 有效聚合: aggre中token价值 >=x% 账户净值

// ============================================================================
// URL Parsing
// ============================================================================

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

UrlParts parse_url(const std::string &url);
std::string derive_ws_url(const std::string &http_url);

// ============================================================================
// AppConfig
// ============================================================================

struct AppConfig {
  // paths
  std::filesystem::path tracker_dir;
  std::filesystem::path address_file;
  std::filesystem::path meta_file;
  std::filesystem::path snapshot_file;
  std::filesystem::path sync_log_file;   // log/sync.log
  std::filesystem::path event_log_file;  // log/event.log
  std::filesystem::path seed_file;
  std::filesystem::path proxy_file;

  // rpc
  std::string rpc_name;
  std::string rpc_http_url;
  std::string rpc_ws_url;

  // external APIs
  std::string snapshot_api_url;

  // server
  std::string backend_host;
  uint16_t backend_port;
  uint16_t frontend_port;

  // sync
  uint32_t resync_interval_sec;
  uint32_t topic_group_size;
  uint32_t get_logs_block_span;

  // limits
  size_t recent_event_limit;
  size_t gamma_batch_limit;
  size_t http_concurrency;

  // proxy (empty = no proxy)
  std::string proxy_url;

  static AppConfig load(const std::filesystem::path &tracker_dir);
};

} // namespace tracker
