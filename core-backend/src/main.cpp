#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

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
  std::cout << "[Main] RPC Node: " << config.rpc_name << " (" << config.rpc_url << ")" << std::endl;
  std::cout << "[Main] RPC Chunk: " << config.rpc_chunk << " blocks" << std::endl;
  std::cout << "[Main] API Port: " << config.api_port << std::endl;
  std::cout << "[Main] Sync Interval: " << config.sync_interval_seconds << "s" << std::endl;

  Database db(config.db_path);
  db.init_schema();

  SyncCoordinator sync(config, db);

  auto sync_getter = [&sync]() -> SyncStatus {
    return {sync.is_syncing(), sync.get_head_block(), sync.get_blocks_per_second()};
  };

  // Sync 使用单独的 io_context 和线程，避免阻塞 API
  boost::asio::io_context sync_ioc;
  sync.start(sync_ioc);
  std::thread sync_thread([&sync_ioc]() { sync_ioc.run(); });

  boost::asio::io_context api_ioc;
  ApiServer api_server(api_ioc, db, config.api_port, sync_getter);

  boost::asio::signal_set signals(api_ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code &, int sig) {
    std::cout << "\n[Main] 正在关闭..." << std::endl;
    api_ioc.stop();
  });

  std::cout << "[Main] 服务已启动" << std::endl;
  api_ioc.run();

  std::cout << "[Main] 正在停止同步..." << std::endl;
  sync_ioc.stop();
  sync_thread.join();

  std::cout << "[Main] 已退出" << std::endl;
  return 0;
}
