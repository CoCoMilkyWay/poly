#include <cstring>
#include <iostream>
#include <string>

#include "api/api_server.hpp"
#include "core/config.hpp"
#include "core/database.hpp"
#include "infra/https_pool.hpp"
#include "sync/sync_incremental_coordinator.hpp"
#include "sync/sync_repair.hpp"

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

  // 大sync (修复数据完整性)
  SyncRepair sync_repair(db, pool, config);

  // HTTP 服务器 (查询 API + 大 sync 触发)
  ApiServer api_server(ioc, db, sync_repair, 8001);

  // 数据拉取 (周期性小 sync, 大 sync 运行时自动跳过)
  SyncIncrementalCoordinator sync_coordinator(config, db, pool, [&sync_repair]() { return sync_repair.is_running(); });
  sync_coordinator.start(ioc);

  ioc.run();

  return 0;
}
