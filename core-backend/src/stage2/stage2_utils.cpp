#include "stage2_utils.hpp"
#include "stage2_assert.hpp"

#include <algorithm>

namespace stage2 {
namespace {

inline std::string strip_hex_prefix(std::string s) {
  if (s.starts_with("0x")) {
    return s.substr(2);
  }
  return s;
}

inline void append_hex_bytes(std::string &out, const std::string &blob) {
  static const char hex_chars[] = "0123456789abcdef";
  for (unsigned char c : blob) {
    out.push_back(hex_chars[c >> 4]);
    out.push_back(hex_chars[c & 0x0f]);
  }
}

} // namespace

std::string blob_to_hex(const std::string &blob) {
  std::string result = "0x";
  result.reserve(2 + blob.size() * 2);
  append_hex_bytes(result, blob);
  return result;
}

std::string blob_to_hex_literal(const std::string &blob) {
  std::string result = "X'";
  result.reserve(3 + blob.size() * 2);
  append_hex_bytes(result, blob);
  result.push_back('\'');
  return result;
}

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

std::array<uint8_t, 32> hex_to_bytes32(const std::string &hex) {
  std::array<uint8_t, 32> result{};
  std::string h = strip_hex_prefix(hex);
  for (size_t i = 0; i < 32 && i * 2 < h.size(); ++i) {
    try {
      result[i] = static_cast<uint8_t>(std::stoul(h.substr(i * 2, 2), nullptr, 16));
    } catch (const std::exception &) {
      stage2_assert(false, AssertLevel::L0, "Parse", "HexToBytes32InvalidHex");
    }
  }
  return result;
}

std::string hex_to_blob(const std::string &hex) {
  std::string h = strip_hex_prefix(hex);
  std::string result;
  result.reserve(h.size() / 2);
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    try {
      result.push_back(static_cast<char>(std::stoul(h.substr(i, 2), nullptr, 16)));
    } catch (const std::exception &) {
      stage2_assert(false, AssertLevel::L0, "Parse", "HexToBlobInvalidHex");
    }
  }
  return result;
}

} // namespace stage2
