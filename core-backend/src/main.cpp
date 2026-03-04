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
#include "stage0/stage0_sync.hpp"
#include "stage1/stage1_sync.hpp"
#include "stage2/stage2_sync.hpp"
#include "stage3/stage3_sync.hpp"

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
  assert(config.stage1_rpc_block_span > 0);
  assert(config.stage1_rpc_buffer_multiplier >= 1.0);

  std::cout << "[Main] Stage0 DB: " << config.db_path_stage0 << std::endl;
  std::cout << "[Main] Stage1 DB: " << config.db_path_stage1 << std::endl;
  std::cout << "[Main] Stage2 DB: " << config.db_path_stage2 << std::endl;
  std::cout << "[Main] Stage3 DB: " << config.db_path_stage3 << std::endl;
  std::cout << "[Main] RPC Node: " << config.rpc_name << " (" << config.rpc_url << ")" << std::endl;
  std::cout << "[Main] RPC Transport: " << config.rpc_transport << std::endl;
  std::cout << "[Main] Stage0 Enable: " << config.stage0_enable << std::endl;
  std::cout << "[Main] Stage1 Enable: " << config.stage1_enable << std::endl;
  std::cout << "[Main] Stage2 Enable: " << config.stage2_enable << std::endl;
  std::cout << "[Main] Stage3 Enable: " << config.stage3_enable << std::endl;
  std::cout << "[Main] Stage1 RPC Block Span: " << config.stage1_rpc_block_span << std::endl;
  std::cout << "[Main] Stage1 RPC Buffer Multiplier: " << config.stage1_rpc_buffer_multiplier << std::endl;
  std::cout << "[Main] Stage0 Check Interval: " << config.stage0_check_interval_seconds << "s" << std::endl;
  std::cout << "[Main] Stage1 Check Interval: " << config.stage1_check_interval_seconds << "s" << std::endl;
  std::cout << "[Main] Stage2 Check Interval: " << config.stage2_check_interval_seconds << "s" << std::endl;
  std::cout << "[Main] Stage3 Check Interval: " << config.stage3_check_interval_seconds << "s" << std::endl;
  std::cout << "[Main] API Port: " << config.backend_port << std::endl;

  Database stage0_db(config.db_path_stage0);
  Database stage1_db(config.db_path_stage1);
  Database stage2_db(config.db_path_stage2);
  Database stage3_db(config.db_path_stage3);
  {
    TraceN("init/stage1_db");
    stage1_db.init_schema();
  }

  stage0::StageSync stage0(config, stage1_db, stage0_db, config.stage0_check_interval_seconds);
  stage1::StageSync stage1(config, stage1_db, config.stage1_check_interval_seconds);
  stage2::StageSync stage2(stage1_db, stage2_db, config.stage2_check_interval_seconds);
  stage3::StageSync stage3(stage2.builder(), stage2_db, stage3_db, config.stage3_check_interval_seconds);

  auto stage0_getter = [&stage0]() -> Stage0Status {
    const auto s = stage0.status();
    return {s.syncing, s.last_block, s.head_block, s.behind_blocks, s.condition_count,
            s.ctf_condition_count, s.negrisk_condition_count, s.nonpoly_condition_count,
            s.blocks_per_second, s.eta_seconds, s.tag_last_block, s.tagged_count, s.untagged_count};
  };
  auto stage0_retagger = [&stage0]() {
    stage0.reset_tag_progress();
  };
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

  boost::asio::io_context stage0_ioc;
  boost::asio::io_context stage1_ioc;
  boost::asio::io_context stage2_ioc;
  boost::asio::io_context stage3_ioc;
  std::optional<std::thread> stage0_thread;
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
  if (config.stage0_enable == 1) {
    {
      TraceN("start/stage0");
      start_stage(config.stage0_enable, "Stage0", stage0, stage0_ioc, stage0_thread);
    }
  }
  if (config.stage1_enable == 1) {
    {
      TraceN("start/stage1");
      start_stage(config.stage1_enable, "Stage1", stage1, stage1_ioc, stage1_thread);
    }
  }
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
                       stage0_getter, stage0_retagger, stage1_getter, stage2_getter, stage3_getter);

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
    stop_stage("Stage0", config.stage0_enable, stage0);
    stop_stage("Stage1", config.stage1_enable, stage1);
    stop_stage("Stage2", config.stage2_enable, stage2);
    stop_stage("Stage3", config.stage3_enable, stage3);
    std::cout << "[Main] 停止 Stage0 io_context..." << std::endl;
    stage0_ioc.stop();
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

  auto join_stage = [](const char *name, std::optional<std::thread> &thread) {
    if (!thread.has_value()) {
      std::cout << "[Main] " << name << " 线程未启动,跳过 join" << std::endl;
      return;
    }
    std::cout << "[Main] 等待 " << name << " 线程 join..." << std::endl;
    thread->join();
    std::cout << "[Main] " << name << " 线程已 join" << std::endl;
  };
  join_stage("Stage0", stage0_thread);
  join_stage("Stage1", stage1_thread);
  join_stage("Stage2", stage2_thread);
  join_stage("Stage3", stage3_thread);

  std::cout << "[Main] 已退出" << std::endl;
  return 0;
}
