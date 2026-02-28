#include "rpc_client.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

#include "misc/profiler.hpp"
#include "rpc_transport.hpp"
#include "rpc_transport_beast.hpp"
#include "rpc_transport_curl.hpp"

namespace {
class NonRetryableRpcError : public std::runtime_error {
public:
  explicit NonRetryableRpcError(const std::string &msg) : std::runtime_error(msg) {}
};
}

RpcClient::RpcClient(const std::string &url, const std::string &api_key, const std::string &worker_name,
                     const std::string &proxy_url, const std::string &transport_type)
    : transport_type_(transport_type), worker_name_(worker_name) {
  (void)api_key;
  if (transport_type_ == "beast") {
    transport_ = std::make_unique<RpcTransportBeast>(url, proxy_url);
  } else if (transport_type_ == "curl") {
    transport_ = std::make_unique<RpcTransportCurl>(url, proxy_url);
  } else {
    assert(false && "rpc_transport 必须是 beast 或 curl");
  }
  assert(transport_ != nullptr);
  start_worker();
}

RpcClient::~RpcClient() {
  stop_worker();
}

int64_t RpcClient::eth_blockNumber() {
  json request = {
      {"jsonrpc", "2.0"},
      {"id", 1},
      {"method", "eth_blockNumber"},
      {"params", json::array()}};

  std::string response_body = execute_request(request.dump());
  json response = json::parse(response_body);
  if (response.contains("error")) {
    throw std::runtime_error("eth_blockNumber RPC error: " + response["error"].dump());
  }
  return from_hex(response["result"].get<std::string>());
}

std::future<RpcClient::BatchResult> RpcClient::eth_getLogs_batch_async(const std::vector<LogsQuery> &queries) {
  auto promise = std::make_shared<std::promise<BatchResult>>();
  auto future = promise->get_future();
  std::string body = build_batch_request(queries);
  size_t count = queries.size();

  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    request_queue_.push_back(PendingRequest{
        .body = std::move(body),
        .promise = promise,
        .count = count,
    });
    worker_cv_.notify_one();
  }

  return future;
}

void RpcClient::cancel() {
  transport_->cancel();
  std::deque<PendingRequest> pending;
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    pending.swap(request_queue_);
  }
  for (auto &req : pending) {
    BatchResult result;
    result.success = false;
    result.retryable = true;
    result.error_msg = "RPC cancelled";
    req.promise->set_value(std::move(result));
  }
}

void RpcClient::start_worker() {
  worker_running_ = true;
  worker_thread_ = std::thread([this]() {
    TraceThread(worker_name_.c_str());
    TraceN("rpc/thread_start");
    worker_loop();
  });
}

void RpcClient::stop_worker() {
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    worker_running_ = false;
    request_queue_.clear();
    worker_cv_.notify_one();
  }
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
}

void RpcClient::worker_loop() {
  while (true) {
    std::string body;
    std::shared_ptr<std::promise<BatchResult>> promise;
    size_t count = 0;

    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      TraceN("rpc/wait");
      worker_cv_.wait(lock, [this] { return !worker_running_ || !request_queue_.empty(); });

      if (!worker_running_ && request_queue_.empty()) {
        break;
      }

      if (!request_queue_.empty()) {
        auto req = std::move(request_queue_.front());
        request_queue_.pop_front();
        body = std::move(req.body);
        promise = std::move(req.promise);
        count = req.count;
      }
    }

    if (promise) {
      TraceN("rpc/dispatch");
      BatchResult result;
      try {
        std::string response_body = post_json(body);
        result.response_bytes = last_response_size_;
        if (count > 0) {
          try {
            result.results = parse_batch_response(response_body, count);
          } catch (const NonRetryableRpcError &e) {
            throw NonRetryableRpcError(std::string(e.what()) + " | response=" + response_body);
          } catch (const std::exception &e) {
            throw std::runtime_error(std::string(e.what()) + " | response=" + response_body);
          }
        } else {
          result.raw_body = std::move(response_body);
        }
        result.success = true;
        result.retryable = true;
        TraceN("rpc/success");
      } catch (const NonRetryableRpcError &e) {
        result.success = false;
        result.retryable = false;
        result.error_msg = e.what();
        TraceN("rpc/fail");
      } catch (const std::exception &e) {
        result.success = false;
        result.retryable = true;
        result.error_msg = e.what();
        TraceN("rpc/fail");
      }
      promise->set_value(std::move(result));
    }
  }
}

std::string RpcClient::execute_request(const std::string &body) {
  auto promise = std::make_shared<std::promise<BatchResult>>();
  auto future = promise->get_future();

  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    request_queue_.push_back(PendingRequest{
        .body = body,
        .promise = promise,
        .count = 0,
    });
    worker_cv_.notify_one();
  }

  auto result = future.get();
  if (!result.success) {
    std::cerr << "[RPC] Request failed: " << result.error_msg << std::endl;
    throw std::runtime_error(result.error_msg);
  }
  return result.raw_body;
}

std::string RpcClient::build_batch_request(const std::vector<LogsQuery> &queries) {
  json batch = json::array();
  for (const auto &[address, from_block, to_block, topic0_list] : queries) {
    json filter = {
        {"fromBlock", to_hex(from_block)},
        {"toBlock", to_hex(to_block)}};
    if (address.has_value()) {
      filter["address"] = address.value();
    }
    if (!topic0_list.empty()) {
      filter["topics"] = json::array({topic0_list});
    }
    batch.push_back({{"jsonrpc", "2.0"},
                     {"id", batch.size()},
                     {"method", "eth_getLogs"},
                     {"params", json::array({filter})}});
  }
  return batch.dump();
}

std::vector<json> RpcClient::parse_batch_response(const std::string &response_body, size_t count) {
  json responses = json::parse(response_body);
  if (!responses.is_array()) {
    throw NonRetryableRpcError("batch response not array");
  }
  if (responses.size() != count) {
    throw NonRetryableRpcError("batch response count mismatch");
  }
  std::vector<json> results(count);
  for (const auto &resp : responses) {
    if (!resp.is_object()) {
      throw NonRetryableRpcError("batch item is not object");
    }
    if (resp.contains("error")) {
      size_t id = resp.value("id", 0);
      throw std::runtime_error("RPC error id=" + std::to_string(id) + ": " + resp["error"].dump());
    }
    if (!resp.contains("id") || !resp["id"].is_number_unsigned()) {
      throw NonRetryableRpcError("batch item missing/invalid id");
    }
    if (!resp.contains("result") || !resp["result"].is_array()) {
      throw NonRetryableRpcError("batch item missing/invalid result");
    }
    size_t id = resp["id"].get<size_t>();
    if (id >= count) {
      throw NonRetryableRpcError("batch response id out of range");
    }
    if (!results[id].is_null()) {
      throw NonRetryableRpcError("batch response id duplicated");
    }
    for (const auto &log : resp["result"]) {
      if (!log.is_object()) {
        throw NonRetryableRpcError("log item is not object");
      }
      if (!log.contains("address") || !log["address"].is_string()) {
        throw NonRetryableRpcError("log missing/invalid address");
      }
      if (!log.contains("topics") || !log["topics"].is_array()) {
        throw NonRetryableRpcError("log missing/invalid topics");
      }
      if (!log.contains("data") || !log["data"].is_string()) {
        throw NonRetryableRpcError("log missing/invalid data");
      }
      if (!log.contains("transactionHash") || !log["transactionHash"].is_string()) {
        throw NonRetryableRpcError("log missing/invalid transactionHash");
      }
      if (!log.contains("blockNumber") || !log["blockNumber"].is_string()) {
        throw NonRetryableRpcError("log missing/invalid blockNumber");
      }
      if (!log.contains("logIndex") || !log["logIndex"].is_string()) {
        throw NonRetryableRpcError("log missing/invalid logIndex");
      }
      for (const auto &topic : log["topics"]) {
        if (!topic.is_string()) {
          throw NonRetryableRpcError("log topic is not string");
        }
      }
    }
    results[id] = resp["result"];
  }
  for (size_t i = 0; i < count; ++i) {
    if (results[i].is_null()) {
      throw NonRetryableRpcError("batch response missing item");
    }
  }
  return results;
}

std::string RpcClient::post_json(const std::string &body) {
  std::string result = transport_->post_json(body);
  last_response_size_ = transport_->last_response_size();
  return result;
}

std::string RpcClient::to_hex(int64_t value) {
  std::stringstream ss;
  ss << "0x" << std::hex << value;
  return ss.str();
}

int64_t RpcClient::from_hex(const std::string &hex) {
  return std::stoll(hex, nullptr, 16);
}
