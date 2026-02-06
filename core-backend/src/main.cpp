#include <cstring>
#include <iostream>
#include <string>

#include "config.hpp"
#include "db.hpp"
#include "http_server.hpp"
#include "https_pool.hpp"
#include "puller.hpp"

void print_usage(const char *prog) {
  std::cout << "用法: " << prog << " --config <config.json>" << std::endl;
}

int main(int argc, char *argv[]) {
  std::string config_path = "config.json";

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      print_usage(argv[0]);
      return 0;
    }
  }

  std::cout << "========================================" << std::endl;
  std::cout << "    Polymarket Data Syncer" << std::endl;
  std::cout << "========================================" << std::endl;

  Config config = Config::load(config_path);

  std::cout << "[Main] API Key: " << config.api_key.substr(0, 8) << "..." << std::endl;
  std::cout << "[Main] DB Path: " << config.db_path << std::endl;
  std::cout << "[Main] Sync Interval: " << config.sync_interval_seconds << "s" << std::endl;
  std::cout << "[Main] Active Sources: " << config.sources.size() << std::endl;
  for (const auto &src : config.sources) {
    std::cout << "[Main]   - " << src.name << " (" << src.entities.size() << " entities)" << std::endl;
  }

  Database db(config.db_path);

  asio::io_context ioc;

  // HTTPS 连接池
  HttpsPool pool(ioc, config.api_key);

  // HTTP 服务器 (查询 API + 大 sync 触发)
  HttpServer http_server(ioc, db, pool, config, 8001);

  // 数据拉取 (周期性小 sync, 不返回)
  Puller puller(config, db, pool);
  puller.start(ioc);

  ioc.run();

  return 0;
}
