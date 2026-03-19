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
// ProgressBoard - API进度显示
// ============================================================================
// 终端显示格式:
//   API        done/total  [pend]
//   positions  5/10        [3]
//   balances   4/4         [0]
//   tokens     50/100      [5]
//   conditions 20/50       [2]
//   logs       500/1000    [10]
//   gamma      30/50       [3]
//   subscribe  10/0        [0]
//   block      1/0         [0]
//   [current_stage]

enum class API { positions,
                 balances,
                 tokens,
                 conditions,
                 gamma,
                 subscribe,
                 block,
                 logs,
                 COUNT };

struct ProgressState {
  std::atomic<size_t> done{0};
  std::atomic<size_t> total{0};
  std::atomic<size_t> pending{0};
};

struct ProgressBoard {
  static constexpr size_t kApiCount = static_cast<size_t>(API::COUNT);
  static constexpr const char *kApiNames[kApiCount] = {"positions", "balances", "tokens", "conditions", "gamma", "subscribe", "block", "logs"};

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
    // 表头 + kApiCount行 + stage行
    std::cerr << "API        done/total  [pend]" << std::endl;
    for (size_t i = 0; i < kApiCount; ++i) {
      print_row(i);
      std::cerr << std::endl;
    }
    std::cerr << "[init]" << std::endl;
    inited = true;
  }

  void stage(const std::string &name) {
    std::lock_guard<std::mutex> lock(print_mu);
    current_stage = name;
    reprint_all();
  }

  void flush() {
    std::lock_guard<std::mutex> lock(print_mu);
    reprint_all();
  }

  void finish() {
    std::lock_guard<std::mutex> lock(print_mu);
    current_stage = "done";
    reprint_all();
    std::cerr << std::endl;
    inited = false;
  }

private:
  void print_row(size_t i) {
    auto &api = apis[i];
    std::cerr << std::left << std::setw(10) << kApiNames[i] << " ";
    std::cerr << std::right << std::setw(5) << api.done.load() << "/";
    std::cerr << std::left << std::setw(5) << api.total.load() << " ";
    std::cerr << "[" << std::setw(4) << api.pending.load() << "]";
  }

  void reprint_all() {
    // 上移 kApiCount+2 行 (表头+数据+stage)
    std::cerr << "\x1b[" << (kApiCount + 2) << "A";
    std::cerr << "\rAPI        done/total  [pend]\x1b[K" << std::endl;
    for (size_t i = 0; i < kApiCount; ++i) {
      std::cerr << "\r";
      print_row(i);
      std::cerr << "\x1b[K" << std::endl;
    }
    std::cerr << "[" << current_stage << "]\x1b[K" << std::endl;
    std::cerr << std::flush;
  }
};

inline ProgressBoard &progress() {
  static ProgressBoard instance;
  return instance;
}

} // namespace tracker
