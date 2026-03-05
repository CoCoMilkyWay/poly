#pragma once

#include <functional>
#include <string>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>

#include "../infra/rpc_transport.hpp"

namespace asio = boost::asio;

namespace stage0 {

enum class FetchSeedState {
  kFound,
  kEmpty,
  kFailed,
};

struct FetchSeedOutcome {
  FetchSeedState state = FetchSeedState::kFailed;
  nlohmann::json market = nlohmann::json::object();
  std::string detail;
};

void async_seed_fetch(asio::io_context &ioc, asio::ssl::context &ssl_ctx, const RpcEndpoint &endpoint,
                      const std::string &proxy_url, const std::string &condition_hex_lower,
                      std::function<void(FetchSeedOutcome)> done,
                      std::function<void(int, const std::string &)> on_retry);

} // namespace stage0
