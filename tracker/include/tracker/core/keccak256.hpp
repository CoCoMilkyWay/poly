#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace crypto {

class Keccak256 {
public:
  static constexpr size_t HASH_SIZE = 32;
  using Hash = std::array<uint8_t, HASH_SIZE>;

  static Hash hash(const uint8_t *data, size_t len) {
    State state{};
    absorb(state, data, len);
    return squeeze(state);
  }

  static Hash hash(const std::string &data) {
    return hash(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }

  static std::string hash_hex(const uint8_t *data, size_t len) {
    auto h = hash(data, len);
    return to_hex(h);
  }

  static std::string hash_hex(const std::string &data) {
    return hash_hex(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  }

  static std::string to_hex(const Hash &h) {
    static const char hex[] = "0123456789abcdef";
    std::string result = "0x";
    result.reserve(66);
    for (uint8_t b : h) {
      result.push_back(hex[b >> 4]);
      result.push_back(hex[b & 0xf]);
    }
    return result;
  }

private:
  static constexpr size_t RATE = 136;
  static constexpr size_t STATE_SIZE = 25;
  using State = std::array<uint64_t, STATE_SIZE>;

  static constexpr uint64_t RC[24] = {
      0x0000000000000001ULL,
      0x0000000000008082ULL,
      0x800000000000808aULL,
      0x8000000080008000ULL,
      0x000000000000808bULL,
      0x0000000080000001ULL,
      0x8000000080008081ULL,
      0x8000000000008009ULL,
      0x000000000000008aULL,
      0x0000000000000088ULL,
      0x0000000080008009ULL,
      0x000000008000000aULL,
      0x000000008000808bULL,
      0x800000000000008bULL,
      0x8000000000008089ULL,
      0x8000000000008003ULL,
      0x8000000000008002ULL,
      0x8000000000000080ULL,
      0x000000000000800aULL,
      0x800000008000000aULL,
      0x8000000080008081ULL,
      0x8000000000008080ULL,
      0x0000000080000001ULL,
      0x8000000080008008ULL,
  };

  static constexpr int ROTC[24] = {1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14,
                                   27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44};

  static constexpr int PILN[24] = {10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4,
                                   15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1};

  static uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

  static void keccakf(State &st) {
    for (int round = 0; round < 24; ++round) {
      uint64_t bc[5];
      for (int i = 0; i < 5; ++i)
        bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];

      for (int i = 0; i < 5; ++i) {
        uint64_t t = bc[(i + 4) % 5] ^ rotl64(bc[(i + 1) % 5], 1);
        for (int j = 0; j < 25; j += 5)
          st[j + i] ^= t;
      }

      uint64_t t = st[1];
      for (int i = 0; i < 24; ++i) {
        int j = PILN[i];
        uint64_t tmp = st[j];
        st[j] = rotl64(t, ROTC[i]);
        t = tmp;
      }

      for (int j = 0; j < 25; j += 5) {
        uint64_t tmp[5];
        for (int i = 0; i < 5; ++i)
          tmp[i] = st[j + i];
        for (int i = 0; i < 5; ++i)
          st[j + i] ^= (~tmp[(i + 1) % 5]) & tmp[(i + 2) % 5];
      }

      st[0] ^= RC[round];
    }
  }

  static void absorb(State &st, const uint8_t *data, size_t len) {
    size_t offset = 0;
    while (len >= RATE) {
      for (size_t i = 0; i < RATE / 8; ++i) {
        uint64_t lane;
        std::memcpy(&lane, data + offset + i * 8, 8);
        st[i] ^= lane;
      }
      keccakf(st);
      offset += RATE;
      len -= RATE;
    }

    uint8_t pad[RATE] = {};
    std::memcpy(pad, data + offset, len);
    pad[len] = 0x01;
    pad[RATE - 1] |= 0x80;

    for (size_t i = 0; i < RATE / 8; ++i) {
      uint64_t lane;
      std::memcpy(&lane, pad + i * 8, 8);
      st[i] ^= lane;
    }
    keccakf(st);
  }

  static Hash squeeze(const State &st) {
    Hash out;
    for (size_t i = 0; i < HASH_SIZE / 8; ++i)
      std::memcpy(out.data() + i * 8, &st[i], 8);
    return out;
  }
};

inline Keccak256::Hash keccak256(const uint8_t *data, size_t len) {
  return Keccak256::hash(data, len);
}

inline Keccak256::Hash keccak256(const std::string &data) { return Keccak256::hash(data); }

inline std::string keccak256_hex(const std::string &data) { return Keccak256::hash_hex(data); }

} // namespace crypto
