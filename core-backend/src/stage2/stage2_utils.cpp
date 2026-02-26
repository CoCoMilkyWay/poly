#include "stage2_utils.hpp"

#include <algorithm>
#include <cassert>

namespace stage2 {

std::string blob_to_hex(const std::string &blob) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string result = "0x";
  result.reserve(2 + blob.size() * 2);
  for (unsigned char c : blob) {
    result.push_back(hex_chars[c >> 4]);
    result.push_back(hex_chars[c & 0x0f]);
  }
  return result;
}

std::string blob_to_hex_literal(const std::string &blob) {
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

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

std::array<uint8_t, 32> hex_to_bytes32(const std::string &hex) {
  std::array<uint8_t, 32> result{};
  std::string h = hex;
  if (h.starts_with("0x"))
    h = h.substr(2);
  for (size_t i = 0; i < 32 && i * 2 < h.size(); ++i) {
    try {
      result[i] = static_cast<uint8_t>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
    } catch (const std::exception &) {
      assert(false);
    }
  }
  return result;
}

std::string hex_to_blob(const std::string &hex) {
  std::string h = hex;
  if (h.starts_with("0x"))
    h = h.substr(2);
  std::string result;
  result.reserve(h.size() / 2);
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    try {
      result.push_back(static_cast<char>(std::stoul(h.substr(i, 2), nullptr, 16)));
    } catch (const std::exception &) {
      assert(false);
    }
  }
  return result;
}

} // namespace stage2
