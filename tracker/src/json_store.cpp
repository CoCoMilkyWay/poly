#include "tracker/json_store.hpp"

#include <fstream>

namespace tracker {

json load_json_file(const std::filesystem::path &path, const json &default_value) {
  if (!std::filesystem::exists(path)) {
    return default_value;
  }
  std::ifstream in(path);
  assert(in.is_open());
  json payload;
  in >> payload;
  return payload;
}

void write_json_atomic(const std::filesystem::path &path, const json &payload) {
  const std::filesystem::path tmp_path = path.string() + ".tmp";
  {
    std::ofstream out(tmp_path);
    assert(out.is_open());
    out << payload.dump(2) << "\n";
  }
  std::filesystem::rename(tmp_path, path);
}

} // namespace tracker
