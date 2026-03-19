#include "tracker/store.hpp"

#include <cassert>
#include <fstream>

namespace tracker {

json load_json(const std::filesystem::path &path, const json &fallback) {
  if (!std::filesystem::exists(path)) return fallback;
  std::ifstream in(path);
  assert(in.is_open());
  json data;
  in >> data;
  return data;
}

void save_json(const std::filesystem::path &path, const json &data) {
  std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp);
    assert(out.is_open());
    out << data.dump(2) << "\n";
  }
  std::filesystem::rename(tmp, path);
}

} // namespace tracker
