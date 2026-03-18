#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace tracker {

// ============================================================================
// RPC Node Configuration
// ============================================================================

struct RpcNode {
  std::string_view name;
  std::string_view url;
};

// available RPC endpoints
inline constexpr RpcNode kRpcAlchemy  = {"Alchemy", "https://polygon-mainnet.g.alchemy.com/v2/BkfGdYfzIqosg_dYeRWop"};
inline constexpr RpcNode kRpcDrpc     = {"dRPC",    "https://lb.drpc.org/ogrpc?network=polygon&dkey=Aj9Y8zpk1EVEkcL8w3Z1mGcHAZE3DdUR8biw-uF7NYYO"};

// default active RPC (can be overridden by TRACKER_RPC_NAME env)
inline constexpr RpcNode kDefaultRpc = kRpcAlchemy;

// ============================================================================
// The Graph API Configuration
// ============================================================================

inline constexpr const char *kGraphApiKey = "1d7a83f3e6778cd93dfbae707bb192de";

// ============================================================================
// Server Configuration
// ============================================================================

inline constexpr const char *kBackendHost     = "0.0.0.0";  // bind address for backend HTTP server
inline constexpr uint16_t    kBackendPort     = 8871;       // backend API port
inline constexpr uint16_t    kFrontendPort    = 8870;       // frontend dev server port

// ============================================================================
// Sync & Polling Configuration
// ============================================================================

inline constexpr uint32_t kResyncIntervalSec  = 300;   // interval between full resyncs (seconds)
inline constexpr uint32_t kTopicGroupSize     = 50;    // topics per eth_getLogs batch
inline constexpr uint32_t kGetLogsBlockSpan   = 400;   // block range per eth_getLogs call

// ============================================================================
// Query Limits
// ============================================================================

inline constexpr size_t kRecentEventLimit     = 512;   // max recent events to keep in memory
inline constexpr size_t kUserQueryBatchLimit  = 50;    // max items per user query batch
inline constexpr size_t kGraphIdBatchLimit    = 100;   // max IDs per Graph query batch
inline constexpr size_t kGraphPageLimit       = 1000;  // max items per Graph page

// ============================================================================
// HTTP Client Configuration
// ============================================================================

inline constexpr size_t kHttpConcurrency      = 10;    // max concurrent HTTP requests

// ============================================================================
// URL Parsing Utilities
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
// Application Configuration (runtime state)
// ============================================================================

struct AppConfig {
  // paths (derived from tracker_dir)
  std::filesystem::path tracker_dir;
  std::filesystem::path address_file;
  std::filesystem::path meta_file;
  std::filesystem::path snapshot_file;   // 用户持仓快照 (key: user, block_num)
  std::filesystem::path history_file;    // 交易记录 (key: user, block_num)
  std::filesystem::path aggregate_file;  // 聚合仓位 (实时计算的当前状态)
  std::filesystem::path log_file;
  std::filesystem::path seed_rebuild_file;

  // RPC settings (from constants or env override)
  std::string rpc_name;
  std::string rpc_http_url;
  std::string rpc_ws_url;

  // API keys
  std::string graph_api_key;

  // server settings
  std::string backend_host;
  uint16_t    backend_port;
  uint16_t    frontend_port;

  // sync settings
  uint32_t resync_interval_sec;
  uint32_t topic_group_size;
  uint32_t get_logs_block_span;

  // query limits
  size_t recent_event_limit;
  size_t user_query_batch_limit;
  size_t graph_id_batch_limit;
  size_t graph_page_limit;

  // http client
  size_t http_concurrency;

  static AppConfig load(const std::filesystem::path &tracker_dir);
};

} // namespace tracker
