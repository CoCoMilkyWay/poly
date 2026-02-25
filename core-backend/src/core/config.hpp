#pragma once

#include <cassert>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

struct Config {
  std::string db_path_stage1;
  std::string db_path_stage2;
  std::string rpc_name;
  std::string rpc_url;
  std::string rpc_api_key;
  int stage1_rpc_query_threads;
  int stage1_rpc_sync_chunk_basics;
  int backend_port;
  int frontend_port;
  int64_t initial_block;

  static Config load(const std::string &path) {
    std::ifstream f(path);
    assert(f.is_open() && "无法打开配置文件");

    json j;
    f >> j;

    auto require = [&](const char *key) {
      assert(j.contains(key) && "配置文件缺少必填字段");
      return j[key];
    };

    Config config;
    config.db_path_stage1 = require("db_path_stage1").get<std::string>();
    config.db_path_stage2 = require("db_path_stage2").get<std::string>();
    config.backend_port = require("backend_port").get<int>();
    config.frontend_port = require("frontend_port").get<int>();
    config.initial_block = require("initial_block").get<int64_t>();
    config.stage1_rpc_query_threads = require("stage1_rpc_query_threads").get<int>();
    config.stage1_rpc_sync_chunk_basics = require("stage1_rpc_sync_chunk_basics").get<int>();

    std::string active = require("active_rpc").get<std::string>();
    const auto &nodes = require("rpc_nodes");
    for (const auto &node : nodes) {
      if (node["name"].get<std::string>() == active) {
        config.rpc_name = node["name"].get<std::string>();
        config.rpc_url = node["url"].get<std::string>();
        config.rpc_api_key = node.value("key", "");
        break;
      }
    }
    assert(!config.rpc_url.empty() && "active_rpc 在 rpc_nodes 中未找到");

    return config;
  }
};
