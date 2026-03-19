#pragma once

#include "tracker/json.hpp"

#include <filesystem>

namespace tracker {

json load_json(const std::filesystem::path &path, const json &fallback = json::object());
void save_json(const std::filesystem::path &path, const json &data);

} // namespace tracker
