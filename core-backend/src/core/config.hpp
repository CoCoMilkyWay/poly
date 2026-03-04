#pragma once

#include <cassert>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

struct Config {
  int stage0_enable;
  int stage1_enable;
  int stage2_enable;
  int stage3_enable;
  int stage0_check_interval_seconds;
  int stage1_check_interval_seconds;
  int stage2_check_interval_seconds;
  int stage3_check_interval_seconds;

  std::string db_path_stage0;
  std::string db_path_stage1;
  std::string db_path_stage2;
  std::string db_path_stage3;

  int stage1_rpc_threads;
  int stage1_rpc_block_span;
  double stage1_rpc_buffer_multiplier;

  std::string rpc_name;
  std::string rpc_url;
  std::string rpc_api_key;
  std::string proxy_url;
  std::string rpc_transport;

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
    config.stage0_enable = j.value("stage0_enable", 1);
    config.stage1_enable = j.value("stage1_enable", 1);
    config.stage2_enable = j.value("stage2_enable", 0);
    config.stage3_enable = j.value("stage3_enable", 1);
    assert((config.stage0_enable == 0 || config.stage0_enable == 1) && "stage0_enable 必须是 0/1");
    assert((config.stage1_enable == 0 || config.stage1_enable == 1) && "stage1_enable 必须是 0/1");
    assert((config.stage2_enable == 0 || config.stage2_enable == 1) && "stage2_enable 必须是 0/1");
    assert((config.stage3_enable == 0 || config.stage3_enable == 1) && "stage3_enable 必须是 0/1");
    config.stage0_check_interval_seconds = require("stage0_check_interval_seconds").get<int>();
    config.stage1_check_interval_seconds = require("stage1_check_interval_seconds").get<int>();
    config.stage2_check_interval_seconds = require("stage2_check_interval_seconds").get<int>();
    config.stage3_check_interval_seconds = require("stage3_check_interval_seconds").get<int>();
    assert(config.stage0_check_interval_seconds > 0);
    assert(config.stage1_check_interval_seconds > 0);
    assert(config.stage2_check_interval_seconds > 0);
    assert(config.stage3_check_interval_seconds > 0);

    config.db_path_stage0 = require("db_path_stage0").get<std::string>();
    config.db_path_stage1 = require("db_path_stage1").get<std::string>();
    config.db_path_stage2 = require("db_path_stage2").get<std::string>();
    config.db_path_stage3 = require("db_path_stage3").get<std::string>();
    config.stage1_rpc_threads = require("stage1_rpc_threads").get<int>();
    config.stage1_rpc_block_span = require("stage1_rpc_block_span").get<int>();
    config.stage1_rpc_buffer_multiplier = require("stage1_rpc_buffer_multiplier").get<double>();

    config.backend_port = require("backend_port").get<int>();
    config.frontend_port = require("frontend_port").get<int>();
    config.initial_block = require("initial_block").get<int64_t>();

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
    config.proxy_url = j.value("proxy", "");
    config.rpc_transport = j.value("rpc_transport", "beast");

    return config;
  }
};
