#pragma once

#include <cassert>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace tracker {

// 多行进度显示板，参考 rebuild.py ProgressBoard
// 每个stage独占一行，用ANSI转义码更新
struct ProgressBoard {
  struct Stage {
    std::string name;
    std::string unit;  // "users", "tokens", etc.
    size_t done = 0;
    size_t total = 0;
  };

  std::vector<Stage> stages;
  std::map<std::string, size_t> stage_index;
  std::mutex print_mutex;
  bool use_overwrite = true;  // 是否使用ANSI转义码覆盖
  bool initialized = false;

  void init(const std::vector<std::pair<std::string, std::string>> &stage_defs) {
    std::lock_guard<std::mutex> lock(print_mutex);
    stages.clear();
    stage_index.clear();
    stages.reserve(stage_defs.size());
    for (size_t i = 0; i < stage_defs.size(); ++i) {
      stages.push_back(Stage{
          .name = stage_defs[i].first,
          .unit = stage_defs[i].second,
          .done = 0,
          .total = 0,
      });
      stage_index[stage_defs[i].first] = i;
    }
    // 初始化时打印所有行
    for (const Stage &s : stages) {
      std::cerr << "[sync] " << s.name << ": 0/0";
      if (!s.unit.empty()) {
        std::cerr << " " << s.unit;
      }
      std::cerr << std::endl;
    }
    initialized = true;
  }

  void update(const std::string &stage_name, size_t done, size_t total) {
    std::lock_guard<std::mutex> lock(print_mutex);
    auto it = stage_index.find(stage_name);
    assert(it != stage_index.end());
    const size_t idx = it->second;
    stages[idx].done = done;
    stages[idx].total = total;

    if (!use_overwrite) {
      // 非TTY模式，直接打印
      std::cerr << "[sync] " << stages[idx].name << ": " << done << "/" << total;
      if (!stages[idx].unit.empty()) {
        std::cerr << " " << stages[idx].unit;
      }
      std::cerr << std::endl;
      return;
    }
    // ANSI转义码：上移光标到对应行，覆盖内容
    size_t lines_up = stages.size() - idx;
    std::cerr << "\x1b[" << lines_up << "A";  // 上移
    std::cerr << "\r[sync] " << stages[idx].name << ": " << done << "/" << total;
    if (!stages[idx].unit.empty()) {
      std::cerr << " " << stages[idx].unit;
    }
    std::cerr << "\x1b[K";  // 清行尾
    std::cerr << "\x1b[" << lines_up << "B";  // 下移回原位
    std::cerr << std::flush;
  }

  void finish() {
    std::lock_guard<std::mutex> lock(print_mutex);
    std::cerr << "[sync] done" << std::endl;
    initialized = false;
  }

  void reset() {
    std::lock_guard<std::mutex> lock(print_mutex);
    for (Stage &s : stages) {
      s.done = 0;
      s.total = 0;
    }
    initialized = false;
  }
};

// 全局单例
inline ProgressBoard &progress_board() {
  static ProgressBoard instance;
  return instance;
}

// 便捷宏
#define SYNC_STAGES                                      \
  {                                                      \
    {"resolve_block", "subgraphs"},                      \
    {"fetch_positions", "users"},                        \
    {"fetch_balances", ""},                              \
    {"fetch_market_data", "tokens"},                     \
    {"fetch_conditions", "conditions"},                  \
    {"fetch_gamma", "markets"},                          \
    {"backfill_logs", "blocks"},                         \
    {"persist", ""},                                     \
  }

} // namespace tracker
