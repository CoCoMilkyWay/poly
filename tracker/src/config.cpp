#include "tracker/config.hpp"
#include "tracker/codec.hpp"

#include <array>
#include <cassert>
#include <cstdlib>

namespace tracker {

UrlParts parse_url(const std::string &url) {
  size_t pos = url.find("://");
  assert(pos != std::string::npos);

  UrlParts p;
  p.scheme = to_lower(url.substr(0, pos));

  size_t host_begin = pos + 3;
  size_t path_pos = url.find('/', host_begin);
  std::string host_port = path_pos == std::string::npos
      ? url.substr(host_begin)
      : url.substr(host_begin, path_pos - host_begin);

  size_t colon = host_port.find(':');
  if (colon == std::string::npos) {
    p.host = host_port;
    p.port = p.secure() ? "443" : "80";
  } else {
    p.host = host_port.substr(0, colon);
    p.port = host_port.substr(colon + 1);
  }
  p.target = path_pos == std::string::npos ? "/" : url.substr(path_pos);

  assert(!p.host.empty());
  assert(!p.port.empty());
  assert(!p.target.empty());
  return p;
}

std::string derive_ws_url(const std::string &http_url) {
  UrlParts p = parse_url(http_url);
  std::string ws_scheme = p.scheme == "https" ? "wss" : "ws";
  return ws_scheme + "://" + p.host + ":" + p.port + p.target;
}

namespace {

RpcNode select_rpc() {
  const char *env = std::getenv("TRACKER_RPC_NAME");
  if (!env) return kDefaultRpc;
  std::string name(env);
  constexpr std::array<RpcNode, 2> nodes = {kRpcAlchemy, kRpcDrpc};
  for (const auto &n : nodes) {
    if (n.name == name) return n;
  }
  assert(false && "unknown RPC name");
  return kDefaultRpc;
}

} // namespace

AppConfig AppConfig::load(const std::filesystem::path &tracker_dir) {
  assert(std::filesystem::exists(tracker_dir));

  AppConfig c;

  // paths
  c.tracker_dir     = std::filesystem::weakly_canonical(tracker_dir);
  c.address_file    = c.tracker_dir / "address.txt";
  c.meta_file       = c.tracker_dir / "meta.json";
  c.snapshot_file   = c.tracker_dir / "snapshot.json";
  c.history_file    = c.tracker_dir / "history.json";
  c.aggregate_file  = c.tracker_dir / "aggregate.json";
  c.log_file        = c.tracker_dir / "sync.log";
  c.seed_file       = c.tracker_dir / "rebuild.json";

  // rpc
  RpcNode node = select_rpc();
  c.rpc_name = std::string(node.name);
  c.rpc_http_url = std::string(node.url);
  if (const char *env = std::getenv("TRACKER_RPC_HTTP_URL")) c.rpc_http_url = env;
  c.rpc_ws_url = derive_ws_url(c.rpc_http_url);
  if (const char *env = std::getenv("TRACKER_RPC_WS_URL")) c.rpc_ws_url = env;

  // api keys
  c.graph_api_key = kGraphApiKey;
  if (const char *env = std::getenv("THE_GRAPH_API_KEY")) c.graph_api_key = env;

  // server
  c.backend_host = kBackendHost;
  c.backend_port = kBackendPort;
  c.frontend_port = kFrontendPort;
  if (const char *env = std::getenv("TRACKER_BACKEND_HOST")) c.backend_host = env;
  if (const char *env = std::getenv("TRACKER_BACKEND_PORT")) c.backend_port = static_cast<uint16_t>(std::stoi(env));
  if (const char *env = std::getenv("TRACKER_FRONTEND_PORT")) c.frontend_port = static_cast<uint16_t>(std::stoi(env));

  // sync
  c.resync_interval_sec = kResyncIntervalSec;
  c.topic_group_size = kTopicGroupSize;
  c.get_logs_block_span = kGetLogsBlockSpan;
  if (const char *env = std::getenv("TRACKER_RESYNC_INTERVAL_SEC")) c.resync_interval_sec = static_cast<uint32_t>(std::stoul(env));
  if (const char *env = std::getenv("TRACKER_TOPIC_GROUP_SIZE")) c.topic_group_size = static_cast<uint32_t>(std::stoul(env));
  if (const char *env = std::getenv("TRACKER_GET_LOGS_BLOCK_SPAN")) c.get_logs_block_span = static_cast<uint32_t>(std::stoul(env));

  // limits
  c.recent_event_limit = kRecentEventLimit;
  c.user_batch_limit = kUserQueryBatchLimit;
  c.graph_id_batch_limit = kGraphIdBatchLimit;
  c.graph_page_limit = kGraphPageLimit;
  c.http_concurrency = kHttpConcurrency;
  if (const char *env = std::getenv("TRACKER_RECENT_EVENT_LIMIT")) c.recent_event_limit = static_cast<size_t>(std::stoull(env));
  if (const char *env = std::getenv("TRACKER_HTTP_CONCURRENCY")) c.http_concurrency = static_cast<size_t>(std::stoull(env));

  // assertions
  assert(std::filesystem::exists(c.address_file));
  assert(!c.rpc_name.empty());
  assert(!c.rpc_http_url.empty());
  assert(!c.rpc_ws_url.empty());
  assert(!c.graph_api_key.empty());
  assert(c.backend_port > 0);
  assert(c.frontend_port > 0);

  return c;
}

} // namespace tracker
