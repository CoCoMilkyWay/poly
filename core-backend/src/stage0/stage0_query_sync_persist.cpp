#include "stage0_query_sync.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>

namespace stage0 {
namespace {

uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<uint8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<uint8_t>(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return static_cast<uint8_t>(c - 'A' + 10);
  }
  assert(false && "invalid hex nibble");
  return 0;
}

[[noreturn]] void blob_parse_fail(const std::string &condition_hex_lower, const char *key,
                                  const std::string &raw, const std::string &reason,
                                  const json &market_for_log) {
  std::cerr << "[Stage0][blob_parse_fail]"
            << " condition=" << condition_hex_lower
            << " field=" << key
            << " reason=" << reason
            << " raw=" << raw
            << " body=" << market_for_log.dump() << std::endl;
  std::abort();
}

bool has_non_null(const json &obj, const char *key) {
  return obj.contains(key) && !obj.at(key).is_null();
}

std::string require_string_field(const json &obj, const char *key) {
  assert(has_non_null(obj, key));
  const auto &v = obj.at(key);
  assert(v.is_string());
  return v.get<std::string>();
}

std::string hex_to_blob_exact(const std::string &hex, size_t expect_bytes,
                              const std::string &condition_hex_lower, const char *key,
                              const json &market_for_log) {
  std::string h = hex;
  if (h.starts_with("0x") || h.starts_with("0X")) {
    h = h.substr(2);
  }
  if (h.empty()) {
    blob_parse_fail(condition_hex_lower, key, hex, "empty_hex", market_for_log);
  }
  if ((h.size() % 2) != 0) {
    h.insert(h.begin(), '0');
  }
  if (h.size() > expect_bytes * 2) {
    blob_parse_fail(condition_hex_lower, key, hex, "hex_too_long", market_for_log);
  }
  if (h.size() < expect_bytes * 2) {
    h.insert(0, expect_bytes * 2 - h.size(), '0');
  }
  if (h.size() != expect_bytes * 2) {
    blob_parse_fail(condition_hex_lower, key, hex, "hex_size_mismatch", market_for_log);
  }
  for (char c : h) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!ok) {
      blob_parse_fail(condition_hex_lower, key, hex, "invalid_hex_char", market_for_log);
    }
  }
  std::string out;
  out.reserve(expect_bytes);
  for (size_t i = 0; i < h.size(); i += 2) {
    uint8_t v = static_cast<uint8_t>((hex_nibble(h[i]) << 4) | hex_nibble(h[i + 1]));
    out.push_back(static_cast<char>(v));
  }
  assert(out.size() == expect_bytes);
  return out;
}

std::optional<std::string> get_opt_blob_field_strict(const json &obj, const char *key, size_t expect_bytes,
                                                     const std::string &condition_hex_lower,
                                                     const json &market_for_log) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  std::string s = require_string_field(obj, key);
  return hex_to_blob_exact(s, expect_bytes, condition_hex_lower, key, market_for_log);
}

duckdb::Value make_blob_value(const std::string &blob) {
  return duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()), blob.size());
}

} // namespace

void QuerySync::persist_results_in_txn(duckdb::Appender &ap, const std::vector<FetchResult> &rows) {
  for (const auto &row : rows) {
    const json &market = row.market;
    assert(market.is_object());

    auto market_condition_id =
        get_opt_blob_field_strict(market, "conditionId", 32, row.seed.condition_hex_lower, market);
    if (market_condition_id.has_value()) {
      assert(*market_condition_id == row.seed.condition_blob);
    }

    ap.BeginRow();
    ap.Append(make_blob_value(row.seed.condition_blob));
    ap.Append(duckdb::Value(market.dump()));
    ap.EndRow();
  }
}

} // namespace stage0
