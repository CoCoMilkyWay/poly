#include "database.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
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
} // namespace

Database::Database(const std::string &path) : db_path_(path) {
  auto parent = fs::path(path).parent_path();
  data_dir_ = parent.empty() ? "." : parent.string();
  fs::create_directories(data_dir_);
  duckdb::DBConfig config;
  config.SetOption("checkpoint_threshold", duckdb::Value("256MB"));
  config.SetOption("wal_autocheckpoint", duckdb::Value("256MB"));
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

int64_t Database::get_last_block() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  json state = read_state_unlocked();
  if (state.contains("last_block")) {
    return state.value("last_block", static_cast<int64_t>(-1));
  }
  return recover_last_block_from_feather();
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

int64_t Database::recover_last_block_from_feather() {
  static const char *tables[] = {
      "transfer", "condition_preparation", "condition_resolution",
      "split", "merge", "redemption", "fpmm", "fpmm_trade", "fpmm_funding",
      "order_filled", "token_map", "neg_risk_market", "neg_risk_question", "convert"};
  int64_t max_block = -1;
  for (const char *table : tables) {
    std::vector<FeatherChunk> chunks = list_chunks(table);
    for (const auto &chunk : chunks) {
      max_block = std::max(max_block, chunk.end_block);
    }
  }
  return max_block;
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
