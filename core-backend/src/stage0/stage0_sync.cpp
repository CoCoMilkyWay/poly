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
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#define STAGE0_DEBUG_DUMP_JSON 0

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

std::string normalize_inline_text(std::string s) {
  for (char &ch : s) {
    if (ch == '\n' || ch == '\r' || ch == '\t') {
      ch = ' ';
    }
  }
  size_t start = s.find_first_not_of(' ');
  if (start == std::string::npos) {
    return "";
  }
  size_t end = s.find_last_not_of(' ');
  return s.substr(start, end - start + 1);
}

std::string build_tagging_input_text(const json &market, std::string &out_question_for_log) {
  std::string question;
  if (market.contains("question") && market["question"].is_string()) {
    question = market["question"].get<std::string>();
  } else if (market.contains("events") && market["events"].is_array() && !market["events"].empty()) {
    const auto &e0 = market["events"][0];
    if (e0.contains("title") && e0["title"].is_string()) {
      question = e0["title"].get<std::string>();
    }
  }
  question = normalize_inline_text(question);
  if (question.empty()) {
    question = "__MISSING__";
  }
  out_question_for_log = question;

  std::vector<std::string> tag_parts;
  if (market.contains("tags") && market["tags"].is_array()) {
    for (const auto &tag_item : market["tags"]) {
      if (tag_item.is_object()) {
        if (tag_item.contains("label") && tag_item["label"].is_string()) {
          std::string label = normalize_inline_text(tag_item["label"].get<std::string>());
          if (!label.empty()) {
            tag_parts.push_back(std::move(label));
          }
        } else if (tag_item.contains("slug") && tag_item["slug"].is_string()) {
          std::string slug = normalize_inline_text(tag_item["slug"].get<std::string>());
          if (!slug.empty()) {
            tag_parts.push_back(std::move(slug));
          }
        }
      } else if (tag_item.is_string()) {
        std::string t = normalize_inline_text(tag_item.get<std::string>());
        if (!t.empty()) {
          tag_parts.push_back(std::move(t));
        }
      }
    }
  }
  std::string tags_text;
  for (size_t i = 0; i < tag_parts.size(); ++i) {
    if (i > 0) {
      tags_text += " | ";
    }
    tags_text += tag_parts[i];
  }

  std::string description;
  if (market.contains("description") && market["description"].is_string()) {
    description = market["description"].get<std::string>();
  } else if (market.contains("events") && market["events"].is_array() && !market["events"].empty()) {
    const auto &e0 = market["events"][0];
    if (e0.contains("description") && e0["description"].is_string()) {
      description = e0["description"].get<std::string>();
    }
  }
  description = normalize_inline_text(description);

  std::string input = question;
  if (!tags_text.empty()) {
    input += " [TAGS] " + tags_text;
  }
  if (!description.empty()) {
    input += " [DESC] " + description;
  }
  return input;
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

std::string truncate_for_fixed_col(const std::string &s, size_t width) {
  if (s.size() <= width) {
    return s;
  }
  assert(width >= 3);
  return s.substr(0, width - 3) + "...";
}

void append_stage0_tag_log(const std::string &data_dir, int64_t block_num, float confidence,
                           const std::string &tag, const std::string &question,
                           const std::string &condition_id) {
  static std::mutex tag_log_mutex;
  std::lock_guard<std::mutex> lock(tag_log_mutex);
  const std::string log_dir = ensure_stage0_log_dir(data_dir);
  std::ofstream f(log_dir + "/tag", std::ios::app);
  assert(f.is_open());
  std::ostringstream oss;
  oss << std::left << std::setw(12) << block_num
      << std::setw(10) << std::fixed << std::setprecision(4) << confidence
      << std::setw(40) << truncate_for_fixed_col(tag, 39)
      << std::setw(100) << truncate_for_fixed_col(question, 99)
      << std::setw(68) << condition_id;
  f << oss.str() << "\n";
  f.flush();
  assert(f.good());
}

void persist_stage0_parsed_market(const std::string &data_dir, const std::string &condition_hex_lower,
                                  const json &market) {
  assert(condition_hex_lower.starts_with("0x"));
  const std::string log_dir = ensure_stage0_log_dir(data_dir);
  std::ofstream f(log_dir + "/" + condition_hex_lower + ".json", std::ios::trunc);
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
  assert(base_interval_seconds_ > 0);
  init_schema();
  ensure_cursor_floor();
  load_known_conditions();
  load_tag_counts();
  init_tagger();
  sync_.last_block = get_scan_cursor();
  sync_.head_block = sync_.last_block;
  sync_.behind_blocks = 0;
  sync_.condition_count = static_cast<int64_t>(known_condition_ids_.size());
  sync_.ctf_condition_count = known_ctf_condition_count_;
  sync_.negrisk_condition_count = known_negrisk_condition_count_;
  sync_.nonpoly_condition_count = known_nonpoly_condition_count_;
  sync_.tag_last_block = tag_last_block_;
  sync_.tagged_count = tagged_count_;
  sync_.untagged_count = untagged_count_;
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

void StageSync::reset_tag_progress() {
  {
    const std::string log_dir = ensure_stage0_log_dir(stage0_db_.data_dir());
    std::ofstream f(log_dir + "/tag", std::ios::trunc);
    assert(f.is_open());
    f.flush();
    assert(f.good());
  }
  auto conn = stage0_db_.create_connection();
  auto begin = conn->Query("BEGIN TRANSACTION");
  assert(begin && !begin->HasError());
  auto clear_tags = conn->Query(
      "UPDATE pm_condition_scan_class "
      "SET tag_name = NULL "
      "WHERE class IN ('poly_ctf', 'poly_negrisk')");
  assert(clear_tags && !clear_tags->HasError());
  auto reset_cursor = conn->Query("UPDATE pm_tag_cursor SET last_rowid = 0 WHERE id = 0");
  assert(reset_cursor && !reset_cursor->HasError());
  auto commit = conn->Query("COMMIT");
  assert(commit && !commit->HasError());

  load_tag_counts();
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    sync_.tag_last_block = tag_last_block_;
    sync_.tagged_count = tagged_count_;
    sync_.untagged_count = untagged_count_;
  }
  append_stage0_flow_log(stage0_db_.data_dir(),
                         "tag_reset tagged=" + std::to_string(tagged_count_) +
                             " untagged=" + std::to_string(untagged_count_));
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
  Trace;
  if (stop_requested_) {
    return;
  }
  ensure_cursor_floor();

  int64_t stage1_head = stage1_db_.get_last_block();
  int64_t cursor = runtime_scan_cursor_inited_ ? runtime_scan_cursor_ : get_scan_cursor();
  std::string sync_trace_name = "s0/sync " + std::to_string(cursor + 1) + "-" + std::to_string(stage1_head);
  TraceName(sync_trace_name.c_str(), sync_trace_name.size());
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
    const int64_t tagged_now = do_tag_sync();
    schedule_sync(tagged_now > 0 ? 0 : base_interval_seconds_);
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
      persist_results_in_txn(appender, rows);
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
        class_appender.Append(duckdb::Value());
        class_appender.EndRow();
      }
      for (const auto &seed : empty_rows) {
        class_appender.BeginRow();
        class_appender.Append(make_blob_value(seed.condition_blob));
        class_appender.Append(duckdb::Value(kClassNonPoly));
        class_appender.Append(seed.first_seen_block);
        class_appender.Append(now_ms);
        class_appender.Append(duckdb::Value());
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
#if STAGE0_DEBUG_DUMP_JSON
        persist_stage0_parsed_market(stage0_db_.data_dir(), row.seed.condition_hex_lower, row.market);
#endif
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

  // Tag sync - process untagged conditions
  const int64_t tagged_now = do_tag_sync();
  schedule_sync((applied_block < stage1_head || tagged_now > 0) ? 0 : base_interval_seconds_);
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
      "condition_id BLOB PRIMARY KEY, "
      "market_json JSON NOT NULL"
      ")");
  {
    auto conn = stage0_db_.create_connection();
    auto schema_result = conn->Query("PRAGMA table_info('pm_condition_static')");
    assert(schema_result && !schema_result->HasError());
    assert(schema_result->RowCount() == 2);
    std::string col0_name = schema_result->GetValue(1, 0).GetValue<std::string>();
    std::string col1_name = schema_result->GetValue(1, 1).GetValue<std::string>();
    assert(col0_name == "condition_id");
    assert(col1_name == "market_json");
  }
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_condition_scan_class ("
      "condition_id BLOB PRIMARY KEY, "
      "class TEXT NOT NULL, "
      "first_seen_block BIGINT NOT NULL, "
      "first_seen_ms BIGINT NOT NULL, "
      "tag_name TEXT"
      ")");
  {
    auto conn = stage0_db_.create_connection();
    auto schema_result = conn->Query("PRAGMA table_info('pm_condition_scan_class')");
    assert(schema_result && !schema_result->HasError());
    assert(schema_result->RowCount() == 5);
    std::string col0_name = schema_result->GetValue(1, 0).GetValue<std::string>();
    std::string col1_name = schema_result->GetValue(1, 1).GetValue<std::string>();
    std::string col2_name = schema_result->GetValue(1, 2).GetValue<std::string>();
    std::string col3_name = schema_result->GetValue(1, 3).GetValue<std::string>();
    std::string col4_name = schema_result->GetValue(1, 4).GetValue<std::string>();
    assert(col0_name == "condition_id");
    assert(col1_name == "class");
    assert(col2_name == "first_seen_block");
    assert(col3_name == "first_seen_ms");
    assert(col4_name == "tag_name");
  }

  // Tag cursor table
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_tag_cursor ("
      "id INTEGER PRIMARY KEY CHECK (id = 0), "
      "last_rowid BIGINT NOT NULL"
      ")");
  stage0_db_.execute(
      "INSERT OR IGNORE INTO pm_tag_cursor (id, last_rowid) VALUES (0, 0)");
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
      "SELECT block_number, condition_id "
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
    assert(cond_blob.size() == 32);

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

// ============================================================================
// Tag Methods
// ============================================================================

void StageSync::init_tagger() {
  const std::string model_dir = config_.model_dir;
  const std::string tag_md = config_.tag_md_path;
  if (model_dir.empty() || tag_md.empty()) {
    std::cout << "[Stage0] Tagger disabled (model_dir or tag_md not configured)" << std::endl;
    return;
  }
  try {
    tagger_ = std::make_unique<Tagger>(model_dir, tag_md);
    std::cout << "[Stage0] Tagger initialized with " << tagger_->label_count() << " labels" << std::endl;
  } catch (const std::exception &e) {
    std::cerr << "[Stage0] Failed to initialize tagger: " << e.what() << std::endl;
  }
}

void StageSync::load_tag_counts() {
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query(
      "SELECT "
      "  SUM(CASE WHEN tag_name IS NOT NULL AND class IN ('poly_ctf','poly_negrisk') THEN 1 ELSE 0 END) AS tagged, "
      "  SUM(CASE WHEN tag_name IS NULL AND class IN ('poly_ctf','poly_negrisk') THEN 1 ELSE 0 END) AS untagged, "
      "  MAX(CASE WHEN tag_name IS NOT NULL AND class IN ('poly_ctf','poly_negrisk') THEN first_seen_block ELSE NULL END) AS tag_last_block "
      "FROM pm_condition_scan_class");
  assert(result && !result->HasError());
  if (result->RowCount() > 0) {
    auto tagged_val = result->GetValue(0, 0);
    auto untagged_val = result->GetValue(1, 0);
    auto tag_last_block_val = result->GetValue(2, 0);
    tagged_count_ = tagged_val.IsNull() ? 0 : tagged_val.GetValue<int64_t>();
    untagged_count_ = untagged_val.IsNull() ? 0 : untagged_val.GetValue<int64_t>();
    tag_last_block_ = tag_last_block_val.IsNull() ? 0 : tag_last_block_val.GetValue<int64_t>();
  }
}

int64_t StageSync::get_tag_cursor() {
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query("SELECT last_rowid FROM pm_tag_cursor WHERE id = 0");
  assert(result && !result->HasError());
  if (result->RowCount() == 0) {
    return 0;
  }
  return result->GetValue(0, 0).GetValue<int64_t>();
}

void StageSync::set_tag_cursor_in_txn(duckdb::Connection &conn, int64_t cursor) {
  auto result = conn.Query("UPDATE pm_tag_cursor SET last_rowid = " + std::to_string(cursor) + " WHERE id = 0");
  assert(result && !result->HasError());
}

int64_t StageSync::do_tag_sync() {
  if (!tagger_) {
    return 0;
  }
  const int64_t tag_cursor = get_tag_cursor();

  // Fetch untagged poly conditions after current tag cursor.
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query(
      "SELECT c.rowid AS c_rowid, c.first_seen_block, c.condition_id, s.market_json "
      "FROM pm_condition_scan_class c "
      "JOIN pm_condition_static s USING (condition_id) "
      "WHERE c.rowid > " +
      std::to_string(tag_cursor) + " "
                                   "  AND c.tag_name IS NULL "
                                   "  AND c.class IN ('poly_ctf', 'poly_negrisk') "
                                   "ORDER BY c.rowid ASC "
                                   "LIMIT 128"); // model batch size
  assert(result && !result->HasError());

  if (result->RowCount() == 0) {
    return 0;
  }

  std::vector<int64_t> rowids;
  std::vector<int64_t> blocks;
  std::vector<std::string> cond_ids;
  std::vector<std::string> question_logs;
  std::vector<std::string> model_inputs;
  rowids.reserve(result->RowCount());
  blocks.reserve(result->RowCount());
  cond_ids.reserve(result->RowCount());
  question_logs.reserve(result->RowCount());
  model_inputs.reserve(result->RowCount());

  for (idx_t i = 0; i < result->RowCount(); ++i) {
    int64_t rowid = result->GetValue(0, i).GetValue<int64_t>();
    int64_t first_seen_block = result->GetValue(1, i).GetValue<int64_t>();
    std::string cond_blob = result->GetValue(2, i).GetValue<std::string>();
    std::string market_json_str = result->GetValue(3, i).GetValue<std::string>();

    json market = json::parse(market_json_str, nullptr, false);
    std::string question;
    std::string model_input;
    if (market.is_discarded()) {
      question = "__MISSING__";
      model_input = "__MISSING__";
    } else {
      model_input = build_tagging_input_text(market, question);
    }

    rowids.push_back(rowid);
    blocks.push_back(first_seen_block);
    cond_ids.push_back(blob_to_hex_lower(cond_blob));
    question_logs.push_back(std::move(question));
    model_inputs.push_back(std::move(model_input));
  }

  // Batch tag
  auto tags = tagger_->tag_batch(model_inputs);
  assert(tags.size() == rowids.size());

  // Update database
  auto begin = conn->Query("BEGIN TRANSACTION");
  assert(begin && !begin->HasError());

  for (size_t i = 0; i < rowids.size(); ++i) {
    std::string escaped_tag;
    for (char c : tags[i].tag_name) {
      if (c == '\'') {
        escaped_tag += "''";
      } else {
        escaped_tag += c;
      }
    }
    std::string update_sql =
        "UPDATE pm_condition_scan_class SET tag_name = '" + escaped_tag + "' WHERE rowid = " + std::to_string(rowids[i]);
    auto update_result = conn->Query(update_sql);
    assert(update_result && !update_result->HasError());
    append_stage0_tag_log(stage0_db_.data_dir(), blocks[i], tags[i].confidence, tags[i].tag_name,
                          question_logs[i], cond_ids[i]);
  }
  set_tag_cursor_in_txn(*conn, rowids.back());

  auto commit = conn->Query("COMMIT");
  assert(commit && !commit->HasError());

  tagged_count_ += static_cast<int64_t>(rowids.size());
  untagged_count_ -= static_cast<int64_t>(rowids.size());
  tag_last_block_ = std::max(tag_last_block_, blocks.back());
  if (untagged_count_ < 0) {
    untagged_count_ = 0;
  }

  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    sync_.tag_last_block = tag_last_block_;
    sync_.tagged_count = tagged_count_;
    sync_.untagged_count = untagged_count_;
  }

  append_stage0_flow_log(stage0_db_.data_dir(),
                         "tag_batch count=" + std::to_string(rowids.size()) +
                             " tagged=" + std::to_string(tagged_count_) +
                             " untagged=" + std::to_string(untagged_count_));
  return static_cast<int64_t>(rowids.size());
}

} // namespace stage0
