#include "stage0_sync.hpp"

#include <optional>
#include <vector>

namespace stage0 {
namespace {

uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9') {
    return static_cast<uint8_t>(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return static_cast<uint8_t>(c - 'a' + 10);
  }
  assert(false && "invalid hex nibble");
  return 0;
}

std::string hex_to_blob_exact(const std::string &hex, size_t expect_bytes) {
  assert(hex.starts_with("0x"));
  std::string h = hex.substr(2);
  assert(h.size() == expect_bytes * 2);
  std::string out;
  out.reserve(expect_bytes);
  for (size_t i = 0; i < h.size(); i += 2) {
    uint8_t v = static_cast<uint8_t>((hex_nibble(h[i]) << 4) | hex_nibble(h[i + 1]));
    out.push_back(static_cast<char>(v));
  }
  assert(out.size() == expect_bytes);
  return out;
}

duckdb::Value make_blob_value(const std::string &blob) {
  return duckdb::Value::BLOB(reinterpret_cast<duckdb::const_data_ptr_t>(blob.data()), blob.size());
}

bool has_non_null(const json &obj, const char *key) {
  return obj.contains(key) && !obj.at(key).is_null();
}

std::string require_string_field(const json &obj, const char *key) {
  assert(has_non_null(obj, key));
  const auto &v = obj.at(key);
  assert(v.is_string());
  return v.get<std::string>();
}

int64_t parse_i64_value(const json &v) {
  if (v.is_number_integer()) {
    return v.get<int64_t>();
  }
  assert(false && "invalid int64 field");
  return 0;
}

double parse_double_value(const json &v) {
  if (v.is_number()) {
    return v.get<double>();
  }
  assert(false && "invalid double field");
  return 0.0;
}

std::optional<std::string> get_opt_string_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  const auto &v = obj.at(key);
  assert(v.is_string());
  return v.get<std::string>();
}

std::optional<int64_t> get_opt_i64_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  return parse_i64_value(obj.at(key));
}

std::optional<double> get_opt_double_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  return parse_double_value(obj.at(key));
}

std::optional<bool> get_opt_bool_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return std::nullopt;
  }
  const auto &v = obj.at(key);
  assert(v.is_boolean());
  return v.get<bool>();
}

std::optional<duckdb::timestamp_t> get_opt_timestamp_field(const json &obj, const char *key) {
  auto s = get_opt_string_field(obj, key);
  if (!s.has_value()) {
    return std::nullopt;
  }
  return duckdb::Timestamp::FromString(*s, true);
}

std::optional<duckdb::date_t> get_opt_date_field(const json &obj, const char *key) {
  auto s = get_opt_string_field(obj, key);
  if (!s.has_value()) {
    return std::nullopt;
  }
  return duckdb::Date::FromString(*s);
}

std::optional<std::string> get_opt_blob_field(const json &obj, const char *key, size_t expect_bytes) {
  auto s = get_opt_string_field(obj, key);
  if (!s.has_value()) {
    return std::nullopt;
  }
  return hex_to_blob_exact(*s, expect_bytes);
}

json get_array_field(const json &obj, const char *key) {
  if (!has_non_null(obj, key)) {
    return json::array();
  }
  const auto &v = obj.at(key);
  assert(v.is_array());
  return v;
}

void push_opt_i64(std::vector<duckdb::Value> &dst, const std::optional<int64_t> &v) {
  if (v.has_value()) {
    dst.emplace_back(*v);
  } else {
    dst.emplace_back();
  }
}

void push_opt_bool(std::vector<duckdb::Value> &dst, const std::optional<bool> &v) {
  if (v.has_value()) {
    dst.emplace_back(*v);
  } else {
    dst.emplace_back();
  }
}

void push_opt_string(std::vector<duckdb::Value> &dst, const std::optional<std::string> &v) {
  if (v.has_value()) {
    dst.emplace_back(*v);
  } else {
    dst.emplace_back();
  }
}

void push_opt_timestamp(std::vector<duckdb::Value> &dst, const std::optional<duckdb::timestamp_t> &v) {
  if (v.has_value()) {
    dst.emplace_back(duckdb::Value::TIMESTAMP(*v));
  } else {
    dst.emplace_back();
  }
}

void push_opt_date(std::vector<duckdb::Value> &dst, const std::optional<duckdb::date_t> &v) {
  if (v.has_value()) {
    dst.emplace_back(duckdb::Value::DATE(*v));
  } else {
    dst.emplace_back();
  }
}

void push_opt_blob(std::vector<duckdb::Value> &dst, const std::optional<std::string> &v) {
  if (v.has_value()) {
    dst.push_back(make_blob_value(*v));
  } else {
    dst.emplace_back();
  }
}

template <typename T>
void append_opt_scalar(duckdb::Appender &ap, const std::optional<T> &v) {
  if (v.has_value()) {
    ap.Append(*v);
  } else {
    ap.Append(duckdb::Value());
  }
}

void append_opt_string(duckdb::Appender &ap, const std::optional<std::string> &v) {
  if (v.has_value()) {
    ap.Append(duckdb::Value(*v));
  } else {
    ap.Append(duckdb::Value());
  }
}

void append_opt_timestamp(duckdb::Appender &ap, const std::optional<duckdb::timestamp_t> &v) {
  if (v.has_value()) {
    ap.Append(duckdb::Value::TIMESTAMP(*v));
  } else {
    ap.Append(duckdb::Value());
  }
}

void append_opt_blob(duckdb::Appender &ap, const std::optional<std::string> &v) {
  if (v.has_value()) {
    ap.Append(make_blob_value(*v));
  } else {
    ap.Append(duckdb::Value());
  }
}

duckdb::Value make_list_value(const duckdb::LogicalType &child_type, std::vector<duckdb::Value> values) {
  return duckdb::Value::LIST(child_type, std::move(values));
}

} // namespace

void StageSync::persist_results_in_txn(duckdb::Appender &ap, const std::vector<FetchResult> &rows, int64_t now_ms) {
  for (const auto &row : rows) {
    const json &m = row.market;
    assert(m.is_object());

    auto market_question_id = get_opt_blob_field(m, "questionID", 32);
    if (market_question_id.has_value()) {
      assert(*market_question_id == row.seed.question_blob);
    }
    auto market_condition_id = get_opt_blob_field(m, "conditionId", 32);
    if (market_condition_id.has_value()) {
      assert(*market_condition_id == row.seed.condition_blob);
    }

    int64_t market_id = parse_i64_value(m.at("id"));
    std::string market_slug = require_string_field(m, "slug");
    std::string market_question = require_string_field(m, "question");
    auto market_description = get_opt_string_field(m, "description");
    auto market_start_date = get_opt_timestamp_field(m, "startDate");
    auto market_end_date = get_opt_timestamp_field(m, "endDate");
    auto market_created_at = get_opt_timestamp_field(m, "createdAt");
    auto market_image = get_opt_string_field(m, "image");
    auto market_icon = get_opt_string_field(m, "icon");
    auto market_submitted_by = get_opt_blob_field(m, "submitted_by", 20);
    auto market_resolved_by = get_opt_blob_field(m, "resolvedBy", 20);
    auto market_restricted = get_opt_bool_field(m, "restricted");
    auto market_neg_risk = get_opt_bool_field(m, "negRisk");
    assert(market_neg_risk.has_value());
    auto market_neg_risk_request_id = get_opt_string_field(m, "negRiskRequestID");
    auto market_cyom = get_opt_bool_field(m, "cyom");
    auto market_group_item_title = get_opt_string_field(m, "groupItemTitle");
    auto market_group_item_threshold = get_opt_string_field(m, "groupItemThreshold");
    auto market_enable_order_book = get_opt_bool_field(m, "enableOrderBook");
    auto market_order_min_size = get_opt_double_field(m, "orderMinSize");
    auto market_order_min_tick = get_opt_double_field(m, "orderPriceMinTickSize");
    auto market_clear_book_on_start = get_opt_bool_field(m, "clearBookOnStart");
    auto market_manual_activation = get_opt_bool_field(m, "manualActivation");
    auto market_automatically_active = get_opt_bool_field(m, "automaticallyActive");
    auto market_uma_bond = get_opt_string_field(m, "umaBond");
    auto market_uma_reward = get_opt_string_field(m, "umaReward");
    auto market_rewards_min_size = get_opt_double_field(m, "rewardsMinSize");
    auto market_rewards_max_spread = get_opt_double_field(m, "rewardsMaxSpread");
    auto market_holding_rewards_enable = get_opt_bool_field(m, "holdingRewardsEnabled");
    auto market_rfq_enabled = get_opt_bool_field(m, "rfqEnabled");
    auto market_fees_enabled = get_opt_bool_field(m, "feesEnabled");
    auto market_fee_type = get_opt_string_field(m, "feeType");
    auto market_series_color = get_opt_string_field(m, "seriesColor");
    auto market_show_gmp_series = get_opt_bool_field(m, "showGmpSeries");
    auto market_show_gmp_outcome = get_opt_bool_field(m, "showGmpOutcome");

    json events = get_array_field(m, "events");
    std::vector<duckdb::Value> event_ids;
    std::vector<duckdb::Value> event_tickers;
    std::vector<duckdb::Value> event_slugs;
    std::vector<duckdb::Value> event_titles;
    std::vector<duckdb::Value> event_descriptions;
    std::vector<duckdb::Value> event_resolution_sources;
    std::vector<duckdb::Value> event_start_dates;
    std::vector<duckdb::Value> event_creation_dates;
    std::vector<duckdb::Value> event_end_dates;
    std::vector<duckdb::Value> event_created_ats;
    std::vector<duckdb::Value> event_images;
    std::vector<duckdb::Value> event_icons;
    std::vector<duckdb::Value> event_start_times;
    std::vector<duckdb::Value> event_gmp_chart_modes;
    std::vector<duckdb::Value> event_enable_order_books;
    std::vector<duckdb::Value> event_neg_risks;
    std::vector<duckdb::Value> event_enable_neg_risks;
    std::vector<duckdb::Value> event_show_all_outcomes;
    std::vector<duckdb::Value> event_show_market_images;
    std::vector<duckdb::Value> event_auto_resolveds;
    std::vector<duckdb::Value> event_auto_actives;
    std::vector<duckdb::Value> event_cyoms;
    std::vector<duckdb::Value> event_requires_translations;
    size_t events_size = events.size();
    event_ids.reserve(events_size);
    event_tickers.reserve(events_size);
    event_slugs.reserve(events_size);
    event_titles.reserve(events_size);
    event_descriptions.reserve(events_size);
    event_resolution_sources.reserve(events_size);
    event_start_dates.reserve(events_size);
    event_creation_dates.reserve(events_size);
    event_end_dates.reserve(events_size);
    event_created_ats.reserve(events_size);
    event_images.reserve(events_size);
    event_icons.reserve(events_size);
    event_start_times.reserve(events_size);
    event_gmp_chart_modes.reserve(events_size);
    event_enable_order_books.reserve(events_size);
    event_neg_risks.reserve(events_size);
    event_enable_neg_risks.reserve(events_size);
    event_show_all_outcomes.reserve(events_size);
    event_show_market_images.reserve(events_size);
    event_auto_resolveds.reserve(events_size);
    event_auto_actives.reserve(events_size);
    event_cyoms.reserve(events_size);
    event_requires_translations.reserve(events_size);
    for (const auto &e : events) {
      assert(e.is_object());
      push_opt_i64(event_ids, get_opt_i64_field(e, "id"));
      push_opt_string(event_tickers, get_opt_string_field(e, "ticker"));
      push_opt_string(event_slugs, get_opt_string_field(e, "slug"));
      push_opt_string(event_titles, get_opt_string_field(e, "title"));
      push_opt_string(event_descriptions, get_opt_string_field(e, "description"));
      push_opt_string(event_resolution_sources, get_opt_string_field(e, "resolutionSource"));
      push_opt_timestamp(event_start_dates, get_opt_timestamp_field(e, "startDate"));
      push_opt_timestamp(event_creation_dates, get_opt_timestamp_field(e, "creationDate"));
      push_opt_timestamp(event_end_dates, get_opt_timestamp_field(e, "endDate"));
      push_opt_timestamp(event_created_ats, get_opt_timestamp_field(e, "createdAt"));
      push_opt_string(event_images, get_opt_string_field(e, "image"));
      push_opt_string(event_icons, get_opt_string_field(e, "icon"));
      push_opt_timestamp(event_start_times, get_opt_timestamp_field(e, "startTime"));
      push_opt_string(event_gmp_chart_modes, get_opt_string_field(e, "gmpChartMode"));
      push_opt_bool(event_enable_order_books, get_opt_bool_field(e, "enableOrderBook"));
      push_opt_bool(event_neg_risks, get_opt_bool_field(e, "negRisk"));
      push_opt_bool(event_enable_neg_risks, get_opt_bool_field(e, "enableNegRisk"));
      push_opt_bool(event_show_all_outcomes, get_opt_bool_field(e, "showAllOutcomes"));
      push_opt_bool(event_show_market_images, get_opt_bool_field(e, "showMarketImages"));
      push_opt_bool(event_auto_resolveds, get_opt_bool_field(e, "automaticallyResolved"));
      push_opt_bool(event_auto_actives, get_opt_bool_field(e, "automaticallyActive"));
      push_opt_bool(event_cyoms, get_opt_bool_field(e, "cyom"));
      push_opt_bool(event_requires_translations, get_opt_bool_field(e, "requiresTranslation"));
    }

    json tags = get_array_field(m, "tags");
    std::vector<duckdb::Value> tag_ids;
    std::vector<duckdb::Value> tag_labels;
    std::vector<duckdb::Value> tag_slugs;
    std::vector<duckdb::Value> tag_created_ats;
    size_t tags_size = tags.size();
    tag_ids.reserve(tags_size);
    tag_labels.reserve(tags_size);
    tag_slugs.reserve(tags_size);
    tag_created_ats.reserve(tags_size);
    for (const auto &t : tags) {
      assert(t.is_object());
      push_opt_i64(tag_ids, get_opt_i64_field(t, "id"));
      push_opt_string(tag_labels, get_opt_string_field(t, "label"));
      push_opt_string(tag_slugs, get_opt_string_field(t, "slug"));
      push_opt_timestamp(tag_created_ats, get_opt_timestamp_field(t, "createdAt"));
    }

    json rewards = get_array_field(m, "clobRewards");
    std::vector<duckdb::Value> reward_ids;
    std::vector<duckdb::Value> reward_condition_ids;
    std::vector<duckdb::Value> reward_asset_addresses;
    std::vector<duckdb::Value> reward_start_dates;
    std::vector<duckdb::Value> reward_end_dates;
    size_t rewards_size = rewards.size();
    reward_ids.reserve(rewards_size);
    reward_condition_ids.reserve(rewards_size);
    reward_asset_addresses.reserve(rewards_size);
    reward_start_dates.reserve(rewards_size);
    reward_end_dates.reserve(rewards_size);
    for (const auto &r : rewards) {
      assert(r.is_object());
      push_opt_i64(reward_ids, get_opt_i64_field(r, "id"));
      push_opt_blob(reward_condition_ids, get_opt_blob_field(r, "conditionId", 32));
      push_opt_blob(reward_asset_addresses, get_opt_blob_field(r, "assetAddress", 20));
      push_opt_date(reward_start_dates, get_opt_date_field(r, "startDate"));
      push_opt_date(reward_end_dates, get_opt_date_field(r, "endDate"));
    }

    ap.BeginRow();
    ap.Append(market_id);
    ap.Append(make_blob_value(row.seed.condition_blob));
    ap.Append(make_blob_value(row.seed.question_blob));
    ap.Append(duckdb::Value(market_slug));
    ap.Append(duckdb::Value(market_question));
    append_opt_string(ap, market_description);
    append_opt_timestamp(ap, market_start_date);
    append_opt_timestamp(ap, market_end_date);
    append_opt_timestamp(ap, market_created_at);
    append_opt_string(ap, market_image);
    append_opt_string(ap, market_icon);
    append_opt_blob(ap, market_submitted_by);
    append_opt_blob(ap, market_resolved_by);
    append_opt_scalar(ap, market_restricted);
    ap.Append(*market_neg_risk);
    append_opt_string(ap, market_neg_risk_request_id);
    append_opt_scalar(ap, market_cyom);
    append_opt_string(ap, market_group_item_title);
    append_opt_string(ap, market_group_item_threshold);
    append_opt_scalar(ap, market_enable_order_book);
    append_opt_scalar(ap, market_order_min_size);
    append_opt_scalar(ap, market_order_min_tick);
    append_opt_scalar(ap, market_clear_book_on_start);
    append_opt_scalar(ap, market_manual_activation);
    append_opt_scalar(ap, market_automatically_active);
    append_opt_string(ap, market_uma_bond);
    append_opt_string(ap, market_uma_reward);
    append_opt_scalar(ap, market_rewards_min_size);
    append_opt_scalar(ap, market_rewards_max_spread);
    append_opt_scalar(ap, market_holding_rewards_enable);
    append_opt_scalar(ap, market_rfq_enabled);
    append_opt_scalar(ap, market_fees_enabled);
    append_opt_string(ap, market_fee_type);
    append_opt_string(ap, market_series_color);
    append_opt_scalar(ap, market_show_gmp_series);
    append_opt_scalar(ap, market_show_gmp_outcome);

    ap.Append(make_list_value(duckdb::LogicalType::BIGINT, std::move(event_ids)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_tickers)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_slugs)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_titles)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_descriptions)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_resolution_sources)));
    ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_start_dates)));
    ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_creation_dates)));
    ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_end_dates)));
    ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_created_ats)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_images)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_icons)));
    ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(event_start_times)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(event_gmp_chart_modes)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_enable_order_books)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_neg_risks)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_enable_neg_risks)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_show_all_outcomes)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_show_market_images)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_auto_resolveds)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_auto_actives)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_cyoms)));
    ap.Append(make_list_value(duckdb::LogicalType::BOOLEAN, std::move(event_requires_translations)));

    ap.Append(make_list_value(duckdb::LogicalType::BIGINT, std::move(tag_ids)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(tag_labels)));
    ap.Append(make_list_value(duckdb::LogicalType::VARCHAR, std::move(tag_slugs)));
    ap.Append(make_list_value(duckdb::LogicalType::TIMESTAMP, std::move(tag_created_ats)));

    ap.Append(make_list_value(duckdb::LogicalType::BIGINT, std::move(reward_ids)));
    ap.Append(make_list_value(duckdb::LogicalType::BLOB, std::move(reward_condition_ids)));
    ap.Append(make_list_value(duckdb::LogicalType::BLOB, std::move(reward_asset_addresses)));
    ap.Append(make_list_value(duckdb::LogicalType::DATE, std::move(reward_start_dates)));
    ap.Append(make_list_value(duckdb::LogicalType::DATE, std::move(reward_end_dates)));

    ap.Append(row.seed.first_seen_block);
    ap.Append(now_ms);
    ap.EndRow();
  }
}

} // namespace stage0
