#pragma once

#include <cassert>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

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
            const std::string &proxy_url = "");

  ~RpcClient();

  int64_t eth_blockNumber();
  std::future<BatchResult> eth_getLogs_batch_async(const std::vector<LogsQuery> &queries);

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
  void parse_proxy_url(const std::string &proxy_url);
  static void socks5_handshake(asio::ip::tcp::socket &sock, const std::string &dst_host, int dst_port);
  void parse_url(const std::string &url);
  void ensure_connected();
  void disconnect();
  std::string http_post(const std::string &body);
  static std::string to_hex(int64_t value);
  static int64_t from_hex(const std::string &hex);

  std::string host_;
  std::string port_;
  std::string target_;
  std::string api_key_;
  std::string worker_name_;
  std::string proxy_host_;
  std::string proxy_port_;
  bool use_ssl_ = false;
  size_t last_response_size_ = 0;

  asio::io_context ioc_;
  asio::ssl::context ssl_ctx_;
  std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> ssl_stream_;
  std::unique_ptr<beast::tcp_stream> tcp_stream_;
  bool connected_ = false;

  std::thread worker_thread_;
  std::mutex worker_mutex_;
  std::condition_variable worker_cv_;
  bool worker_running_ = false;
  std::deque<PendingRequest> request_queue_;
};
