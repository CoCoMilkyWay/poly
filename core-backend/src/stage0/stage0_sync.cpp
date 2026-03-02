#include "stage0_sync.hpp"

#include "misc/profiler.hpp"
#include "../infra/rpc_transport.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <cctype>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <openssl/ssl.h>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace stage0 {
namespace {

constexpr std::string_view kEmptyRangeSql = "(SELECT 1 WHERE 1=0)";
constexpr const char *kGammaApiBase = "https://gamma-api.polymarket.com";
constexpr int64_t kMaxDispatchAheadBlocks = 20000;
constexpr int kFetchTimeoutMs = 1100;
constexpr int kFetchMaxAttempts = 1;
constexpr int kFetchRetryDelayMs = 0;

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

std::string to_lower_ascii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string strip_hex_prefix(std::string s) {
  if (s.starts_with("0x") || s.starts_with("0X")) {
    return s.substr(2);
  }
  return s;
}

std::string normalize_hex_id_no_prefix(std::string s) {
  s = to_lower_ascii(strip_hex_prefix(std::move(s)));
  assert(!s.empty());
  size_t first_non_zero = s.find_first_not_of('0');
  if (first_non_zero == std::string::npos) {
    return "0";
  }
  return s.substr(first_non_zero);
}

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

std::string hex_to_blob_exact(const std::string &hex, size_t expect_bytes) {
  std::string h = strip_hex_prefix(hex);
  assert((h.size() % 2) == 0);
  std::string out;
  out.reserve(h.size() / 2);
  for (size_t i = 0; i < h.size(); i += 2) {
    uint8_t v = static_cast<uint8_t>((hex_nibble(h[i]) << 4) | hex_nibble(h[i + 1]));
    out.push_back(static_cast<char>(v));
  }
  assert(out.size() == expect_bytes);
  return out;
}

std::string blob_to_hex_lower(const std::string &blob) {
  static const char hex_chars[] = "0123456789abcdef";
  std::string out = "0x";
  out.reserve(2 + blob.size() * 2);
  for (unsigned char c : blob) {
    out.push_back(hex_chars[c >> 4]);
    out.push_back(hex_chars[c & 0x0f]);
  }
  return out;
}

duckdb::Value make_blob_value(const std::string &blob) {
  return duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()), blob.size());
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

int64_t parse_i64_value(const json &v) {
  if (v.is_number_integer()) {
    return v.get<int64_t>();
  }
  if (v.is_number_unsigned()) {
    return static_cast<int64_t>(v.get<uint64_t>());
  }
  if (v.is_string()) {
    return std::stoll(v.get<std::string>());
  }
  assert(false && "invalid int64 field");
  return 0;
}

double parse_double_value(const json &v) {
  if (v.is_number()) {
    return v.get<double>();
  }
  if (v.is_string()) {
    return std::stod(v.get<std::string>());
  }
  assert(false && "invalid double field");
  return 0.0;
}

std::optional<std::string> get_opt_string_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  const auto &v = obj.at(key);
  assert(v.is_string());
  std::string s = v.get<std::string>();
  if (s.empty()) {
    return std::nullopt;
  }
  return s;
}

std::optional<int64_t> get_opt_i64_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  return parse_i64_value(obj.at(key));
}

std::optional<double> get_opt_double_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  return parse_double_value(obj.at(key));
}

std::optional<bool> get_opt_bool_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  const auto &v = obj.at(key);
  assert(v.is_boolean());
  return v.get<bool>();
}

std::optional<duckdb::timestamp_t> get_opt_timestamp_field(const json &obj, const char *key) {
  auto s = get_opt_string_field(obj, key);
  if (!s.has_value()) {
    return std::nullopt;
  }
  return duckdb::Timestamp::FromString(*s, true);
}

std::optional<duckdb::date_t> get_opt_date_field(const json &obj, const char *key) {
  auto s = get_opt_string_field(obj, key);
  if (!s.has_value()) {
    return std::nullopt;
  }
  return duckdb::Date::FromString(*s);
}

std::optional<std::string> get_opt_blob_field(const json &obj, const char *key, size_t expect_bytes) {
  auto s = get_opt_string_field(obj, key);
  if (!s.has_value()) {
    return std::nullopt;
  }
  return hex_to_blob_exact(*s, expect_bytes);
}

json get_array_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return json::array();
  }
  const auto &v = obj.at(key);
  if (v.is_array()) {
    return v;
  }
  if (v.is_string()) {
    json parsed = json::parse(v.get<std::string>());
    assert(parsed.is_array());
    return parsed;
  }
  assert(false && "array field must be array/string");
  return json::array();
}

void push_opt_i64(std::vector<duckdb::Value> &dst, const std::optional<int64_t> &v) {
  if (v.has_value()) {
    dst.emplace_back(*v);
  } else {
    dst.emplace_back();
  }
}

void push_opt_double(std::vector<duckdb::Value> &dst, const std::optional<double> &v) {
  if (v.has_value()) {
    dst.emplace_back(*v);
  } else {
    dst.emplace_back();
  }
}

void push_opt_bool(std::vector<duckdb::Value> &dst, const std::optional<bool> &v) {
  if (v.has_value()) {
    dst.emplace_back(*v);
  } else {
    dst.emplace_back();
  }
}

void push_opt_string(std::vector<duckdb::Value> &dst, const std::optional<std::string> &v) {
  if (v.has_value()) {
    dst.emplace_back(*v);
  } else {
    dst.emplace_back();
  }
}

void push_opt_timestamp(std::vector<duckdb::Value> &dst, const std::optional<duckdb::timestamp_t> &v) {
  if (v.has_value()) {
    dst.emplace_back(duckdb::Value::TIMESTAMP(*v));
  } else {
    dst.emplace_back();
  }
}

void push_opt_date(std::vector<duckdb::Value> &dst, const std::optional<duckdb::date_t> &v) {
  if (v.has_value()) {
    dst.emplace_back(duckdb::Value::DATE(*v));
  } else {
    dst.emplace_back();
  }
}

void push_opt_blob(std::vector<duckdb::Value> &dst, const std::optional<std::string> &v) {
  if (v.has_value()) {
    dst.push_back(make_blob_value(*v));
  } else {
    dst.emplace_back();
  }
}

template <typename T>
void append_opt_scalar(duckdb::Appender &ap, const std::optional<T> &v) {
  if (v.has_value()) {
    ap.Append(*v);
  } else {
    ap.Append(duckdb::Value());
  }
}

void append_opt_string(duckdb::Appender &ap, const std::optional<std::string> &v) {
  if (v.has_value()) {
    ap.Append(duckdb::Value(*v));
  } else {
    ap.Append(duckdb::Value());
  }
}

void append_opt_timestamp(duckdb::Appender &ap, const std::optional<duckdb::timestamp_t> &v) {
  if (v.has_value()) {
    ap.Append(duckdb::Value::TIMESTAMP(*v));
  } else {
    ap.Append(duckdb::Value());
  }
}

void append_opt_blob(duckdb::Appender &ap, const std::optional<std::string> &v) {
  if (v.has_value()) {
    ap.Append(make_blob_value(*v));
  } else {
    ap.Append(duckdb::Value());
  }
}

struct HttpGetOutcome {
  bool ok = false;
  long status_code = 0;
  std::string body;
};

enum class FetchSeedState {
  kFound,
  kEmpty,
  kFailed,
};

struct FetchSeedOutcome {
  FetchSeedState state = FetchSeedState::kFailed;
  json market = json::object();
};

std::string build_gamma_target(std::string base_target, const std::string &condition_hex_lower) {
  if (base_target.empty()) {
    base_target = "/";
  }
  if (!base_target.ends_with('/')) {
    base_target.push_back('/');
  }
  return base_target + "markets?condition_ids=" + condition_hex_lower + "&include_tag=true";
}

class AsyncHttpsGetOp final : public std::enable_shared_from_this<AsyncHttpsGetOp> {
public:
  using DoneFn = std::function<void(HttpGetOutcome)>;

  AsyncHttpsGetOp(asio::io_context &ioc, asio::ssl::context &ssl_ctx, std::string host, std::string port,
                  std::string target, std::chrono::milliseconds timeout, DoneFn done)
      : resolver_(ioc), stream_(ioc, ssl_ctx), timer_(ioc), host_(std::move(host)), port_(std::move(port)),
        target_(std::move(target)), timeout_(timeout), done_(std::move(done)) {}

  void start() {
    const int sni_ok = SSL_set_tlsext_host_name(stream_.native_handle(), host_.c_str());
    assert(sni_ok == 1);
    req_.version(11);
    req_.method(http::verb::get);
    req_.target(target_);
    req_.set(http::field::host, host_);
    req_.set(http::field::user_agent, "PolySync/1.0");
    req_.set(http::field::accept, "application/json");
    req_.set(http::field::connection, "close");
    arm_timeout();
    auto self = shared_from_this();
    resolver_.async_resolve(host_, port_,
                            [self](const boost::system::error_code &ec, const tcp::resolver::results_type &results) {
                              self->on_resolve(ec, results);
                            });
  }

private:
  void arm_timeout() {
    timer_.expires_after(timeout_);
    auto self = shared_from_this();
    timer_.async_wait([self](const boost::system::error_code &ec) {
      if (ec || self->finished_) {
        return;
      }
      self->resolver_.cancel();
      beast::error_code ignored;
      auto &sock = beast::get_lowest_layer(self->stream_).socket();
      [[maybe_unused]] auto _ = sock.cancel(ignored);
      [[maybe_unused]] auto __ = sock.shutdown(tcp::socket::shutdown_both, ignored);
      [[maybe_unused]] auto ___ = sock.close(ignored);
    });
  }

  void finish(HttpGetOutcome out) {
    if (finished_) {
      return;
    }
    finished_ = true;
    timer_.cancel();
    done_(std::move(out));
  }

  void on_resolve(const boost::system::error_code &ec, const tcp::resolver::results_type &results) {
    if (ec) {
      finish(HttpGetOutcome{});
      return;
    }
    beast::get_lowest_layer(stream_).expires_after(timeout_);
    auto self = shared_from_this();
    beast::get_lowest_layer(stream_).async_connect(
        results, [self](const boost::system::error_code &ec2, const tcp::resolver::results_type::endpoint_type &) {
          self->on_connect(ec2);
        });
  }

  void on_connect(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{});
      return;
    }
    auto self = shared_from_this();
    stream_.async_handshake(asio::ssl::stream_base::client,
                            [self](const boost::system::error_code &ec2) { self->on_handshake(ec2); });
  }

  void on_handshake(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{});
      return;
    }
    auto self = shared_from_this();
    http::async_write(stream_, req_, [self](const boost::system::error_code &ec2, std::size_t) {
      self->on_write(ec2);
    });
  }

  void on_write(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{});
      return;
    }
    auto self = shared_from_this();
    http::async_read(stream_, buffer_, res_, [self](const boost::system::error_code &ec2, std::size_t) {
      self->on_read(ec2);
    });
  }

  void on_read(const boost::system::error_code &ec) {
    if (ec) {
      finish(HttpGetOutcome{});
      return;
    }
    finish(HttpGetOutcome{
        .ok = true,
        .status_code = res_.result_int(),
        .body = std::move(res_.body()),
    });
  }

  tcp::resolver resolver_;
  beast::ssl_stream<beast::tcp_stream> stream_;
  beast::flat_buffer buffer_;
  http::request<http::empty_body> req_;
  http::response<http::string_body> res_;
  asio::steady_timer timer_;
  std::string host_;
  std::string port_;
  std::string target_;
  std::chrono::milliseconds timeout_;
  DoneFn done_;
  bool finished_ = false;
};

void async_https_get(asio::io_context &ioc, asio::ssl::context &ssl_ctx, const std::string &host,
                     const std::string &port, const std::string &target, std::chrono::milliseconds timeout,
                     std::function<void(HttpGetOutcome)> done) {
  auto op = std::make_shared<AsyncHttpsGetOp>(ioc, ssl_ctx, host, port, target, timeout, std::move(done));
  op->start();
}

FetchSeedOutcome parse_seed_outcome(const std::string &condition_hex_lower, HttpGetOutcome out) {
  if (!out.ok || out.status_code != 200) {
    return FetchSeedOutcome{
        .state = FetchSeedState::kFailed,
        .market = json::object(),
    };
  }
  json arr = json::parse(out.body);
  assert(arr.is_array());
  const std::string condition_norm = normalize_hex_id_no_prefix(condition_hex_lower);
  for (const auto &item : arr) {
    assert(item.is_object());
    std::optional<std::string> cid_raw;
    if (item.contains("conditionId") && item.at("conditionId").is_string()) {
      cid_raw = item.at("conditionId").get<std::string>();
    } else if (item.contains("condition_id") && item.at("condition_id").is_string()) {
      cid_raw = item.at("condition_id").get<std::string>();
    }
    if (!cid_raw.has_value()) {
      continue;
    }
    std::string cid = normalize_hex_id_no_prefix(*cid_raw);
    if (cid == condition_norm) {
      return FetchSeedOutcome{
          .state = FetchSeedState::kFound,
          .market = item,
      };
    }
  }
  return FetchSeedOutcome{
      .state = FetchSeedState::kEmpty,
      .market = json::object(),
  };
}

class AsyncSeedFetchOp final : public std::enable_shared_from_this<AsyncSeedFetchOp> {
public:
  using DoneFn = std::function<void(FetchSeedOutcome)>;

  AsyncSeedFetchOp(asio::io_context &ioc, asio::ssl::context &ssl_ctx, RpcEndpoint endpoint,
                   std::string condition_hex_lower, DoneFn done)
      : ioc_(ioc), ssl_ctx_(ssl_ctx), retry_timer_(ioc), endpoint_(std::move(endpoint)),
        condition_hex_lower_(std::move(condition_hex_lower)), done_(std::move(done)) {}

  void start() {
    attempt_once();
  }

private:
  void attempt_once() {
    attempts_ += 1;
    std::string target = build_gamma_target(endpoint_.target, condition_hex_lower_);
    auto self = shared_from_this();
    async_https_get(ioc_, ssl_ctx_, endpoint_.host, endpoint_.port, target, std::chrono::milliseconds(kFetchTimeoutMs),
                    [self](HttpGetOutcome out) { self->on_http(std::move(out)); });
  }

  void on_http(HttpGetOutcome out) {
    FetchSeedOutcome parsed = parse_seed_outcome(condition_hex_lower_, std::move(out));
    if (parsed.state == FetchSeedState::kFailed && attempts_ < kFetchMaxAttempts) {
      if (kFetchRetryDelayMs <= 0) {
        attempt_once();
        return;
      }
      auto self = shared_from_this();
      retry_timer_.expires_after(std::chrono::milliseconds(kFetchRetryDelayMs));
      retry_timer_.async_wait([self](const boost::system::error_code &ec) {
        if (ec || self->finished_) {
          return;
        }
        self->attempt_once();
      });
      return;
    }
    finish(std::move(parsed));
  }

  void finish(FetchSeedOutcome out) {
    if (finished_) {
      return;
    }
    finished_ = true;
    retry_timer_.cancel();
    done_(std::move(out));
  }

  asio::io_context &ioc_;
  asio::ssl::context &ssl_ctx_;
  asio::steady_timer retry_timer_;
  RpcEndpoint endpoint_;
  std::string condition_hex_lower_;
  DoneFn done_;
  int attempts_ = 0;
  bool finished_ = false;
};

duckdb::Value make_list_value(const duckdb::LogicalType &child_type, std::vector<duckdb::Value> values) {
  return duckdb::Value::LIST(child_type, std::move(values));
}

void append_stage0_flow_log(const std::string &data_dir, const std::string &msg) {
  static std::mutex log_mutex;
  static std::string last_msg;
  std::lock_guard<std::mutex> lock(log_mutex);
  if (msg == last_msg) {
    return;
  }
  last_msg = msg;
  const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
  std::ofstream f(data_dir + "/log", std::ios::app);
  assert(f.is_open());
  f << now_ms << " " << msg << "\n";
  f.flush();
  assert(f.good());
}

} // namespace

StageSync::StageSync(const Config &config, Database &stage1_db, Database &stage0_db,
                     int base_interval_seconds)
    : config_(config), stage1_db_(stage1_db), stage0_db_(stage0_db),
      base_interval_seconds_(base_interval_seconds) {
  init_schema();
  ensure_cursor_floor();
  load_known_conditions();
  sync_.last_block = get_cursor();
  sync_.head_block = sync_.last_block;
  sync_.behind_blocks = 0;
  sync_.condition_count = static_cast<int64_t>(known_condition_ids_.size());
  sync_.syncing = false;
}

void StageSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(0);
}

void StageSync::stop() {
  stop_requested_ = true;
}

StageSync::Status StageSync::status() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return sync_;
}

void StageSync::schedule_sync(int delay_seconds) {
  if (stop_requested_) {
    return;
  }
  auto timer = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer->async_wait([this, timer](const boost::system::error_code &ec) {
    if (!ec && !stop_requested_) {
      do_sync();
    }
  });
}

void StageSync::record_commit_locked(int64_t cursor) {
  commit_history_.push_back({std::chrono::steady_clock::now(), cursor});
  if (commit_history_.size() > kEtaWindowSize) {
    commit_history_.pop_front();
  }
}

void StageSync::refresh_status_locked(int64_t head_block, int64_t cursor, bool syncing) {
  sync_.syncing = syncing;
  sync_.head_block = head_block;
  sync_.last_block = cursor;
  sync_.behind_blocks = std::max<int64_t>(0, head_block - cursor);
  sync_.condition_count = static_cast<int64_t>(known_condition_ids_.size());
  if (commit_history_.size() < 2) {
    sync_.blocks_per_second = 0.0;
    sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0 : -1.0;
    return;
  }
  const auto &first = commit_history_.front();
  const auto &last = commit_history_.back();
  double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
  if (elapsed_s <= 0.0) {
    sync_.blocks_per_second = 0.0;
    sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0 : -1.0;
    return;
  }
  int64_t committed_blocks = std::max<int64_t>(0, last.cursor - first.cursor);
  if (committed_blocks == 0) {
    sync_.blocks_per_second = 0.0;
    sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0 : -1.0;
    return;
  }
  sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
  sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0
                                                 : static_cast<double>(sync_.behind_blocks) / sync_.blocks_per_second;
}

void StageSync::do_sync() {
  TraceN("s0/sync");
  if (stop_requested_) {
    return;
  }
  ensure_cursor_floor();

  int64_t stage1_head = stage1_db_.get_last_block();
  int64_t cursor = get_cursor();
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    refresh_status_locked(stage1_head, cursor, true);
  }

  if (stage1_head <= cursor) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      refresh_status_locked(stage1_head, cursor, false);
    }
    schedule_sync(base_interval_seconds_);
    return;
  }

  assert(config_.proxy_url.empty() && "stage0 异步 beast 抓取暂不支持 proxy");
  RpcEndpoint gamma_ep = parse_rpc_endpoint(kGammaApiBase);
  assert(gamma_ep.use_ssl);

  asio::io_context fetch_ioc;
  asio::ssl::context fetch_ssl_ctx(asio::ssl::context::tls_client);
  fetch_ssl_ctx.set_verify_mode(asio::ssl::verify_none);

  auto conn = stage0_db_.create_connection();
  auto exec_sql = [&](const std::string &sql) {
    auto r = conn->Query(sql);
    assert(r && !r->HasError());
  };
  exec_sql("BEGIN TRANSACTION");
  duckdb::Appender appender(*conn, "pm_condition_static");

  int64_t next_dispatch_block = cursor + 1;
  int64_t next_commit_block = cursor + 1;
  std::vector<std::shared_ptr<InFlightTask>> inflight;
  inflight.reserve(static_cast<size_t>(kWorkerCount));
  std::map<int64_t, BlockTaskResult> ready;
  std::map<int64_t, std::vector<ConditionSeed>> scanned_seeds;
  int64_t scanned_to_block = cursor;
  int scheduler_sleep_ms = kSchedulerSleepMs;
  std::vector<int64_t> worker_blocks(static_cast<size_t>(kWorkerCount), -1);
  int64_t applied_block = cursor;

  std::function<void(const std::shared_ptr<InFlightTask> &)> dispatch_next_seed;
  dispatch_next_seed = [&](const std::shared_ptr<InFlightTask> &task) {
    assert(task->next_seed_index <= task->seeds.size());
    if (task->next_seed_index == task->seeds.size()) {
      task->done = true;
      return;
    }
    const ConditionSeed seed = task->seeds[task->next_seed_index];
    task->next_seed_index += 1;
    auto on_done = [&, task, seed](FetchSeedOutcome out) mutable {
      if (out.state == FetchSeedState::kFound) {
        task->rows.push_back(FetchResult{
            .seed = seed,
            .market = std::move(out.market),
        });
      } else if (out.state == FetchSeedState::kEmpty) {
        task->empty_seeds += 1;
      } else {
        task->failed_seeds += 1;
      }
      dispatch_next_seed(task);
    };
    std::make_shared<AsyncSeedFetchOp>(fetch_ioc, fetch_ssl_ctx, gamma_ep, seed.condition_hex_lower,
                                       std::move(on_done))
        ->start();
  };

  while (!stop_requested_ && next_commit_block <= stage1_head) {
    bool progressed = false;
    fetch_ioc.restart();
    while (!stop_requested_ && static_cast<int>(inflight.size()) < kWorkerCount &&
           next_dispatch_block <= stage1_head &&
           next_dispatch_block <= next_commit_block + kMaxDispatchAheadBlocks) {
      if (next_dispatch_block > scanned_to_block) {
        SeedScanBatch batch = load_seed_scan_batch(next_dispatch_block, stage1_head, static_cast<size_t>(kWorkerCount));
        scanned_to_block = batch.scanned_to_block;
        const bool no_pending_before_dispatch =
            inflight.empty() && ready.empty() && (next_commit_block == next_dispatch_block);
        if (no_pending_before_dispatch) {
          if (batch.seeds_by_block.empty()) {
            applied_block = scanned_to_block;
            {
              std::lock_guard<std::mutex> lock(status_mutex_);
              record_commit_locked(scanned_to_block);
              refresh_status_locked(stage1_head, scanned_to_block, true);
            }
            next_commit_block = scanned_to_block + 1;
            next_dispatch_block = next_commit_block;
            progressed = true;
            continue;
          }
          int64_t first_seed_block = batch.seeds_by_block.begin()->first;
          if (first_seed_block > next_commit_block) {
            applied_block = first_seed_block - 1;
            {
              std::lock_guard<std::mutex> lock(status_mutex_);
              record_commit_locked(first_seed_block - 1);
              refresh_status_locked(stage1_head, first_seed_block - 1, true);
            }
            next_commit_block = first_seed_block;
            next_dispatch_block = first_seed_block;
          }
        }
        for (auto &it : batch.seeds_by_block) {
          auto [ins_it, inserted] = scanned_seeds.emplace(it.first, std::move(it.second));
          assert(inserted);
        }
      }

      const int64_t block = next_dispatch_block;
      next_dispatch_block += 1;
      auto seeds_it = scanned_seeds.find(block);
      if (seeds_it == scanned_seeds.end()) {
        auto [it, inserted] = ready.emplace(block, BlockTaskResult{.block = block, .rows = {}});
        assert(inserted);
        continue;
      }

      std::vector<ConditionSeed> seeds = std::move(seeds_it->second);
      scanned_seeds.erase(seeds_it);
      int worker_slot = -1;
      for (int i = 0; i < kWorkerCount; ++i) {
        if (worker_blocks[static_cast<size_t>(i)] < 0) {
          worker_slot = i;
          break;
        }
      }
      assert(worker_slot >= 0);
      worker_blocks[static_cast<size_t>(worker_slot)] = block;
      append_stage0_flow_log(stage0_db_.data_dir(),
                             "worker_start worker=" + std::to_string(worker_slot) +
                                 " block=" + std::to_string(block) +
                                 " seeds=" + std::to_string(seeds.size()));
      auto task = std::make_shared<InFlightTask>();
      task->worker_slot = worker_slot;
      task->block = block;
      task->started_at = std::chrono::steady_clock::now();
      task->seeds = std::move(seeds);
      task->rows.reserve(task->seeds.size());
      inflight.push_back(task);
      dispatch_next_seed(task);
      progressed = true;
    }

    if (fetch_ioc.poll() > 0) {
      progressed = true;
    }

    for (size_t i = 0; i < inflight.size();) {
      auto &task = inflight[i];
      if (!task->done) {
        ++i;
        continue;
      }
      assert(task->worker_slot >= 0 && task->worker_slot < kWorkerCount);
      const int64_t cost_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - task->started_at)
                                  .count();
      worker_blocks[static_cast<size_t>(task->worker_slot)] = -1;
      std::string status = "success";
      if (task->failed_seeds > 0) {
        status = "fail";
      } else if (task->rows.empty()) {
        status = "empty";
      }
      append_stage0_flow_log(stage0_db_.data_dir(),
                             "worker_done worker=" + std::to_string(task->worker_slot) +
                                 " block=" + std::to_string(task->block) +
                                 " status=" + status +
                                 " rows=" + std::to_string(task->rows.size()) +
                                 " fail=" + std::to_string(task->failed_seeds) +
                                 " cost_ms=" + std::to_string(cost_ms));
      auto [it, inserted] = ready.emplace(task->block, BlockTaskResult{
                                                           .block = task->block,
                                                           .rows = std::move(task->rows),
                                                       });
      assert(inserted);
      inflight[i] = std::move(inflight.back());
      inflight.pop_back();
      progressed = true;
    }

    while (!stop_requested_) {
      auto it = ready.find(next_commit_block);
      if (it == ready.end()) {
        break;
      }
      std::vector<FetchResult> rows_to_persist;
      rows_to_persist.reserve(it->second.rows.size());
      for (auto &row : it->second.rows) {
        if (known_condition_ids_.contains(row.seed.condition_hex_lower)) {
          continue;
        }
        rows_to_persist.push_back(std::move(row));
      }
      if (!rows_to_persist.empty()) {
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        persist_results_in_txn(appender, rows_to_persist, now_ms);
        for (const auto &row : rows_to_persist) {
          known_condition_ids_.insert(row.seed.condition_hex_lower);
        }
      }
      applied_block = next_commit_block;
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        record_commit_locked(applied_block);
        refresh_status_locked(stage1_head, applied_block, true);
      }
      ready.erase(it);
      next_commit_block += 1;
      progressed = true;
    }

    if (stop_requested_) {
      break;
    }
    if (!progressed) {
      std::this_thread::sleep_for(std::chrono::milliseconds(scheduler_sleep_ms));
      scheduler_sleep_ms = std::min(scheduler_sleep_ms << 1, kSchedulerSleepMaxMs);
    } else {
      scheduler_sleep_ms = kSchedulerSleepMs;
    }
  }

  appender.Close();

  if (stop_requested_) {
    exec_sql("ROLLBACK");
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      sync_.syncing = false;
    }
    return;
  }

  set_cursor_in_txn(*conn, applied_block);
  exec_sql("COMMIT");
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    refresh_status_locked(stage1_head, applied_block, false);
  }
  schedule_sync((applied_block < stage1_head) ? 0 : base_interval_seconds_);
}

void StageSync::init_schema() {
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_sync_cursor ("
      "id INTEGER PRIMARY KEY CHECK (id = 0), "
      "last_block BIGINT NOT NULL"
      ")");
  stage0_db_.execute(
      "INSERT OR IGNORE INTO pm_sync_cursor (id, last_block) VALUES (0, -1)");
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_condition_static ("
      "id BIGINT NOT NULL, "
      "condition_id BLOB PRIMARY KEY, "
      "question_id BLOB NOT NULL, "
      "market_slug TEXT NOT NULL, "
      "market_question TEXT NOT NULL, "
      "market_description TEXT, "
      "market_start_date TIMESTAMP, "
      "market_end_date TIMESTAMP, "
      "market_created_at TIMESTAMP, "
      "market_image TEXT, "
      "market_icon TEXT, "
      "market_submitted_by BLOB, "
      "market_resolved_by BLOB, "
      "market_restricted BOOLEAN, "
      "market_neg_risk BOOLEAN NOT NULL, "
      "market_neg_risk_request_id TEXT, "
      "market_cyom BOOLEAN, "
      "market_group_item_title TEXT, "
      "market_group_item_threshold TEXT, "
      "market_enable_order_book BOOLEAN, "
      "market_order_min_size DOUBLE, "
      "market_order_min_tick DOUBLE, "
      "market_clear_book_on_start BOOLEAN, "
      "market_manual_activation BOOLEAN, "
      "market_automatically_active BOOLEAN, "
      "market_uma_bond TEXT, "
      "market_uma_reward TEXT, "
      "market_rewards_min_size DOUBLE, "
      "market_rewards_max_spread DOUBLE, "
      "market_holding_rewards_enable BOOLEAN, "
      "market_rfq_enabled BOOLEAN, "
      "market_fees_enabled BOOLEAN, "
      "market_fee_type TEXT, "
      "market_series_color TEXT, "
      "market_show_gmp_series BOOLEAN, "
      "market_show_gmp_outcome BOOLEAN, "
      "event_ids BIGINT[], "
      "event_tickers TEXT[], "
      "event_slugs TEXT[], "
      "event_titles TEXT[], "
      "event_descriptions TEXT[], "
      "event_resolution_sources TEXT[], "
      "event_start_dates TIMESTAMP[], "
      "event_creation_dates TIMESTAMP[], "
      "event_end_dates TIMESTAMP[], "
      "event_created_ats TIMESTAMP[], "
      "event_images TEXT[], "
      "event_icons TEXT[], "
      "event_start_times TIMESTAMP[], "
      "event_gmp_chart_modes TEXT[], "
      "event_enable_order_books BOOLEAN[], "
      "event_neg_risks BOOLEAN[], "
      "event_enable_neg_risks BOOLEAN[], "
      "event_show_all_outcomes BOOLEAN[], "
      "event_show_market_images BOOLEAN[], "
      "event_auto_resolveds BOOLEAN[], "
      "event_auto_actives BOOLEAN[], "
      "event_cyoms BOOLEAN[], "
      "event_requires_translations BOOLEAN[], "
      "tag_ids BIGINT[], "
      "tag_labels TEXT[], "
      "tag_slugs TEXT[], "
      "tag_created_ats TIMESTAMP[], "
      "reward_ids BIGINT[], "
      "reward_condition_ids BLOB[], "
      "reward_asset_addresses BLOB[], "
      "reward_start_dates DATE[], "
      "reward_end_dates DATE[], "
      "first_seen_block BIGINT NOT NULL, "
      "first_seen_ms BIGINT NOT NULL"
      ")");
}

void StageSync::load_known_conditions() {
  known_condition_ids_.clear();
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query("SELECT lower(hex(condition_id)) AS cid FROM pm_condition_static");
  assert(result && !result->HasError());
  known_condition_ids_.reserve(static_cast<size_t>(result->RowCount()) * 2 + 1);
  for (idx_t row = 0; row < result->RowCount(); ++row) {
    std::string cid = result->GetValue(0, row).GetValue<std::string>();
    known_condition_ids_.insert("0x" + cid);
  }
}

void StageSync::ensure_cursor_floor() {
  int64_t floor_block = config_.initial_block - 1;
  int64_t cursor = get_cursor();
  if (cursor >= floor_block) {
    return;
  }
  auto conn = stage0_db_.create_connection();
  auto begin = conn->Query("BEGIN TRANSACTION");
  assert(begin && !begin->HasError());
  set_cursor_in_txn(*conn, floor_block);
  auto commit = conn->Query("COMMIT");
  assert(commit && !commit->HasError());
}

StageSync::SeedScanBatch StageSync::load_seed_scan_batch(int64_t start_block, int64_t head_block,
                                                         size_t max_conditions) const {
  SeedScanBatch out;
  assert(start_block <= head_block);
  int64_t end_block = std::min<int64_t>(head_block, start_block + kSeedScanBlockSpan - 1);
  out.scanned_to_block = end_block;

  std::string range_sql = stage1_db_.feather_table_range("condition_preparation", start_block, end_block);
  if (range_sql == kEmptyRangeSql) {
    return out;
  }

  std::string sql =
      "SELECT block_number, condition_id, question_id "
      "FROM " +
      range_sql +
      " WHERE block_number >= " + std::to_string(start_block) +
      " AND block_number <= " + std::to_string(end_block) +
      " ORDER BY block_number ASC, log_index ASC";

  auto conn = stage1_db_.create_connection();
  auto result = conn->Query(sql);
  assert(result && !result->HasError());

  std::unordered_set<std::string> seen_in_block;
  int64_t current_block = -1;
  size_t scanned_conditions = 0;

  for (idx_t row = 0; row < result->RowCount(); ++row) {
    int64_t block_number = result->GetValue(0, row).GetValue<int64_t>();
    std::string cond_blob = result->GetValue(1, row).GetValueUnsafe<std::string>();
    std::string question_blob = result->GetValue(2, row).GetValueUnsafe<std::string>();
    assert(cond_blob.size() == 32);
    assert(question_blob.size() == 32);

    if (block_number != current_block) {
      current_block = block_number;
      seen_in_block.clear();
    }

    std::string cond_hex = blob_to_hex_lower(cond_blob);
    if (seen_in_block.contains(cond_hex)) {
      continue;
    }
    seen_in_block.insert(cond_hex);
    out.seeds_by_block[block_number].push_back(ConditionSeed{
        .condition_blob = std::move(cond_blob),
        .question_blob = std::move(question_blob),
        .condition_hex_lower = std::move(cond_hex),
        .first_seen_block = block_number,
    });

    scanned_conditions += 1;
    if (scanned_conditions >= max_conditions) {
      out.scanned_to_block = block_number;
      break;
    }
  }
  return out;
}

void StageSync::persist_results_in_txn(duckdb::Appender &ap, const std::vector<FetchResult> &rows, int64_t now_ms) {
  if (!rows.empty()) {
    for (const auto &row : rows) {
      const json &m = row.market;
      assert(m.is_object());

      auto market_question_id = get_opt_blob_field(m, "questionID", 32);
      if (market_question_id.has_value()) {
        assert(*market_question_id == row.seed.question_blob);
      }
      auto market_condition_id = get_opt_blob_field(m, "conditionId", 32);
      if (market_condition_id.has_value()) {
        assert(*market_condition_id == row.seed.condition_blob);
      }

      int64_t market_id = parse_i64_value(m.at("id"));
      std::string market_slug = require_string_field(m, "slug");
      std::string market_question = require_string_field(m, "question");
      auto market_description = get_opt_string_field(m, "description");
      auto market_start_date = get_opt_timestamp_field(m, "startDate");
      auto market_end_date = get_opt_timestamp_field(m, "endDate");
      auto market_created_at = get_opt_timestamp_field(m, "createdAt");
      auto market_image = get_opt_string_field(m, "image");
      auto market_icon = get_opt_string_field(m, "icon");
      auto market_submitted_by = get_opt_blob_field(m, "submitted_by", 20);
      auto market_resolved_by = get_opt_blob_field(m, "resolvedBy", 20);
      auto market_restricted = get_opt_bool_field(m, "restricted");
      auto market_neg_risk = get_opt_bool_field(m, "negRisk");
      assert(market_neg_risk.has_value());
      auto market_neg_risk_request_id = get_opt_string_field(m, "negRiskRequestID");
      auto market_cyom = get_opt_bool_field(m, "cyom");
      auto market_group_item_title = get_opt_string_field(m, "groupItemTitle");
      auto market_group_item_threshold = get_opt_string_field(m, "groupItemThreshold");
      auto market_enable_order_book = get_opt_bool_field(m, "enableOrderBook");
      auto market_order_min_size = get_opt_double_field(m, "orderMinSize");
      auto market_order_min_tick = get_opt_double_field(m, "orderPriceMinTickSize");
      auto market_clear_book_on_start = get_opt_bool_field(m, "clearBookOnStart");
      auto market_manual_activation = get_opt_bool_field(m, "manualActivation");
      auto market_automatically_active = get_opt_bool_field(m, "automaticallyActive");
      auto market_uma_bond = get_opt_string_field(m, "umaBond");
      auto market_uma_reward = get_opt_string_field(m, "umaReward");
      auto market_rewards_min_size = get_opt_double_field(m, "rewardsMinSize");
      auto market_rewards_max_spread = get_opt_double_field(m, "rewardsMaxSpread");
      auto market_holding_rewards_enable = get_opt_bool_field(m, "holdingRewardsEnabled");
      auto market_rfq_enabled = get_opt_bool_field(m, "rfqEnabled");
      auto market_fees_enabled = get_opt_bool_field(m, "feesEnabled");
      auto market_fee_type = get_opt_string_field(m, "feeType");
      auto market_series_color = get_opt_string_field(m, "seriesColor");
      auto market_show_gmp_series = get_opt_bool_field(m, "showGmpSeries");
      auto market_show_gmp_outcome = get_opt_bool_field(m, "showGmpOutcome");

      json events = get_array_field(m, "events");
      std::vector<duckdb::Value> event_ids;
      std::vector<duckdb::Value> event_tickers;
      std::vector<duckdb::Value> event_slugs;
      std::vector<duckdb::Value> event_titles;
      std::vector<duckdb::Value> event_descriptions;
      std::vector<duckdb::Value> event_resolution_sources;
      std::vector<duckdb::Value> event_start_dates;
      std::vector<duckdb::Value> event_creation_dates;
      std::vector<duckdb::Value> event_end_dates;
      std::vector<duckdb::Value> event_created_ats;
      std::vector<duckdb::Value> event_images;
      std::vector<duckdb::Value> event_icons;
      std::vector<duckdb::Value> event_start_times;
      std::vector<duckdb::Value> event_gmp_chart_modes;
      std::vector<duckdb::Value> event_enable_order_books;
      std::vector<duckdb::Value> event_neg_risks;
      std::vector<duckdb::Value> event_enable_neg_risks;
      std::vector<duckdb::Value> event_show_all_outcomes;
      std::vector<duckdb::Value> event_show_market_images;
      std::vector<duckdb::Value> event_auto_resolveds;
      std::vector<duckdb::Value> event_auto_actives;
      std::vector<duckdb::Value> event_cyoms;
      std::vector<duckdb::Value> event_requires_translations;
      size_t events_size = events.size();
      event_ids.reserve(events_size);
      event_tickers.reserve(events_size);
      event_slugs.reserve(events_size);
      event_titles.reserve(events_size);
      event_descriptions.reserve(events_size);
      event_resolution_sources.reserve(events_size);
      event_start_dates.reserve(events_size);
      event_creation_dates.reserve(events_size);
      event_end_dates.reserve(events_size);
      event_created_ats.reserve(events_size);
      event_images.reserve(events_size);
      event_icons.reserve(events_size);
      event_start_times.reserve(events_size);
      event_gmp_chart_modes.reserve(events_size);
      event_enable_order_books.reserve(events_size);
      event_neg_risks.reserve(events_size);
      event_enable_neg_risks.reserve(events_size);
      event_show_all_outcomes.reserve(events_size);
      event_show_market_images.reserve(events_size);
      event_auto_resolveds.reserve(events_size);
      event_auto_actives.reserve(events_size);
      event_cyoms.reserve(events_size);
      event_requires_translations.reserve(events_size);
      for (const auto &e : events) {
        assert(e.is_object());
        push_opt_i64(event_ids, get_opt_i64_field(e, "id"));
        push_opt_string(event_tickers, get_opt_string_field(e, "ticker"));
        push_opt_string(event_slugs, get_opt_string_field(e, "slug"));
        push_opt_string(event_titles, get_opt_string_field(e, "title"));
        push_opt_string(event_descriptions, get_opt_string_field(e, "description"));
        push_opt_string(event_resolution_sources, get_opt_string_field(e, "resolutionSource"));
        push_opt_timestamp(event_start_dates, get_opt_timestamp_field(e, "startDate"));
        push_opt_timestamp(event_creation_dates, get_opt_timestamp_field(e, "creationDate"));
        push_opt_timestamp(event_end_dates, get_opt_timestamp_field(e, "endDate"));
        push_opt_timestamp(event_created_ats, get_opt_timestamp_field(e, "createdAt"));
        push_opt_string(event_images, get_opt_string_field(e, "image"));
        push_opt_string(event_icons, get_opt_string_field(e, "icon"));
        push_opt_timestamp(event_start_times, get_opt_timestamp_field(e, "startTime"));
        push_opt_string(event_gmp_chart_modes, get_opt_string_field(e, "gmpChartMode"));
        push_opt_bool(event_enable_order_books, get_opt_bool_field(e, "enableOrderBook"));
        push_opt_bool(event_neg_risks, get_opt_bool_field(e, "negRisk"));
        push_opt_bool(event_enable_neg_risks, get_opt_bool_field(e, "enableNegRisk"));
        push_opt_bool(event_show_all_outcomes, get_opt_bool_field(e, "showAllOutcomes"));
        push_opt_bool(event_show_market_images, get_opt_bool_field(e, "showMarketImages"));
        push_opt_bool(event_auto_resolveds, get_opt_bool_field(e, "automaticallyResolved"));
        push_opt_bool(event_auto_actives, get_opt_bool_field(e, "automaticallyActive"));
        push_opt_bool(event_cyoms, get_opt_bool_field(e, "cyom"));
        push_opt_bool(event_requires_translations, get_opt_bool_field(e, "requiresTranslation"));
      }

      json tags = get_array_field(m, "tags");
      std::vector<duckdb::Value> tag_ids;
      std::vector<duckdb::Value> tag_labels;
      std::vector<duckdb::Value> tag_slugs;
      std::vector<duckdb::Value> tag_created_ats;
      size_t tags_size = tags.size();
      tag_ids.reserve(tags_size);
      tag_labels.reserve(tags_size);
      tag_slugs.reserve(tags_size);
      tag_created_ats.reserve(tags_size);
      for (const auto &t : tags) {
        assert(t.is_object());
        push_opt_i64(tag_ids, get_opt_i64_field(t, "id"));
        push_opt_string(tag_labels, get_opt_string_field(t, "label"));
        push_opt_string(tag_slugs, get_opt_string_field(t, "slug"));
        push_opt_timestamp(tag_created_ats, get_opt_timestamp_field(t, "createdAt"));
      }

      json rewards = get_array_field(m, "clobRewards");
      std::vector<duckdb::Value> reward_ids;
      std::vector<duckdb::Value> reward_condition_ids;
      std::vector<duckdb::Value> reward_asset_addresses;
      std::vector<duckdb::Value> reward_start_dates;
      std::vector<duckdb::Value> reward_end_dates;
      size_t rewards_size = rewards.size();
      reward_ids.reserve(rewards_size);
      reward_condition_ids.reserve(rewards_size);
      reward_asset_addresses.reserve(rewards_size);
      reward_start_dates.reserve(rewards_size);
      reward_end_dates.reserve(rewards_size);
      for (const auto &r : rewards) {
        assert(r.is_object());
        push_opt_i64(reward_ids, get_opt_i64_field(r, "id"));
        push_opt_blob(reward_condition_ids, get_opt_blob_field(r, "conditionId", 32));
        push_opt_blob(reward_asset_addresses, get_opt_blob_field(r, "assetAddress", 20));
        push_opt_date(reward_start_dates, get_opt_date_field(r, "startDate"));
        push_opt_date(reward_end_dates, get_opt_date_field(r, "endDate"));
      }

      ap.BeginRow();
      ap.Append(market_id);
      ap.Append(make_blob_value(row.seed.condition_blob));
      ap.Append(make_blob_value(row.seed.question_blob));
      ap.Append(duckdb::Value(market_slug));
      ap.Append(duckdb::Value(market_question));
      append_opt_string(ap, market_description);
      append_opt_timestamp(ap, market_start_date);
      append_opt_timestamp(ap, market_end_date);
      append_opt_timestamp(ap, market_created_at);
      append_opt_string(ap, market_image);
      append_opt_string(ap, market_icon);
      append_opt_blob(ap, market_submitted_by);
      append_opt_blob(ap, market_resolved_by);
      append_opt_scalar(ap, market_restricted);
      ap.Append(*market_neg_risk);
      append_opt_string(ap, market_neg_risk_request_id);
      append_opt_scalar(ap, market_cyom);
      append_opt_string(ap, market_group_item_title);
      append_opt_string(ap, market_group_item_threshold);
      append_opt_scalar(ap, market_enable_order_book);
      append_opt_scalar(ap, market_order_min_size);
      append_opt_scalar(ap, market_order_min_tick);
      append_opt_scalar(ap, market_clear_book_on_start);
      append_opt_scalar(ap, market_manual_activation);
      append_opt_scalar(ap, market_automatically_active);
      append_opt_string(ap, market_uma_bond);
      append_opt_string(ap, market_uma_reward);
      append_opt_scalar(ap, market_rewards_min_size);
      append_opt_scalar(ap, market_rewards_max_spread);
      append_opt_scalar(ap, market_holding_rewards_enable);
      append_opt_scalar(ap, market_rfq_enabled);
      append_opt_scalar(ap, market_fees_enabled);
      append_opt_string(ap, market_fee_type);
      append_opt_string(ap, market_series_color);
      append_opt_scalar(ap, market_show_gmp_series);
      append_opt_scalar(ap, market_show_gmp_outcome);

      ap.Append(make_list_value(duckdb::LogicalType::BIGINT, std::move(event_ids)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_tickers)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_slugs)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_titles)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_descriptions)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_resolution_sources)));
      ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_start_dates)));
      ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_creation_dates)));
      ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_end_dates)));
      ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_created_ats)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_images)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_icons)));
      ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_start_times)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_gmp_chart_modes)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_enable_order_books)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_neg_risks)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_enable_neg_risks)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_show_all_outcomes)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_show_market_images)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_auto_resolveds)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_auto_actives)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_cyoms)));
      ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_requires_translations)));

      ap.Append(make_list_value(duckdb::LogicalType::BIGINT, std::move(tag_ids)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(tag_labels)));
      ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(tag_slugs)));
      ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(tag_created_ats)));

      ap.Append(make_list_value(duckdb::LogicalType::BIGINT, std::move(reward_ids)));
      ap.Append(make_list_value(duckdb::LogicalType::BLOB, std::move(reward_condition_ids)));
      ap.Append(make_list_value(duckdb::LogicalType::BLOB, std::move(reward_asset_addresses)));
      ap.Append(make_list_value(duckdb::LogicalType::DATE, std::move(reward_start_dates)));
      ap.Append(make_list_value(duckdb::LogicalType::DATE, std::move(reward_end_dates)));

      ap.Append(row.seed.first_seen_block);
      ap.Append(now_ms);
      ap.EndRow();
    }
  }
}

int64_t StageSync::get_cursor() {
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query("SELECT last_block FROM pm_sync_cursor WHERE id = 0");
  assert(result && !result->HasError());
  if (result->RowCount() == 0) {
    return -1;
  }
  return result->GetValue(0, 0).GetValue<int64_t>();
}

void StageSync::set_cursor_in_txn(duckdb::Connection &conn, int64_t block) {
  auto result = conn.Query("UPDATE pm_sync_cursor SET last_block = " + std::to_string(block) + " WHERE id = 0");
  assert(result && !result->HasError());
}

} // namespace stage0
