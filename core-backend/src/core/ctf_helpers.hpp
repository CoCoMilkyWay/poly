#pragma once

#include "keccak256.hpp"
#include <cassert>
#include <cstring>
#include <openssl/bn.h>
#include <string>

// CTF (Conditional Token Framework) position ID computation.
// Implements CTHelpers.getCollectionId and getPositionId from:
//   gnosis/conditional-tokens-contracts/CTHelpers.sol
//
// BN128 curve: y^2 = x^3 + 3 (mod P)
// P = 21888242871839275222246405745257275088696311157297823662689037894645226208583
// P ≡ 3 (mod 4), so sqrt(yy) = yy^((P+1)/4) mod P

namespace ctf {

namespace detail {

inline BIGNUM *bn_P() {
  static BIGNUM *p = []() {
    BIGNUM *n = BN_new();
    BN_dec2bn(&n, "21888242871839275222246405745257275088696311157297823662689037894645226208583");
    return n;
  }();
  return p;
}

inline BIGNUM *bn_sqrt_exp() {
  // (P+1)/4
  static BIGNUM *e = []() {
    BIGNUM *n = BN_new();
    BN_dec2bn(&n, "5472060717959818805221448978750891763617087977550011513538245736788228476646");
    return n;
  }();
  return e;
}

inline BIGNUM *bn_B() {
  static BIGNUM *b = []() {
    BIGNUM *n = BN_new();
    BN_set_word(n, 3);
    return n;
  }();
  return b;
}

struct BNPtr {
  BIGNUM *bn;
  explicit BNPtr(BIGNUM *n = BN_new()) : bn(n) { assert(bn); }
  ~BNPtr() { BN_free(bn); }
  BNPtr(const BNPtr &) = delete;
  BNPtr &operator=(const BNPtr &) = delete;
  operator BIGNUM *() const { return bn; }
};

struct CTXPtr {
  BN_CTX *ctx;
  CTXPtr() : ctx(BN_CTX_new()) { assert(ctx); }
  ~CTXPtr() { BN_CTX_free(ctx); }
  operator BN_CTX *() const { return ctx; }
};

} // namespace detail

// Implements CTHelpers.getCollectionId(bytes32(0), conditionId, indexSet).
// Assumes parentCollectionId = 0 (top-level positions only).
//
// condition_id: raw 32 bytes (big-endian)
// index_set: uint (1 = outcome 0 = YES, 2 = outcome 1 = NO in binary markets)
// Returns: raw 32 bytes (big-endian), the collection ID
inline std::string get_collection_id(const std::string &condition_id, uint32_t index_set) {
  assert(condition_id.size() == 32);

  // x1 = keccak256(abi.encodePacked(conditionId, uint256(indexSet)))
  std::string hash_input(64, '\0');
  std::memcpy(hash_input.data(), condition_id.data(), 32);
  hash_input[60] = (index_set >> 24) & 0xff;
  hash_input[61] = (index_set >> 16) & 0xff;
  hash_input[62] = (index_set >> 8) & 0xff;
  hash_input[63] = index_set & 0xff;

  auto h = crypto::keccak256(hash_input);
  bool odd = (h[0] & 0x80) != 0;

  BIGNUM *P = detail::bn_P();
  BIGNUM *SQRT_EXP = detail::bn_sqrt_exp();
  BIGNUM *B = detail::bn_B();
  detail::CTXPtr ctx;

  detail::BNPtr x1(BN_bin2bn(h.data(), 32, BN_new()));
  BN_mod(x1, x1, P, ctx);

  detail::BNPtr yy, y1, tmp;

  // Find point on curve y^2 = x^3 + 3 (mod P)
  do {
    BN_mod_add(x1, x1, BN_value_one(), P, ctx);
    BN_mod_sqr(tmp, x1, P, ctx);
    BN_mod_mul(yy, tmp, x1, P, ctx);
    BN_mod_add(yy, yy, B, P, ctx);
    BN_mod_exp(y1, yy, SQRT_EXP, P, ctx);
    BN_mod_sqr(tmp, y1, P, ctx);
  } while (BN_cmp(tmp, yy) != 0);

  // Adjust y parity to match `odd`
  bool y1_odd = BN_is_odd(y1) != 0;
  if ((odd && !y1_odd) || (!odd && y1_odd))
    BN_sub(y1, P, y1);

  // Encode: bit 254 of x1 = parity of y (x1 < P < 2^254 so bit 254 was 0)
  if (BN_is_odd(y1))
    BN_set_bit(x1, 254);

  std::string result(32, '\0');
  BN_bn2binpad(x1, reinterpret_cast<unsigned char *>(result.data()), 32);
  return result;
}

// Implements CTHelpers.getPositionId(collateralToken, collectionId).
//
// collateral: raw 20 bytes
// collection_id: raw 32 bytes
// Returns: the keccak256 hash (32 bytes = position token ID as uint256)
inline crypto::Keccak256::Hash get_position_id(const std::string &collateral,
                                               const std::string &collection_id) {
  assert(collateral.size() == 20);
  assert(collection_id.size() == 32);
  std::string input = collateral + collection_id;
  return crypto::keccak256(input);
}

} // namespace ctf
