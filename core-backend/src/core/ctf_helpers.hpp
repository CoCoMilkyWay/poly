#pragma once

#include "keccak256.hpp"
#include <algorithm>
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
    BN_dec2bn(&n, "5472060717959818805561601436314318772174077789324455915672259473661306552146");
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

struct ECPoint {
  BNPtr x;
  BNPtr y;
};

inline void recover_y_from_x_parity(BIGNUM *x, bool odd, BIGNUM *y, BN_CTX *ctx) {
  BIGNUM *P = bn_P();
  BIGNUM *B = bn_B();
  BIGNUM *SQRT_EXP = bn_sqrt_exp();
  BNPtr yy, tmp;
  BN_mod_sqr(tmp, x, P, ctx);
  BN_mod_mul(yy, tmp, x, P, ctx);
  BN_mod_add(yy, yy, B, P, ctx);
  BN_mod_exp(y, yy, SQRT_EXP, P, ctx);
  BN_mod_sqr(tmp, y, P, ctx);
  assert(BN_cmp(tmp, yy) == 0);
  bool y_odd = BN_is_odd(y) != 0;
  if ((odd && !y_odd) || (!odd && y_odd))
    BN_sub(y, P, y);
}

inline void hash_to_curve(const std::string &condition_id, uint32_t index_set,
                          BIGNUM *x, BIGNUM *y, BN_CTX *ctx) {
  assert(condition_id.size() == 32);
  std::string hash_input(64, '\0');
  std::memcpy(hash_input.data(), condition_id.data(), 32);
  hash_input[60] = (index_set >> 24) & 0xff;
  hash_input[61] = (index_set >> 16) & 0xff;
  hash_input[62] = (index_set >> 8) & 0xff;
  hash_input[63] = index_set & 0xff;
  auto h = crypto::keccak256(hash_input);
  bool odd = (h[0] & 0x80) != 0; // bit255

  BIGNUM *P = bn_P();
  BIGNUM *B = bn_B();
  BIGNUM *SQRT_EXP = bn_sqrt_exp();
  BNPtr yy, tmp;

  assert(BN_bin2bn(h.data(), 32, x));
  BN_mod(x, x, P, ctx);
  do {
    BN_mod_add(x, x, BN_value_one(), P, ctx);
    BN_mod_sqr(tmp, x, P, ctx);
    BN_mod_mul(yy, tmp, x, P, ctx);
    BN_mod_add(yy, yy, B, P, ctx);
    BN_mod_exp(y, yy, SQRT_EXP, P, ctx);
    BN_mod_sqr(tmp, y, P, ctx);
  } while (BN_cmp(tmp, yy) != 0);
  bool y_odd = BN_is_odd(y) != 0;
  if ((odd && !y_odd) || (!odd && y_odd))
    BN_sub(y, P, y);
}

inline void decode_collection_point(const std::string &collection_id, BIGNUM *x, BIGNUM *y,
                                    BN_CTX *ctx) {
  assert(collection_id.size() == 32);
  assert(BN_bin2bn(reinterpret_cast<const unsigned char *>(collection_id.data()), 32, x));
  bool odd = BN_is_bit_set(x, 254) != 0;
  BN_clear_bit(x, 254);
  BN_clear_bit(x, 255);
  recover_y_from_x_parity(x, odd, y, ctx);
}

inline void ec_add(const BIGNUM *x1, const BIGNUM *y1, const BIGNUM *x2, const BIGNUM *y2,
                   BIGNUM *xr, BIGNUM *yr, BN_CTX *ctx) {
  BIGNUM *P = bn_P();
  BNPtr s, num, den, inv, tmp;

  if (BN_cmp(x1, x2) == 0) {
    // 这里只会遇到有效点,且 parent != -base,不考虑无穷远点
    BN_mod_add(tmp, y1, y2, P, ctx);
    assert(!BN_is_zero(tmp));
    BN_mod_sqr(num, x1, P, ctx);
    BNPtr three;
    BN_set_word(three, 3);
    BN_mod_mul(num, num, three, P, ctx);
    BN_mod_add(den, y1, y1, P, ctx);
  } else {
    BN_mod_sub(num, y2, y1, P, ctx);
    BN_mod_sub(den, x2, x1, P, ctx);
  }

  BIGNUM *inv_ptr = BN_mod_inverse(nullptr, den, P, ctx);
  assert(inv_ptr != nullptr);
  BN_copy(inv, inv_ptr);
  BN_free(inv_ptr);

  BN_mod_mul(s, num, inv, P, ctx);
  BN_mod_sqr(xr, s, P, ctx);
  BN_mod_sub(xr, xr, x1, P, ctx);
  BN_mod_sub(xr, xr, x2, P, ctx);

  BN_mod_sub(tmp, x1, xr, P, ctx);
  BN_mod_mul(yr, s, tmp, P, ctx);
  BN_mod_sub(yr, yr, y1, P, ctx);
}

} // namespace detail

// Implements CTHelpers.getCollectionId(parentCollectionId, conditionId, indexSet).
//
// parent_collection_id: raw 32 bytes (bytes32)
// condition_id: raw 32 bytes (big-endian)
// index_set: uint (1 = outcome 0 = YES, 2 = outcome 1 = NO in binary markets)
// Returns: raw 32 bytes (big-endian), the collection ID
inline std::string get_collection_id(const std::string &parent_collection_id,
                                     const std::string &condition_id, uint32_t index_set) {
  assert(parent_collection_id.size() == 32);
  assert(condition_id.size() == 32);
  detail::CTXPtr ctx;
  detail::ECPoint p1;
  detail::hash_to_curve(condition_id, index_set, p1.x, p1.y, ctx);

  detail::BNPtr xr, yr;
  bool parent_is_zero = std::all_of(parent_collection_id.begin(), parent_collection_id.end(),
                                    [](char c) { return c == '\0'; });
  if (parent_is_zero) {
    BN_copy(xr, p1.x);
    BN_copy(yr, p1.y);
  } else {
    detail::ECPoint p2;
    detail::decode_collection_point(parent_collection_id, p2.x, p2.y, ctx);
    detail::ec_add(p1.x, p1.y, p2.x, p2.y, xr, yr, ctx);
  }

  if (BN_is_odd(yr))
    BN_set_bit(xr, 254);
  std::string result(32, '\0');
  BN_bn2binpad(xr, reinterpret_cast<unsigned char *>(result.data()), 32);
  return result;
}

// Top-level helper: CTHelpers.getCollectionId(bytes32(0), conditionId, indexSet)
inline std::string get_collection_id(const std::string &condition_id, uint32_t index_set) {
  std::string parent(32, '\0');
  return get_collection_id(parent, condition_id, index_set);
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
