#include "tracker/config.hpp"
#include "tracker/common.hpp"

#include <cstdlib>
#include <fstream>

namespace tracker {
namespace {

inline constexpr const char *kDefaultGraphApiKey = "1d7a83f3e6778cd93dfbae707bb192de";

}

UrlParts parse_url(const std::string &url) {
  const size_t scheme_pos = url.find("://");
  assert(scheme_pos != std::string::npos);
  UrlParts parts;
  parts.scheme = to_lower_ascii(url.substr(0, scheme_pos));

  const size_t host_begin = scheme_pos + 3;
  const size_t path_pos = url.find('/', host_begin);
  const std::string host_port = path_pos == std::string::npos ? url.substr(host_begin) : url.substr(host_begin, path_pos - host_begin);
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

AppConfig AppConfig::load(const std::filesystem::path &tracker_dir) {
  assert(std::filesystem::exists(tracker_dir));
  AppConfig config;
  config.tracker_dir = std::filesystem::weakly_canonical(tracker_dir);
  config.address_file = config.tracker_dir / "address.txt";
  config.meta_file = config.tracker_dir / "meta.json";
  config.aggregate_file = config.tracker_dir / "aggregate.json";
  config.history_file = config.tracker_dir / "history.json";
  config.seed_rebuild_file = config.tracker_dir / "rebuild.json";

  const std::filesystem::path config_file = config.tracker_dir / "config.json";
  std::ifstream in(config_file);
  assert(in.is_open());
  json payload;
  in >> payload;
  assert(payload.is_object());

  if (payload.contains("rpc_http_url")) {
    config.rpc_http_url = payload.at("rpc_http_url").get<std::string>();
    config.rpc_name = payload.value("rpc_name", std::string("direct"));
  } else {
    const std::string active_rpc = std::getenv("TRACKER_RPC_NAME") != nullptr
                                       ? std::string(std::getenv("TRACKER_RPC_NAME"))
                                       : payload.at("active_rpc").get<std::string>();
    const json rpc_nodes = payload.at("rpc_nodes");
    assert(rpc_nodes.is_array());
    for (const json &node : rpc_nodes) {
      if (node.at("name").get<std::string>() != active_rpc) {
        continue;
      }
      config.rpc_name = node.at("name").get<std::string>();
      config.rpc_http_url = node.at("url").get<std::string>();
      break;
    }
  }

  if (const char *env_http = std::getenv("TRACKER_RPC_HTTP_URL")) {
    config.rpc_http_url = env_http;
    if (config.rpc_name.empty()) {
      config.rpc_name = "env";
    }
  }
  assert(!config.rpc_http_url.empty());

  if (payload.contains("rpc_ws_url")) {
    config.rpc_ws_url = payload.at("rpc_ws_url").get<std::string>();
  }

  if (const char *env_ws = std::getenv("TRACKER_RPC_WS_URL")) {
    config.rpc_ws_url = env_ws;
  }
  if (config.rpc_ws_url.empty()) {
    config.rpc_ws_url = derive_ws_url(config.rpc_http_url);
  }

  config.graph_api_key = payload.value("graph_api_key", std::string(kDefaultGraphApiKey));
  if (const char *env_key = std::getenv("THE_GRAPH_API_KEY")) {
    config.graph_api_key = env_key;
  }

  config.backend_host = payload.value("backend_host", std::string("0.0.0.0"));
  if (payload.contains("backend_port")) {
    config.backend_port = payload.at("backend_port").get<uint16_t>();
  }
  if (payload.contains("frontend_port")) {
    config.frontend_port = payload.at("frontend_port").get<uint16_t>();
  }
  if (payload.contains("resync_interval_sec")) {
    config.resync_interval_sec = payload.at("resync_interval_sec").get<uint32_t>();
  }
  if (payload.contains("topic_group_size")) {
    config.topic_group_size = payload.at("topic_group_size").get<uint32_t>();
  }
  if (payload.contains("get_logs_block_span")) {
    config.get_logs_block_span = payload.at("get_logs_block_span").get<uint32_t>();
  }
  if (payload.contains("recent_event_limit")) {
    config.recent_event_limit = payload.at("recent_event_limit").get<size_t>();
  }
  if (payload.contains("user_query_batch_limit")) {
    config.user_query_batch_limit = payload.at("user_query_batch_limit").get<size_t>();
  }
  if (payload.contains("graph_id_batch_limit")) {
    config.graph_id_batch_limit = payload.at("graph_id_batch_limit").get<size_t>();
  }
  if (payload.contains("graph_page_limit")) {
    config.graph_page_limit = payload.at("graph_page_limit").get<size_t>();
  }

  if (const char *env_host = std::getenv("TRACKER_BACKEND_HOST")) {
    config.backend_host = env_host;
  }

  if (const char *env_backend_port = std::getenv("TRACKER_BACKEND_PORT")) {
    config.backend_port = static_cast<uint16_t>(std::stoi(env_backend_port));
  }
  if (const char *env_frontend_port = std::getenv("TRACKER_FRONTEND_PORT")) {
    config.frontend_port = static_cast<uint16_t>(std::stoi(env_frontend_port));
  }
  if (const char *env_resync = std::getenv("TRACKER_RESYNC_INTERVAL_SEC")) {
    config.resync_interval_sec = static_cast<uint32_t>(std::stoul(env_resync));
  }
  if (const char *env_topic_group = std::getenv("TRACKER_TOPIC_GROUP_SIZE")) {
    config.topic_group_size = static_cast<uint32_t>(std::stoul(env_topic_group));
  }
  if (const char *env_logs_span = std::getenv("TRACKER_GET_LOGS_BLOCK_SPAN")) {
    config.get_logs_block_span = static_cast<uint32_t>(std::stoul(env_logs_span));
  }
  if (const char *env_recent_limit = std::getenv("TRACKER_RECENT_EVENT_LIMIT")) {
    config.recent_event_limit = static_cast<size_t>(std::stoull(env_recent_limit));
  }

  assert(config.address_file.empty() == false);
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
  return config;
}

} // namespace tracker
