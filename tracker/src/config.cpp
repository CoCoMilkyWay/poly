#include "tracker/config.hpp"
#include "tracker/codec.hpp"

#include <cassert>
#include <fstream>

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

AppConfig AppConfig::load(const std::filesystem::path &tracker_dir) {
  assert(std::filesystem::exists(tracker_dir));

  AppConfig c;

  // paths
  c.tracker_dir = std::filesystem::weakly_canonical(tracker_dir);
  auto data_dir = c.tracker_dir / "data";
  c.address_file = c.tracker_dir / "address.txt";
  c.proxy_file = c.tracker_dir / "proxy.txt";

  // log files in log/ subdirectory
  auto log_dir = c.tracker_dir / "log";
  c.sync_log_file = log_dir / "sync.log";
  c.event_log_file = log_dir / "event.log";

  // data files in data/ subdirectory
  c.meta_file = data_dir / "meta.json";
  c.snapshot_file = data_dir / "snapshot.json";
  c.seed_file = data_dir / "rebuild.json";

  // rpc
  c.rpc_name = std::string(kDefaultRpc.name);
  c.rpc_http_url = std::string(kDefaultRpc.url);
  c.rpc_ws_url = derive_ws_url(c.rpc_http_url);

  // external APIs
  c.snapshot_api_url = kSnapshotApiUrl;

  // server
  c.backend_host = kBackendHost;
  c.backend_port = kBackendPort;
  c.frontend_port = kFrontendPort;

  // sync
  c.resync_interval_sec = kResyncIntervalSec;
  c.topic_group_size = kTopicGroupSize;
  c.get_logs_block_span = kGetLogsBlockSpan;

  // limits
  c.recent_event_limit = kRecentEventLimit;
  c.gamma_batch_limit = kGammaBatchLimit;
  c.http_concurrency = kHttpConcurrency;

  // proxy (read from file, empty if not exists)
  if (std::filesystem::exists(c.proxy_file)) {
    std::ifstream f(c.proxy_file);
    std::getline(f, c.proxy_url);
  }

  // assertions
  assert(std::filesystem::exists(c.address_file));
  assert(!c.rpc_name.empty());
  assert(!c.rpc_http_url.empty());
  assert(!c.rpc_ws_url.empty());
  assert(!c.snapshot_api_url.empty());
  assert(c.backend_port > 0);
  assert(c.frontend_port > 0);

  return c;
}

} // namespace tracker
