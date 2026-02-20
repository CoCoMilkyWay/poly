#pragma once

#include <cassert>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

struct Config {
  std::string db_path;
  std::string rpc_url;
  std::string rpc_api_key;
  int api_port;
  int frontend_port;
  int sync_batch_size;
  int sync_interval_seconds;
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
    config.db_path = require("db_path").get<std::string>();
    config.rpc_url = require("rpc_url").get<std::string>();
    config.rpc_api_key = require("rpc_api_key").get<std::string>();
    config.api_port = require("api_port").get<int>();
    config.frontend_port = require("frontend_port").get<int>();
    config.sync_batch_size = require("sync_batch_size").get<int>();
    config.sync_interval_seconds = require("sync_interval_seconds").get<int>();
    config.initial_block = require("initial_block").get<int64_t>();

    return config;
  }
};
