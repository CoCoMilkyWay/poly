#include "api_session.hpp"

#include <arrow/io/file.h>
#include <arrow/ipc/reader.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include "../core/mem.hpp"
#include "misc/profiler.hpp"

namespace {

std::string sql_quote_literal(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char c : value) {
    if (c == '\'') {
      escaped += "''";
    } else {
      escaped += c;
    }
  }
  return "'" + escaped + "'";
}

std::string sql_quote_ident(const std::string &value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (char c : value) {
    if (c == '"') {
      escaped += "\"\"";
    } else {
      escaped += c;
    }
  }
  return "\"" + escaped + "\"";
}

std::string build_human_readable_select_list(duckdb::Connection &conn, const std::string &arrow_file) {
  std::string describe_sql = "DESCRIBE SELECT * FROM read_arrow(" + sql_quote_literal(arrow_file) + ")";
  auto schema = conn.Query(describe_sql);
  assert(!schema->HasError());

  std::vector<std::string> columns;
  columns.reserve(schema->RowCount());
  for (idx_t i = 0; i < schema->RowCount(); ++i) {
    std::string col_name = schema->GetValue(0, i).GetValueUnsafe<std::string>();
    std::string col_type = schema->GetValue(1, i).GetValueUnsafe<std::string>();
    std::string quoted_col = sql_quote_ident(col_name);
    if (col_type == "BLOB") {
      columns.push_back(
          "'0x' || coalesce(nullif(regexp_replace(lower(hex(" + quoted_col + ")), '^0+', ''), ''), '0') AS " + quoted_col);
    } else if (col_type == "BLOB[]") {
      columns.push_back(
          "list_transform(" + quoted_col +
          ", x -> '0x' || coalesce(nullif(regexp_replace(lower(hex(x)), '^0+', ''), ''), '0')) AS " +
          quoted_col);
    } else {
      columns.push_back(quoted_col);
    }
  }

  if (columns.empty()) {
    return "*";
  }

  std::ostringstream oss;
  for (size_t i = 0; i < columns.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << columns[i];
  }
  return oss.str();
}

std::pair<int64_t, int64_t> parse_feather_chunk_filename(const std::string &filename) {
  assert(filename.ends_with(".feather"));
  std::string stem = filename.substr(0, filename.size() - 8);
  size_t dash = stem.find('-');
  assert(dash != std::string::npos);
  std::string lhs = stem.substr(0, dash);
  std::string rhs = stem.substr(dash + 1);
  assert(!lhs.empty());
  assert(!rhs.empty());
  assert(std::all_of(lhs.begin(), lhs.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }));
  assert(std::all_of(rhs.begin(), rhs.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }));
  int64_t start_block = std::stoll(lhs);
  int64_t end_block = std::stoll(rhs);
  assert(start_block <= end_block);
  return {start_block, end_block};
}

std::string latest_feather_file_path(const std::string &dir) {
  std::string latest;
  int64_t max_end_block = -1;
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    std::string filename = entry.path().filename().string();
    if (!filename.ends_with(".feather")) {
      continue;
    }
    auto parsed = parse_feather_chunk_filename(filename);
    int64_t end_block = parsed.second;
    if (end_block > max_end_block) {
      max_end_block = end_block;
      latest = entry.path().string();
    }
  }
  return latest;
}

int64_t update_peak_bytes(std::atomic<int64_t> &peak, int64_t current) {
  int64_t observed = peak.load(std::memory_order_relaxed);
  while (observed < current &&
         !peak.compare_exchange_weak(observed, current, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
  return peak.load(std::memory_order_relaxed);
}

json to_stage1_status_json(const Stage1Status &status) {
  return {
      {"syncing", status.syncing},
      {"last_block", status.last_block},
      {"head_block", status.head_block},
      {"behind_blocks", status.behind_blocks},
      {"behind_chunks", status.behind_chunks},
      {"blocks_per_second", status.blocks_per_second},
      {"eta_seconds", status.eta_seconds},
      {"bytes_per_block", status.bytes_per_block},
  };
}

json to_stage0_status_json(const Stage0Status &status) {
  const int64_t total_condition_count =
      status.ctf_condition_count + status.negrisk_condition_count + status.nonpoly_condition_count;
  return {
      {"syncing", status.syncing},
      {"last_block", status.last_block},
      {"head_block", status.head_block},
      {"behind_blocks", status.behind_blocks},
      {"condition_count", total_condition_count},
      {"ctf_condition_count", status.ctf_condition_count},
      {"negrisk_condition_count", status.negrisk_condition_count},
      {"nonpoly_condition_count", status.nonpoly_condition_count},
      {"blocks_per_second", status.blocks_per_second},
      {"eta_seconds", status.eta_seconds},
      {"tag_last_block", status.tag_last_block},
      {"tagged_count", status.tagged_count},
      {"untagged_count", status.untagged_count},
      {"tag_device", status.tag_device},
      {"tag_model_path", status.tag_model_path},
      {"tag_model_size_text", status.tag_model_size_text},
  };
}

json to_stage2_status_json(const Stage2Status &status) {
  return {
      {"syncing", status.syncing},
      {"last_block", status.last_block},
      {"head_block", status.head_block},
      {"behind_blocks", status.behind_blocks},
      {"behind_chunks", status.behind_chunks},
      {"blocks_per_second", status.blocks_per_second},
      {"eta_seconds", status.eta_seconds},
      {"ready", status.behind_blocks == 0},
  };
}

json to_stage3_status_json(const Stage3Status &status) {
  return {
      {"syncing", status.syncing},
      {"last_block", status.last_block},
      {"head_block", status.head_block},
      {"behind_blocks", status.behind_blocks},
      {"behind_chunks", status.behind_chunks},
      {"blocks_per_second", status.blocks_per_second},
      {"eta_seconds", status.eta_seconds},
      {"ready", status.behind_blocks == 0},
      {"max_bucket", status.max_bucket},
  };
}

void write_ok_json_response(http::response<http::string_body> &res, const json &payload) {
  res.result(http::status::ok);
  res.body() = payload.dump();
}

bool ensure_required_param(const std::string &value, const char *name, http::response<http::string_body> &res) {
  if (!value.empty()) {
    return true;
  }
  res.result(http::status::bad_request);
  res.body() = json{{"error", std::string("Missing ") + name + " parameter"}}.dump();
  return false;
}

} // namespace

std::mutex ApiSession::s3_meta_cache_mu_;
std::string ApiSession::s3_meta_cache_user_lower_;
std::unordered_map<uint32_t, ApiSession::Stage3CondMeta> ApiSession::s3_meta_cache_cond_meta_;

ApiSession::ApiSession(tcp::socket socket, Database &stage0_db, Database &stage1_db, Database &stage2_db, Database &stage3_db,
                       stage3::StageSync &stage3,
                       Stage0Getter stage0_getter, Stage0Retagger stage0_retagger, Stage1Getter stage1_getter,
                       Stage2Getter stage2_getter, Stage3Getter stage3_getter,
                       Stage0MemGetter stage0_mem_getter, Stage1MemGetter stage1_mem_getter,
                       Stage2MemGetter stage2_mem_getter, Stage3MemGetter stage3_mem_getter)
    : socket_(std::move(socket)), stage0_db_(stage0_db), stage1_db_(stage1_db), stage2_db_(stage2_db), stage3_db_(stage3_db), stage3_(stage3),
      stage0_getter_(std::move(stage0_getter)), stage0_retagger_(std::move(stage0_retagger)),
      stage1_getter_(std::move(stage1_getter)),
      stage2_getter_(std::move(stage2_getter)),
      stage3_getter_(std::move(stage3_getter)),
      stage0_mem_getter_(std::move(stage0_mem_getter)),
      stage1_mem_getter_(std::move(stage1_mem_getter)),
      stage2_mem_getter_(std::move(stage2_mem_getter)),
      stage3_mem_getter_(std::move(stage3_mem_getter)) {}

void ApiSession::run() {
  do_read();
}

void ApiSession::do_read() {
  TraceN("api/read");
  req_ = {};
  http::async_read(socket_, buffer_, req_,
                   [self = shared_from_this()](beast::error_code ec, std::size_t) {
                     if (ec) {
                       return;
                     }
                     self->handle_request();
                   });
}

void ApiSession::handle_request() {
  TraceN("api/handle");
  res_ = {};
  res_.version(req_.version());
  res_.keep_alive(req_.keep_alive());

  res_.set(http::field::access_control_allow_origin, "*");
  res_.set(http::field::access_control_allow_methods, "GET, POST, OPTIONS");
  res_.set(http::field::access_control_allow_headers, "Content-Type");

  if (req_.method() == http::verb::options) {
    res_.result(http::status::ok);
    return do_write();
  }

  std::string target(req_.target());

  try {
    if (target.starts_with("/api/health")) {
      handle_health();
    } else if (target.starts_with("/api/tables")) {
      handle_tables();
    } else if (target.starts_with("/api/query")) {
      handle_query();
    } else if (target.starts_with("/api/export-csv")) {
      handle_export_csv();
    } else if (target.starts_with("/api/table-sample")) {
      handle_table_sample();
    } else if (target.starts_with("/api/stage0-status")) {
      handle_stage0_status();
    } else if (target.starts_with("/api/stage0-retag")) {
      handle_stage0_retag();
    } else if (target.starts_with("/api/stage1-status")) {
      handle_stage1_status();
    } else if (target.starts_with("/api/stage2-status")) {
      handle_stage2_status();
    } else if (target.starts_with("/api/stage2-data")) {
      handle_stage2_data();
    } else if (target.starts_with("/api/stage3-status")) {
      handle_stage3_status();
    } else if (target.starts_with("/api/stage3-filter")) {
      handle_stage3_filter();
    } else if (target.starts_with("/api/stage3-positions")) {
      handle_stage3_positions();
    } else if (target.starts_with("/api/stage3-pnl")) {
      handle_stage3_pnl();
    } else if (target.starts_with("/api/memory")) {
      handle_memory();
    } else {
      res_.result(http::status::not_found);
      res_.set(http::field::content_type, "application/json");
      res_.body() = R"({"error":"Not found"})";
    }
  } catch (const std::exception &e) {
    res_.result(http::status::internal_server_error);
    res_.set(http::field::content_type, "application/json");
    res_.body() = json{{"error", e.what()}}.dump();
  } catch (...) {
    res_.result(http::status::internal_server_error);
    res_.set(http::field::content_type, "application/json");
    res_.body() = R"({"error":"Unknown error"})";
  }

  res_.prepare_payload();
  do_write();
}

void ApiSession::handle_health() {
  res_.result(http::status::ok);
  res_.set(http::field::content_type, "application/json");
  res_.body() = R"({"status":"ok"})";
}

int64_t ApiSession::feather_row_count(const std::string &path) {
  auto file = arrow::io::ReadableFile::Open(path);
  assert(file.ok());
  auto reader = arrow::ipc::RecordBatchFileReader::Open(*file);
  assert(reader.ok());
  auto count = (*reader)->CountRows();
  assert(count.ok());
  return *count;
}

void ApiSession::handle_tables() {
  TraceN("api/tables");
  res_.set(http::field::content_type, "application/json");

  json result = json::object();
  result["duckdb"] = json::array();

  static const char *feather_names[] = {
      "transfer", "condition_preparation", "condition_resolution",
      "split", "merge", "redemption", "fpmm", "fpmm_trade", "fpmm_funding",
      "order_filled", "token_map", "neg_risk_market", "neg_risk_question", "convert"};

  struct TableCache {
    std::unordered_map<std::string, int64_t> files;
    int64_t total = 0;
    std::mutex mtx;
  };
  static std::unordered_map<std::string, TableCache> table_cache;
  static std::once_flag cache_loaded_flag;
  static std::atomic<bool> cache_dirty{false};
  static std::mutex save_mutex;

  std::call_once(cache_loaded_flag, [this]() {
    json cached = stage1_db_.load_counts_cache();
    for (auto &[table, files] : cached.items()) {
      auto &tc = table_cache[table];
      for (auto &[fname, cnt] : files.items()) {
        int64_t c = cnt.get<int64_t>();
        tc.files[fname] = c;
        tc.total += c;
      }
    }
  });

  std::vector<std::future<std::pair<std::string, int64_t>>> futures;
  for (const char *name : feather_names) {
    std::string table_name = name;
    std::string dir = stage1_db_.feather_dir(name);
    if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir)) {
      continue;
    }

    futures.push_back(std::async(std::launch::async, [table_name, dir]() -> std::pair<std::string, int64_t> {
      auto &cache = table_cache[table_name];
      for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        std::string filename = entry.path().filename().string();
        if (!filename.ends_with(".feather")) {
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(cache.mtx);
          if (cache.files.count(filename)) {
            continue;
          }
        }
        int64_t cnt = feather_row_count(entry.path().string());
        std::lock_guard<std::mutex> lock(cache.mtx);
        if (cache.files.emplace(filename, cnt).second) {
          cache.total += cnt;
          cache_dirty = true;
        }
      }
      std::lock_guard<std::mutex> lock(cache.mtx);
      return {table_name, cache.total};
    }));
  }

  json feather_files = json::array();
  for (auto &f : futures) {
    auto [name, count] = f.get();
    feather_files.push_back({{"name", name}, {"count", count}});
  }
  result["feather"] = feather_files;

  if (cache_dirty.exchange(false)) {
    std::lock_guard<std::mutex> slock(save_mutex);
    json cache_json = json::object();
    for (auto &[tname, tc] : table_cache) {
      cache_json[tname] = json::object();
      std::lock_guard<std::mutex> tlock(tc.mtx);
      for (auto &[fname, cnt] : tc.files) {
        cache_json[tname][fname] = cnt;
      }
    }
    stage1_db_.save_counts_cache(cache_json);
  }

  res_.result(http::status::ok);
  res_.body() = result.dump();
}

void ApiSession::handle_stage0_status() {
  TraceN("api/s0_status");
  res_.set(http::field::content_type, "application/json");

  const Stage0Status status = stage0_getter_();
  write_ok_json_response(res_, to_stage0_status_json(status));
}

void ApiSession::handle_stage0_retag() {
  TraceN("api/s0_retag");
  res_.set(http::field::content_type, "application/json");
  if (req_.method() != http::verb::post) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"POST required"})";
    return;
  }
  stage0_retagger_();
  write_ok_json_response(res_, json{{"ok", true}});
}

void ApiSession::handle_stage1_status() {
  TraceN("api/s1_status");
  res_.set(http::field::content_type, "application/json");

  const Stage1Status status = stage1_getter_();
  write_ok_json_response(res_, to_stage1_status_json(status));
}

void ApiSession::handle_stage2_status() {
  TraceN("api/s2_status");
  res_.set(http::field::content_type, "application/json");

  const Stage2Status status = stage2_getter_();
  write_ok_json_response(res_, to_stage2_status_json(status));
}

void ApiSession::handle_stage3_status() {
  TraceN("api/s3_status");
  res_.set(http::field::content_type, "application/json");

  const Stage3Status status = stage3_getter_();
  write_ok_json_response(res_, to_stage3_status_json(status));
}

void ApiSession::handle_query() {
  TraceN("api/query");
  res_.set(http::field::content_type, "application/json");

  std::string query = get_param("q");
  if (query.empty()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"Missing query parameter 'q'"})";
    return;
  }

  std::string upper = query;
  for (auto &c : upper) {
    c = std::toupper(static_cast<unsigned char>(c));
  }

  if (!upper.starts_with("SELECT")) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"Only SELECT queries allowed"})";
    return;
  }

  json result = stage1_db_.query_json(query);
  res_.result(http::status::ok);
  res_.body() = result.dump();
}

void ApiSession::handle_export_csv() {
  TraceN("api/export_csv");
  res_.set(http::field::content_type, "application/json");

  std::string table = get_param("table");
  std::string output = get_param("output");
  std::string limit_str = get_param("limit");
  int64_t limit = limit_str.empty() ? 1000 : std::stoll(limit_str);

  if (table.empty() || output.empty()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"Missing table or output parameter"})";
    return;
  }

  std::string dir = stage1_db_.feather_dir(table);
  if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir)) {
    res_.result(http::status::ok);
    res_.body() = R"({"rows":0})";
    return;
  }

  std::string latest = latest_feather_file_path(dir);

  if (latest.empty()) {
    res_.result(http::status::ok);
    res_.body() = R"({"rows":0})";
    return;
  }

  auto conn = stage1_db_.create_connection();
  std::string select_list = build_human_readable_select_list(*conn, latest);
  std::string sql = "COPY (SELECT " + select_list + " FROM read_arrow(" + sql_quote_literal(latest) + ") LIMIT " +
                    std::to_string(limit) + ") TO " + sql_quote_literal(output) + " (HEADER)";
  auto result = conn->Query(sql);
  assert(!result->HasError());

  res_.result(http::status::ok);
  res_.body() = R"({"rows":1})";
}

void ApiSession::handle_table_sample() {
  TraceN("api/table_sample");
  res_.set(http::field::content_type, "application/json");

  std::string table = get_param("table");
  if (table.empty()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"Missing table parameter"})";
    return;
  }

  std::string dir = stage1_db_.feather_dir(table);
  if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir)) {
    res_.result(http::status::ok);
    res_.body() = "[]";
    return;
  }

  std::string latest = latest_feather_file_path(dir);

  if (latest.empty()) {
    res_.result(http::status::ok);
    res_.body() = "[]";
    return;
  }

  auto conn = stage1_db_.create_connection();
  std::string select_list = build_human_readable_select_list(*conn, latest);
  json result = stage1_db_.query_json("SELECT " + select_list + " FROM read_arrow(" + sql_quote_literal(latest) + ") LIMIT 1");
  res_.result(http::status::ok);
  res_.body() = result.dump();
}

void ApiSession::handle_stage2_data() {
  TraceN("api/s2_detail");
  res_.set(http::field::content_type, "application/json");

  const auto &p = stage3_.stage2_data();
  const auto &ct = p.cond_tree;
  const auto &tt = p.token_tree;
  std::unordered_map<uint8_t, std::string> coll_id_to_addr;
  {
    auto conn = stage2_db_.create_connection();
    auto coll_rows = conn->Query("SELECT coll_id, lower(hex(collateral_addr)) AS addr FROM rb_collateral");
    for (idx_t i = 0; i < coll_rows->RowCount(); ++i) {
      uint8_t coll_id = static_cast<uint8_t>(coll_rows->GetValue(0, i).GetValue<int32_t>());
      std::string addr = "0x" + coll_rows->GetValue(1, i).GetValueUnsafe<std::string>();
      coll_id_to_addr[coll_id] = addr;
    }
  }
  auto resolve_collateral = [&](uint8_t coll_id) {
    auto it = coll_id_to_addr.find(coll_id);
    std::string addr = (it != coll_id_to_addr.end()) ? it->second : stage2::collateral_addr(static_cast<stage2::Collateral>(coll_id));
    const char *known_name = stage2::collateral_name(static_cast<stage2::Collateral>(coll_id));
    std::string name = std::string(known_name).empty() ? addr : std::string(known_name);
    return std::make_pair(addr, name);
  };
  auto by_collateral_from_events = [&](std::initializer_list<uint8_t> event_types) {
    std::unordered_map<uint8_t, int64_t> merged;
    for (const auto &[key, cnt] : p.event_by_collateral) {
      uint8_t event_type = key / 256;
      bool matched = false;
      for (uint8_t et : event_types) {
        if (event_type == et) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        continue;
      }
      uint8_t coll_id = key % 256;
      merged[coll_id] += cnt;
    }
    json arr = json::array();
    for (const auto &[coll_id, cnt] : merged) {
      auto [addr, name] = resolve_collateral(coll_id);
      arr.push_back({
          {"collateral_id", coll_id},
          {"addr", addr},
          {"name", name},
          {"count", cnt},
      });
    }
    return arr;
  };
  const auto &nr = ct.polymarket.token_reg.negrisk_stats;
  json nr_qpm = json::object();
  for (const auto &[k, v] : nr.by_questions_per_market) {
    nr_qpm[std::to_string(k)] = v;
  }
  json cond_by_coll = json::array();
  for (const auto &[coll, cnt] : ct.by_collateral) {
    auto [addr, name] = resolve_collateral(coll);
    cond_by_coll.push_back({
        {"collateral_id", coll},
        {"addr", addr},
        {"name", name},
        {"count", cnt},
    });
  }
  json cov_raw_outcome = json::object();
  for (const auto &[k, v] : ct.coverage.raw_by_outcome_count) {
    cov_raw_outcome[std::to_string(k)] = v;
  }
  int64_t cov_recorded_conditions = ct.coverage.raw_rows;
  json cond_tree = {
      {"total", ct.total},
      {"polymarket",
       {{"total", ct.polymarket.total},
        {"token_reg",
         {{"total", ct.polymarket.token_reg.total},
          {"amm", ct.polymarket.token_reg.amm},
          {"negrisk", ct.polymarket.token_reg.negrisk},
          {"orderbook", ct.polymarket.token_reg.orderbook},
          {"other", ct.polymarket.token_reg.other},
          {"negrisk_stats",
           {{"market_count", nr.market_count},
            {"question_count", nr.question_count},
            {"condition_count", nr.condition_count},
            {"by_questions_per_market", nr_qpm}}}}},
        {"fpmm_poly", ct.polymarket.fpmm_poly}}},
      {"other",
       {{"total", ct.other.total},
        {"prep", ct.other.prep},
        {"fpmm_other", ct.other.fpmm_other},
        {"split", ct.other.split},
        {"merge", ct.other.merge},
        {"redemption", ct.other.redemption}}},
      {"resolve",
       {{"resolved", ct.resolve.resolved},
        {"unresolved", ct.resolve.unresolved}}},
      {"tokenized",
       {{"none", ct.tokenized.none},
        {"partial", ct.tokenized.partial},
        {"full", ct.tokenized.full}}},
      {"by_collateral", cond_by_coll},
      {"coverage",
       {{"recorded_conditions", cov_recorded_conditions},
        {"has_question_id", ct.coverage.raw_has_question_id},
        {"no_question_id", ct.coverage.raw_no_question_id},
        {"by_outcome_count", cov_raw_outcome}}},
  };

  json token_tree = {
      {"total", tt.total},
      {"polymarket",
       {{"total", tt.polymarket.total},
        {"token_reg",
         {{"total", tt.polymarket.token_reg.total},
          {"amm", tt.polymarket.token_reg.amm},
          {"negrisk", tt.polymarket.token_reg.negrisk},
          {"orderbook", tt.polymarket.token_reg.orderbook},
          {"other", tt.polymarket.token_reg.other}}},
        {"fpmm_poly",
         {{"total", tt.polymarket.fpmm_poly.total},
          {"by_collateral", [&]() {
             json arr = json::array();
             for (const auto &[coll, cnt] : tt.polymarket.fpmm_poly.by_collateral) {
               auto [addr, name] = resolve_collateral(coll);
               arr.push_back({
                   {"addr", addr},
                   {"name", name},
                   {"count", cnt},
               });
             }
             return arr;
           }()},
          {"by_collateral_fpmm", [&]() {
             json arr = json::array();
             for (const auto &[coll, cnt] : tt.polymarket.fpmm_poly.by_collateral) {
               auto [addr, name] = resolve_collateral(coll);
               arr.push_back({
                   {"collateral_id", coll},
                   {"addr", addr},
                   {"name", name},
                   {"count", cnt},
               });
             }
             return arr;
           }()}}}}},
      {"other", {{"total", tt.other.total}, {"fpmm_other", tt.other.fpmm_other}, {"split", tt.other.split}, {"merge", tt.other.merge}, {"redemption", tt.other.redemption}, {"transfer_inferred", tt.other.transfer_inferred}, {"by_collateral_transfer_inferred", by_collateral_from_events({
                                                                                                                                                                                                                                                                       static_cast<uint8_t>(stage2::EventType::TransferInNonPoly),
                                                                                                                                                                                                                                                                       static_cast<uint8_t>(stage2::EventType::TransferOutNonPoly),
                                                                                                                                                                                                                                                                   })}}},
  };

  json transfer_tree = {
      {"xfer_total", p.xfer_total},
      {"xfer_split_normal", p.xfer_split_normal},
      {"xfer_split_negrisk", p.xfer_split_negrisk},
      {"xfer_split_non_poly", p.xfer_split_non_poly},
      {"xfer_merge_normal", p.xfer_merge_normal},
      {"xfer_merge_negrisk", p.xfer_merge_negrisk},
      {"xfer_merge_non_poly", p.xfer_merge_non_poly},
      {"xfer_redemption", p.xfer_redemption},
      {"xfer_redemption_non_poly", p.xfer_redemption_non_poly},
      {"xfer_convert", p.xfer_convert},
      {"xfer_order_buy", p.xfer_order_buy},
      {"xfer_order_sell", p.xfer_order_sell},
      {"xfer_fpmm_buy", p.xfer_fpmm_buy},
      {"xfer_fpmm_sell", p.xfer_fpmm_sell},
      {"xfer_lp_add", p.xfer_lp_add},
      {"xfer_lp_remove", p.xfer_lp_remove},
      {"xfer_lp_return", p.xfer_lp_return},
      {"xfer_transfer_in_negrisk", p.xfer_transfer_in_negrisk},
      {"xfer_transfer_in_other", p.xfer_transfer_in_other},
      {"xfer_transfer_in_non_poly", p.xfer_transfer_in_non_poly},
      {"xfer_transfer_out_negrisk", p.xfer_transfer_out_negrisk},
      {"xfer_transfer_out_other", p.xfer_transfer_out_other},
      {"xfer_transfer_out_non_poly", p.xfer_transfer_out_non_poly},
      {"xfer_internal_mint_negrisk", p.xfer_internal_mint_negrisk},
      {"xfer_internal_mint_fpmm", p.xfer_internal_mint_fpmm},
      {"xfer_internal_burn_negrisk", p.xfer_internal_burn_negrisk},
      {"xfer_internal_burn_fpmm", p.xfer_internal_burn_fpmm},
      {"xfer_internal_burn_convert", p.xfer_internal_burn_convert},
      {"xfer_internal_transfer_zero", p.xfer_internal_transfer_zero},
      {"xfer_internal_transfer_order", p.xfer_internal_transfer_order},
      {"xfer_internal_transfer_negrisk", p.xfer_internal_transfer_negrisk},
      {"xfer_internal_transfer_fpmm", p.xfer_internal_transfer_fpmm},
      {"xfer_internal_transfer_other", p.xfer_internal_transfer_other},
  };

  const auto &sst = p.split_sem_tree;
  const auto &mst = p.merge_sem_tree;
  const auto &cst = p.convert_sem_tree;
  const auto &ost = p.order_sem_tree;
  json split_sem_tree = {
      {"total", sst.total},
      {"amount_zero", sst.amount_zero},
      {"amount_positive", sst.amount_positive},
      {"parent_root", sst.parent_root},
      {"parent_nested", sst.parent_nested},
      {"partition_single", sst.partition_single},
      {"partition_multi", sst.partition_multi},
      {"observed_leg", sst.observed_leg},
      {"consumed", sst.consumed},
      {"covered_by_parent", sst.covered_by_parent},
      {"unobserved_leg", sst.unobserved_leg},
  };
  json merge_sem_tree = {
      {"total", mst.total},
      {"amount_zero", mst.amount_zero},
      {"amount_positive", mst.amount_positive},
      {"parent_root", mst.parent_root},
      {"parent_nested", mst.parent_nested},
      {"partition_single", mst.partition_single},
      {"partition_multi", mst.partition_multi},
      {"observed_leg", mst.observed_leg},
      {"consumed", mst.consumed},
      {"covered_by_parent", mst.covered_by_parent},
      {"unobserved_leg", mst.unobserved_leg},
  };
  json convert_sem_tree = {
      {"total", cst.total},
      {"amount_zero", cst.amount_zero},
      {"amount_positive", cst.amount_positive},
      {"consumed", cst.consumed},
  };
  json question_counts = json::object();
  for (const auto &[qcnt, cnt] : cst.by_question_count) {
    std::string key = (qcnt < 0) ? "unknown" : std::to_string(qcnt);
    question_counts[key] = cnt;
  }
  convert_sem_tree["question_counts"] = question_counts;
  json order_sem_tree = {
      {"total", ost.total},
      {"maker_buy", ost.maker_buy},
      {"maker_sell", ost.maker_sell},
      {"token_zero", ost.token_zero},
      {"token_positive", ost.token_positive},
      {"quote_zero", ost.quote_zero},
      {"quote_positive", ost.quote_positive},
      {"observed_leg", ost.observed_leg},
      {"consumed", ost.consumed},
      {"unobserved_leg", ost.unobserved_leg},
  };
  json result = {
      {"phase", p.phase},
      {"running", p.running},
      {"total_users", p.total_users},
      {"total_events", p.total_events},
      {"cond_tree", cond_tree},
      {"token_tree", token_tree},
      {"split_sem_tree", split_sem_tree},
      {"merge_sem_tree", merge_sem_tree},
      {"convert_sem_tree", convert_sem_tree},
      {"order_sem_tree", order_sem_tree},
  };
  result.update(transfer_tree);

  json event_by_collateral = json::object();
  for (const auto &[key, cnt] : p.event_by_collateral) {
    uint8_t event_type = key / 256;
    uint8_t coll_id = key % 256;
    std::string et_key = std::to_string(event_type);
    if (!event_by_collateral.contains(et_key)) {
      event_by_collateral[et_key] = json::array();
    }
    auto [addr, name] = resolve_collateral(coll_id);
    event_by_collateral[et_key].push_back({
        {"collateral_id", coll_id},
        {"addr", addr},
        {"name", name},
        {"count", cnt},
    });
  }
  result["event_by_collateral"] = event_by_collateral;
  result["by_collateral_split"] = by_collateral_from_events(
      {static_cast<uint8_t>(stage2::EventType::SplitNonPoly)});
  result["by_collateral_merge"] = by_collateral_from_events(
      {static_cast<uint8_t>(stage2::EventType::MergeNonPoly)});
  result["by_collateral_redemption"] = by_collateral_from_events(
      {static_cast<uint8_t>(stage2::EventType::RedemptionNonPoly)});
  result["by_collateral_fpmm_trade"] = by_collateral_from_events(
      {static_cast<uint8_t>(stage2::EventType::FPMMBuy),
       static_cast<uint8_t>(stage2::EventType::FPMMSell)});
  result["by_collateral_fpmm_lp"] = by_collateral_from_events(
      {static_cast<uint8_t>(stage2::EventType::FPMMLPAdd),
       static_cast<uint8_t>(stage2::EventType::FPMMLPRemove),
       static_cast<uint8_t>(stage2::EventType::FPMMLPReturn)});

  res_.result(http::status::ok);
  res_.body() = result.dump();
}

void ApiSession::handle_stage3_filter() {
  TraceN("api/s3_filter");
  res_.set(http::field::content_type, "application/json");
  if (req_.method() != http::verb::post) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"POST required"})";
    return;
  }

  const json body = req_.body().empty() ? json::object() : json::parse(req_.body(), nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"Invalid JSON body"})";
    return;
  }

  stage3::filter::Request filter_req;
  if (!body.contains("anchor_bucket") || !body["anchor_bucket"].is_number_integer()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"anchor_bucket is required and must be integer"})";
    return;
  }
  filter_req.anchor_bucket = body["anchor_bucket"].get<int64_t>();

  if (!body.contains("limit") || !body["limit"].is_number_integer()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"limit is required and must be integer"})";
    return;
  }
  filter_req.limit = body["limit"].get<int32_t>();

  if (!body.contains("sort_asc") || !body["sort_asc"].is_boolean()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"sort_asc is required and must be boolean"})";
    return;
  }
  filter_req.sort_asc = body["sort_asc"].get<bool>();
  if (!body.contains("sort_expr") || !body["sort_expr"].is_string()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"sort_expr is required"})";
    return;
  }
  filter_req.sort_expr = body["sort_expr"].get<std::string>();

  if (body.contains("filters") && !body["filters"].is_null()) {
    if (!body["filters"].is_array()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"filters must be string array"})";
      return;
    }
    for (const auto &it : body["filters"]) {
      if (!it.is_string()) {
        res_.result(http::status::bad_request);
        res_.body() = R"({"error":"filters must be string array"})";
        return;
      }
      filter_req.filters.push_back(it.get<std::string>());
    }
  }

  try {
    stage3::filter::Result filter_result = stage3_.filter_users_by_features(filter_req);
    json users = json::array();
    for (const auto &u : filter_result.users) {
      users.push_back({
          {"addr", u.addr},
          {"sort_value", u.sort_value},
      });
    }
    write_ok_json_response(res_, {
                                     {"anchor_bucket", filter_result.anchor_bucket},
                                     {"users", users},
                                 });
  } catch (const std::invalid_argument &e) {
    res_.result(http::status::bad_request);
    res_.body() = json{{"error", e.what()}}.dump();
  }
}

void ApiSession::handle_stage3_pnl() {
  TraceN("api/s3_timeline");
  res_.set(http::field::content_type, "application/json");

  std::string user = get_param("user");
  if (!ensure_required_param(user, "user", res_)) {
    return;
  }

  auto timeline = stage3_.get_user_timeline(user);
  if (timeline.empty()) {
    res_.result(http::status::not_found);
    res_.body() = R"({"error":"User not found or no events"})";
    return;
  }

  const int64_t latest_sort_key = timeline.back().sort_key;
  const int64_t latest_block = latest_sort_key / stage2::SORT_KEY_SCALE;
  std::vector<uint32_t> cond_idxs;
  cond_idxs.reserve(timeline.size());
  for (const auto &e : timeline) {
    if (e.cond_idx < 0) {
      continue;
    }
    cond_idxs.push_back(static_cast<uint32_t>(e.cond_idx));
  }
  auto cond_meta = load_stage3_cond_meta(cond_idxs);
  stage3_store_cond_meta_cache(user, cond_meta);

  json timeline_arr = json::array();
  for (const auto &e : timeline) {
    Stage3CondMeta meta;
    if (e.cond_idx >= 0) {
      auto it = cond_meta.find(static_cast<uint32_t>(e.cond_idx));
      if (it != cond_meta.end()) {
        meta = it->second;
      }
    }
    timeline_arr.push_back({
        {"sk", e.sort_key},
        {"ci", e.cond_idx},
        {"ti", e.token_idx},
        {"ty", e.event_type},
        {"tag", meta.tag},
        {"amt", e.amount},
        {"px", e.price},
        {"rpnl", e.realized_pnl},
        {"upnl", e.unrealized_pnl},
        {"tk", e.token_count},
    });
  }

  json result = {
      {"user", user},
      {"block", latest_block},
      {"total_events", static_cast<int64_t>(timeline.size())},
      {"timeline", timeline_arr},
  };

  res_.result(http::status::ok);
  res_.body() = result.dump();
}

std::string ApiSession::normalize_stage3_user(const std::string &addr) {
  std::string lower;
  lower.reserve(addr.size());
  for (char c : addr) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower.size() != 42 || !lower.starts_with("0x")) {
    return {};
  }
  return lower;
}

std::unordered_map<uint32_t, ApiSession::Stage3CondMeta>
ApiSession::stage3_cond_meta_from_cache(const std::string &user) {
  std::string lower = normalize_stage3_user(user);
  if (lower.empty()) {
    return {};
  }
  std::lock_guard<std::mutex> lock(s3_meta_cache_mu_);
  if (s3_meta_cache_user_lower_ != lower) {
    return {};
  }
  return s3_meta_cache_cond_meta_;
}

void ApiSession::stage3_store_cond_meta_cache(const std::string &user,
                                              std::unordered_map<uint32_t, Stage3CondMeta> cond_meta) {
  std::string lower = normalize_stage3_user(user);
  if (lower.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(s3_meta_cache_mu_);
  s3_meta_cache_user_lower_ = std::move(lower);
  s3_meta_cache_cond_meta_ = std::move(cond_meta);
}

std::unordered_map<uint32_t, ApiSession::Stage3CondMeta>
ApiSession::load_stage3_cond_meta(const std::vector<uint32_t> &cond_idxs) {
  std::unordered_map<uint32_t, Stage3CondMeta> out;
  if (cond_idxs.empty()) {
    return out;
  }

  std::vector<uint32_t> conds;
  conds.reserve(cond_idxs.size());
  conds.assign(cond_idxs.begin(), cond_idxs.end());
  std::sort(conds.begin(), conds.end());
  conds.erase(std::unique(conds.begin(), conds.end()), conds.end());

  std::string cond_idx_list;
  cond_idx_list.reserve(conds.size() * 12);
  for (size_t i = 0; i < conds.size(); ++i) {
    if (i > 0) {
      cond_idx_list += ",";
    }
    cond_idx_list += std::to_string(conds[i]);
  }
  assert(!cond_idx_list.empty());

  std::unordered_map<uint32_t, std::string> cond_idx_to_hex;
  {
    auto conn = stage2_db_.create_connection();
    auto q = conn->Query(
        "SELECT cond_idx, lower(hex(cond_id)) AS cid "
        "FROM rb_condition "
        "WHERE cond_idx IN (" +
        cond_idx_list + ")");
    assert(q && !q->HasError());
    cond_idx_to_hex.reserve(static_cast<size_t>(q->RowCount()));
    for (idx_t i = 0; i < q->RowCount(); ++i) {
      uint32_t cond_idx = q->GetValue(0, i).GetValue<uint32_t>();
      cond_idx_to_hex[cond_idx] = q->GetValue(1, i).GetValueUnsafe<std::string>();
    }
  }

  std::unordered_map<std::string, Stage3CondMeta> cond_hex_to_meta;
  if (!cond_idx_to_hex.empty()) {
    std::string cond_hex_list;
    cond_hex_list.reserve(cond_idx_to_hex.size() * 72);
    bool first = true;
    for (const auto &[_, cid_hex] : cond_idx_to_hex) {
      if (!first) {
        cond_hex_list += ",";
      }
      first = false;
      cond_hex_list += "from_hex('" + cid_hex + "')";
    }
    assert(!cond_hex_list.empty());

    auto conn = stage0_db_.create_connection();
    auto q = conn->Query(
        "SELECT "
        "  lower(hex(ps.condition_id)) AS cid, "
        "  coalesce(pc.tag_name, 'unknown') AS tag_name, "
        "  coalesce("
        "    json_extract_string(ps.market_json, '$.question'), "
        "    json_extract_string(ps.market_json, '$.events[0].title'), "
        "    'unknown'"
        "  ) AS question, "
        "  coalesce("
        "    json_extract_string(ps.market_json, '$.description'), "
        "    json_extract_string(ps.market_json, '$.events[0].description'), "
        "    ''"
        "  ) AS description "
        "FROM pm_condition_static ps "
        "LEFT JOIN pm_condition_scan_class pc ON pc.condition_id = ps.condition_id "
        "WHERE ps.condition_id IN (" +
        cond_hex_list + ")");
    assert(q && !q->HasError());
    cond_hex_to_meta.reserve(static_cast<size_t>(q->RowCount()));
    for (idx_t i = 0; i < q->RowCount(); ++i) {
      std::string cid_hex = q->GetValue(0, i).GetValueUnsafe<std::string>();
      Stage3CondMeta meta;
      meta.tag = q->GetValue(1, i).GetValueUnsafe<std::string>();
      meta.question = q->GetValue(2, i).GetValueUnsafe<std::string>();
      meta.description = q->GetValue(3, i).GetValueUnsafe<std::string>();
      if (meta.tag.empty()) {
        meta.tag = "unknown";
      }
      if (meta.question.empty()) {
        meta.question = "unknown";
      }
      cond_hex_to_meta.emplace(std::move(cid_hex), std::move(meta));
    }
  }

  for (uint32_t cond_idx : conds) {
    auto cid_it = cond_idx_to_hex.find(cond_idx);
    if (cid_it == cond_idx_to_hex.end()) {
      out.emplace(cond_idx, Stage3CondMeta{});
      continue;
    }
    auto meta_it = cond_hex_to_meta.find(cid_it->second);
    if (meta_it == cond_hex_to_meta.end()) {
      out.emplace(cond_idx, Stage3CondMeta{});
      continue;
    }
    out.emplace(cond_idx, meta_it->second);
  }

  return out;
}

void ApiSession::handle_stage3_positions() {
  TraceN("api/s3_positions");
  res_.set(http::field::content_type, "application/json");

  std::string user = get_param("user");
  if (!ensure_required_param(user, "user", res_)) {
    return;
  }
  std::string sort_key_str = get_param("sort_key");
  if (!ensure_required_param(sort_key_str, "sort_key", res_)) {
    return;
  }
  int64_t target_sort_key = std::stoll(sort_key_str);
  assert(target_sort_key >= 0);

  auto positions = stage3_.get_positions_at(user, target_sort_key);
  if (positions.empty()) {
    json result = {
        {"user", user},
        {"sort_key", target_sort_key},
        {"block", target_sort_key / stage2::SORT_KEY_SCALE},
        {"positions", json::array()},
    };
    res_.result(http::status::ok);
    res_.body() = result.dump();
    return;
  }

  auto cond_meta = stage3_cond_meta_from_cache(user);
  if (cond_meta.empty()) {
    std::vector<uint32_t> cond_idxs;
    cond_idxs.reserve(positions.size());
    for (const auto &p : positions) {
      cond_idxs.push_back(p.cond_idx);
    }
    cond_meta = load_stage3_cond_meta(cond_idxs);
    stage3_store_cond_meta_cache(user, cond_meta);
  }
  json pos_arr = json::array();
  for (const auto &p : positions) {
    const auto it = cond_meta.find(p.cond_idx);
    Stage3CondMeta meta = (it == cond_meta.end()) ? Stage3CondMeta{} : it->second;
    pos_arr.push_back({
        {"ci", p.cond_idx},
        {"ti", p.token_idx},
        {"oc", p.outcome_count},
        {"qty", p.qty},
        {"cost", p.cost},
        {"lp", p.last_price},
        {"eb", p.entry_block},
        {"tag", meta.tag},
        {"q", meta.question},
        {"desc", meta.description},
    });
  }

  json result = {
      {"user", user},
      {"sort_key", target_sort_key},
      {"block", target_sort_key / stage2::SORT_KEY_SCALE},
      {"positions", pos_arr},
  };
  res_.result(http::status::ok);
  res_.body() = result.dump();
}

std::string ApiSession::get_param(const char *name) {
  std::string target(req_.target());
  std::string key = std::string(name) + "=";
  auto pos = target.find(key);
  if (pos == std::string::npos) {
    return "";
  }
  std::string value = url_decode(target.substr(pos + key.size()));
  auto amp = value.find('&');
  return (amp != std::string::npos) ? value.substr(0, amp) : value;
}

std::string ApiSession::url_decode(const std::string &str) {
  std::string result;
  for (size_t i = 0; i < str.size(); ++i) {
    if (str[i] == '%' && i + 2 < str.size()) {
      int hex = std::stoi(str.substr(i + 1, 2), nullptr, 16);
      result += static_cast<char>(hex);
      i += 2;
    } else if (str[i] == '+') {
      result += ' ';
    } else {
      result += str[i];
    }
  }
  return result;
}

void ApiSession::handle_memory() {
  TraceN("api/memory");
  res_.set(http::field::content_type, "application/json");
  static std::atomic<int64_t> stage0_peak_bytes{0};
  static std::atomic<int64_t> stage1_peak_bytes{0};
  static std::atomic<int64_t> stage2_peak_bytes{0};
  static std::atomic<int64_t> stage3_peak_bytes{0};
  json result = json::object();
  const int64_t rss_bytes = core::mem::get_process_rss_bytes();
  result["process_rss_bytes"] = rss_bytes;
  json breakdown = {
      {"stage0", stage0_mem_getter_()},
      {"stage1", stage1_mem_getter_()},
      {"stage2", stage2_mem_getter_()},
      {"stage3", stage3_mem_getter_()},
  };

  const int64_t stage0_bytes = breakdown["stage0"].value("estimated_total_bytes", int64_t{0});
  const int64_t stage1_bytes = breakdown["stage1"].value("estimated_total_bytes", int64_t{0});
  const int64_t stage2_bytes = breakdown["stage2"].value("estimated_total_bytes", int64_t{0});
  const int64_t stage3_bytes = breakdown["stage3"].value("estimated_total_bytes", int64_t{0});
  const int64_t stage0_peak = update_peak_bytes(stage0_peak_bytes, stage0_bytes);
  const int64_t stage1_peak = update_peak_bytes(stage1_peak_bytes, stage1_bytes);
  const int64_t stage2_peak = update_peak_bytes(stage2_peak_bytes, stage2_bytes);
  const int64_t stage3_peak = update_peak_bytes(stage3_peak_bytes, stage3_bytes);

  breakdown["stage0"]["estimated_peak_bytes"] = stage0_peak;
  breakdown["stage1"]["estimated_peak_bytes"] = stage1_peak;
  breakdown["stage2"]["estimated_peak_bytes"] = stage2_peak;
  breakdown["stage3"]["estimated_peak_bytes"] = stage3_peak;

  result["estimated_sum_bytes"] = stage0_bytes + stage1_bytes + stage2_bytes + stage3_bytes;
  result["estimated_peak_sum_bytes"] = stage0_peak + stage1_peak + stage2_peak + stage3_peak;
  result["rss_gap_bytes"] = rss_bytes - result["estimated_sum_bytes"].get<int64_t>();
  result["rss_gap_vs_peak_sum_bytes"] = rss_bytes - result["estimated_peak_sum_bytes"].get<int64_t>();

  result["object_breakdown"] = std::move(breakdown);
  res_.result(http::status::ok);
  res_.body() = result.dump(2);
}

void ApiSession::do_write() {
  TraceN("api/write");
  http::async_write(socket_, res_,
                    [self = shared_from_this()](beast::error_code ec, std::size_t) {
                      if (!ec && self->res_.keep_alive()) {
                        self->do_read();
                      } else {
                        beast::error_code shutdown_ec;
                        [[maybe_unused]] auto ret = self->socket_.shutdown(tcp::socket::shutdown_send, shutdown_ec);
                      }
                    });
}
