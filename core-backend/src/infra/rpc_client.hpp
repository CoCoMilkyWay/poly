#pragma once

#include <cassert>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using json = nlohmann::json;
class RpcTransport;

class RpcClient {
public:
  using LogsQuery = std::tuple<std::optional<std::string>, int64_t, int64_t, std::vector<std::string>>;

  struct BatchResult {
    std::vector<json> results;
    std::string raw_body;
    size_t response_bytes = 0;
    bool success = false;
    std::string error_msg;
  };

  RpcClient(const std::string &url, const std::string &api_key = "", const std::string &worker_name = "RPC-Worker",
            const std::string &proxy_url = "", const std::string &transport_type = "beast");

  ~RpcClient();

  int64_t eth_blockNumber();
  std::future<BatchResult> eth_getLogs_batch_async(const std::vector<LogsQuery> &queries);
  void cancel();

private:
  struct PendingRequest {
    std::string body;
    std::shared_ptr<std::promise<BatchResult>> promise;
    size_t count = 0;
  };

  void start_worker();
  void stop_worker();
  void worker_loop();
  std::string execute_request(const std::string &body);
  std::string build_batch_request(const std::vector<LogsQuery> &queries);
  std::vector<json> parse_batch_response(const std::string &response_body, size_t count);
  std::string post_json(const std::string &body);
  static std::string to_hex(int64_t value);
  static int64_t from_hex(const std::string &hex);

  std::string transport_type_;
  std::string worker_name_;
  size_t last_response_size_ = 0;
  std::unique_ptr<RpcTransport> transport_;

  std::thread worker_thread_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  bool worker_running_ = false;
  std::deque<PendingRequest> request_queue_;
};
