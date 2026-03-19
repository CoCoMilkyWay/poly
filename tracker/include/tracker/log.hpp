#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace tracker {

// ============================================================================
// Logger
// ============================================================================

enum class LogLevel { DEBUG, INFO, WARN, ERROR };

inline const char *log_level_str(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARN:  return "WARN";
    case LogLevel::ERROR: return "ERROR";
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
    if (!out_.is_open()) return;
    out_ << timestamp() << " [" << log_level_str(level) << "] " << msg << std::endl;
  }

  void debug(const std::string &msg) { log(LogLevel::DEBUG, msg); }
  void info(const std::string &msg)  { log(LogLevel::INFO, msg); }
  void warn(const std::string &msg)  { log(LogLevel::WARN, msg); }
  void error(const std::string &msg) { log(LogLevel::ERROR, msg); }

private:
  static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
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

// ============================================================================
// ProgressBoard - 多行进度显示
// ============================================================================

struct ProgressBoard {
  struct Stage {
    std::string name;
    std::string unit;
    size_t done = 0;
    size_t total = 0;
  };

  std::vector<Stage> stages;
  std::map<std::string, size_t> index;
  std::mutex mu;
  bool ansi = true;
  bool inited = false;

  void init(const std::vector<std::pair<std::string, std::string>> &defs) {
    std::lock_guard<std::mutex> lock(mu);
    stages.clear();
    index.clear();
    for (size_t i = 0; i < defs.size(); ++i) {
      stages.push_back({defs[i].first, defs[i].second, 0, 0});
      index[defs[i].first] = i;
    }
    for (const auto &s : stages) {
      std::cerr << "[sync] " << s.name << ": 0/0";
      if (!s.unit.empty()) std::cerr << " " << s.unit;
      std::cerr << std::endl;
    }
    inited = true;
  }

  void update(const std::string &name, size_t done, size_t total) {
    std::lock_guard<std::mutex> lock(mu);
    auto it = index.find(name);
    if (it == index.end()) return;
    size_t idx = it->second;
    stages[idx].done = done;
    stages[idx].total = total;

    if (!ansi) {
      std::cerr << "[sync] " << stages[idx].name << ": " << done << "/" << total;
      if (!stages[idx].unit.empty()) std::cerr << " " << stages[idx].unit;
      std::cerr << std::endl;
      return;
    }

    size_t up = stages.size() - idx;
    std::cerr << "\x1b[" << up << "A";
    std::cerr << "\r[sync] " << stages[idx].name << ": " << done << "/" << total;
    if (!stages[idx].unit.empty()) std::cerr << " " << stages[idx].unit;
    std::cerr << "\x1b[K";
    std::cerr << "\x1b[" << up << "B";
    std::cerr << std::flush;
  }

  void finish() {
    std::lock_guard<std::mutex> lock(mu);
    std::cerr << "[sync] done" << std::endl;
    inited = false;
  }
};

inline ProgressBoard &progress() {
  static ProgressBoard instance;
  return instance;
}

#define SYNC_STAGES \
  { \
    {"fetch_positions", "users"}, \
    {"fetch_balances", ""}, \
    {"fetch_market_data", "tokens"}, \
    {"fetch_conditions", "conditions"}, \
    {"fetch_gamma", "markets"}, \
    {"backfill_logs", "blocks"}, \
    {"persist", ""}, \
  }

} // namespace tracker
