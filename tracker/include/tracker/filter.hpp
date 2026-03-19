#pragma once

#include "tracker/codec.hpp"
#include "tracker/const.hpp"
#include "tracker/json.hpp"

#include <string>
#include <vector>

namespace tracker {

// ============================================================================
// Log Filter Builder - 供 SyncThread 和 WsThread 共用
// ============================================================================

inline std::vector<json> build_user_log_filters(
    const std::vector<std::string> &users,
    size_t topic_group_size,
    std::optional<uint64_t> from_block = std::nullopt,
    std::optional<uint64_t> to_block = std::nullopt) {
  std::vector<json> filters;

  for (const auto &group : chunked(users, topic_group_size)) {
    json topics = json::array();
    for (const auto &user : group) {
      topics.push_back(addr_to_topic(user));
    }

    std::vector<json> group_filters = {
        // TransferSingle/Batch: topics[2] = from
        {{"address", kConditionalTokens},
         {"topics",
          json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}),
                       nullptr, topics})}},
        // TransferSingle/Batch: topics[3] = to
        {{"address", kConditionalTokens},
         {"topics",
          json::array({json::array({kTransferSingleTopic, kTransferBatchTopic}),
                       nullptr, nullptr, topics})}},
        // Split/Merge/Redeem: topics[1] = stakeholder
        {{"address", kConditionalTokens},
         {"topics",
          json::array({json::array({kPositionSplitTopic, kPositionMergeTopic,
                                    kPositionRedeemTopic}),
                       topics})}},
        // OrderFill: topics[2] = maker
        {{"address", json::array({kCtfExchange, kNegRiskCtfExchange})},
         {"topics", json::array({json::array({kOrderFillTopic}), nullptr, topics})}},
        // OrderFill: topics[3] = taker
        {{"address", json::array({kCtfExchange, kNegRiskCtfExchange})},
         {"topics",
          json::array({json::array({kOrderFillTopic}), nullptr, nullptr, topics})}},
        // PositionConvert: topics[1] = stakeholder
        {{"address", kNegRiskAdapter},
         {"topics", json::array({json::array({kPositionConvertTopic}), topics})}},
    };

    for (auto &filter : group_filters) {
      if (from_block) {
        filter["fromBlock"] = u64_to_hex(*from_block);
      }
      if (to_block) {
        filter["toBlock"] = u64_to_hex(*to_block);
      }
      filters.push_back(std::move(filter));
    }
  }

  // ConditionResolution: 全局监听 (不按用户过滤)
  json resolve_filter = {
      {"address", kConditionalTokens},
      {"topics", json::array({json::array({kConditionResolveTopic})})},
  };
  if (from_block) {
    resolve_filter["fromBlock"] = u64_to_hex(*from_block);
  }
  if (to_block) {
    resolve_filter["toBlock"] = u64_to_hex(*to_block);
  }
  filters.push_back(std::move(resolve_filter));

  return filters;
}

} // namespace tracker
