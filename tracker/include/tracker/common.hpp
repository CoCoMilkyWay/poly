#pragma once

#include "tracker/json.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <iostream>
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>

namespace tracker {

using BigInt = boost::multiprecision::cpp_int;

inline constexpr const char *kZeroAddress = "0x0000000000000000000000000000000000000000";
inline constexpr const char *kConditionalTokens = "0x4d97dcd97ec945f40cf65f87097ace5ea0476045";
inline constexpr const char *kCtfExchange = "0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e";
inline constexpr const char *kNegRiskCtfExchange = "0xc5d563a36ae78145c45a50134d48a1215220f80a";
inline constexpr const char *kNegRiskAdapter = "0xd91e80cf2e7be2e162c6513ced06f1dd0da35296";

inline constexpr const char *kTransferSingleTopic = "0xc3d58168c5ae7397731d063d5bbf3d657854427343f4c083240f7aacaa2d0f62";
inline constexpr const char *kTransferBatchTopic = "0x4a39dc06d4c0dbc64b70af90fd698a233a518aa5d07e595d983b8c0526c8f7fb";
inline constexpr const char *kConditionResolveTopic = "0xb44d84d3289691f71497564b85d4233648d9dbae8cbdbb4329f301c3a0185894";
inline constexpr const char *kPositionSplitTopic = "0x2e6bb91f8cbcda0c93623c54d0403a43514fabc40084ec96b6d5379a74786298";
inline constexpr const char *kPositionMergeTopic = "0x6f13ca62553fcc2bcd2372180a43949c1e4cebba603901ede2f4e14f36b282ca";
inline constexpr const char *kPositionRedeemTopic = "0x2682012a4a4f1973119f1c9b90745d1bd91fa2bab387344f044cb3586864d18d";
inline constexpr const char *kOrderFillTopic = "0xd0a08e8c493f9c94f29311604c9de1b4e8c8d4c06bd0c789af57f2d65bfec0f6";
inline constexpr const char *kTokenRegisterTopic = "0xbc9a2432e8aeb48327246cddd6e872ef452812b4243c04e6bfb786a2cd8faf0d";
inline constexpr const char *kPositionConvertTopic = "0xb03d19dddbc72a87e735ff0ea3b57bef133ebe44e1894284916a84044deb367e";

inline constexpr const char *kUsdcE = "0x2791bca1f2de4661ed88a30c99a7a9449aa84174";
inline constexpr const char *kWrappedCollateral = "0x3a3bd7bb9528e159577f7c2e685cc81a765002e2";

inline constexpr int64_t kSnapshotBlockLag = 64;
inline constexpr int64_t kTransferFlatLogScale = 10'000;
inline constexpr long double kUnit = 1'000'000.0L;

inline std::string to_lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

inline std::string strip_hex_prefix(std::string value) {
  if (value.starts_with("0x") || value.starts_with("0X")) {
    return value.substr(2);
  }
  return value;
}

inline std::string normalize_hex(std::string value) {
  value = to_lower_ascii(std::move(value));
  assert(value.starts_with("0x"));
  return value;
}

inline std::string normalize_address(std::string value) {
  value = normalize_hex(std::move(value));
  assert(value.size() == 42);
  for (size_t i = 2; i < value.size(); ++i) {
    assert(std::isxdigit(static_cast<unsigned char>(value[i])) != 0);
  }
  return value;
}

inline BigInt big_int_from_dec(const std::string &value) {
  BigInt result = 0;
  assert(!value.empty());
  for (char ch : value) {
    assert(ch >= '0' && ch <= '9');
    result *= 10;
    result += static_cast<int>(ch - '0');
  }
  return result;
}

inline BigInt big_int_from_hex(std::string value) {
  value = strip_hex_prefix(to_lower_ascii(std::move(value)));
  BigInt result = 0;
  for (char ch : value) {
    result <<= 4;
    if (ch >= '0' && ch <= '9') {
      result += static_cast<int>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      result += static_cast<int>(10 + ch - 'a');
    } else {
      assert(false);
    }
  }
  return result;
}

inline std::string big_int_to_string(const BigInt &value) {
  return value.convert_to<std::string>();
}

inline long double big_int_to_units(const BigInt &value) {
  return value.convert_to<long double>() / kUnit;
}

inline std::string format_decimal(long double value, int digits = 10) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(digits) << value;
  return oss.str();
}

inline std::string int_to_hex(uint64_t value) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::nouppercase << value;
  return oss.str();
}

inline uint64_t hex_to_u64(const std::string &value) {
  return std::stoull(strip_hex_prefix(value), nullptr, 16);
}

inline std::string block_key(uint64_t block_number) {
  std::ostringstream oss;
  oss << std::setw(16) << std::setfill('0') << block_number;
  return oss.str();
}

inline std::string address_to_topic(const std::string &address) {
  std::string normalized = normalize_address(address);
  return "0x" + std::string(24, '0') + normalized.substr(2);
}

inline std::string extract_word_hex(const std::string &data, size_t index) {
  const std::string payload = strip_hex_prefix(normalize_hex(data));
  const size_t begin = index * 64;
  const size_t end = begin + 64;
  assert(end <= payload.size());
  return payload.substr(begin, end - begin);
}

inline BigInt extract_uint256_word(const std::string &data, size_t index) {
  return big_int_from_hex(extract_word_hex(data, index));
}

inline std::vector<BigInt> extract_uint256_array_from_offset(const std::string &data, const BigInt &offset_bytes) {
  const uint64_t offset = offset_bytes.convert_to<uint64_t>();
  assert(offset % 32 == 0);
  const size_t base_index = static_cast<size_t>(offset / 32);
  const size_t length = extract_uint256_word(data, base_index).convert_to<size_t>();
  std::vector<BigInt> result;
  result.reserve(length);
  for (size_t index = 0; index < length; ++index) {
    result.push_back(extract_uint256_word(data, base_index + 1 + index));
  }
  return result;
}

inline std::string extract_address_from_topic(const std::string &topic) {
  const std::string normalized = normalize_hex(topic);
  assert(normalized.size() == 66);
  return normalize_address("0x" + normalized.substr(normalized.size() - 40));
}

inline long double parse_decimal_string(const std::string &value) {
  return std::stold(value);
}

inline json bigint_vector_to_json_string_array(const std::vector<BigInt> &values) {
  json result = json::array();
  for (const BigInt &value : values) {
    result.push_back(big_int_to_string(value));
  }
  return result;
}

inline std::vector<std::string> load_address_file_lines(const std::filesystem::path &path) {
  std::ifstream in(path);
  assert(in.is_open());
  std::vector<std::string> result;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    result.push_back(normalize_address(line));
  }
  return result;
}

template <typename TValue>
inline std::vector<std::vector<TValue>> chunked(const std::vector<TValue> &values, size_t size) {
  assert(size > 0);
  std::vector<std::vector<TValue>> result;
  for (size_t index = 0; index < values.size(); index += size) {
    const size_t end = std::min(values.size(), index + size);
    result.emplace_back(values.begin() + static_cast<std::ptrdiff_t>(index),
                        values.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return result;
}

inline json safe_json_parse(const std::string &body) {
  try {
    return json::parse(body);
  } catch (const json::parse_error &e) {
    std::cerr << "[ERROR] JSON parse failed: " << e.what() << std::endl;
    std::cerr << "[ERROR] Response body: " << body << std::endl;
    throw;
  }
}

} // namespace tracker
