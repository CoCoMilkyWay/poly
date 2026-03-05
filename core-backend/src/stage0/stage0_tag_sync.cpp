#include "stage0_tag_sync.hpp"

#include "config.hpp"
#include "misc/profiler.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <boost/asio/steady_timer.hpp>
#include <nlohmann/json.hpp>

namespace stage0 {
namespace {

using json = nlohmann::json;

int64_t unix_ms_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
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

} // namespace

TagSync::TagSync(const Config &config, Database &stage0_db, int base_interval_seconds)
    : config_(config), stage0_db_(stage0_db), base_interval_seconds_(base_interval_seconds) {
  assert(base_interval_seconds_ > 0);
  init_schema();
  load_tag_counts();
  init_tagger();
  sync_.tag_last_block = tag_last_block_;
  sync_.tagged_count = tagged_count_;
  sync_.untagged_count = untagged_count_;
}

void TagSync::start(asio::io_context &ioc) {
  ioc_ = &ioc;
  stop_requested_ = false;
  schedule_sync(0);
}

void TagSync::stop() {
  stop_requested_ = true;
}

TagSync::Status TagSync::status() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  Status s = sync_;
  {
    std::lock_guard<std::mutex> tagger_lock(tagger_mutex_);
    if (tagger_) {
      s.tag_device = tagger_->device_name();
    }
  }
  return s;
}

void TagSync::reset_progress() {
  tag_reset_epoch_.fetch_add(1, std::memory_order_seq_cst);

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
  auto reset_cursor = conn->Query("UPDATE pm_tag_cursor SET last_rowid = -1 WHERE id = 0");
  assert(reset_cursor && !reset_cursor->HasError());
  auto commit = conn->Query("COMMIT");
  assert(commit && !commit->HasError());

  load_tag_counts();
  tag_last_block_ = 0;
  tagged_count_ = 0;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    sync_.tag_last_block = 0;
    sync_.tagged_count = 0;
    sync_.untagged_count = untagged_count_;
  }
  append_stage0_flow_log(stage0_db_.data_dir(),
                         "tag_reset untagged=" + std::to_string(untagged_count_));
}

void TagSync::schedule_sync(int delay_seconds) {
  if (stop_requested_) {
    return;
  }
  assert(ioc_ != nullptr);
  auto timer = std::make_shared<asio::steady_timer>(*ioc_, std::chrono::seconds(delay_seconds));
  timer->async_wait([this, timer](const boost::system::error_code &ec) {
    if (!ec && !stop_requested_) {
      do_sync_tick();
    }
  });
}

void TagSync::do_sync_tick() {
  if (stop_requested_) {
    return;
  }
  const int64_t tagged_now = do_tag_sync();
  schedule_sync(tagged_now > 0 ? 0 : base_interval_seconds_);
}

void TagSync::init_schema() {
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_condition_static ("
      "condition_id BLOB PRIMARY KEY, "
      "market_json JSON NOT NULL"
      ")");
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_condition_scan_class ("
      "condition_id BLOB PRIMARY KEY, "
      "class TEXT NOT NULL, "
      "first_seen_block BIGINT NOT NULL, "
      "first_seen_ms BIGINT NOT NULL, "
      "tag_name TEXT"
      ")");
  stage0_db_.execute(
      "CREATE TABLE IF NOT EXISTS pm_tag_cursor ("
      "id INTEGER PRIMARY KEY CHECK (id = 0), "
      "last_rowid BIGINT NOT NULL"
      ")");
  stage0_db_.execute(
      "INSERT OR IGNORE INTO pm_tag_cursor (id, last_rowid) VALUES (0, 0)");
}

void TagSync::init_tagger() {
  const std::string model_dir = config_.model_dir;
  const std::string tag_md = config_.tag_md_path;
  const TaggerModelInfo model_info = detect_tagger_model_info(model_dir);
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    sync_.tag_model_path = model_info.model_path;
    sync_.tag_model_size_text = model_info.model_size_text;
  }
  if (model_dir.empty() || tag_md.empty()) {
    std::cout << "[Tagger] Tagger disabled (model_dir or tag_md not configured)" << std::endl;
    return;
  }
  try {
    auto new_tagger = std::make_unique<Tagger>(model_dir, tag_md);
    std::lock_guard<std::mutex> tagger_lock(tagger_mutex_);
    tagger_ = std::move(new_tagger);
  } catch (const std::exception &e) {
    std::cerr << "[Tagger] Failed to initialize tagger: " << e.what() << std::endl;
  }
}

void TagSync::load_tag_counts() {
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

int64_t TagSync::get_tag_cursor() {
  auto conn = stage0_db_.create_connection();
  auto result = conn->Query("SELECT last_rowid FROM pm_tag_cursor WHERE id = 0");
  assert(result && !result->HasError());
  if (result->RowCount() == 0) {
    return 0;
  }
  return result->GetValue(0, 0).GetValue<int64_t>();
}

void TagSync::set_tag_cursor_in_txn(duckdb::Connection &conn, int64_t cursor) {
  auto result = conn.Query("UPDATE pm_tag_cursor SET last_rowid = " + std::to_string(cursor) + " WHERE id = 0");
  assert(result && !result->HasError());
}

int64_t TagSync::do_tag_sync() {
  Trace;
  const uint64_t epoch_at_start = tag_reset_epoch_.load(std::memory_order_seq_cst);
  const int64_t tag_cursor = get_tag_cursor();
  std::string tag_trace_name = "s0/sync >" + std::to_string(tag_cursor);
  TraceName(tag_trace_name.c_str(), tag_trace_name.size());

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
                                   "LIMIT " +
      std::to_string(config::kModelBatchSize));
  assert(result && !result->HasError());

  if (result->RowCount() == 0) {
    std::lock_guard<std::mutex> tagger_lock(tagger_mutex_);
    tagger_.reset();
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

  bool need_init = false;
  {
    std::lock_guard<std::mutex> tagger_lock(tagger_mutex_);
    need_init = (tagger_ == nullptr);
  }
  if (need_init) {
    init_tagger();
  }
  Tagger *tagger_ptr = nullptr;
  {
    std::lock_guard<std::mutex> tagger_lock(tagger_mutex_);
    if (!tagger_) {
      return 0;
    }
    tagger_ptr = tagger_.get();
  }
  assert(tagger_ptr != nullptr);
  auto tags = tagger_ptr->tag_batch(model_inputs);
  assert(tags.size() == rowids.size());

  if (tag_reset_epoch_.load(std::memory_order_seq_cst) != epoch_at_start) {
    append_stage0_flow_log(stage0_db_.data_dir(), "tag_batch discarded due to reset");
    return 1;
  }

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
