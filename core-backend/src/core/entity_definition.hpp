#pragma once

// ============================================================================
// Entity 定义
// 每个 entity 包含：GraphQL 字段、DDL、转换函数、同步模式
// ============================================================================

#include <cassert>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace entities {

// ============================================================================
// 同步模式
// ============================================================================
enum class SyncMode {
  TIMESTAMP,     // orderBy: timestamp, where: {timestamp_gte}, + skip
  RESOLUTION_TS, // orderBy: resolutionTimestamp, where: {resolutionTimestamp_gte}, + skip
  ID,            // orderBy: id, where: {id_gt} (no skip)
};

// ============================================================================
// 工具函数
// ============================================================================

inline std::string escape_sql_raw(const std::string &s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s) {
    if (c == '\'')
      r += "''";
    else
      r += c;
  }
  return r;
}

inline std::string escape_sql(const std::string &s) {
  return "'" + escape_sql_raw(s) + "'";
}

inline std::string json_str(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_string())
    return escape_sql(j[key].get<std::string>());
  return escape_sql(j[key].dump());
}

inline std::string json_int(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_number())
    return std::to_string(j[key].get<int64_t>());
  if (j[key].is_string())
    return j[key].get<std::string>();
  return "NULL";
}

inline std::string json_decimal(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_number())
    return std::to_string(j[key].get<double>());
  if (j[key].is_string())
    return j[key].get<std::string>();
  return "NULL";
}

inline std::string json_ref(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_object() && j[key].contains("id"))
    return escape_sql(j[key]["id"].get<std::string>());
  if (j[key].is_string())
    return escape_sql(j[key].get<std::string>());
  return "NULL";
}

inline std::string json_array(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  return escape_sql(j[key].dump());
}

// ============================================================================
// 基础设施：同步状态表
// ============================================================================

inline const char *SYNC_STATE_DDL = R"(
CREATE TABLE IF NOT EXISTS sync_state (
    source VARCHAR NOT NULL,
    entity VARCHAR NOT NULL,
    cursor_value VARCHAR,
    cursor_skip INT DEFAULT 0,
    last_sync_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (source, entity)
))";

inline const char *ENTITY_STATS_META_DDL = R"(
CREATE TABLE IF NOT EXISTS entity_stats_meta (
    source VARCHAR NOT NULL,
    entity VARCHAR NOT NULL,
    total_requests BIGINT DEFAULT 0,
    success_requests BIGINT DEFAULT 0,
    fail_network BIGINT DEFAULT 0,
    fail_json BIGINT DEFAULT 0,
    fail_graphql BIGINT DEFAULT 0,
    fail_format BIGINT DEFAULT 0,
    total_rows_synced BIGINT DEFAULT 0,
    total_api_time_ms BIGINT DEFAULT 0,
    success_rate DOUBLE DEFAULT 100.0,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (source, entity)
))";

inline const char *INDEXER_FAIL_META_DDL = R"(
CREATE TABLE IF NOT EXISTS indexer_fail_meta (
    source VARCHAR NOT NULL,
    entity VARCHAR NOT NULL,
    indexer VARCHAR NOT NULL,
    fail_requests BIGINT DEFAULT 0,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (source, entity, indexer)
))";

// ============================================================================
// Entity 定义结构
// ============================================================================

struct EntityDef {
  const char *name;                       // Entity 名称(GraphQL 单数)
  const char *plural;                     // GraphQL 复数形式
  const char *table;                      // 数据库表名
  const char *fields;                     // GraphQL 查询字段
  const char *ddl;                        // CREATE TABLE 语句
  const char *columns;                    // INSERT 列名
  SyncMode sync_mode;                     // 同步模式
  const char *order_field;                // orderBy 字段名
  const char *where_field;                // where 过滤字段名
  std::string (*to_values)(const json &); // JSON 转 SQL values
};

// ============================================================================
// 估算：单条记录的"结构体大小"(字节)
// ============================================================================
inline int64_t estimate_row_size_bytes(const EntityDef *e) {
  assert(e != nullptr);
  std::string ddl = e->ddl ? std::string(e->ddl) : std::string();
  assert(!ddl.empty());

  auto lp = ddl.find('(');
  auto rp = ddl.find(')', lp == std::string::npos ? 0 : lp + 1);
  assert(lp != std::string::npos && rp != std::string::npos && rp > lp);

  std::string cols = ddl.substr(lp + 1, rp - lp - 1);

  auto trim = [](std::string s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
      ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n' || s[e - 1] == ','))
      --e;
    return s.substr(b, e - b);
  };

  auto upper = [](std::string s) {
    for (auto &c : s)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  };

  auto type_token = [&](const std::string &line, std::string *out_col) -> std::string {
    size_t sp1 = line.find_first_of(" \t");
    if (sp1 == std::string::npos)
      return "";
    std::string col = trim(line.substr(0, sp1));
    size_t sp2 = line.find_first_not_of(" \t", sp1);
    if (sp2 == std::string::npos)
      return "";
    size_t sp3 = line.find_first_of(" \t", sp2);
    std::string type = (sp3 == std::string::npos) ? line.substr(sp2) : line.substr(sp2, sp3 - sp2);
    type = upper(trim(type));
    auto p = type.find('(');
    if (p != std::string::npos)
      type = type.substr(0, p);
    if (out_col)
      *out_col = upper(col);
    return type;
  };

  auto varchar_guess = [&](const std::string &col_upper) -> int64_t {
    if (col_upper == "ID" || col_upper.ends_with("_ID"))
      return 66;
    if (col_upper.find("HASH") != std::string::npos)
      return 66;
    if (col_upper.find("ADDR") != std::string::npos || col_upper.find("ADDRESS") != std::string::npos)
      return 42;
    return 32;
  };

  auto fixed_size = [&](const std::string &type_upper, const std::string &col_upper) -> int64_t {
    struct Pair {
      const char *t;
      int64_t sz;
    };
    static constexpr Pair kFixed[] = {
        {"INT", 4},
        {"INTEGER", 4},
        {"BIGINT", 8},
        {"DOUBLE", 8},
        {"FLOAT", 8},
        {"BOOLEAN", 1},
        {"BOOL", 1},
        {"TIMESTAMP", 8},
    };
    for (const auto &p : kFixed) {
      if (type_upper == p.t)
        return p.sz;
    }
    if (type_upper == "VARCHAR" || type_upper == "TEXT" || type_upper == "STRING")
      return varchar_guess(col_upper);
    return 16;
  };

  int64_t total = 8;
  size_t pos = 0;
  while (pos < cols.size()) {
    size_t nl = cols.find('\n', pos);
    std::string line = (nl == std::string::npos) ? cols.substr(pos) : cols.substr(pos, nl - pos);
    pos = (nl == std::string::npos) ? cols.size() : nl + 1;

    line = trim(line);
    if (line.empty())
      continue;

    std::string u = upper(line);
    if (u.starts_with("PRIMARY KEY") || u.starts_with("UNIQUE") || u.starts_with("CONSTRAINT"))
      continue;

    std::string col_upper;
    std::string type = type_token(line, &col_upper);
    if (type.empty())
      continue;
    total += fixed_size(type, col_upper);
  }

  if (total < 16)
    total = 16;
  return total;
}

// ============================================================================
// Polymarket Entities
// ============================================================================

// Condition - 条件 (含结算信息)
// positionIds 不从 GraphQL 拉取, 由大 sync 从 PnL 合入
inline std::string condition_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_str(j, "questionId") + "," +
         json_str(j, "oracle") + "," +
         json_int(j, "outcomeSlotCount") + "," +
         json_int(j, "resolutionTimestamp") + "," +
         json_array(j, "payoutNumerators") + "," +
         json_int(j, "payoutDenominator");
}

inline const EntityDef Condition = {
    .name = "Condition",
    .plural = "conditions",
    .table = "condition",
    .fields = "id questionId oracle outcomeSlotCount resolutionTimestamp payoutNumerators payoutDenominator",
    .ddl = R"(CREATE TABLE IF NOT EXISTS condition (
        id VARCHAR PRIMARY KEY,
        questionId VARCHAR NOT NULL,
        oracle VARCHAR NOT NULL,
        outcomeSlotCount INT NOT NULL,
        resolutionTimestamp BIGINT,
        payoutNumerators VARCHAR,
        payoutDenominator BIGINT,
        positionIds VARCHAR
    ))",
    .columns = "id, questionId, oracle, outcomeSlotCount, resolutionTimestamp, payoutNumerators, payoutDenominator",
    .sync_mode = SyncMode::RESOLUTION_TS,
    .order_field = "resolutionTimestamp",
    .where_field = "resolutionTimestamp_gte",
    .to_values = condition_to_values};

// EnrichedOrderFilled - 订单成交
inline std::string enriched_order_filled_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_ref(j, "maker") + "," +
         json_ref(j, "taker") + "," +
         json_ref(j, "market") + "," +
         json_str(j, "side") + "," +
         json_int(j, "size") + "," +
         json_decimal(j, "price");
}

inline const EntityDef EnrichedOrderFilled = {
    .name = "EnrichedOrderFilled",
    .plural = "enrichedOrderFilleds",
    .table = "enriched_order_filled",
    .fields = "id timestamp maker { id } taker { id } market { id } side size price",
    .ddl = R"(CREATE TABLE IF NOT EXISTS enriched_order_filled (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT NOT NULL,
        maker VARCHAR NOT NULL,
        taker VARCHAR NOT NULL,
        market VARCHAR NOT NULL,
        side VARCHAR NOT NULL,
        size VARCHAR NOT NULL,
        price DOUBLE NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_eof_ts ON enriched_order_filled(timestamp))",
    .columns = "id, timestamp, maker, taker, market, side, size, price",
    .sync_mode = SyncMode::TIMESTAMP,
    .order_field = "timestamp",
    .where_field = "timestamp_gte",
    .to_values = enriched_order_filled_to_values};

// ============================================================================
// Activity Polygon Entities (flat fields, no { id } expansion)
// ============================================================================

// Split - 铸造 (USDC → YES + NO)
inline std::string split_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_str(j, "stakeholder") + "," +
         json_str(j, "condition") + "," +
         json_int(j, "amount");
}

inline const EntityDef Split = {
    .name = "Split",
    .plural = "splits",
    .table = "split",
    .fields = "id timestamp stakeholder condition amount",
    .ddl = R"(CREATE TABLE IF NOT EXISTS split (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT NOT NULL,
        stakeholder VARCHAR NOT NULL,
        condition VARCHAR NOT NULL,
        amount VARCHAR NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_split_ts ON split(timestamp))",
    .columns = "id, timestamp, stakeholder, condition, amount",
    .sync_mode = SyncMode::TIMESTAMP,
    .order_field = "timestamp",
    .where_field = "timestamp_gte",
    .to_values = split_to_values};

// Merge - 销毁 (YES + NO → USDC)
inline std::string merge_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_str(j, "stakeholder") + "," +
         json_str(j, "condition") + "," +
         json_int(j, "amount");
}

inline const EntityDef Merge = {
    .name = "Merge",
    .plural = "merges",
    .table = "merge",
    .fields = "id timestamp stakeholder condition amount",
    .ddl = R"(CREATE TABLE IF NOT EXISTS merge (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT NOT NULL,
        stakeholder VARCHAR NOT NULL,
        condition VARCHAR NOT NULL,
        amount VARCHAR NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_merge_ts ON merge(timestamp))",
    .columns = "id, timestamp, stakeholder, condition, amount",
    .sync_mode = SyncMode::TIMESTAMP,
    .order_field = "timestamp",
    .where_field = "timestamp_gte",
    .to_values = merge_to_values};

// Redemption - 赎回 (tokens → USDC, 市场结算后)
inline std::string redemption_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_str(j, "redeemer") + "," +
         json_str(j, "condition") + "," +
         json_array(j, "indexSets") + "," +
         json_int(j, "payout");
}

inline const EntityDef Redemption = {
    .name = "Redemption",
    .plural = "redemptions",
    .table = "redemption",
    .fields = "id timestamp redeemer condition indexSets payout",
    .ddl = R"(CREATE TABLE IF NOT EXISTS redemption (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT NOT NULL,
        redeemer VARCHAR NOT NULL,
        condition VARCHAR NOT NULL,
        indexSets VARCHAR NOT NULL,
        payout VARCHAR NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_redemption_ts ON redemption(timestamp))",
    .columns = "id, timestamp, redeemer, condition, indexSets, payout",
    .sync_mode = SyncMode::TIMESTAMP,
    .order_field = "timestamp",
    .where_field = "timestamp_gte",
    .to_values = redemption_to_values};

// ============================================================================
// PnL Subgraph Entities (临时, 大 sync 后合入 condition 并删除)
// ============================================================================

inline std::string pnl_condition_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_array(j, "positionIds");
}

inline const EntityDef PnlCondition = {
    .name = "Condition",
    .plural = "conditions",
    .table = "pnl_condition",
    .fields = "id positionIds",
    .ddl = R"(CREATE TABLE IF NOT EXISTS pnl_condition (
        id VARCHAR PRIMARY KEY,
        positionIds VARCHAR
    ))",
    .columns = "id, positionIds",
    .sync_mode = SyncMode::ID,
    .order_field = "id",
    .where_field = "id_gt",
    .to_values = pnl_condition_to_values};

// ============================================================================
// Entity 注册表 (按 subgraph 分组)
// ============================================================================

inline const EntityDef *POLYMARKET_ENTITIES[] = {
    &Condition, &EnrichedOrderFilled};
inline constexpr size_t POLYMARKET_ENTITY_COUNT = sizeof(POLYMARKET_ENTITIES) / sizeof(POLYMARKET_ENTITIES[0]);

inline const EntityDef *ACTIVITY_ENTITIES[] = {
    &Split, &Merge, &Redemption};
inline constexpr size_t ACTIVITY_ENTITY_COUNT = sizeof(ACTIVITY_ENTITIES) / sizeof(ACTIVITY_ENTITIES[0]);

inline const EntityDef *PNL_ENTITIES[] = {&PnlCondition};
inline constexpr size_t PNL_ENTITY_COUNT = sizeof(PNL_ENTITIES) / sizeof(PNL_ENTITIES[0]);

// 所有 entity (用于 stats/export)
inline const EntityDef *ALL_ENTITIES[] = {
    &Condition, &EnrichedOrderFilled, &Split, &Merge, &Redemption, &PnlCondition};
inline constexpr size_t ALL_ENTITY_COUNT = sizeof(ALL_ENTITIES) / sizeof(ALL_ENTITIES[0]);

// 查找 entity
inline const EntityDef *find_entity(const char *name, const EntityDef *const *list, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (std::string(list[i]->name) == name)
      return list[i];
  }
  return nullptr;
}

inline const EntityDef *find_entity_by_table(const char *table) {
  for (size_t i = 0; i < ALL_ENTITY_COUNT; ++i) {
    if (std::string(ALL_ENTITIES[i]->table) == table)
      return ALL_ENTITIES[i];
  }
  return nullptr;
}

} // namespace entities
