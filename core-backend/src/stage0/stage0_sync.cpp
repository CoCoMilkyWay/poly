#include "stage0_sync.hpp"

#include "misc/profiler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <curl/curl.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace stage0 {
namespace {

constexpr std::string_view kEmptyRangeSql = "(SELECT 1 WHERE 1=0)";
constexpr const char *kGammaApiBase = "https://gamma-api.polymarket.com";

struct HttpResponse {
  CURLcode curl_code = CURLE_OK;
  long status_code = 0;
  std::string body;
  std::string error;
};

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

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t bytes = size * nmemb;
  auto *out = static_cast<std::string *>(userdata);
  out->append(ptr, bytes);
  return bytes;
}

HttpResponse http_get_once(const std::string &url, const std::string &proxy_url) {
  static std::once_flag curl_init_once;
  std::call_once(curl_init_once, []() {
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    assert(rc == CURLE_OK);
  });

  HttpResponse resp;
  CURL *easy = curl_easy_init();
  assert(easy != nullptr);

  char errbuf[CURL_ERROR_SIZE] = {0};
  curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &curl_write_cb);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, &resp.body);
  curl_easy_setopt(easy, CURLOPT_USERAGENT, "PolySync/1.0");
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, errbuf);
  if (!proxy_url.empty()) {
    curl_easy_setopt(easy, CURLOPT_PROXY, proxy_url.c_str());
  }

  resp.curl_code = curl_easy_perform(easy);
  if (resp.curl_code == CURLE_OK) {
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &resp.status_code);
  } else {
    resp.error = errbuf[0] ? std::string(errbuf) : std::string(curl_easy_strerror(resp.curl_code));
  }
  curl_easy_cleanup(easy);
  return resp;
}

duckdb::Value make_list_value(const duckdb::LogicalType &child_type, std::vector<duckdb::Value> values) {
  return duckdb::Value::LIST(child_type, std::move(values));
}

} // namespace

StageSync::StageSync(const Config &config, Database &stage1_db, Database &stage0_db,
                     int base_interval_seconds)
    : config_(config), stage1_db_(stage1_db), stage0_db_(stage0_db),
      base_interval_seconds_(base_interval_seconds) {
  init_schema();
  ensure_cursor_floor();
  load_known_conditions();
  sync_.last_block = stage0_db_.get_last_block();
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
  int64_t cursor = stage0_db_.get_last_block();
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

  int64_t next_dispatch_block = cursor + 1;
  int64_t next_commit_block = cursor + 1;
  std::vector<InFlightTask> inflight;
  inflight.reserve(static_cast<size_t>(kWorkerCount));
  std::map<int64_t, BlockTaskResult> ready;
  std::map<int64_t, std::vector<ConditionSeed>> scanned_seeds;
  int64_t scanned_to_block = cursor;
  int scheduler_sleep_ms = kSchedulerSleepMs;

  while (!stop_requested_ && next_commit_block <= stage1_head) {
    while (!stop_requested_ && static_cast<int>(inflight.size()) < kWorkerCount &&
           next_dispatch_block <= stage1_head) {
      if (next_dispatch_block > scanned_to_block) {
        SeedScanBatch batch = load_seed_scan_batch(next_dispatch_block, stage1_head, static_cast<size_t>(kWorkerCount));
        scanned_to_block = batch.scanned_to_block;
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
      inflight.push_back(InFlightTask{
          .future = std::async(std::launch::async, [this, block, seeds = std::move(seeds)]() {
            return process_block_with_retry(block, seeds);
          }),
      });
    }

    bool progressed = false;
    for (size_t i = 0; i < inflight.size();) {
      auto &task = inflight[i];
      if (task.future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        ++i;
        continue;
      }
      BlockTaskResult result = task.future.get();
      auto [it, inserted] = ready.emplace(result.block, std::move(result));
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
        persist_results(rows_to_persist);
        for (const auto &row : rows_to_persist) {
          known_condition_ids_.insert(row.seed.condition_hex_lower);
        }
      }
      advance_cursor(next_commit_block);
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        record_commit_locked(next_commit_block);
        refresh_status_locked(stage1_head, next_commit_block, true);
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

  if (stop_requested_) {
    {
      std::lock_guard<std::mutex> lock(status_mutex_);
      sync_.syncing = false;
    }
    return;
  }

  int64_t new_cursor = stage0_db_.get_last_block();
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    refresh_status_locked(stage1_head, new_cursor, false);
  }
  schedule_sync((new_cursor < stage1_head) ? 0 : base_interval_seconds_);
}

void StageSync::init_schema() {
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
  int64_t cursor = stage0_db_.get_last_block();
  if (cursor >= floor_block) {
    return;
  }
  bool locked = stage0_db_.try_write_lock();
  assert(locked && "stage0 db state写锁被占用");
  stage0_db_.set_last_block(floor_block);
  stage0_db_.release_write_lock();
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

json StageSync::fetch_market_by_condition(const std::string &condition_hex_lower) {
  std::string url = std::string(kGammaApiBase) + "/markets?condition_ids=" + condition_hex_lower + "&include_tag=true";
  const std::string condition_norm = normalize_hex_id_no_prefix(condition_hex_lower);
  constexpr int kBaseBackoffMs = 1000;
  constexpr int kBackoffMaxMs = 10000;
  int delay_ms = kBaseBackoffMs;
  while (true) {
    if (stop_requested_) {
      return json::object();
    }
    HttpResponse resp = http_get_once(url, config_.proxy_url);
    bool retryable = false;
    if (resp.curl_code == CURLE_OK && resp.status_code == 200) {
      json arr = json::parse(resp.body);
      assert(arr.is_array());
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
          return item;
        }
      }
      retryable = true;
    } else if (resp.curl_code != CURLE_OK) {
      retryable = (resp.curl_code == CURLE_OPERATION_TIMEDOUT ||
                   resp.curl_code == CURLE_COULDNT_CONNECT ||
                   resp.curl_code == CURLE_COULDNT_RESOLVE_HOST ||
                   resp.curl_code == CURLE_RECV_ERROR ||
                   resp.curl_code == CURLE_SEND_ERROR);
    } else {
      retryable = (resp.status_code == 403 || resp.status_code == 429 || resp.status_code >= 500);
    }
    if (!retryable) {
      assert(false && "Gamma markets请求失败");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    delay_ms = std::min(delay_ms << 1, kBackoffMaxMs);
  }
}

StageSync::BlockTaskResult StageSync::process_block_with_retry(int64_t block, const std::vector<ConditionSeed> &seeds) {
  std::vector<FetchResult> rows;
  rows.reserve(seeds.size());
  for (const auto &seed : seeds) {
    if (stop_requested_) {
      break;
    }
    rows.push_back(FetchResult{
        .seed = seed,
        .market = fetch_market_by_condition(seed.condition_hex_lower),
    });
  }
  return BlockTaskResult{
      .block = block,
      .rows = std::move(rows),
  };
}

void StageSync::persist_results(const std::vector<FetchResult> &rows) {
  if (rows.empty()) {
    return;
  }
  int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

  auto conn = stage0_db_.create_connection();
  auto exec_sql = [&](const std::string &sql) {
    auto r = conn->Query(sql);
    assert(r && !r->HasError());
  };

  exec_sql("BEGIN TRANSACTION");
  {
    duckdb::Appender ap(*conn, "pm_condition_static");
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
    ap.Close();
  }
  exec_sql("COMMIT");
}

void StageSync::advance_cursor(int64_t block) {
  bool locked = stage0_db_.try_write_lock();
  assert(locked && "stage0 db state写锁被占用");
  stage0_db_.set_last_block(block);
  stage0_db_.release_write_lock();
}

} // namespace stage0
