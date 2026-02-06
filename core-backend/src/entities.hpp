#pragma once

// ============================================================================
// Entity 定义
// 每个 entity 包含：GraphQL 字段、DDL、转换函数
// ============================================================================

#include <cassert>
#include <cctype>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

namespace entities {

// ============================================================================
// 工具函数
// ============================================================================

// SQL 字符串转义(不带引号)
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

// SQL 字符串转义(带引号)
inline std::string escape_sql(const std::string &s) {
  return "'" + escape_sql_raw(s) + "'";
}

// JSON 提取：字符串
inline std::string json_str(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_string())
    return escape_sql(j[key].get<std::string>());
  return escape_sql(j[key].dump());
}

// JSON 提取：整数
inline std::string json_int(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_number())
    return std::to_string(j[key].get<int64_t>());
  if (j[key].is_string())
    return j[key].get<std::string>();
  return "NULL";
}

// JSON 提取：浮点
inline std::string json_decimal(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_number())
    return std::to_string(j[key].get<double>());
  if (j[key].is_string())
    return j[key].get<std::string>();
  return "NULL";
}

// JSON 提取：关系字段(兼容 {id:"xxx"} 和直接字符串两种格式)
inline std::string json_ref(const json &j, const char *key) {
  if (!j.contains(key) || j[key].is_null())
    return "NULL";
  if (j[key].is_object() && j[key].contains("id")) {
    return escape_sql(j[key]["id"].get<std::string>());
  }
  if (j[key].is_string()) {
    return escape_sql(j[key].get<std::string>());
  }
  return "NULL";
}

// JSON 提取：数组(序列化为 JSON 字符串)
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
    last_id VARCHAR,
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
  std::string (*to_values)(const json &); // JSON 转 SQL values
};

// ============================================================================
// 估算：单条记录的"结构体大小"(字节) (前端仪表盘需要)
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
// Main Subgraph Entities
// ============================================================================

// ----------------------------------------------------------------------------
// Condition - 条件 (含结算信息)
// CSV: id,oracle,questionId,outcomeSlotCount,resolutionTimestamp,payouts,payoutNumerators,payoutDenominator,fixedProductMarketMakers,resolutionHash
// ----------------------------------------------------------------------------
inline std::string condition_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_str(j, "oracle") + "," +
         json_str(j, "questionId") + "," +
         json_int(j, "outcomeSlotCount") + "," +
         json_int(j, "resolutionTimestamp") + "," +
         json_array(j, "payouts") + "," +
         json_array(j, "payoutNumerators") + "," +
         json_int(j, "payoutDenominator") + "," +
         json_str(j, "resolutionHash");
}

inline const EntityDef Condition = {
    .name = "Condition",
    .plural = "conditions",
    .table = "condition",
    .fields = "id oracle questionId outcomeSlotCount resolutionTimestamp payouts payoutNumerators payoutDenominator resolutionHash",
    .ddl = R"(CREATE TABLE IF NOT EXISTS condition (
        id VARCHAR PRIMARY KEY,
        oracle VARCHAR,
        questionId VARCHAR,
        outcomeSlotCount INT,
        resolutionTimestamp BIGINT,
        payouts VARCHAR,
        payoutNumerators VARCHAR,
        payoutDenominator VARCHAR,
        resolutionHash VARCHAR
    ))",
    .columns = "id, oracle, questionId, outcomeSlotCount, resolutionTimestamp, payouts, payoutNumerators, payoutDenominator, resolutionHash",
    .to_values = condition_to_values};

// ----------------------------------------------------------------------------
// Merge - 铸造 (用 collateral 铸造 condition 的所有 outcome tokens)
// CSV: id,timestamp,stakeholder,collateralToken,parentCollectionId,condition,partition,amount
// stakeholder/collateralToken/condition 是 OBJECT 类型，需要 { id }
// ----------------------------------------------------------------------------
inline std::string merge_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_ref(j, "stakeholder") + "," +
         json_ref(j, "collateralToken") + "," +
         json_str(j, "parentCollectionId") + "," +
         json_ref(j, "condition") + "," +
         json_array(j, "partition") + "," +
         json_int(j, "amount");
}

inline const EntityDef Merge = {
    .name = "Merge",
    .plural = "merges",
    .table = "merge",
    .fields = "id timestamp stakeholder { id } collateralToken { id } parentCollectionId condition { id } partition amount",
    .ddl = R"(CREATE TABLE IF NOT EXISTS merge (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT,
        stakeholder VARCHAR,
        collateralToken VARCHAR,
        parentCollectionId VARCHAR,
        condition VARCHAR,
        partition VARCHAR,
        amount VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_merge_ts ON merge(timestamp);
    CREATE INDEX IF NOT EXISTS idx_merge_user ON merge(stakeholder))",
    .columns = "id, timestamp, stakeholder, collateralToken, parentCollectionId, condition, partition, amount",
    .to_values = merge_to_values};

// ----------------------------------------------------------------------------
// Redemption - 赎回 (用 outcome tokens 换回 collateral)
// CSV: id,timestamp,redeemer,collateralToken,parentCollectionId,condition,indexSets,payout
// redeemer/collateralToken/condition 是 OBJECT 类型，需要 { id }
// ----------------------------------------------------------------------------
inline std::string redemption_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_int(j, "timestamp") + "," +
         json_ref(j, "redeemer") + "," +
         json_ref(j, "collateralToken") + "," +
         json_str(j, "parentCollectionId") + "," +
         json_ref(j, "condition") + "," +
         json_array(j, "indexSets") + "," +
         json_int(j, "payout");
}

inline const EntityDef Redemption = {
    .name = "Redemption",
    .plural = "redemptions",
    .table = "redemption",
    .fields = "id timestamp redeemer { id } collateralToken { id } parentCollectionId condition { id } indexSets payout",
    .ddl = R"(CREATE TABLE IF NOT EXISTS redemption (
        id VARCHAR PRIMARY KEY,
        timestamp BIGINT,
        redeemer VARCHAR,
        collateralToken VARCHAR,
        parentCollectionId VARCHAR,
        condition VARCHAR,
        indexSets VARCHAR,
        payout VARCHAR
    );
    CREATE INDEX IF NOT EXISTS idx_redemption_ts ON redemption(timestamp);
    CREATE INDEX IF NOT EXISTS idx_redemption_user ON redemption(redeemer))",
    .columns = "id, timestamp, redeemer, collateralToken, parentCollectionId, condition, indexSets, payout",
    .to_values = redemption_to_values};

// ----------------------------------------------------------------------------
// EnrichedOrderFilled - 订单成交 (orderbook 交易)
// CSV: id,transactionHash,timestamp,maker,taker,orderHash,market,side,size,price
// market 就是 tokenId (positionId)
// ----------------------------------------------------------------------------
inline std::string enriched_order_filled_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_str(j, "transactionHash") + "," +
         json_int(j, "timestamp") + "," +
         json_ref(j, "maker") + "," +
         json_ref(j, "taker") + "," +
         json_str(j, "orderHash") + "," +
         json_str(j, "market") + "," +
         json_str(j, "side") + "," +
         json_int(j, "size") + "," +
         json_decimal(j, "price");
}

inline const EntityDef EnrichedOrderFilled = {
    .name = "EnrichedOrderFilled",
    .plural = "enrichedOrderFilleds",
    .table = "enriched_order_filled",
    .fields = "id transactionHash timestamp maker { id } taker { id } orderHash market side size price",
    .ddl = R"(CREATE TABLE IF NOT EXISTS enriched_order_filled (
        id VARCHAR PRIMARY KEY,
        transactionHash VARCHAR,
        timestamp BIGINT,
        maker VARCHAR,
        taker VARCHAR,
        orderHash VARCHAR,
        market VARCHAR,
        side VARCHAR,
        size VARCHAR,
        price DOUBLE
    );
    CREATE INDEX IF NOT EXISTS idx_eof_ts ON enriched_order_filled(timestamp);
    CREATE INDEX IF NOT EXISTS idx_eof_maker ON enriched_order_filled(maker);
    CREATE INDEX IF NOT EXISTS idx_eof_taker ON enriched_order_filled(taker);
    CREATE INDEX IF NOT EXISTS idx_eof_market ON enriched_order_filled(market))",
    .columns = "id, transactionHash, timestamp, maker, taker, orderHash, market, side, size, price",
    .to_values = enriched_order_filled_to_values};

// ============================================================================
// PnL Subgraph Entities
// ============================================================================

// ----------------------------------------------------------------------------
// PnlCondition - tokenId ↔ conditionId 映射
// CSV: id,positionIds,payoutNumerators,payoutDenominator
// positionIds[0] = YES token, positionIds[1] = NO token
// ----------------------------------------------------------------------------
inline std::string pnl_condition_to_values(const json &j) {
  return json_str(j, "id") + "," +
         json_array(j, "positionIds") + "," +
         json_array(j, "payoutNumerators") + "," +
         json_int(j, "payoutDenominator");
}

inline const EntityDef PnlCondition = {
    .name = "Condition",
    .plural = "conditions",
    .table = "pnl_condition",
    .fields = "id positionIds payoutNumerators payoutDenominator",
    .ddl = R"(CREATE TABLE IF NOT EXISTS pnl_condition (
        id VARCHAR PRIMARY KEY,
        positionIds VARCHAR,
        payoutNumerators VARCHAR,
        payoutDenominator VARCHAR
    ))",
    .columns = "id, positionIds, payoutNumerators, payoutDenominator",
    .to_values = pnl_condition_to_values};

// ============================================================================
// Entity 注册表
// ============================================================================

inline const EntityDef *MAIN_ENTITIES[] = {
    &Condition, &Merge, &Redemption, &EnrichedOrderFilled};
inline constexpr size_t MAIN_ENTITY_COUNT = sizeof(MAIN_ENTITIES) / sizeof(MAIN_ENTITIES[0]);

inline const EntityDef *PNL_ENTITIES[] = {&PnlCondition};
inline constexpr size_t PNL_ENTITY_COUNT = sizeof(PNL_ENTITIES) / sizeof(PNL_ENTITIES[0]);

// 查找 entity
inline const EntityDef *find_entity(const char *name, const EntityDef *const *list, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (std::string(list[i]->name) == name) {
      return list[i];
    }
  }
  return nullptr;
}

} // namespace entities
