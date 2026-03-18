#include "tracker/config.hpp"
#include "tracker/common.hpp"

#include <array>
#include <cstdlib>

namespace tracker {

UrlParts parse_url(const std::string &url) {
  const size_t scheme_pos = url.find("://");
  assert(scheme_pos != std::string::npos);
  UrlParts parts;
  parts.scheme = to_lower_ascii(url.substr(0, scheme_pos));

  const size_t host_begin = scheme_pos + 3;
  const size_t path_pos = url.find('/', host_begin);
  const std::string host_port =
      path_pos == std::string::npos
          ? url.substr(host_begin)
          : url.substr(host_begin, path_pos - host_begin);
  const size_t colon_pos = host_port.find(':');
  if (colon_pos == std::string::npos) {
    parts.host = host_port;
    parts.port = parts.secure() ? "443" : "80";
  } else {
    parts.host = host_port.substr(0, colon_pos);
    parts.port = host_port.substr(colon_pos + 1);
  }
  parts.target = path_pos == std::string::npos ? "/" : url.substr(path_pos);
  assert(!parts.host.empty());
  assert(!parts.port.empty());
  assert(!parts.target.empty());
  return parts;
}

std::string derive_ws_url(const std::string &http_url) {
  const UrlParts parts = parse_url(http_url);
  const std::string ws_scheme = parts.scheme == "https" ? "wss" : "ws";
  return ws_scheme + "://" + parts.host + ":" + parts.port + parts.target;
}

namespace {

RpcNode select_rpc_node() {
  const char *env_name = std::getenv("TRACKER_RPC_NAME");
  if (env_name == nullptr) {
    return kDefaultRpc;
  }
  const std::string name(env_name);
  constexpr std::array<RpcNode, 3> kAllNodes = {kRpcAlchemy, kRpcDrpc};
  for (const RpcNode &node : kAllNodes) {
    if (node.name == name) {
      return node;
    }
  }
  assert(false && "unknown RPC name in TRACKER_RPC_NAME env");
  return kDefaultRpc;
}

} // namespace

AppConfig AppConfig::load(const std::filesystem::path &tracker_dir) {
  assert(std::filesystem::exists(tracker_dir));
  AppConfig config;

  // paths
  config.tracker_dir = std::filesystem::weakly_canonical(tracker_dir);
  config.address_file = config.tracker_dir / "address.txt";
  config.meta_file = config.tracker_dir / "meta.json";
  config.aggregate_file = config.tracker_dir / "aggregate.json";
  config.history_file = config.tracker_dir / "history.json";
  config.seed_rebuild_file = config.tracker_dir / "rebuild.json";

  // RPC (from constants, env override for URL)
  const RpcNode node = select_rpc_node();
  config.rpc_name = std::string(node.name);
  config.rpc_http_url = std::string(node.url);
  if (const char *env_http = std::getenv("TRACKER_RPC_HTTP_URL")) {
    config.rpc_http_url = env_http;
  }
  config.rpc_ws_url = derive_ws_url(config.rpc_http_url);
  if (const char *env_ws = std::getenv("TRACKER_RPC_WS_URL")) {
    config.rpc_ws_url = env_ws;
  }

  // Graph API key
  config.graph_api_key = kGraphApiKey;
  if (const char *env_key = std::getenv("THE_GRAPH_API_KEY")) {
    config.graph_api_key = env_key;
  }

  // server settings (from constants, env override)
  config.backend_host = kBackendHost;
  config.backend_port = kBackendPort;
  config.frontend_port = kFrontendPort;
  if (const char *env_host = std::getenv("TRACKER_BACKEND_HOST")) {
    config.backend_host = env_host;
  }
  if (const char *env_port = std::getenv("TRACKER_BACKEND_PORT")) {
    config.backend_port = static_cast<uint16_t>(std::stoi(env_port));
  }
  if (const char *env_port = std::getenv("TRACKER_FRONTEND_PORT")) {
    config.frontend_port = static_cast<uint16_t>(std::stoi(env_port));
  }

  // sync settings (from constants, env override)
  config.resync_interval_sec = kResyncIntervalSec;
  config.topic_group_size = kTopicGroupSize;
  config.get_logs_block_span = kGetLogsBlockSpan;
  if (const char *env_val = std::getenv("TRACKER_RESYNC_INTERVAL_SEC")) {
    config.resync_interval_sec = static_cast<uint32_t>(std::stoul(env_val));
  }
  if (const char *env_val = std::getenv("TRACKER_TOPIC_GROUP_SIZE")) {
    config.topic_group_size = static_cast<uint32_t>(std::stoul(env_val));
  }
  if (const char *env_val = std::getenv("TRACKER_GET_LOGS_BLOCK_SPAN")) {
    config.get_logs_block_span = static_cast<uint32_t>(std::stoul(env_val));
  }

  // query limits (from constants, env override)
  config.recent_event_limit = kRecentEventLimit;
  config.user_query_batch_limit = kUserQueryBatchLimit;
  config.graph_id_batch_limit = kGraphIdBatchLimit;
  config.graph_page_limit = kGraphPageLimit;
  if (const char *env_val = std::getenv("TRACKER_RECENT_EVENT_LIMIT")) {
    config.recent_event_limit = static_cast<size_t>(std::stoull(env_val));
  }

  // http client
  config.http_concurrency = kHttpConcurrency;
  if (const char *env_val = std::getenv("TRACKER_HTTP_CONCURRENCY")) {
    config.http_concurrency = static_cast<size_t>(std::stoull(env_val));
  }

  // assertions
  assert(std::filesystem::exists(config.address_file));
  assert(!config.rpc_name.empty());
  assert(!config.rpc_http_url.empty());
  assert(!config.rpc_ws_url.empty());
  assert(!config.graph_api_key.empty());
  assert(config.backend_port > 0);
  assert(config.frontend_port > 0);
  assert(config.resync_interval_sec > 0);
  assert(config.topic_group_size > 0);
  assert(config.get_logs_block_span > 0);
  assert(config.recent_event_limit > 0);
  assert(config.user_query_batch_limit > 0);
  assert(config.graph_id_batch_limit > 0);
  assert(config.graph_page_limit > 0);
  assert(config.http_concurrency > 0);
  return config;
}

} // namespace tracker
