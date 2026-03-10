#include "database.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
Database::FeatherChunk parse_chunk_filename(const std::string &filename) {
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

bool has_any_feather_partition(const std::string &data_dir) {
  static const char *tables[] = {
      "transfer", "condition_preparation", "condition_resolution",
      "split", "merge", "redemption", "fpmm", "fpmm_trade", "fpmm_funding",
      "order_filled", "token_map", "neg_risk_market", "neg_risk_question", "convert"};
  for (const char *table : tables) {
    fs::path dir = fs::path(data_dir) / table;
    if (!fs::exists(dir) || !fs::is_directory(dir)) {
      continue;
    }
    for (const auto &entry : fs::directory_iterator(dir)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const std::string filename = entry.path().filename().string();
      if (filename.ends_with(".feather")) {
        return true;
      }
    }
  }
  return false;
}

std::string trim_ascii(std::string_view input) {
  size_t begin = 0;
  while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    begin++;
  }
  size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    end--;
  }
  return std::string(input.substr(begin, end - begin));
}

int64_t parse_size_text_to_bytes(const std::string &text) {
  const std::string trimmed = trim_ascii(text);
  assert(!trimmed.empty());

  size_t value_end = 0;
  while (value_end < trimmed.size() &&
         (std::isdigit(static_cast<unsigned char>(trimmed[value_end])) != 0 || trimmed[value_end] == '.')) {
    value_end++;
  }
  assert(value_end > 0);

  const double value = std::stod(trimmed.substr(0, value_end));
  std::string unit = trim_ascii(trimmed.substr(value_end));
  std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (unit.empty()) {
    unit = "b";
  }

  double multiplier = 1.0;
  if (unit == "b" || unit == "byte" || unit == "bytes") {
    multiplier = 1.0;
  } else if (unit == "kb") {
    multiplier = 1000.0;
  } else if (unit == "mb") {
    multiplier = 1000.0 * 1000.0;
  } else if (unit == "gb") {
    multiplier = 1000.0 * 1000.0 * 1000.0;
  } else if (unit == "tb") {
    multiplier = 1000.0 * 1000.0 * 1000.0 * 1000.0;
  } else if (unit == "kib") {
    multiplier = 1024.0;
  } else if (unit == "mib") {
    multiplier = 1024.0 * 1024.0;
  } else if (unit == "gib") {
    multiplier = 1024.0 * 1024.0 * 1024.0;
  } else if (unit == "tib") {
    multiplier = 1024.0 * 1024.0 * 1024.0 * 1024.0;
  } else {
    assert(false && "unknown size unit");
  }
  return static_cast<int64_t>(std::llround(value * multiplier));
}
} // namespace

Database::Database(const std::string &path) : db_path_(path) {
  auto parent = fs::path(path).parent_path();
  data_dir_ = parent.empty() ? "." : parent.string();
  fs::create_directories(data_dir_);
  duckdb::DBConfig config;
  // 限制单个DuckDB实例内存，避免多实例合计占用过多
  // 4 instances × 2GB = 8GB total
  // config.SetOption("memory_limit", duckdb::Value("2GB"));
  db_ = std::make_unique<duckdb::DuckDB>(path, &config);
  read_conn_ = std::make_unique<duckdb::Connection>(*db_);
  write_conn_ = std::make_unique<duckdb::Connection>(*db_);

  lock_path_ = path + ".lock";
  lock_fd_ = open(lock_path_.c_str(), O_CREAT | O_RDWR, 0666);
  assert(lock_fd_ >= 0 && "无法创建锁文件");
}

Database::~Database() {
  if (has_write_lock_) {
    release_write_lock();
  }
  if (lock_fd_ >= 0) {
    close(lock_fd_);
  }
}

void Database::acquire_write_lock() {
  assert(!has_write_lock_ && "已持有写锁");
  int ret = flock(lock_fd_, LOCK_EX);
  assert(ret == 0 && "获取写锁失败");
  has_write_lock_ = true;
}

void Database::release_write_lock() {
  assert(has_write_lock_ && "未持有写锁");
  int ret = flock(lock_fd_, LOCK_UN);
  assert(ret == 0 && "释放写锁失败");
  has_write_lock_ = false;
}

bool Database::try_write_lock() {
  if (has_write_lock_) {
    return true;
  }
  int ret = flock(lock_fd_, LOCK_EX | LOCK_NB);
  if (ret == 0) {
    has_write_lock_ = true;
    return true;
  }
  return false;
}

Database::WriteLock::WriteLock(Database &db) : db_(db) {
  db_.acquire_write_lock();
}

Database::WriteLock::~WriteLock() {
  db_.release_write_lock();
}

void Database::execute(const std::string &sql) {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto result = write_conn_->Query(sql);
  if (result->HasError()) {
    std::cerr << "[DB] execute failed: " << result->GetError() << std::endl;
    std::cerr << "[DB] SQL: " << sql << std::endl;
  }
  assert(!result->HasError() && "execute failed");
}

void Database::execute_read(const std::string &sql) {
  std::lock_guard<std::mutex> lock(read_mutex_);
  auto result = read_conn_->Query(sql);
  assert(!result->HasError() && "execute_read failed");
}

json Database::query_json(const std::string &sql) {
  std::lock_guard<std::mutex> lock(read_mutex_);
  auto result = read_conn_->Query(sql);
  if (result->HasError()) {
    std::cerr << "[DB] query_json failed: " << result->GetError() << std::endl;
    std::cerr << "[DB] SQL: " << sql << std::endl;
  }
  assert(!result->HasError() && "query_json failed");

  json rows = json::array();
  auto &types = result->types;
  auto names = result->names;

  for (size_t row = 0; row < result->RowCount(); ++row) {
    json obj = json::object();
    for (size_t col = 0; col < result->ColumnCount(); ++col) {
      auto value = result->GetValue(col, row);
      if (value.IsNull()) {
        obj[names[col]] = nullptr;
      } else {
        switch (types[col].id()) {
        case duckdb::LogicalTypeId::BOOLEAN:
          obj[names[col]] = value.GetValue<bool>();
          break;
        case duckdb::LogicalTypeId::TINYINT:
        case duckdb::LogicalTypeId::SMALLINT:
        case duckdb::LogicalTypeId::INTEGER:
          obj[names[col]] = value.GetValue<int32_t>();
          break;
        case duckdb::LogicalTypeId::BIGINT:
          obj[names[col]] = value.GetValue<int64_t>();
          break;
        case duckdb::LogicalTypeId::FLOAT:
        case duckdb::LogicalTypeId::DOUBLE:
          obj[names[col]] = value.GetValue<double>();
          break;
        case duckdb::LogicalTypeId::BLOB: {
          auto blob = duckdb::StringValue::Get(value);
          std::string hex = "0x";
          hex.reserve(2 + blob.size() * 2);
          static const char hex_chars[] = "0123456789abcdef";
          for (unsigned char c : blob) {
            hex.push_back(hex_chars[c >> 4]);
            hex.push_back(hex_chars[c & 0x0f]);
          }
          obj[names[col]] = hex;
          break;
        }
        default:
          obj[names[col]] = value.ToString();
          break;
        }
      }
    }
    rows.push_back(std::move(obj));
  }
  return rows;
}

int64_t Database::query_single_int(const std::string &sql) {
  std::lock_guard<std::mutex> lock(read_mutex_);
  auto result = read_conn_->Query(sql);
  if (result->HasError() || result->RowCount() == 0) {
    return 0;
  }
  auto val = result->GetValue(0, 0);
  return val.IsNull() ? 0 : val.GetValue<int64_t>();
}

json Database::get_tables() {
  return query_json(
      "SELECT table_name FROM information_schema.tables "
      "WHERE table_schema='main' ORDER BY table_name");
}

int64_t Database::get_table_count(const std::string &table) {
  return query_single_int("SELECT COUNT(*) FROM " + table);
}

duckdb::DuckDB &Database::get_duckdb() {
  return *db_;
}

std::unique_ptr<duckdb::Connection> Database::create_connection() {
  return std::make_unique<duckdb::Connection>(*db_);
}

void Database::checkpoint() {
  std::lock_guard<std::mutex> lock(write_mutex_);
  auto result = write_conn_->Query("CHECKPOINT");
  assert(!result->HasError());
}

void Database::init_schema() {
  execute("INSTALL nanoarrow FROM community");
  execute("LOAD nanoarrow");
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    json state = read_state_unlocked();
    if (!state.contains("last_block")) {
      assert(!has_any_feather_partition(data_dir_) &&
             "state.json 缺少 last_block,但已存在 feather 数据；禁止自动恢复");
      state["last_block"] = -1;
      write_state_unlocked(state);
    }
  }
  cleanup_incomplete_partitions();
}

void Database::cleanup_incomplete_partitions() {
  static const char *tables[] = {
      "transfer", "condition_preparation", "condition_resolution",
      "split", "merge", "redemption", "fpmm", "fpmm_trade", "fpmm_funding",
      "order_filled", "token_map", "neg_risk_market", "neg_risk_question", "convert"};

  int64_t cursor = get_last_block();

  int removed = 0;
  for (const char *table : tables) {
    std::string dir = feather_dir(table);
    if (!fs::exists(dir)) {
      continue;
    }

    for (const auto &entry : fs::directory_iterator(dir)) {
      std::string filename = entry.path().filename().string();
      if (filename.ends_with(".tmp")) {
        fs::remove(entry.path());
        ++removed;
        continue;
      }
      if (!filename.ends_with(".feather")) {
        continue;
      }
      auto chunk = parse_chunk_filename(filename);
      if (chunk.end_block > cursor) {
        fs::remove(entry.path());
        ++removed;
      }
    }
  }
  if (removed > 0) {
    std::cout << "[DB] 清理了 " << removed << " 个不完整分区文件" << std::endl;
  }
}

std::string Database::feather_dir(const std::string &table) const {
  return data_dir_ + "/" + table;
}

std::vector<Database::FeatherChunk> Database::list_chunks(const std::string &table) const {
  std::vector<FeatherChunk> chunks;
  std::string dir = feather_dir(table);
  if (!fs::exists(dir)) {
    return chunks;
  }
  for (const auto &entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string filename = entry.path().filename().string();
    if (!filename.ends_with(".feather")) {
      continue;
    }
    chunks.push_back(parse_chunk_filename(filename));
  }
  std::sort(chunks.begin(), chunks.end(), [](const FeatherChunk &a, const FeatherChunk &b) {
    if (a.start_block != b.start_block) {
      return a.start_block < b.start_block;
    }
    return a.end_block < b.end_block;
  });
  chunks.erase(std::unique(chunks.begin(), chunks.end(), [](const FeatherChunk &a, const FeatherChunk &b) {
                 return a.start_block == b.start_block && a.end_block == b.end_block;
               }),
               chunks.end());
  return chunks;
}

std::string Database::feather_table_range(const std::string &table, int64_t start_block, int64_t end_block) {
  if (start_block > end_block) {
    return "(SELECT 1 WHERE 1=0)";
  }
  std::vector<FeatherChunk> chunks = list_chunks(table);
  if (chunks.empty()) {
    return "(SELECT 1 WHERE 1=0)";
  }

  std::vector<std::string> paths;
  for (const auto &chunk : chunks) {
    if (chunk.end_block < start_block) {
      continue;
    }
    if (chunk.start_block > end_block) {
      break;
    }
    std::string path = feather_dir(table) + "/" + std::to_string(chunk.start_block) + "-" +
                       std::to_string(chunk.end_block) + ".feather";
    paths.push_back(path);
  }

  if (paths.empty()) {
    return "(SELECT 1 WHERE 1=0)";
  }
  if (paths.size() == 1) {
    return "read_arrow('" + paths[0] + "')";
  }
  std::string result = "(";
  for (size_t i = 0; i < paths.size(); ++i) {
    if (i > 0) {
      result += " UNION ALL ";
    }
    result += "SELECT * FROM read_arrow('" + paths[i] + "')";
  }
  return result + ")";
}

std::vector<Database::FeatherChunk> Database::feather_chunks(const std::string &table) const {
  return list_chunks(table);
}

const std::string &Database::data_dir() const {
  return data_dir_;
}

const std::string &Database::db_path() const {
  return db_path_;
}

json Database::memory_breakdown() {
  std::lock_guard<std::mutex> lock(read_mutex_);
  auto result = read_conn_->Query(
      "SELECT database_name, memory_usage, memory_limit, wal_size "
      "FROM pragma_database_size()");
  assert(result && !result->HasError());
  assert(result->RowCount() > 0);

  idx_t target_row = 0;
  for (idx_t i = 0; i < result->RowCount(); ++i) {
    const std::string db_name = result->GetValue(0, i).GetValueUnsafe<std::string>();
    if (db_name == "main") {
      target_row = i;
      break;
    }
  }

  const std::string database_name = result->GetValue(0, target_row).GetValueUnsafe<std::string>();
  const std::string memory_usage_text = result->GetValue(1, target_row).GetValueUnsafe<std::string>();
  const std::string memory_limit_text = result->GetValue(2, target_row).GetValueUnsafe<std::string>();
  const std::string wal_size_text = result->GetValue(3, target_row).GetValueUnsafe<std::string>();

  return {
      {"engine", "duckdb"},
      {"database_name", database_name},
      {"path", db_path_},
      {"memory_usage_text", memory_usage_text},
      {"memory_limit_text", memory_limit_text},
      {"wal_size_text", wal_size_text},
      {"memory_usage_bytes", parse_size_text_to_bytes(memory_usage_text)},
      {"memory_limit_bytes", parse_size_text_to_bytes(memory_limit_text)},
      {"wal_size_bytes", parse_size_text_to_bytes(wal_size_text)},
  };
}

int64_t Database::get_last_block() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  json state = read_state_unlocked();
  assert(state.contains("last_block") && "state.json 缺少 last_block,禁止自动恢复");
  return state.value("last_block", static_cast<int64_t>(-1));
}

void Database::set_last_block(int64_t block) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  json state = read_state_unlocked();
  state["last_block"] = block;
  write_state_unlocked(state);
}

json Database::load_counts_cache() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  json state = read_state_unlocked();
  if (!state.contains("counts_cache")) {
    return json::object();
  }
  return state["counts_cache"];
}

void Database::save_counts_cache(const json &cache) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  json state = read_state_unlocked();
  state["counts_cache"] = cache;
  write_state_unlocked(state);
}

std::string Database::state_path() const {
  return data_dir_ + "/state.json";
}

json Database::read_state_unlocked() const {
  std::string path = state_path();
  if (!fs::exists(path)) {
    return json::object();
  }
  std::ifstream f(path);
  return json::parse(f);
}

void Database::write_state_unlocked(const json &state) const {
  std::string path = state_path();
  std::string tmp_path = path + ".tmp";
  {
    std::ofstream f(tmp_path, std::ios::binary | std::ios::trunc);
    assert(f.is_open());
    f << state.dump(2);
    f.flush();
    assert(f.good());
  }

  int fd = open(tmp_path.c_str(), O_RDONLY);
  assert(fd >= 0);
  int ret = fsync(fd);
  assert(ret == 0);
  int close_ret = close(fd);
  assert(close_ret == 0);

  ret = std::rename(tmp_path.c_str(), path.c_str());
  assert(ret == 0);

  std::string dir = fs::path(path).parent_path().string();
  if (dir.empty()) {
    dir = ".";
  }
  int dir_fd = open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  assert(dir_fd >= 0);
  ret = fsync(dir_fd);
  assert(ret == 0);
  close_ret = close(dir_fd);
  assert(close_ret == 0);
}

