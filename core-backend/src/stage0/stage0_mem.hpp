#pragma once

#include "stage0_query_sync.hpp"
#include "stage0_tag_sync.hpp"

namespace stage0 {

json memory_breakdown(const QuerySync &query, const TagSync &tag);

} // namespace stage0
