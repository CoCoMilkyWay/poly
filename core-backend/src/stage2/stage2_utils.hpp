#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace stage2 {

std::string blob_to_hex(const std::string &blob);

std::string blob_to_hex_literal(const std::string &blob);

std::string to_lower(std::string s);

std::array<uint8_t, 32> hex_to_bytes32(const std::string &hex);

std::string hex_to_blob(const std::string &hex);

} // namespace stage2
