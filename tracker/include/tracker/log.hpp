#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

namespace tracker {

// ============================================================================
// Logger
// ============================================================================

enum class LogLevel { DEBUG,
                      INFO,
                      WARN,
                      ERROR };

inline const char *log_level_str(LogLevel level) {
  switch (level) {
  case LogLevel::DEBUG:
    return "DEBUG";
  case LogLevel::INFO:
    return "INFO";
  case LogLevel::WARN:
    return "WARN";
  case LogLevel::ERROR:
    return "ERROR";
  }
  return "?";
}

class Logger {
public:
  void init(const std::filesystem::path &path) {
    std::lock_guard<std::mutex> lock(mu_);
    path_ = path;
    if (path_.has_parent_path()) {
      std::filesystem::create_directories(path_.parent_path());
    }
    out_.open(path_, std::ios::trunc);
  }

  void log(LogLevel level, const std::string &msg) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_.is_open())
      return;
    out_ << timestamp() << " [" << log_level_str(level) << "] " << msg << std::endl;
  }

  void debug(const std::string &msg) { log(LogLevel::DEBUG, msg); }
  void info(const std::string &msg) { log(LogLevel::INFO, msg); }
  void warn(const std::string &msg) { log(LogLevel::WARN, msg); }
  void error(const std::string &msg) { log(LogLevel::ERROR, msg); }

private:
  static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) %
              1000;
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
  }

  std::filesystem::path path_;
  std::ofstream out_;
  std::mutex mu_;
};

inline Logger &logger() {
  static Logger instance;
  return instance;
}

inline void log_query(const std::string &channel,
                      const std::string &name,
                      size_t attempt,
                      bool ok,
                      const std::string &detail = "") {
  std::string msg = "query channel=" + channel +
                    " name=" + name +
                    " attempt=" + std::to_string(attempt) +
                    " result=" + (ok ? "ok" : "fail");
  if (!detail.empty())
    msg += " detail=" + detail;
  if (ok)
    logger().info(msg);
  else
    logger().warn(msg);
}

// ============================================================================
// ProgressBoard - API进度状态 (供UI前端轮询)
// ============================================================================
// 终端显示格式 (按 full_resync 流程顺序):
//   API       done/total  [pend]   说明
//   snapshot  5/10        [3]      [a] Graph.userPositions    (done=完成用户数, total=用户数)
//   stables   40/40       [0]      [b] RPC.eth_call           (done=完成查询数, total=用户数*4)
//   meta      50/100      [5]      [c] Gamma.markets          (done=完成token数, total=token数)
//   prices    80/100      [5]      [d] CLOB.prices            (done=完成token数, total=token数)
//   ws_sub    10/10       [0]      [e] ws.eth_subscribe       (done=订阅用户数, total=用户数)
//   head      1/1         [0]      [f] RPC.eth_blockNumber    (done=1表示完成)
//   backfill  500/1000    [10]     [g] RPC.eth_getLogs        (done=已处理区块数, total=区块范围)
//   [current_stage]

enum class API { snapshot, // [a] 用户持仓快照
                 stables,  // [b] 稳定币余额
                 meta,     // [c] token/condition 元数据
                 prices,   // [d] 价格刷新
                 ws_sub,   // [e] WebSocket 订阅
                 head,     // [f] 区块高度
                 backfill, // [g] 历史补齐
                 COUNT };

struct ProgressState {
  std::atomic<size_t> done{0};
  std::atomic<size_t> total{0};
  std::atomic<size_t> pending{0};
};

struct ProgressBoard {
  static constexpr size_t kApiCount = static_cast<size_t>(API::COUNT);
  static constexpr const char *kApiNames[kApiCount] = {"snapshot", "stables", "meta", "prices", "ws_sub", "head", "backfill"};

  std::array<ProgressState, kApiCount> apis;
  std::string current_stage;
  std::mutex print_mu;
  bool inited = false;

  ProgressState &operator[](API api) { return apis[static_cast<size_t>(api)]; }

  void init() {
    std::lock_guard<std::mutex> lock(print_mu);
    for (auto &api : apis) {
      api.done = 0;
      api.total = 0;
      api.pending = 0;
    }
    current_stage = "init";
    inited = true;
  }

  void stage(const std::string &name) {
    std::lock_guard<std::mutex> lock(print_mu);
    current_stage = name;
  }

  void flush() {
    // no-op: UI前端自行轮询
  }

  void finish() {
    std::lock_guard<std::mutex> lock(print_mu);
    current_stage = "done";
    inited = false;
  }
};

inline ProgressBoard &progress() {
  static ProgressBoard instance;
  return instance;
}

} // namespace tracker
