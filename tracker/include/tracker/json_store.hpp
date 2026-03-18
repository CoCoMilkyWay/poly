#pragma once

#include "tracker/json.hpp"

#include <filesystem>

namespace tracker {

json load_json_file(const std::filesystem::path &path, const json &default_value);
void write_json_atomic(const std::filesystem::path &path, const json &payload);

} // namespace tracker
