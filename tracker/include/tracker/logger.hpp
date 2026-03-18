#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

namespace tracker {

enum class LogLevel {
  DEBUG,
  INFO,
  WARN,
  ERROR,
};

inline const char *log_level_str(LogLevel level) {
  switch (level) {
    case LogLevel::DEBUG: return "DEBUG";
    case LogLevel::INFO:  return "INFO";
    case LogLevel::WARN:  return "WARN";
    case LogLevel::ERROR: return "ERROR";
  }
  return "UNKNOWN";
}

class Logger {
public:
  void init(const std::filesystem::path &log_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    log_file_ = log_file;
    // 确保目录存在
    if (log_file_.has_parent_path()) {
      std::filesystem::create_directories(log_file_.parent_path());
    }
    // 追加模式打开
    stream_.open(log_file_, std::ios::app);
  }

  void log(LogLevel level, const std::string &message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_.is_open()) {
      return;
    }
    stream_ << timestamp() << " [" << log_level_str(level) << "] " << message << std::endl;
    stream_.flush();
  }

  void debug(const std::string &message) { log(LogLevel::DEBUG, message); }
  void info(const std::string &message) { log(LogLevel::INFO, message); }
  void warn(const std::string &message) { log(LogLevel::WARN, message); }
  void error(const std::string &message) { log(LogLevel::ERROR, message); }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_.is_open()) {
      stream_.close();
    }
  }

private:
  static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    std::tm tm_now{};
    localtime_r(&time_t_now, &tm_now);
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
  }

  std::filesystem::path log_file_;
  std::ofstream stream_;
  std::mutex mutex_;
};

// 全局单例
inline Logger &sync_logger() {
  static Logger instance;
  return instance;
}

} // namespace tracker
