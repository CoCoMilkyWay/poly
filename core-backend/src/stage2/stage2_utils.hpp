#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace stage2 {

// 已知协议的合约地址列表
// 用于识别NonPolymarket transfer属于哪个协议
// 通过transfer事件的operator字段来匹配

enum class Protocol : uint8_t {
  Unknown = 0,
  Polymarket = 1,
  Omen = 2,      // Gnosis预测市场
  Azuro = 3,     // 体育博彩
  Thales = 4,    // 二元期权
  Overtime = 5,  // 体育博彩
  PredictIt = 6, // 预测市场
  // 预留更多协议...
};

const char *protocol_name(Protocol p);

// 已知协议的合约地址 -> 协议枚举
// 包括：Factory、Exchange、Router、Market等合约
const std::unordered_map<std::string, Protocol> &known_protocol_contracts();

// 通过地址识别协议
Protocol identify_protocol(const std::string &addr);

std::string blob_to_hex(const std::string &blob);

std::string blob_to_hex_literal(const std::string &blob);

std::string to_lower(std::string s);

std::array<uint8_t, 32> hex_to_bytes32(const std::string &hex);

std::string hex_to_blob(const std::string &hex);

} // namespace stage2
