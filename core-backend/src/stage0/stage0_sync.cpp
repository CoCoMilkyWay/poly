#include "stage0_sync.hpp"
#include "stage0_sync_http.hpp"

#include "../infra/rpc_transport.hpp"
#include "misc/profiler.hpp"

#include <algorithm>
#include <atomic>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace stage0 {
namespace {

constexpr std::string_view kEmptyRangeSql = "(SELECT 1 WHERE 1=0)";
constexpr const char *kGammaApiBase = "https://gamma-api.polymarket.com";
constexpr const char *kProxyProbeCondition = "0x0000000000000000000000000000000000000000000000000000000000000001";
constexpr const char *kClassPolyCtf = "poly_ctf";
constexpr const char *kClassPolyNegRisk = "poly_negrisk";
constexpr const char *kClassNonPoly = "non_poly";

duckdb::Value make_blob_value(const std::string &blob) {
  return duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()), blob.size());
}

int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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

bool is_poly_neg_risk(const json &market) {
  if (market.contains("negRisk") && market.at("negRisk").is_boolean()) {
    return market.at("negRisk").get<bool>();
  }
  return false;
}

std::string ensure_stage0_log_dir(const std::string &data_dir) {
  const std::string log_dir = data_dir + "/log";
  if (std::filesystem::exists(log_dir) && std::filesystem::is_regular_file(log_dir)) {
    std::filesystem::rename(log_dir, log_dir + ".legacy_file");
  }
  std::filesystem::create_directories(log_dir);
  assert(std::filesystem::exists(log_dir));
  assert(std::filesystem::is_directory(log_dir));
  return log_dir;
}

template <typename SeedVec>
std::string summarize_seed_ids(const SeedVec &seeds, size_t limit = 3) {
  if (seeds.empty()) {
    return "";
  }
  std::string out;
  const size_t n = std::min(limit, seeds.size());
  for (size_t i = 0; i < n; ++i) {
    if (!out.empty()) {
      out += ",";
    }
    out += seeds[i].condition_hex_lower;
  }
  if (seeds.size() > n) {
    out += ",...(" + std::to_string(seeds.size()) + ")";
  }
  return out;
}

void append_stage0_flow_log(const std::string &data_dir, const std::string &msg) {
  static std::mutex log_mutex;
  std::lock_guard<std::mutex> lock(log_mutex);
  const std::string log_dir = ensure_stage0_log_dir(data_dir);
  const int64_t now_ms = unix_ms_now();
  std::ofstream f(log_dir + "/log", std::ios::app);
  assert(f.is_open());
  f << now_ms << " " << msg << "\n";
  f.flush();
  assert(f.good());
}

void persist_stage0_parsed_market(const std::string &data_dir, const std::string &condition_hex_lower,
                                  const json &market) {
  assert(condition_hex_lower.starts_with("0x"));
  const std::string log_dir = ensure_stage0_log_dir(data_dir);
  std::ofstream f(log_dir + "/" + condition_hex_lower, std::ios::trunc);
  assert(f.is_open());
  f << market.dump(2) << "\n";
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
  sync_.last_block = get_scan_cursor();
  sync_.head_block = sync_.last_block;
  sync_.behind_blocks = 0;
  sync_.condition_count = static_cast<int64_t>(known_condition_ids_.size());
  sync_.ctf_condition_count = known_ctf_condition_count_;
  sync_.negrisk_condition_count = known_negrisk_condition_count_;
  sync_.nonpoly_condition_count = known_nonpoly_condition_count_;
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

void StageSync::refresh_status_locked(int64_t head_block, int64_t cursor, bool syncing) {
  auto reset_speed_and_eta = [&]() {
    sync_.blocks_per_second = 0.0;
    sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0 : -1.0;
  };
  sync_.syncing = syncing;
  sync_.head_block = head_block;
  sync_.last_block = cursor;
  sync_.behind_blocks = std::max<int64_t>(0, head_block - cursor);
  sync_.condition_count = static_cast<int64_t>(known_condition_ids_.size());
  sync_.ctf_condition_count = known_ctf_condition_count_;
  sync_.negrisk_condition_count = known_negrisk_condition_count_;
  sync_.nonpoly_condition_count = known_nonpoly_condition_count_;
  assert(sync_.condition_count ==
         sync_.ctf_condition_count + sync_.negrisk_condition_count + sync_.nonpoly_condition_count);
  if (commit_history_.size() < 2) {
    reset_speed_and_eta();
    return;
  }
  const auto &first = commit_history_.front();
  const auto &last = commit_history_.back();
  double elapsed_s = std::chrono::duration<double>(last.committed_at - first.committed_at).count();
  if (elapsed_s <= 0.0) {
    reset_speed_and_eta();
    return;
  }
  int64_t committed_blocks = std::max<int64_t>(0, last.cursor - first.cursor);
  if (committed_blocks == 0) {
    reset_speed_and_eta();
    return;
  }
  sync_.blocks_per_second = static_cast<double>(committed_blocks) / elapsed_s;
  sync_.eta_seconds = (sync_.behind_blocks == 0) ? 0.0
                                                 : static_cast<double>(sync_.behind_blocks) / sync_.blocks_per_second;
}

void StageSync::record_commit_locked(int64_t cursor) {
  commit_history_.push_back({std::chrono::steady_clock::now(), cursor});
  if (commit_history_.size() > kEtaWindowSize) {
    commit_history_.pop_front();
  }
}

void StageSync::do_sync() {
  TraceN("s0/sync");
  if (stop_requested_) {
    return;
  }
  ensure_cursor_floor();

  int64_t stage1_head = stage1_db_.get_last_block();
  int64_t cursor = runtime_scan_cursor_inited_ ? runtime_scan_cursor_ : get_scan_cursor();
  runtime_scan_cursor_inited_ = true;
  runtime_scan_cursor_ = cursor;
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

  RpcEndpoint gamma_ep = parse_rpc_endpoint(kGammaApiBase);
  assert(gamma_ep.use_ssl);
  const std::string stage0_proxy_url = config_.proxy_url;
  if (!stage0_proxy_url.empty()) {
    append_stage0_flow_log(stage0_db_.data_dir(), "stage0_proxy enabled url=" + stage0_proxy_url);
  } else {
    append_stage0_flow_log(stage0_db_.data_dir(), "stage0_proxy disabled (direct)");
  }

  asio::io_context fetch_ioc;
  asio::ssl::context fetch_ssl_ctx(asio::ssl::context::tls_client);
  fetch_ssl_ctx.set_verify_mode(asio::ssl::verify_none);

  if (!stage0_proxy_url.empty()) {
    append_stage0_flow_log(stage0_db_.data_dir(), "stage0_proxy_wait_start");
    bool proxy_probe_done = false;
    FetchSeedOutcome proxy_probe_out;
    async_seed_fetch(
        fetch_ioc, fetch_ssl_ctx, gamma_ep, stage0_proxy_url, kProxyProbeCondition,
        [&](FetchSeedOutcome out) {
          proxy_probe_out = std::move(out);
          proxy_probe_done = true;
        },
        [&](int attempt, const std::string &detail) {
          append_stage0_flow_log(stage0_db_.data_dir(),
                                 "stage0_proxy_wait_retry attempt=" + std::to_string(attempt) +
                                     " detail=" + detail);
        });
    while (!stop_requested_ && !proxy_probe_done) {
      fetch_ioc.restart();
      if (fetch_ioc.poll() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    if (stop_requested_) {
      std::lock_guard<std::mutex> lock(status_mutex_);
      sync_.syncing = false;
      return;
    }
    const char *probe_state = "failed";
    if (proxy_probe_out.state == FetchSeedState::kFound) {
      probe_state = "found";
    } else if (proxy_probe_out.state == FetchSeedState::kEmpty) {
      probe_state = "empty";
    }
    append_stage0_flow_log(stage0_db_.data_dir(),
                           "stage0_proxy_ready state=" + std::string(probe_state) +
                               " detail=" + proxy_probe_out.detail);
  }

  auto conn = stage0_db_.create_connection();
  auto exec_sql = [&](const std::string &sql) {
    auto r = conn->Query(sql);
    assert(r && !r->HasError());
  };
  auto commit_condition_block_atomic = [&](int64_t block, const std::vector<FetchResult> &rows,
                                           const std::vector<ConditionSeed> &empty_rows) {
    exec_sql("BEGIN TRANSACTION");
    if (!rows.empty()) {
      duckdb::Appender appender(*conn, "pm_condition_static");
      int64_t now_ms = unix_ms_now();
      persist_results_in_txn(appender, rows, now_ms);
      appender.Close();
    }
    if (!rows.empty() || !empty_rows.empty()) {
      duckdb::Appender class_appender(*conn, "pm_condition_scan_class");
      int64_t now_ms = unix_ms_now();
      for (const auto &row : rows) {
        bool is_neg_risk = is_poly_neg_risk(row.market);
        class_appender.BeginRow();
        class_appender.Append(make_blob_value(row.seed.condition_blob));
        class_appender.Append(is_neg_risk ? duckdb::Value(kClassPolyNegRisk) : duckdb::Value(kClassPolyCtf));
        class_appender.Append(row.seed.first_seen_block);
        class_appender.Append(now_ms);
        class_appender.EndRow();
      }
      for (const auto &seed : empty_rows) {
        class_appender.BeginRow();
        class_appender.Append(make_blob_value(seed.condition_blob));
        class_appender.Append(duckdb::Value(kClassNonPoly));
        class_appender.Append(seed.first_seen_block);
        class_appender.Append(now_ms);
        class_appender.EndRow();
      }
      class_appender.Close();
    }
    set_scan_cursor_in_txn(*conn, block);
    exec_sql("COMMIT");
  };

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
    append_stage0_flow_log(stage0_db_.data_dir(),
                           "seed_start worker=" + std::to_string(task->worker_slot) +
                               " block=" + std::to_string(task->block) +
                               " idx=" + std::to_string(task->next_seed_index) +
                               "/" + std::to_string(task->seeds.size()) +
                               " condition=" + seed.condition_hex_lower);
    auto on_done = [&, task, seed](FetchSeedOutcome out) mutable {
      if (out.state == FetchSeedState::kFound) {
        task->rows.push_back(FetchResult{
            .seed = seed,
            .market = std::move(out.market),
        });
      } else if (out.state == FetchSeedState::kEmpty) {
        task->empty_seeds += 1;
        task->empty_rows.push_back(seed);
        task->debug_logs.push_back(
            "seed_empty worker=" + std::to_string(task->worker_slot) +
            " block=" + std::to_string(task->block) +
            " condition=" + seed.condition_hex_lower +
            " detail=" + out.detail);
      } else {
        task->failed_seeds += 1;
        task->debug_logs.push_back(
            "seed_fail worker=" + std::to_string(task->worker_slot) +
            " block=" + std::to_string(task->block) +
            " condition=" + seed.condition_hex_lower +
            " detail=" + out.detail);
      }
      dispatch_next_seed(task);
    };
    auto on_retry = [&, task, seed](int attempt, const std::string &detail) {
      append_stage0_flow_log(stage0_db_.data_dir(),
                             "seed_retry worker=" + std::to_string(task->worker_slot) +
                                 " block=" + std::to_string(task->block) +
                                 " attempt=" + std::to_string(attempt) +
                                 " condition=" + seed.condition_hex_lower +
                                 " detail=" + detail);
    };
    async_seed_fetch(fetch_ioc, fetch_ssl_ctx, gamma_ep, stage0_proxy_url, seed.condition_hex_lower,
                     std::move(on_done), std::move(on_retry));
  };

  while (!stop_requested_ && next_commit_block <= stage1_head) {
    bool progressed = false;
    auto fast_forward_cursor = [&](int64_t committed_block, int64_t next_block) {
      applied_block = committed_block;
      runtime_scan_cursor_ = committed_block;
      {
        std::lock_guard<std::mutex> lock(status_mutex_);
        record_commit_locked(committed_block);
        refresh_status_locked(stage1_head, committed_block, true);
      }
      next_commit_block = next_block;
      next_dispatch_block = next_block;
      progressed = true;
    };
    fetch_ioc.restart();
    while (!stop_requested_ && static_cast<int>(inflight.size()) < kWorkerCount &&
           next_dispatch_block <= stage1_head) {
      if (next_dispatch_block > scanned_to_block) {
        SeedScanBatch batch = load_seed_scan_batch(next_dispatch_block, stage1_head, static_cast<size_t>(kWorkerCount));
        scanned_to_block = batch.scanned_to_block;
        const bool no_pending_before_dispatch =
            inflight.empty() && ready.empty() && (next_commit_block == next_dispatch_block);
        if (no_pending_before_dispatch) {
          if (batch.seeds_by_block.empty()) {
            fast_forward_cursor(scanned_to_block, scanned_to_block + 1);
            continue;
          }
          int64_t first_seed_block = batch.seeds_by_block.begin()->first;
          if (first_seed_block > next_commit_block) {
            fast_forward_cursor(first_seed_block - 1, first_seed_block);
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
        auto [it, inserted] =
            ready.emplace(block, BlockTaskResult{.block = block, .has_seeds = false, .rows = {}});
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
                                 " seeds=" + std::to_string(seeds.size()) +
                                 " conditions=" + summarize_seed_ids(seeds));
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
                                 " empty=" + std::to_string(task->empty_seeds) +
                                 " cost_ms=" + std::to_string(cost_ms));
      for (const auto &msg : task->debug_logs) {
        append_stage0_flow_log(stage0_db_.data_dir(), msg);
      }
      auto [it, inserted] = ready.emplace(task->block, BlockTaskResult{
                                                           .block = task->block,
                                                           .has_seeds = true,
                                                           .rows = std::move(task->rows),
                                                           .empty_seeds = std::move(task->empty_rows),
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
      std::vector<ConditionSeed> empty_rows_to_persist;
      rows_to_persist.reserve(it->second.rows.size());
      empty_rows_to_persist.reserve(it->second.empty_seeds.size());
      for (auto &row : it->second.rows) {
        if (known_condition_ids_.contains(row.seed.condition_hex_lower)) {
          continue;
        }
        rows_to_persist.push_back(std::move(row));
      }
      for (auto &seed : it->second.empty_seeds) {
        if (known_condition_ids_.contains(seed.condition_hex_lower)) {
          continue;
        }
        empty_rows_to_persist.push_back(std::move(seed));
      }
      if (it->second.has_seeds) {
        commit_condition_block_atomic(next_commit_block, rows_to_persist, empty_rows_to_persist);
      }
      runtime_scan_cursor_ = next_commit_block;
      for (const auto &row : rows_to_persist) {
        known_condition_ids_.insert(row.seed.condition_hex_lower);
        bool is_neg_risk = is_poly_neg_risk(row.market);
        if (is_neg_risk) {
          known_negrisk_condition_count_ += 1;
        } else {
          known_ctf_condition_count_ += 1;
        }
        persist_stage0_parsed_market(stage0_db_.data_dir(), row.seed.condition_hex_lower, row.market);
      }
      for (const auto &seed : empty_rows_to_persist) {
        known_condition_ids_.insert(seed.condition_hex_lower);
        known_nonpoly_condition_count_ += 1;
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

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    refresh_status_locked(stage1_head, applied_block, false);
  }
  if (stop_requested_) {
    return;
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
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_condition_scan_class ("
      "condition_id BLOB PRIMARY KEY, "
      "class TEXT NOT NULL, "
      "first_seen_block BIGINT NOT NULL, "
      "first_seen_ms BIGINT NOT NULL"
      ")");
  stage0_db_.execute(
      "INSERT OR IGNORE INTO pm_condition_scan_class "
      "SELECT condition_id, "
      "CASE WHEN market_neg_risk THEN 'poly_negrisk' ELSE 'poly_ctf' END, "
      "first_seen_block, first_seen_ms "
      "FROM pm_condition_static");
}

void StageSync::load_known_conditions() {
  known_condition_ids_.clear();
  known_ctf_condition_count_ = 0;
  known_negrisk_condition_count_ = 0;
  known_nonpoly_condition_count_ = 0;
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query("SELECT lower(hex(condition_id)) AS cid, class FROM pm_condition_scan_class");
  assert(result && !result->HasError());
  known_condition_ids_.reserve(static_cast<size_t>(result->RowCount()) * 2 + 1);
  for (idx_t row = 0; row < result->RowCount(); ++row) {
    std::string cid = result->GetValue(0, row).GetValue<std::string>();
    known_condition_ids_.insert("0x" + cid);
    std::string cls = result->GetValue(1, row).GetValue<std::string>();
    if (cls == kClassPolyNegRisk) {
      known_negrisk_condition_count_ += 1;
    } else if (cls == kClassPolyCtf) {
      known_ctf_condition_count_ += 1;
    } else {
      assert(cls == kClassNonPoly);
      known_nonpoly_condition_count_ += 1;
    }
  }
  assert(static_cast<int64_t>(known_condition_ids_.size()) ==
         known_ctf_condition_count_ + known_negrisk_condition_count_ + known_nonpoly_condition_count_);
}

void StageSync::ensure_cursor_floor() {
  int64_t floor_block = config_.initial_block - 1;
  int64_t cursor = get_scan_cursor();
  if (cursor >= floor_block) {
    return;
  }
  auto conn = stage0_db_.create_connection();
  auto begin = conn->Query("BEGIN TRANSACTION");
  assert(begin && !begin->HasError());
  set_scan_cursor_in_txn(*conn, floor_block);
  auto commit = conn->Query("COMMIT");
  assert(commit && !commit->HasError());
}

StageSync::SeedScanBatch StageSync::load_seed_scan_batch(int64_t start_block, int64_t head_block,
                                                         size_t max_conditions) const {
  SeedScanBatch out;
  assert(start_block <= head_block);
  int64_t end_block = head_block;
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

int64_t StageSync::get_scan_cursor() {
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query("SELECT last_block FROM pm_sync_cursor WHERE id = 0");
  assert(result && !result->HasError());
  if (result->RowCount() == 0) {
    return -1;
  }
  return result->GetValue(0, 0).GetValue<int64_t>();
}

void StageSync::set_scan_cursor_in_txn(duckdb::Connection &conn, int64_t block) {
  auto result = conn.Query("UPDATE pm_sync_cursor SET last_block = " + std::to_string(block) + " WHERE id = 0");
  assert(result && !result->HasError());
}

} // namespace stage0
