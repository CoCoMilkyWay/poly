#pragma once

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

namespace stage2 {

enum class AssertLevel : uint8_t {
  L0 = 0, // 输入/结构
  L1 = 1, // 映射不变量
  L2 = 2, // 匹配唯一性
  L3 = 3, // 语义约束
  L4 = 4, // 语义消费闭环
  L5 = 5, // 结果守恒
};

inline const char *assert_level_tag(AssertLevel level) {
  switch (level) {
  case AssertLevel::L0:
    return "L0";
  case AssertLevel::L1:
    return "L1";
  case AssertLevel::L2:
    return "L2";
  case AssertLevel::L3:
    return "L3";
  case AssertLevel::L4:
    return "L4";
  case AssertLevel::L5:
    return "L5";
  }
  return "L?";
}

inline std::string build_assert_message(AssertLevel level, const char *domain,
                                        const char *rule, const char *detail = nullptr) {
  std::string msg = std::string("[S2][") + assert_level_tag(level) + "][" + domain + "][" + rule + "]";
  if (detail != nullptr && detail[0] != '\0') {
    msg += " ";
    msg += detail;
  }
  return msg;
}

inline void stage2_log_info(const std::string &msg) {
  std::cerr << "[Stage2] " << msg << std::endl;
}

inline void stage2_log_assert(const std::string &msg) {
  std::cerr << "[Stage2][ASSERT] " << msg << std::endl;
}

[[noreturn]] inline void fail_stage2_assert(AssertLevel level, const char *domain,
                                             const char *rule, const char *detail = nullptr) {
  stage2_log_assert(build_assert_message(level, domain, rule, detail));
  assert(false && "stage2 assertion");
  std::abort();
}

inline void assert_stage2(bool cond, AssertLevel level, const char *domain,
                          const char *rule, const char *detail = nullptr) {
  if (!cond) {
    fail_stage2_assert(level, domain, rule, detail);
  }
}

} // namespace stage2

