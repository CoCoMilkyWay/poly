#include <cstring>
#include <iostream>
#include <string>

#include "api/api_server.hpp"
#include "core/config.hpp"
#include "core/database.hpp"
#include "sync/sync_coordinator.hpp"

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
  std::cout << "    Polymarket Backend" << std::endl;
  std::cout << "========================================" << std::endl;

  Config config = Config::load(config_path);

  std::cout << "[Main] DB Path: " << config.db_path << std::endl;
  std::cout << "[Main] RPC URL: " << config.rpc_url << std::endl;
  std::cout << "[Main] API Port: " << config.api_port << std::endl;
  std::cout << "[Main] Sync Batch: " << config.sync_batch_size << " blocks" << std::endl;
  std::cout << "[Main] Sync Interval: " << config.sync_interval_seconds << "s" << std::endl;

  Database db(config.db_path);
  db.init_schema();

  SyncCoordinator sync(config, db);

  auto sync_getter = [&sync]() -> SyncStatus {
    return {sync.is_syncing(), sync.get_head_block()};
  };

  boost::asio::io_context ioc;
  ApiServer api_server(ioc, db, config.api_port, sync_getter);

  sync.start(ioc);

  std::cout << "[Main] 服务已启动" << std::endl;
  ioc.run();

  return 0;
}
