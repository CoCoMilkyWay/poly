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

inline void stage2_log_info(const std::string &msg) {
  std::cerr << "[Stage2] " << msg << std::endl;
}

inline void stage2_log_assert(const std::string &msg) {
  std::cerr << "[Stage2][ASSERT] " << msg << std::endl;
}

namespace detail {

inline const std::string *&assert_context_slot() {
  static thread_local const std::string *ctx = nullptr;
  return ctx;
}

inline std::string format_assert_message(AssertLevel level, const char *domain,
                                         const char *rule, const char *detail) {
  std::string msg =
      std::string("[S2][") + assert_level_tag(level) + "][" + domain + "][" + rule + "]";
  if (detail != nullptr && detail[0] != '\0') {
    msg += " ";
    msg += detail;
  }
  return msg;
}

[[noreturn]] inline void fail_with(AssertLevel level, const char *domain,
                                   const char *rule, const char *msg_detail) {
  stage2_log_assert(format_assert_message(level, domain, rule, msg_detail));
  const std::string *ctx = assert_context_slot();
  if (ctx != nullptr && !ctx->empty()) {
    stage2_log_assert(std::string("context: ") + *ctx);
  }
  assert(false && "stage2 assertion");
  std::abort();
}

} // namespace detail

class Stage2AssertContextScope {
public:
  explicit Stage2AssertContextScope(const std::string *ctx)
      : prev_(detail::assert_context_slot()) {
    detail::assert_context_slot() = ctx;
  }
  ~Stage2AssertContextScope() { detail::assert_context_slot() = prev_; }

  Stage2AssertContextScope(const Stage2AssertContextScope &) = delete;
  Stage2AssertContextScope &operator=(const Stage2AssertContextScope &) = delete;

private:
  const std::string *prev_;
};

inline void stage2_assert(bool cond, AssertLevel level, const char *domain,
                          const char *rule, const char *msg_detail = nullptr) {
  if (!cond) {
    detail::fail_with(level, domain, rule, msg_detail);
  }
}

} // namespace stage2

