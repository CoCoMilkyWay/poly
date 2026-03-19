#pragma once

#include "tracker/const.hpp"
#include "tracker/json.hpp"

#include "tracker/core/ctf_helpers.hpp"
#include "tracker/core/keccak256.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_set>
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
  if (!s.starts_with("0x")) {
    s = "0x" + s;
  }
  return s;
}

inline std::string norm_addr(std::string s) {
  s = norm_hex(std::move(s));
  assert(s.size() == 42);
  return s;
}

inline std::string norm_b32(std::string s) {
  s = norm_hex(std::move(s));
  assert(s.size() == 66);
  return s;
}

// ============================================================================
// BigInt Conversions
// ============================================================================

inline BigInt bigint_from_dec(const std::string &s) {
  BigInt sign = 1;
  size_t pos = 0;
  if (!s.empty() && s[0] == '-') {
    sign = -1;
    pos = 1;
  }
  BigInt r = 0;
  assert(pos < s.size());
  for (; pos < s.size(); ++pos) {
    char c = s[pos];
    assert(c >= '0' && c <= '9');
    r = r * 10 + (c - '0');
  }
  return sign * r;
}

inline BigInt bigint_from_hex(std::string s) {
  s = strip_0x(to_lower(std::move(s)));
  BigInt r = 0;
  for (char c : s) {
    r <<= 4;
    if (c >= '0' && c <= '9') {
      r += c - '0';
    } else if (c >= 'a' && c <= 'f') {
      r += 10 + c - 'a';
    } else {
      assert(false);
    }
  }
  return r;
}

inline std::string bigint_to_str(const BigInt &v) { return v.convert_to<std::string>(); }

inline int64_t bigint_to_i64(const BigInt &v) {
  assert(v >= std::numeric_limits<int64_t>::min());
  assert(v <= std::numeric_limits<int64_t>::max());
  return v.convert_to<int64_t>();
}

inline uint64_t bigint_to_u64(const BigInt &v) {
  assert(v >= 0);
  assert(v <= std::numeric_limits<uint64_t>::max());
  return v.convert_to<uint64_t>();
}

inline long double bigint_to_units(const BigInt &v, long double unit = kUnit) {
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
  return std::stoull(strip_0x(norm_hex(s)), nullptr, 16);
}

inline int64_t now_unix_sec() {
  return static_cast<int64_t>(std::time(nullptr));
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

inline std::string extract_addr_from_word(const std::string &data, size_t idx) {
  std::string word = extract_word_hex(data, idx);
  return norm_addr("0x" + word.substr(24));
}

inline std::string extract_b32_from_word(const std::string &data, size_t idx) {
  return norm_b32("0x" + extract_word_hex(data, idx));
}

inline std::vector<BigInt> extract_u256_array(const std::string &data,
                                              const BigInt &offset_bytes) {
  uint64_t offset = bigint_to_u64(offset_bytes);
  assert((offset % 32) == 0);
  size_t base = static_cast<size_t>(offset / 32);
  size_t len = static_cast<size_t>(bigint_to_u64(extract_u256(data, base)));
  std::vector<BigInt> r;
  r.reserve(len);
  for (size_t i = 0; i < len; ++i) {
    r.push_back(extract_u256(data, base + 1 + i));
  }
  return r;
}

// ============================================================================
// CTF Helpers
// ============================================================================

inline std::string hex_to_blob(std::string hex, size_t byte_len) {
  hex = strip_0x(norm_hex(std::move(hex)));
  assert(hex.size() == byte_len * 2);
  std::string out(byte_len, '\0');
  for (size_t i = 0; i < byte_len; ++i) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    auto nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') {
        return static_cast<uint8_t>(c - '0');
      }
      assert(c >= 'a' && c <= 'f');
      return static_cast<uint8_t>(10 + c - 'a');
    };
    out[i] = static_cast<char>((nibble(hi) << 4) | nibble(lo));
  }
  return out;
}

inline std::string blob_to_hex(const std::string &blob) {
  static constexpr char hex_chars[] = "0123456789abcdef";
  std::string out = "0x";
  out.reserve(2 + blob.size() * 2);
  for (unsigned char c : blob) {
    out.push_back(hex_chars[c >> 4]);
    out.push_back(hex_chars[c & 0x0F]);
  }
  return out;
}

// question_id = market_id[0:31] | question_index
inline std::string build_negrisk_question_id(const std::string &market_id, int idx) {
  assert(idx >= 0 && idx < 256);
  std::string blob = hex_to_blob(market_id, 32);
  blob[31] = static_cast<char>(idx);
  return blob_to_hex(blob);
}

inline std::string build_negrisk_condition_id(const std::string &question_id) {
  std::string oracle = hex_to_blob(kNegRiskAdapter, 20);
  std::string qid = hex_to_blob(question_id, 32);
  std::string input(84, '\0');
  std::memcpy(input.data(), oracle.data(), 20);
  std::memcpy(input.data() + 20, qid.data(), 32);
  input[83] = 2;
  return norm_hex(crypto::Keccak256::hash_hex(input));
}

inline std::string condition_token_id(const std::string &condition_id,
                                      const std::string &collateral_addr,
                                      uint8_t token_idx) {
  assert(token_idx < 31);
  std::string condition = hex_to_blob(condition_id, 32);
  std::string collateral = hex_to_blob(collateral_addr, 20);
  std::string collection = ctf::get_collection_id(condition, 1u << token_idx);
  return norm_hex(crypto::Keccak256::to_hex(
      ctf::get_position_id(collateral, collection)));
}

inline uint8_t index_set_to_token_idx(const BigInt &index_set) {
  uint64_t value = bigint_to_u64(index_set);
  assert(value > 0);
  assert((value & (value - 1)) == 0);
  uint8_t idx = 0;
  while ((value >> idx) != 1) {
    ++idx;
  }
  return idx;
}

// ============================================================================
// JSON Helpers
// ============================================================================

inline json bigint_vec_to_json(const std::vector<BigInt> &v) {
  json r = json::array();
  for (const auto &x : v) {
    r.push_back(bigint_to_str(x));
  }
  return r;
}

inline long double parse_decimal(const std::string &s) { return std::stold(s); }

inline json safe_parse(const std::string &body) { return json::parse(body); }

// ============================================================================
// JSON Field Accessors
// ============================================================================

inline std::string json_str(const json &row, const char *key) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return "";
  }
  if (row.at(key).is_string()) {
    return row.at(key).get<std::string>();
  }
  return row.at(key).dump();
}

inline int json_int(const json &row, const char *key, int fallback = -1) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return fallback;
  }
  if (row.at(key).is_number_integer()) {
    return row.at(key).get<int>();
  }
  if (row.at(key).is_string()) {
    return std::stoi(row.at(key).get<std::string>());
  }
  return fallback;
}

inline int64_t json_i64(const json &row, const char *key, int64_t fallback = -1) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return fallback;
  }
  if (row.at(key).is_number_integer()) {
    return row.at(key).get<int64_t>();
  }
  if (row.at(key).is_string()) {
    return std::stoll(row.at(key).get<std::string>());
  }
  return fallback;
}

inline BigInt json_bigint(const json &row, const char *key) {
  if (!row.contains(key) || row.at(key).is_null()) {
    return 0;
  }
  if (row.at(key).is_string()) {
    return bigint_from_dec(row.at(key).get<std::string>());
  }
  if (row.at(key).is_number_integer()) {
    return bigint_from_dec(std::to_string(row.at(key).get<int64_t>()));
  }
  return 0;
}

inline std::vector<BigInt> json_bigint_arr(const json &row, const char *key) {
  std::vector<BigInt> out;
  if (!row.contains(key) || !row.at(key).is_array()) {
    return out;
  }
  for (const auto &value : row.at(key)) {
    if (value.is_string()) {
      out.push_back(bigint_from_dec(value.get<std::string>()));
    } else if (value.is_number_integer()) {
      out.push_back(bigint_from_dec(std::to_string(value.get<int64_t>())));
    }
  }
  return out;
}

inline std::vector<std::string> json_str_arr(const json &row, const char *key) {
  std::vector<std::string> out;
  if (!row.contains(key) || !row.at(key).is_array()) {
    return out;
  }
  for (const auto &value : row.at(key)) {
    if (value.is_string()) {
      out.push_back(value.get<std::string>());
    }
  }
  return out;
}

inline std::string json_str_or_int(const json &value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<int64_t>());
  }
  return "";
}

// ============================================================================
// File Helpers
// ============================================================================

inline std::vector<std::string> load_addr_file(const std::filesystem::path &path) {
  std::ifstream in(path);
  assert(in.is_open());
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    std::string addr = norm_addr(line);
    if (seen.insert(addr).second) {
      out.push_back(addr);
    }
  }
  assert(!out.empty());
  return out;
}

inline std::string raw_log_key(const json &log) {
  return std::to_string(hex_to_u64(log.at("blockNumber").get<std::string>())) +
         "|" + norm_hex(log.at("transactionHash").get<std::string>()) + "|" +
         std::to_string(hex_to_u64(log.at("logIndex").get<std::string>())) +
         "|" + norm_hex(log.at("address").get<std::string>());
}

inline std::tuple<uint64_t, uint64_t, uint64_t, std::string>
raw_log_sort_key(const json &log) {
  return {
      hex_to_u64(log.at("blockNumber").get<std::string>()),
      hex_to_u64(log.at("transactionIndex").get<std::string>()),
      hex_to_u64(log.at("logIndex").get<std::string>()),
      norm_hex(log.at("address").get<std::string>()),
  };
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
