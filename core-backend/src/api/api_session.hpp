#pragma once

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <arrow/io/file.h>
#include <arrow/ipc/reader.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <nlohmann/json.hpp>

#include "../core/database.hpp"
#include "../stage3/pnl_replay.hpp"
#include "misc/profiler.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;
using json = nlohmann::json;

struct Stage1SyncStatus {
  bool is_syncing = false;
  int64_t head_block = 0;
  double blocks_per_second = 0.0;
  double bytes_per_block = 0.0;
};

struct Stage2SyncStatus {
  bool syncing = false;
  int64_t stage1_last_block = 0;
  int64_t stage2_cursor = 0;
  int64_t behind_chunks = 0;
};

class ApiSession : public std::enable_shared_from_this<ApiSession> {
public:
  using Stage1SyncGetter = std::function<Stage1SyncStatus()>;
  using Stage2SyncGetter = std::function<Stage2SyncStatus()>;

  ApiSession(tcp::socket socket, Database &db, stage3::PnlEngine &pnl_engine,
             Stage1SyncGetter stage1_getter = nullptr, Stage2SyncGetter stage2_getter = nullptr)
      : socket_(std::move(socket)), db_(db), pnl_engine_(pnl_engine),
        sync_getter_(std::move(stage1_getter)), stage2_getter_(std::move(stage2_getter)) {}

  void run() { do_read(); }

private:
  void do_read() {
    TraceN("api/read");
    req_ = {};
    http::async_read(socket_, buffer_, req_,
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                       if (ec)
                         return;
                       self->handle_request();
                     });
  }

  void handle_request() {
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
      } else if (target.starts_with("/api/sync-state")) {
        handle_sync_state();
      } else if (target.starts_with("/api/query")) {
        handle_query();
      } else if (target.starts_with("/api/rebuild-status")) {
        handle_rebuild_status();
      } else if (target.starts_with("/api/user/") && target.find("/pnl") != std::string::npos) {
        handle_user_pnl(target);
      } else if (target.starts_with("/api/user/") && target.find("/positions") != std::string::npos) {
        handle_user_positions(target);
      } else if (target.starts_with("/api/replay-users")) {
        handle_replay_users();
      } else if (target.starts_with("/api/replay-positions")) {
        handle_replay_positions();
      } else if (target.starts_with("/api/replay-trades")) {
        handle_replay_trades();
      } else if (target.starts_with("/api/replay")) {
        handle_replay();
      } else if (target.starts_with("/api/export-csv")) {
        handle_export_csv();
      } else if (target.starts_with("/api/table-sample")) {
        handle_table_sample();
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

  void handle_health() {
    res_.result(http::status::ok);
    res_.set(http::field::content_type, "application/json");
    res_.body() = R"({"status":"ok"})";
  }

  static int64_t feather_row_count(const std::string &path) {
    auto file = arrow::io::ReadableFile::Open(path);
    assert(file.ok());
    auto reader = arrow::ipc::RecordBatchFileReader::Open(*file);
    assert(reader.ok());
    auto count = (*reader)->CountRows();
    assert(count.ok());
    return *count;
  }

  void handle_tables() {
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

    std::string cache_path = db_.data_dir() + "/counts_cache.json";
    std::call_once(cache_loaded_flag, [cache_path]() {
      if (!std::filesystem::exists(cache_path))
        return;
      std::ifstream f(cache_path);
      json cached = json::parse(f);
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
      std::string dir = db_.feather_dir(name);
      if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir))
        continue;

      futures.push_back(std::async(std::launch::async, [table_name, dir]() -> std::pair<std::string, int64_t> {
        auto &cache = table_cache[table_name];
        for (const auto &entry : std::filesystem::directory_iterator(dir)) {
          std::string filename = entry.path().filename().string();
          if (!filename.ends_with(".feather"))
            continue;
          {
            std::lock_guard<std::mutex> lock(cache.mtx);
            if (cache.files.count(filename))
              continue;
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
        for (auto &[fname, cnt] : tc.files)
          cache_json[tname][fname] = cnt;
      }
      std::ofstream ofs(cache_path);
      ofs << cache_json.dump(2);
    }

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_sync_state() {
    TraceN("api/sync_state");
    res_.set(http::field::content_type, "application/json");

    int64_t last_block = db_.get_last_block();
    json result = {{"last_block", last_block}};

    if (sync_getter_) {
      Stage1SyncStatus status = sync_getter_();
      result["head_block"] = status.head_block;
      result["is_syncing"] = status.is_syncing;
      result["blocks_per_second"] = status.blocks_per_second;
      result["bytes_per_block"] = status.bytes_per_block;
    }

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_query() {
    TraceN("api/query");
    res_.set(http::field::content_type, "application/json");

    std::string query = get_param("q");
    if (query.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing query parameter 'q'"})";
      return;
    }

    std::string upper = query;
    for (auto &c : upper)
      c = std::toupper(c);

    if (!upper.starts_with("SELECT")) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Only SELECT queries allowed"})";
      return;
    }

    json result = db_.query_json(query);
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_export_csv() {
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

    std::string dir = db_.feather_dir(table);
    if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir)) {
      res_.result(http::status::ok);
      res_.body() = R"({"rows":0})";
      return;
    }

    std::string latest;
    int64_t max_block = -1;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      std::string filename = entry.path().filename().string();
      if (!filename.ends_with(".feather"))
        continue;
      int64_t block = std::stoll(filename.substr(0, filename.size() - 8));
      if (block > max_block) {
        max_block = block;
        latest = entry.path().string();
      }
    }

    if (latest.empty()) {
      res_.result(http::status::ok);
      res_.body() = R"({"rows":0})";
      return;
    }

    auto conn = db_.create_connection();
    std::string sql = "COPY (SELECT * FROM read_arrow('" + latest + "') LIMIT " +
                      std::to_string(limit) + ") TO '" + output + "' (HEADER)";
    auto result = conn->Query(sql);
    assert(!result->HasError());

    res_.result(http::status::ok);
    res_.body() = R"({"rows":1})";
  }

  void handle_table_sample() {
    TraceN("api/table_sample");
    res_.set(http::field::content_type, "application/json");

    std::string table = get_param("table");
    if (table.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing table parameter"})";
      return;
    }

    std::string dir = db_.feather_dir(table);
    if (!std::filesystem::exists(dir) || std::filesystem::is_empty(dir)) {
      res_.result(http::status::ok);
      res_.body() = "[]";
      return;
    }

    std::string latest;
    int64_t max_block = -1;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
      std::string filename = entry.path().filename().string();
      if (!filename.ends_with(".feather"))
        continue;
      int64_t block = std::stoll(filename.substr(0, filename.size() - 8));
      if (block > max_block) {
        max_block = block;
        latest = entry.path().string();
      }
    }

    if (latest.empty()) {
      res_.result(http::status::ok);
      res_.body() = "[]";
      return;
    }

    json result = db_.query_json("SELECT * FROM read_arrow('" + latest + "') LIMIT 1");
    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_rebuild_status() {
    TraceN("api/rebuild");
    res_.set(http::field::content_type, "application/json");

    const auto &p = pnl_engine_.progress();
    const auto &ct = p.cond_tree;
    const auto &tt = p.token_tree;
    std::unordered_map<uint8_t, std::string> coll_id_to_addr;
    {
      auto conn = db_.create_connection();
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
    json cond_tree = {
        {"total", ct.total},
        {"polymarket",
         {{"total", ct.polymarket.total},
          {"token_reg",
           {{"total", ct.polymarket.token_reg.total},
            {"amm", ct.polymarket.token_reg.amm},
            {"negrisk", ct.polymarket.token_reg.negrisk},
            {"normal", ct.polymarket.token_reg.normal}}},
          {"fpmm_only", ct.polymarket.fpmm_only}}},
        {"other",
         {{"total", ct.other.total},
          {"prep", ct.other.prep},
          {"other_fpmm", ct.other.other_fpmm},
          {"split", ct.other.split}}},
    };

    json token_tree = {
        {"total", tt.total},
        {"polymarket",
         {{"total", tt.polymarket.total},
          {"token_reg",
           {{"total", tt.polymarket.token_reg.total},
            {"amm", tt.polymarket.token_reg.amm},
            {"negrisk", tt.polymarket.token_reg.negrisk},
            {"normal", tt.polymarket.token_reg.normal}}},
          {"fpmm_only",
           {{"total", tt.polymarket.fpmm_only.total},
            {"by_collateral", [&]() {
               json arr = json::array();
               for (const auto &[coll, cnt] : tt.polymarket.fpmm_only.by_collateral) {
                 auto [addr, name] = resolve_collateral(coll);
                 arr.push_back({
                     {"addr", addr},
                     {"name", name},
                     {"count", cnt},
                 });
               }
               return arr;
             }()}}}}},
        {"other",
         {{"total", tt.other.total},
          {"other_fpmm", tt.other.other_fpmm},
          {"split", tt.other.split},
          {"transfer_inferred", tt.other.transfer_inferred},
          {"transfer_inferred_by_token", [&]() {
             json arr = json::array();
             for (const auto &[token, cnt] : tt.other.transfer_inferred_by_token) {
               arr.push_back({{"token", token}, {"count", cnt}});
             }
             return arr;
           }()}}},
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

    json result = {
        {"phase", p.phase},
        {"running", p.running},
        {"total_users", p.total_users},
        {"total_markets", p.total_markets},
        {"total_events", p.total_events},
        {"cond_tree", cond_tree},
        {"token_tree", token_tree},
    };
    result.update(transfer_tree);

    // 按 event_type 分组，每组内以 collateral_addr 为 key 的统计 dict
    // 格式: {"event_type_int": [{"addr": "0x...", "name": "USDC", "count": 100}, ...]}
    json event_by_coll = json::object();
    for (const auto &[key, cnt] : p.event_by_collateral) {
      uint8_t event_type = key / 256;
      uint8_t coll = key % 256;
      std::string et_key = std::to_string(event_type);
      if (!event_by_coll.contains(et_key))
        event_by_coll[et_key] = json::array();
      auto [addr, name] = resolve_collateral(coll);
      event_by_coll[et_key].push_back({{"addr", addr}, {"name", name}, {"count", cnt}});
    }
    result["event_by_collateral"] = event_by_coll;

    if (stage2_getter_) {
      auto s2 = stage2_getter_();
      result["stage2_sync"] = {
          {"syncing", s2.syncing},
          {"stage1_last_block", s2.stage1_last_block},
          {"stage2_cursor", s2.stage2_cursor},
          {"behind_chunks", s2.behind_chunks},
      };
    }

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_user_pnl(const std::string &target) {
    TraceN("api/user_pnl");
    res_.set(http::field::content_type, "application/json");

    std::string addr = extract_user_addr(target);
    if (addr.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Invalid address"})";
      return;
    }

    const auto *state = pnl_engine_.get_user_state(addr);
    if (!state) {
      res_.result(http::status::not_found);
      res_.body() = R"({"error":"User not found"})";
      return;
    }

    int64_t total_realized_pnl = 0;
    int64_t total_cost_basis = 0;
    json conditions = json::array();

    for (const auto &ch : state->conditions) {
      if (ch.snapshots.empty())
        continue;
      const auto &last = ch.snapshots.back();
      total_realized_pnl += last.realized_pnl;
      total_cost_basis += last.cost_basis;

      json cond_obj = {
          {"condition_id", pnl_engine_.get_condition_id(ch.cond_idx)},
          {"realized_pnl", last.realized_pnl},
          {"cost_basis", last.cost_basis},
          {"positions", json::array()},
      };
      for (int i = 0; i < last.outcome_count; ++i) {
        cond_obj["positions"].push_back(last.positions[i]);
      }
      conditions.push_back(cond_obj);
    }

    json result = {
        {"address", addr},
        {"total_realized_pnl", total_realized_pnl},
        {"total_cost_basis", total_cost_basis},
        {"conditions_count", state->conditions.size()},
        {"conditions", conditions},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_user_positions(const std::string &target) {
    TraceN("api/user_pos");
    res_.set(http::field::content_type, "application/json");

    std::string addr = extract_user_addr(target);
    if (addr.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Invalid address"})";
      return;
    }

    const auto *state = pnl_engine_.get_user_state(addr);
    if (!state) {
      res_.result(http::status::not_found);
      res_.body() = R"({"error":"User not found"})";
      return;
    }

    json positions = json::array();
    for (const auto &ch : state->conditions) {
      if (ch.snapshots.empty())
        continue;
      const auto &last = ch.snapshots.back();
      bool has_position = false;
      for (int i = 0; i < last.outcome_count; ++i) {
        if (last.positions[i] != 0) {
          has_position = true;
          break;
        }
      }
      if (!has_position)
        continue;

      json pos_obj = {
          {"condition_id", pnl_engine_.get_condition_id(ch.cond_idx)},
          {"positions", json::array()},
          {"cost_basis", last.cost_basis},
      };
      for (int i = 0; i < last.outcome_count; ++i) {
        pos_obj["positions"].push_back(last.positions[i]);
      }
      positions.push_back(pos_obj);
    }

    json result = {
        {"address", addr},
        {"active_positions", positions.size()},
        {"positions", positions},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_replay_users() {
    TraceN("api/replay_users");
    res_.set(http::field::content_type, "application/json");

    std::string limit_str = get_param("limit");
    int64_t limit = limit_str.empty() ? 200 : std::stoll(limit_str);

    auto users = pnl_engine_.get_users_sorted(limit);
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

  void handle_replay() {
    TraceN("api/replay");
    res_.set(http::field::content_type, "application/json");

    std::string user = get_param("user");
    if (user.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing user parameter"})";
      return;
    }

    auto timeline = pnl_engine_.get_user_timeline(user);
    if (timeline.empty()) {
      res_.result(http::status::not_found);
      res_.body() = R"({"error":"User not found or no events"})";
      return;
    }

    int64_t first_ts = timeline.front().sort_key / 1000000000LL;
    int64_t last_ts = timeline.back().sort_key / 1000000000LL;

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

    json result = {
        {"total_events", timeline.size()},
        {"first_ts", first_ts},
        {"last_ts", last_ts},
        {"timeline", timeline_arr},
    };

    res_.result(http::status::ok);
    res_.body() = result.dump();
  }

  void handle_replay_positions() {
    TraceN("api/replay_pos");
    res_.set(http::field::content_type, "application/json");

    std::string user = get_param("user");
    std::string sk_str = get_param("sk");
    if (user.empty() || sk_str.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing user or sk parameter"})";
      return;
    }

    int64_t sort_key = std::stoll(sk_str);
    auto positions = pnl_engine_.get_positions_at(user, sort_key);

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

    res_.result(http::status::ok);
    res_.body() = json{{"positions", pos_arr}}.dump();
  }

  void handle_replay_trades() {
    TraceN("api/replay_trades");
    res_.set(http::field::content_type, "application/json");

    std::string user = get_param("user");
    std::string sk_str = get_param("sk");
    std::string radius_str = get_param("radius");
    if (user.empty() || sk_str.empty()) {
      res_.result(http::status::bad_request);
      res_.body() = R"({"error":"Missing user or sk parameter"})";
      return;
    }

    int64_t sort_key = std::stoll(sk_str);
    int radius = radius_str.empty() ? 20 : std::stoi(radius_str);

    auto trades = pnl_engine_.get_trades_near(user, sort_key, radius);
    size_t center = pnl_engine_.get_trades_center_index(user, sort_key, radius);

    json events_arr = json::array();
    for (const auto &t : trades) {
      events_arr.push_back({
          {"sk", t.sort_key},
          {"ty", t.event_type},
          {"d", t.delta},
          {"p", t.price},
          {"ci", t.cond_idx},
          {"ti", t.token_idx},
      });
    }

    res_.result(http::status::ok);
    res_.body() = json{{"events", events_arr}, {"center", center}}.dump();
  }

  static std::string extract_user_addr(const std::string &target) {
    size_t start = target.find("/api/user/");
    if (start == std::string::npos)
      return "";
    start += 10;
    size_t end = target.find('/', start);
    if (end == std::string::npos)
      end = target.find('?', start);
    if (end == std::string::npos)
      end = target.size();
    return target.substr(start, end - start);
  }

  std::string get_param(const char *name) {
    std::string target(req_.target());
    std::string key = std::string(name) + "=";
    auto pos = target.find(key);
    if (pos == std::string::npos)
      return "";
    std::string value = url_decode(target.substr(pos + key.size()));
    auto amp = value.find('&');
    return (amp != std::string::npos) ? value.substr(0, amp) : value;
  }

  static std::string url_decode(const std::string &str) {
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

  void do_write() {
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

  tcp::socket socket_;
  Database &db_;
  stage3::PnlEngine &pnl_engine_;
  Stage1SyncGetter sync_getter_;
  Stage2SyncGetter stage2_getter_;
  beast::flat_buffer buffer_;
  http::request<http::string_body> req_;
  http::response<http::string_body> res_;
};
