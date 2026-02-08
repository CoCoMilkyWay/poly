#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include "api/api_server.hpp"
#include "core/config.hpp"
#include "core/database.hpp"
#include "infra/https_pool.hpp"
#include "rebuild/rebuilder.hpp"
#include "sync/sync_incremental_coordinator.hpp"
#include "sync/sync_token_filler.hpp"

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

  asio::io_context ioc_api;  // API 专用
  asio::io_context ioc_sync; // sync + HTTPS 专用

  // HTTPS 连接池
  HttpsPool pool(ioc_sync, config.api_key);

  // Token ID 填充 (手动触发)
  SyncTokenFiller token_filler(db, pool, config);

  // PnL 重建引擎
  rebuild::Engine rebuild_engine(db.get_duckdb());

  // HTTP 服务器 (查询 API) — 独立线程, 不被 sync 阻塞
  ApiServer api_server(ioc_api, db, token_filler, rebuild_engine, 8001);

  // 数据拉取 (周期性增量 sync)
  SyncIncrementalCoordinator sync_coordinator(config, db, pool);
  sync_coordinator.start(ioc_sync);

  std::thread api_thread([&ioc_api]() { ioc_api.run(); });
  ioc_sync.run();
  api_thread.join();

  return 0;
}
