#pragma once

#include <cassert>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace core::mem {

constexpr int64_t kNodeOverheadBytes = 32;

inline int64_t estimate_string_extra(const std::string &s) {
  return static_cast<int64_t>(s.capacity());
}

template <typename T>
inline int64_t estimate_vector_plain(const std::vector<T> &v) {
  return static_cast<int64_t>(sizeof(v)) + static_cast<int64_t>(v.capacity()) * static_cast<int64_t>(sizeof(T));
}

template <typename T, typename ExtraFn>
inline int64_t estimate_vector(const std::vector<T> &v, ExtraFn extra_fn) {
  int64_t bytes = estimate_vector_plain(v);
  for (const auto &item : v) {
    bytes += extra_fn(item);
  }
  return bytes;
}

template <typename K, typename V, typename H, typename Eq, typename Alloc, typename KeyExtraFn, typename ValExtraFn>
inline int64_t estimate_unordered_map(const std::unordered_map<K, V, H, Eq, Alloc> &m, KeyExtraFn key_extra_fn,
                                      ValExtraFn val_extra_fn) {
  int64_t bytes = static_cast<int64_t>(sizeof(m));
  bytes += static_cast<int64_t>(m.bucket_count()) * static_cast<int64_t>(sizeof(void *));
  bytes += static_cast<int64_t>(m.size()) *
           (static_cast<int64_t>(sizeof(typename std::unordered_map<K, V, H, Eq, Alloc>::value_type)) + kNodeOverheadBytes);
  for (const auto &kv : m) {
    bytes += key_extra_fn(kv.first);
    bytes += val_extra_fn(kv.second);
  }
  return bytes;
}

template <typename K, typename H, typename Eq, typename Alloc, typename KeyExtraFn>
inline int64_t estimate_unordered_set(const std::unordered_set<K, H, Eq, Alloc> &s, KeyExtraFn key_extra_fn) {
  int64_t bytes = static_cast<int64_t>(sizeof(s));
  bytes += static_cast<int64_t>(s.bucket_count()) * static_cast<int64_t>(sizeof(void *));
  bytes += static_cast<int64_t>(s.size()) *
           (static_cast<int64_t>(sizeof(typename std::unordered_set<K, H, Eq, Alloc>::value_type)) + kNodeOverheadBytes);
  for (const auto &k : s) {
    bytes += key_extra_fn(k);
  }
  return bytes;
}

inline int64_t get_process_rss_bytes() {
  std::ifstream status("/proc/self/status");
  assert(status.is_open());
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      std::istringstream iss(line.substr(6));
      int64_t val = 0;
      std::string unit;
      iss >> val >> unit;
      if (unit == "kB" || unit == "KB") {
        return val * 1024;
      }
      return val;
    }
  }
  return 0;
}

} // namespace core::mem
