#pragma once

#include <chrono>

namespace infra::network {

inline constexpr long kRpcConnectTimeoutSeconds = 30L;
inline constexpr long kRpcRequestTimeoutSeconds = 60L * 10;
inline constexpr auto kRpcConnectTimeout = std::chrono::seconds(kRpcConnectTimeoutSeconds);
inline constexpr auto kRpcRequestTimeout = std::chrono::seconds(kRpcRequestTimeoutSeconds);

} // namespace infra::network