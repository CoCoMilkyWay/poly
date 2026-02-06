#pragma once

// ============================================================================
// Schema Introspector - 从 The Graph 自动发现 schema 并导出原始数据
// ============================================================================

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

#include "config.hpp"
#include "https_pool.hpp"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace introspector {

// ============================================================================
// 类型工具
// ============================================================================

// 穿透 NON_NULL / LIST
inline json unwrap_type(const json &t) {
  if (t.is_null())
    return json::object();
  std::string kind = t.value("kind", "");
  if (kind == "NON_NULL" || kind == "LIST") {
    return unwrap_type(t.value("ofType", json::object()));
  }
  return t;
}

// 检查类型是否是 [TargetName]
inline bool is_list_of(const json &t, const std::string &target_name) {
  if (t.is_null())
    return false;
  std::string kind = t.value("kind", "");
  if (kind == "LIST") {
    json base = unwrap_type(t.value("ofType", json::object()));
    return base.value("name", "") == target_name;
  }
  if (kind == "NON_NULL") {
    return is_list_of(t.value("ofType", json::object()), target_name);
  }
  return false;
}

// ============================================================================
// EntityInfo - entity 的 schema 信息
// ============================================================================
struct EntityInfo {
  std::string type_name;                // 类型名 (如 EnrichedOrderFilled)
  std::string plural;                   // 复数查询名 (如 enrichedOrderFilleds)
  std::string table_name;               // 表名 (如 enriched_order_filled)
  std::vector<std::string> field_parts; // 字段选择 (如 "id", "maker { id }")
  std::vector<std::string> flat_fields; // 扁平字段名 (用于 CSV header)
};

// ============================================================================
// ExportResult - 导出结果
// ============================================================================
struct ExportResult {
  std::string source;
  std::string entity;
  int rows = 0;
  std::string error;
};

// ============================================================================
// Introspector - 主类
// ============================================================================
class Introspector {
public:
  Introspector(HttpsPool &pool, const Config &config)
      : pool_(pool), config_(config) {}

  // 启动导出(异步)
  // order_desc: true=最新数据(desc), false=最早数据(asc)
  void start(const std::string &export_dir, int limit, bool order_desc = true) {
    results_.clear();
    pending_count_ = 0;
    done_count_ = 0;
    export_dir_ = export_dir;
    limit_ = limit;
    order_desc_ = order_desc;

    fs::create_directories(export_dir_);

    // 计算总任务数
    for (const auto &src : config_.sources) {
      pending_count_ += static_cast<int>(src.entities.size());
    }

    if (pending_count_ == 0) {
      return;
    }

    // 对每个 source 启动 introspection
    for (const auto &src : config_.sources) {
      start_source_introspection(src);
    }
  }

  bool is_done() const { return done_count_ >= pending_count_; }

  const std::vector<ExportResult> &get_results() const { return results_; }

private:
  void start_source_introspection(const SourceConfig &src) {
    std::string target = "/api/subgraphs/id/" + src.subgraph_id;

    // introspection query: 获取所有 query 字段和所有类型
    std::string query = R"({"query":"{ __schema { queryType { fields { name type { name kind ofType { name kind ofType { name kind ofType { name kind } } } } } } types { name fields { name type { name kind ofType { name kind ofType { name kind } } } } } } }"})";

    pool_.async_post(target, query, [this, src, target](std::string body) {
      on_introspection_done(src, target, body);
    });
  }

  void on_introspection_done(const SourceConfig &src, const std::string &target, const std::string &body) {
    if (body.empty()) {
      // introspection 失败，所有 entity 标记失败
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto &entity_name : src.entities) {
        results_.push_back({src.name, entity_name, 0, "introspection failed"});
        ++done_count_;
      }
      return;
    }

    json data;
    try {
      data = json::parse(body);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto &entity_name : src.entities) {
        results_.push_back({src.name, entity_name, 0, "introspection JSON parse failed"});
        ++done_count_;
      }
      return;
    }

    if (data.contains("errors") || !data.contains("data")) {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto &entity_name : src.entities) {
        results_.push_back({src.name, entity_name, 0, "introspection GraphQL error"});
        ++done_count_;
      }
      return;
    }

    // 解析 schema
    auto query_fields = data["data"]["__schema"]["queryType"]["fields"];
    auto schema_types = data["data"]["__schema"]["types"];

    // 构建类型映射
    std::unordered_map<std::string, json> type_map;
    for (const auto &t : schema_types) {
      type_map[t.value("name", "")] = t;
    }

    // 对每个 entity 并行拉取数据
    for (const auto &entity_name : src.entities) {
      EntityInfo info;
      info.type_name = entity_name;
      info.table_name = src.entity_table_map.at(entity_name);

      // 找 plural
      for (const auto &f : query_fields) {
        if (is_list_of(f["type"], entity_name)) {
          info.plural = f.value("name", "");
          break;
        }
      }

      if (info.plural.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back({src.name, entity_name, 0, "no list query exposed"});
        ++done_count_;
        continue;
      }

      // 获取字段
      if (type_map.find(entity_name) == type_map.end()) {
        std::lock_guard<std::mutex> lock(mutex_);
        results_.push_back({src.name, entity_name, 0, "type not found"});
        ++done_count_;
        continue;
      }

      auto fields = type_map[entity_name].value("fields", json::array());
      for (const auto &f : fields) {
        std::string name = f.value("name", "");
        if (name.empty() || name[0] == '_')
          continue;

        json base = unwrap_type(f["type"]);
        std::string kind = base.value("kind", "");

        if (kind == "OBJECT" || kind == "INTERFACE") {
          info.field_parts.push_back(name + " { id }");
        } else {
          info.field_parts.push_back(name);
        }
        info.flat_fields.push_back(name);
      }

      // 发起数据请求
      fetch_entity_data(src, target, info);
    }
  }

  void fetch_entity_data(const SourceConfig &src, const std::string &target, const EntityInfo &info) {
    std::cout << "[Export] " << src.name << "/" << info.type_name << " start" << std::endl;

    // 构建查询
    std::string fields_str;
    for (size_t i = 0; i < info.field_parts.size(); ++i) {
      if (i > 0)
        fields_str += " ";
      fields_str += info.field_parts[i];
    }

    // 先尝试带 orderBy
    std::string order_dir = order_desc_ ? "desc" : "asc";
    std::string query = R"({"query":"{ )" + info.plural +
                        "(first: " + std::to_string(limit_) +
                        ", orderBy: id, orderDirection: " + order_dir + ") { " + fields_str + R"( } }"})";

    pool_.async_post(target, query, [this, src, target, info, fields_str](std::string body) {
      on_data_response(src, target, info, fields_str, body, true);
    });
  }

  void on_data_response(const SourceConfig &src, const std::string &target,
                        const EntityInfo &info, const std::string &fields_str,
                        const std::string &body, bool with_order) {
    if (body.empty()) {
      std::lock_guard<std::mutex> lock(mutex_);
      results_.push_back({src.name, info.type_name, 0, "network error"});
      ++done_count_;
      std::cout << "[Export] " << src.name << "/" << info.type_name << " failed: network error" << std::endl;
      return;
    }

    json data;
    try {
      data = json::parse(body);
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      results_.push_back({src.name, info.type_name, 0, "JSON parse error"});
      ++done_count_;
      std::cout << "[Export] " << src.name << "/" << info.type_name << " failed: JSON parse error" << std::endl;
      return;
    }

    // 如果带 orderBy 失败，fallback 不带
    if (data.contains("errors") && with_order) {
      std::string query = R"({"query":"{ )" + info.plural +
                          "(first: " + std::to_string(limit_) +
                          ") { " + fields_str + R"( } }"})";
      pool_.async_post(target, query, [this, src, target, info, fields_str](std::string body) {
        on_data_response(src, target, info, fields_str, body, false);
      });
      return;
    }

    if (data.contains("errors") || !data.contains("data")) {
      std::string err = data.contains("errors") ? data["errors"].dump() : "no data";
      std::lock_guard<std::mutex> lock(mutex_);
      results_.push_back({src.name, info.type_name, 0, err});
      ++done_count_;
      std::cout << "[Export] " << src.name << "/" << info.type_name << " failed: " << err << std::endl;
      return;
    }

    auto rows = data["data"].value(info.plural, json::array());

    // 写 CSV
    std::string path = export_dir_ + "/" + info.table_name + ".csv";
    std::ofstream ofs(path);
    assert(ofs.is_open());

    // header
    for (size_t i = 0; i < info.flat_fields.size(); ++i) {
      if (i > 0)
        ofs << ",";
      ofs << info.flat_fields[i];
    }
    ofs << "\n";

    // rows
    for (const auto &row : rows) {
      for (size_t i = 0; i < info.flat_fields.size(); ++i) {
        if (i > 0)
          ofs << ",";
        const auto &field = info.flat_fields[i];
        if (row.contains(field)) {
          const auto &v = row[field];
          if (v.is_null()) {
            // empty
          } else if (v.is_object() && v.contains("id")) {
            ofs << escape_csv(v["id"].get<std::string>());
          } else if (v.is_array()) {
            // 序列化数组
            json arr = json::array();
            for (const auto &item : v) {
              if (item.is_object() && item.contains("id")) {
                arr.push_back(item["id"]);
              } else {
                arr.push_back(item);
              }
            }
            ofs << escape_csv(arr.dump());
          } else if (v.is_string()) {
            ofs << escape_csv(v.get<std::string>());
          } else {
            ofs << v.dump();
          }
        }
      }
      ofs << "\n";
    }

    std::lock_guard<std::mutex> lock(mutex_);
    results_.push_back({src.name, info.type_name, static_cast<int>(rows.size()), ""});
    ++done_count_;
    std::cout << "[Export] " << src.name << "/" << info.type_name << " done, " << rows.size() << " rows" << std::endl;
  }

  static std::string escape_csv(const std::string &s) {
    bool need_quote = s.find(',') != std::string::npos ||
                      s.find('"') != std::string::npos ||
                      s.find('\n') != std::string::npos;
    if (!need_quote)
      return s;

    std::string r = "\"";
    for (char c : s) {
      if (c == '"')
        r += "\"\"";
      else
        r += c;
    }
    r += "\"";
    return r;
  }

  HttpsPool &pool_;
  const Config &config_;
  std::string export_dir_;
  int limit_ = 100;
  bool order_desc_ = true;

  std::mutex mutex_;
  std::vector<ExportResult> results_;
  int pending_count_ = 0;
  int done_count_ = 0;
};

} // namespace introspector
