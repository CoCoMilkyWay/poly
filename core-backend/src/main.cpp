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
  assert(config.stage1_rpc_chunk_basics > 0);
  assert(kStage1ChunkBlocks % config.stage1_rpc_chunk_basics == 0);
  int64_t stage1_basic_chunk_blocks = kStage1ChunkBlocks / config.stage1_rpc_chunk_basics;

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
            s.blocks_per_second, s.eta_seconds};
  };

  boost::asio::io_context stage1_ioc;
  boost::asio::io_context stage2_ioc;
  boost::asio::io_context stage3_ioc;
  std::optional<std::thread> stage1_thread;
  std::optional<std::thread> stage2_thread;
  std::optional<std::thread> stage3_thread;

  auto start_stage = [](int enable, const char *thread_name, auto &sync_stage,
                        boost::asio::io_context &ioc, std::optional<std::thread> &thread) {
    if (enable != 1) {
      return;
    }
    sync_stage.start(ioc);
    thread.emplace([&ioc, thread_name]() {
      TraceThread(thread_name);
      ioc.run();
    });
  };
  if (config.stage1_enable == 1) {
    TraceN("start/stage1");
  }
  start_stage(config.stage1_enable, "Stage1", stage1, stage1_ioc, stage1_thread);
  if (config.stage2_enable == 1) {
    TraceN("start/stage2");
  }
  start_stage(config.stage2_enable, "Stage2", stage2, stage2_ioc, stage2_thread);
  if (config.stage3_enable == 1) {
    TraceN("start/stage3");
  }
  start_stage(config.stage3_enable, "Stage3", stage3, stage3_ioc, stage3_thread);

  boost::asio::io_context api_ioc;
  ApiServer api_server(api_ioc, stage1_db, stage2_db, stage3, config.backend_port,
                       stage1_getter, stage2_getter, stage3_getter);

  boost::asio::signal_set signals(api_ioc, SIGINT, SIGTERM);
  signals.async_wait([&](const boost::system::error_code &, int sig) {
    std::cout << "\n[Main] 收到退出信号: " << sig << std::endl;
    std::cout << "[Main] 正在关闭..." << std::endl;
    auto stop_stage = [](const char *name, int enable, auto &sync_stage) {
      if (enable == 1) {
        std::cout << "[Main] 停止 " << name << "..." << std::endl;
        sync_stage.stop();
        std::cout << "[Main] " << name << " 已停止" << std::endl;
        return;
      }
      std::cout << "[Main] 跳过 " << name << " (未启用)" << std::endl;
    };
    stop_stage("Stage1", config.stage1_enable, stage1);
    stop_stage("Stage2", config.stage2_enable, stage2);
    stop_stage("Stage3", config.stage3_enable, stage3);
    std::cout << "[Main] 停止 Stage1 io_context..." << std::endl;
    stage1_ioc.stop();
    std::cout << "[Main] 停止 Stage2 io_context..." << std::endl;
    stage2_ioc.stop();
    std::cout << "[Main] 停止 Stage3 io_context..." << std::endl;
    stage3_ioc.stop();
    std::cout << "[Main] 停止 API io_context..." << std::endl;
    api_ioc.stop();
    std::cout << "[Main] 关停指令已全部发出" << std::endl;
  });

  std::cout << "[Main] 服务已启动" << std::endl;
  api_ioc.run();

  auto join_stage = [](std::optional<std::thread> &thread) {
    if (thread.has_value()) {
      thread->join();
    }
  };
  join_stage(stage1_thread);
  join_stage(stage2_thread);
  join_stage(stage3_thread);

  std::cout << "[Main] 已退出" << std::endl;
  return 0;
}
