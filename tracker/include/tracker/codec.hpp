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
#include <vector>

#include <boost/multiprecision/cpp_int.hpp>

namespace tracker {

using BigInt = boost::multiprecision::cpp_int;

// ============================================================================
// String Transforms
// ============================================================================

inline std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

inline std::string strip_0x(std::string s) {
  if (s.starts_with("0x") || s.starts_with("0X")) {
    return s.substr(2);
  }
  return s;
}

inline std::string norm_hex(std::string s) {
  s = to_lower(std::move(s));
  assert(s.starts_with("0x"));
  return s;
}

inline std::string norm_addr(std::string s) {
  s = norm_hex(std::move(s));
  assert(s.size() == 42);
  return s;
}

// ============================================================================
// BigInt Conversions
// ============================================================================

inline BigInt bigint_from_dec(const std::string &s) {
  BigInt r = 0;
  assert(!s.empty());
  for (char c : s) {
    assert(c >= '0' && c <= '9');
    r = r * 10 + (c - '0');
  }
  return r;
}

inline BigInt bigint_from_hex(std::string s) {
  s = strip_0x(to_lower(std::move(s)));
  BigInt r = 0;
  for (char c : s) {
    r <<= 4;
    if (c >= '0' && c <= '9') r += c - '0';
    else if (c >= 'a' && c <= 'f') r += 10 + c - 'a';
    else assert(false);
  }
  return r;
}

inline std::string bigint_to_str(const BigInt &v) {
  return v.convert_to<std::string>();
}

inline long double bigint_to_units(const BigInt &v, long double unit = 1'000'000.0L) {
  return v.convert_to<long double>() / unit;
}

// ============================================================================
// Hex / Integer Conversions
// ============================================================================

inline std::string u64_to_hex(uint64_t v) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::nouppercase << v;
  return oss.str();
}

inline uint64_t hex_to_u64(const std::string &s) {
  return std::stoull(strip_0x(s), nullptr, 16);
}

// ============================================================================
// Formatting
// ============================================================================

inline std::string fmt_decimal(long double v, int digits = 10) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(digits) << v;
  return oss.str();
}

inline std::string block_key(uint64_t block) {
  std::ostringstream oss;
  oss << std::setw(16) << std::setfill('0') << block;
  return oss.str();
}

// ============================================================================
// Address / Topic Encoding
// ============================================================================

inline std::string addr_to_topic(const std::string &addr) {
  std::string n = norm_addr(addr);
  return "0x" + std::string(24, '0') + n.substr(2);
}

inline std::string topic_to_addr(const std::string &topic) {
  std::string n = norm_hex(topic);
  assert(n.size() == 66);
  return norm_addr("0x" + n.substr(n.size() - 40));
}

// ============================================================================
// ABI Decoding
// ============================================================================

inline std::string extract_word_hex(const std::string &data, size_t idx) {
  std::string payload = strip_0x(norm_hex(data));
  size_t begin = idx * 64;
  assert(begin + 64 <= payload.size());
  return payload.substr(begin, 64);
}

inline BigInt extract_u256(const std::string &data, size_t idx) {
  return bigint_from_hex(extract_word_hex(data, idx));
}

inline std::vector<BigInt> extract_u256_array(const std::string &data, const BigInt &offset_bytes) {
  uint64_t offset = offset_bytes.convert_to<uint64_t>();
  assert(offset % 32 == 0);
  size_t base = offset / 32;
  size_t len = extract_u256(data, base).convert_to<size_t>();
  std::vector<BigInt> r;
  r.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    r.push_back(extract_u256(data, base + 1 + i));
  }
  return r;
}

// ============================================================================
// JSON Helpers
// ============================================================================

inline json bigint_vec_to_json(const std::vector<BigInt> &v) {
  json r = json::array();
  for (const auto &x : v) r.push_back(bigint_to_str(x));
  return r;
}

inline long double parse_decimal(const std::string &s) {
  return std::stold(s);
}

inline json safe_parse(const std::string &body) {
  return json::parse(body);
}

// ============================================================================
// File Helpers
// ============================================================================

inline std::vector<std::string> load_addr_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  assert(in.is_open());
  std::vector<std::string> r;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    r.push_back(norm_addr(line));
  }
  return r;
}

// ============================================================================
// Chunking
// ============================================================================

template <typename T>
inline std::vector<std::vector<T>> chunked(const std::vector<T> &v, size_t n) {
  assert(n > 0);
  std::vector<std::vector<T>> r;
  for (size_t i = 0; i < v.size(); i += n) {
    size_t end = std::min(v.size(), i + n);
    r.emplace_back(v.begin() + static_cast<std::ptrdiff_t>(i),
                   v.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return r;
}

} // namespace tracker
