#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace stage2 {

// Polymarket 合约地址
constexpr const char *POLYMARKET_FPMM_FACTORY = "0x8b9805a2f595b6705e74f7310829f2d299d21522";

inline bool is_polymarket_factory(const std::string &factory) {
  return factory == POLYMARKET_FPMM_FACTORY;
}

inline std::string blob_to_hex(const std::string &blob) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string result = "0x";
  result.reserve(2 + blob.size() * 2);
  for (unsigned char c : blob) {
    result.push_back(hex_chars[c >> 4]);
    result.push_back(hex_chars[c & 0x0f]);
  }
  return result;
}

inline std::string blob_to_hex_literal(const std::string &blob) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string result = "X'";
  result.reserve(3 + blob.size() * 2);
  for (unsigned char c : blob) {
    result.push_back(hex_chars[c >> 4]);
    result.push_back(hex_chars[c & 0x0f]);
  }
  result.push_back('\'');
  return result;
}

inline std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

inline std::array<uint8_t, 32> hex_to_bytes32(const std::string &hex) {
  std::array<uint8_t, 32> result{};
  std::string h = hex;
  if (h.starts_with("0x"))
    h = h.substr(2);
  for (size_t i = 0; i < 32 && i * 2 < h.size(); ++i) {
    try {
      result[i] = static_cast<uint8_t>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] hex_to_bytes32 failed at i=" << i << ", hex='" << hex << "', substr='" << h.substr(i * 2, 2) << "'" << std::endl;
      throw;
    }
  }
  return result;
}

inline std::string hex_to_blob(const std::string &hex) {
  std::string h = hex;
  if (h.starts_with("0x"))
    h = h.substr(2);
  std::string result;
  result.reserve(h.size() / 2);
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    try {
      result.push_back(static_cast<char>(std::stoul(h.substr(i, 2), nullptr, 16)));
    } catch (const std::exception &e) {
      std::cerr << "[ERROR] hex_to_blob failed at i=" << i << ", hex='" << hex << "', substr='" << h.substr(i, 2) << "'" << std::endl;
      throw;
    }
  }
  return result;
}

} // namespace stage2
