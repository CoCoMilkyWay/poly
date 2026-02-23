#include <csignal>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "api/api_server.hpp"
#include "core/config.hpp"
#include "core/database.hpp"
#include "misc/profiler.hpp"
#include "stage1/chain_sync.hpp"
#include "stage2/event_sync.hpp"
#include "stage3/pnl_replay.hpp"

void print_usage(const char *prog) {
  std::cout << "用法: " << prog << " --config <config.json>" << std::endl;
}

int main(int argc, char *argv[]) {
  TraceThread("Main");
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

  std::cout << "[Main] Stage1 DB: " << config.db_path_stage1 << std::endl;
  std::cout << "[Main] Stage2 DB: " << config.db_path_stage2 << std::endl;
  std::cout << "[Main] RPC Node: " << config.rpc_name << " (" << config.rpc_url << ")" << std::endl;
  std::cout << "[Main] RPC Chunk: " << config.rpc_chunk << " blocks" << std::endl;
  std::cout << "[Main] API Port: " << config.backend_port << std::endl;
  std::cout << "[Main] Sync Interval: " << config.sync_interval_seconds << "s" << std::endl;

  Database stage1_db(config.db_path_stage1);
  Database stage2_db(config.db_path_stage2);
  {
    TraceN("init/stage1_db");
    stage1_db.init_schema();
  }

  stage1::ChainSync chain_sync(config, stage1_db);

  auto stage1_getter = [&chain_sync]() -> Stage1SyncStatus {
    return {chain_sync.is_syncing(), chain_sync.get_head_block(),
            chain_sync.get_blocks_per_second(), chain_sync.get_bytes_per_block()};
  };

  boost::asio::io_context sync_ioc;
  {
    TraceN("start/stage1_sync");
    chain_sync.start(sync_ioc);
  }
  std::thread stage1_thread([&sync_ioc]() {
    TraceThread("Stage1-Sync");
    sync_ioc.run();
  });

  stage2::EventSync event_sync(stage1_db, stage2_db, config.rpc_chunk);
  boost::asio::io_context stage2_ioc;
  std::optional<std::thread> stage2_thread;
  {
    TraceN("start/stage2_sync");
    event_sync.start(stage2_ioc);
  }
  stage2_thread.emplace([&stage2_ioc]() {
    TraceThread("Stage2-Sync");
    stage2_ioc.run();
  });

  stage3::PnlEngine pnl_engine(event_sync.builder());

  auto stage2_getter = [&event_sync]() -> Stage2SyncStatus {
    const auto &p = event_sync.progress();
    return {p.syncing, p.stage1_last_block, p.stage2_cursor, p.behind_chunks};
  };

  boost::asio::io_context api_ioc;
  ApiServer api_server(api_ioc, stage1_db, pnl_engine, config.backend_port, stage1_getter, stage2_getter);

  boost::asio::signal_set signals(api_ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code &, int sig) {
    std::cout << "\n[Main] 正在关闭..." << std::endl;
    chain_sync.stop();
    event_sync.stop();
    sync_ioc.stop();
    stage2_ioc.stop();
    api_ioc.stop();
  });

  std::cout << "[Main] 服务已启动" << std::endl;
  api_ioc.run();

  stage1_thread.join();
  stage2_thread->join();

  std::cout << "[Main] 已退出" << std::endl;
  return 0;
}
