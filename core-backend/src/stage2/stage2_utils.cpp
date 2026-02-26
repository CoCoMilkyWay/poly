#include "stage2_utils.hpp"

#include <algorithm>
#include <cassert>

namespace stage2 {

const char *protocol_name(Protocol p) {
  switch (p) {
  case Protocol::Polymarket:
    return "Polymarket";
  case Protocol::Omen:
    return "Omen";
  case Protocol::Azuro:
    return "Azuro";
  case Protocol::Thales:
    return "Thales";
  case Protocol::Overtime:
    return "Overtime";
  case Protocol::PredictIt:
    return "PredictIt";
  default:
    return "Unknown";
  }
}

const std::unordered_map<std::string, Protocol> &known_protocol_contracts() {
  static const std::unordered_map<std::string, Protocol> contracts = {
      // ========== Polymarket ==========
      {"0x8b9805a2f595b6705e74f7310829f2d299d21522", Protocol::Polymarket}, // FPMM Factory
      {"0x4bfb41d5b3570defd03c39a9a4d8de6bd8b8982e", Protocol::Polymarket}, // CTF Exchange
      {"0xc5d563a36ae78145c45a50134d48a1215220f80a", Protocol::Polymarket}, // NegRisk CTF Exchange
      {"0xd91e80cf2e7be2e162c6513ced06f1dd0da35296", Protocol::Polymarket}, // NegRisk Adapter

      // ========== Omen (Gnosis预测市场, Polygon部署) ==========
      {"0x0000000000000000000000000000000000000000", Protocol::Omen}, // 占位，需要填入实际地址
                                                                      // Omen在Polygon上的Factory/Market合约地址需要查证

      // ========== Azuro (体育博彩) ==========
      // Azuro在Polygon上的合约地址需要查证

      // ========== Thales (二元期权) ==========
      // Thales主要在Optimism，Polygon上可能有部署

      // ========== Overtime (体育博彩) ==========
      // Overtime主要在Optimism，Polygon上可能有部署
  };
  return contracts;
}

Protocol identify_protocol(const std::string &addr) {
  auto it = known_protocol_contracts().find(addr);
  if (it != known_protocol_contracts().end()) {
    return it->second;
  }
  return Protocol::Unknown;
}

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
