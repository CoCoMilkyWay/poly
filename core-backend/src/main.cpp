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
#include "stage2/stage2_sync.hpp"
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
  constexpr int64_t kStage1ChunkBlocks = 100000;
  assert(config.stage1_rpc_sync_chunk_basics > 0);
  assert(kStage1ChunkBlocks % config.stage1_rpc_sync_chunk_basics == 0);
  int64_t stage1_basic_chunk_blocks = kStage1ChunkBlocks / config.stage1_rpc_sync_chunk_basics;

  std::cout << "[Main] Stage1 DB: " << config.db_path_stage1 << std::endl;
  std::cout << "[Main] Stage2 DB: " << config.db_path_stage2 << std::endl;
  std::cout << "[Main] Stage3 DB: " << config.db_path_stage3 << std::endl;
  std::cout << "[Main] RPC Node: " << config.rpc_name << " (" << config.rpc_url << ")" << std::endl;
  std::cout << "[Main] RPC Transport: " << config.rpc_transport << std::endl;
  std::cout << "[Main] Stage1 Enable: " << config.stage1_enable << std::endl;
  std::cout << "[Main] Stage2 Enable: " << config.stage2_enable << std::endl;
  std::cout << "[Main] Stage3 Enable: " << config.stage3_enable << std::endl;
  std::cout << "[Main] RPC Chunk: " << stage1_basic_chunk_blocks << " blocks (computed)" << std::endl;
  std::cout << "[Main] API Port: " << config.backend_port << std::endl;

  Database stage1_db(config.db_path_stage1);
  Database stage2_db(config.db_path_stage2);
  Database stage3_db(config.db_path_stage3);
  {
    TraceN("init/stage1_db");
    stage1_db.init_schema();
  }

  stage1::StageSync stage1(config, stage1_db);
  stage2::StageSync stage2(stage1_db, stage2_db);
  stage3::StageSync stage3(stage2.builder(), stage2_db, stage3_db);

  auto stage1_getter = [&stage1]() -> Stage1Status {
    const auto s = stage1.status();
    return {s.syncing, s.last_block, s.head_block, s.behind_blocks, s.behind_chunks,
            s.blocks_per_second, s.eta_seconds, s.bytes_per_block};
  };
  auto stage2_getter = [&stage2]() -> Stage2Status {
    const auto &s = stage2.status();
    return {s.syncing, s.last_block, s.head_block, s.behind_blocks,
            s.behind_chunks, s.blocks_per_second, s.eta_seconds};
  };
  auto stage3_getter = [&stage3]() -> Stage3Status {
    const auto s = stage3.status();
    return {s.syncing, s.last_block, s.head_block, s.behind_blocks, s.behind_chunks,
            s.blocks_per_second, s.eta_seconds, s.processed_events, s.stage3_sort_key};
  };

  boost::asio::io_context stage1_ioc;
  boost::asio::io_context stage2_ioc;
  boost::asio::io_context stage3_ioc;
  std::optional<std::thread> stage1_thread;
  std::optional<std::thread> stage2_thread;
  std::optional<std::thread> stage3_thread;

  if (config.stage1_enable == 1) {
    {
      TraceN("start/stage1");
      stage1.start(stage1_ioc);
    }
    stage1_thread.emplace([&stage1_ioc]() {
      TraceThread("Stage1");
      stage1_ioc.run();
    });
  }
  if (config.stage2_enable == 1) {
    {
      TraceN("start/stage2");
      stage2.start(stage2_ioc);
    }
    stage2_thread.emplace([&stage2_ioc]() {
      TraceThread("Stage2");
      stage2_ioc.run();
    });
  }
  if (config.stage3_enable == 1) {
    {
      TraceN("start/stage3");
      stage3.start(stage3_ioc);
    }
    stage3_thread.emplace([&stage3_ioc]() {
      TraceThread("Stage3");
      stage3_ioc.run();
    });
  }

  boost::asio::io_context api_ioc;
  ApiServer api_server(api_ioc, stage1_db, stage2_db, stage3, config.backend_port,
                       stage1_getter, stage2_getter, stage3_getter);

  boost::asio::signal_set signals(api_ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code &, int sig) {
    std::cout << "\n[Main] 正在关闭..." << std::endl;
    if (config.stage1_enable == 1) {
      stage1.stop();
    }
    if (config.stage2_enable == 1) {
      stage2.stop();
    }
    if (config.stage3_enable == 1) {
      stage3.stop();
    }
    stage1_ioc.stop();
    stage2_ioc.stop();
    stage3_ioc.stop();
    api_ioc.stop();
  });

  std::cout << "[Main] 服务已启动" << std::endl;
  api_ioc.run();

  if (stage1_thread.has_value()) {
    stage1_thread->join();
  }
  if (stage2_thread.has_value()) {
    stage2_thread->join();
  }
  if (stage3_thread.has_value()) {
    stage3_thread->join();
  }

  std::cout << "[Main] 已退出" << std::endl;
  return 0;
}
