#include "api_session.hpp"

#include <arrow/io/file.h>
#include <arrow/ipc/reader.h>

#include <atomic>
#include <cctype>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <unordered_map>

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
  return {
      {"syncing", status.syncing},
      {"last_block", status.last_block},
      {"head_block", status.head_block},
      {"behind_blocks", status.behind_blocks},
      {"condition_count", status.condition_count},
      {"blocks_per_second", status.blocks_per_second},
      {"eta_seconds", status.eta_seconds},
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
  };
}

void write_ok_json_response(http::response<http::string_body> &res, const json &payload) {
  res.result(http::status::ok);
  res.body() = payload.dump();
}

} // namespace

ApiSession::ApiSession(tcp::socket socket, Database &stage1_db, Database &stage2_db, stage3::StageSync &stage3,
                       Stage0Getter stage0_getter, Stage1Getter stage1_getter,
                       Stage2Getter stage2_getter, Stage3Getter stage3_getter)
    : socket_(std::move(socket)), stage1_db_(stage1_db), stage2_db_(stage2_db), stage3_(stage3),
      stage0_getter_(std::move(stage0_getter)), stage1_getter_(std::move(stage1_getter)),
      stage2_getter_(std::move(stage2_getter)),
      stage3_getter_(std::move(stage3_getter)) {}

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
    } else if (target.starts_with("/api/stage1-status")) {
      handle_stage1_status();
    } else if (target.starts_with("/api/stage2-status")) {
      handle_stage2_status();
    } else if (target.starts_with("/api/stage2-data")) {
      handle_stage2_data();
    } else if (target.starts_with("/api/stage3-status")) {
      handle_stage3_status();
    } else if (target.starts_with("/api/stage3-users")) {
      handle_stage3_users();
    } else if (target.starts_with("/api/stage3-data")) {
      handle_stage3_data();
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

void ApiSession::handle_stage3_users() {
  TraceN("api/s3_users");
  res_.set(http::field::content_type, "application/json");

  std::string limit_str = get_param("limit");
  int64_t limit = limit_str.empty() ? 200 : std::stoll(limit_str);

  auto users = stage3_.get_users_sorted(limit);
  json result = json::array();
  for (const auto &u : users) {
    result.push_back({
        {"user_addr", u.addr},
        {"event_count", u.event_count},
        {"realized_pnl", u.realized_pnl},
    });
  }

  res_.result(http::status::ok);
  res_.body() = result.dump();
}

void ApiSession::handle_stage3_data() {
  TraceN("api/s3_data");
  res_.set(http::field::content_type, "application/json");

  std::string user = get_param("user");
  if (user.empty()) {
    res_.result(http::status::bad_request);
    res_.body() = R"({"error":"Missing user parameter"})";
    return;
  }

  auto timeline = stage3_.get_user_timeline(user);
  if (timeline.empty()) {
    res_.result(http::status::not_found);
    res_.body() = R"({"error":"User not found or no events"})";
    return;
  }

  int64_t first_ts = timeline.front().sort_key / stage2::SORT_KEY_SCALE;
  int64_t last_ts = timeline.back().sort_key / stage2::SORT_KEY_SCALE;
  std::string sk_str = get_param("sk");
  int64_t detail_sk = sk_str.empty() ? timeline.back().sort_key : std::stoll(sk_str);

  json timeline_arr = json::array();
  for (const auto &e : timeline) {
    timeline_arr.push_back({
        {"sk", e.sort_key},
        {"ty", e.event_type},
        {"rpnl", e.realized_pnl},
        {"d", e.delta},
        {"p", e.price},
        {"ci", e.cond_idx},
        {"ti", e.token_idx},
        {"tk", e.token_count},
    });
  }

  auto positions = stage3_.get_positions_at(user, detail_sk);
  json pos_arr = json::array();
  for (const auto &p : positions) {
    json pos_obj = {
        {"id", p.condition_id},
        {"pos", json::array()},
        {"cost", p.cost_basis},
        {"rpnl", p.realized_pnl},
    };
    for (int i = 0; i < p.outcome_count; ++i) {
      pos_obj["pos"].push_back(p.positions[i]);
    }
    pos_arr.push_back(pos_obj);
  }

  size_t center = 0;
  if (!timeline.empty()) {
    auto it = std::lower_bound(
        timeline.begin(), timeline.end(), detail_sk,
        [](const stage3::StageSync::TimelineEntry &e, int64_t sk) { return e.sort_key < sk; });
    center = (it == timeline.end())
                 ? timeline.size() - 1
                 : static_cast<size_t>(it - timeline.begin());
  }
  size_t radius = 20;
  size_t start = (center > radius) ? center - radius : 0;
  size_t end = std::min(center + radius + 1, timeline.size());

  json events_arr = json::array();
  for (size_t i = start; i < end; ++i) {
    const auto &t = timeline[i];
    events_arr.push_back({
        {"sk", t.sort_key},
        {"ty", t.event_type},
        {"d", t.delta},
        {"p", t.price},
        {"ci", t.cond_idx},
        {"ti", t.token_idx},
    });
  }

  json result = {
      {"total_events", timeline.size()},
      {"first_ts", first_ts},
      {"last_ts", last_ts},
      {"timeline", timeline_arr},
      {"positions", pos_arr},
      {"events", events_arr},
      {"center", static_cast<int64_t>(center - start)},
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
