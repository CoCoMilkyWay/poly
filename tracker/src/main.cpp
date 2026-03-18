#include "tracker/config.hpp"
#include "tracker/http_server.hpp"
#include "tracker/tracker_service.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

namespace {

std::filesystem::path resolve_tracker_dir_from_config() {
  const std::filesystem::path candidate = std::filesystem::current_path();
  assert(std::filesystem::exists(candidate / "config.json"));
  assert(std::filesystem::exists(candidate / "address.txt"));
  return candidate;
}

} // namespace

int main() {
  const tracker::AppConfig config = tracker::AppConfig::load(resolve_tracker_dir_from_config());
  tracker::TrackerService service(config);
  service.bootstrap();

  tracker::ApiServer server(service, config.backend_host, config.backend_port);
  server.start();

  std::cout
      << tracker::json({
             {"backend_url", "http://localhost:" + std::to_string(config.backend_port)},
             {"frontend_url", "http://localhost:" + std::to_string(config.frontend_port)},
             {"address_file", config.address_file.string()},
             {"meta_file", config.meta_file.string()},
             {"aggregate_file", config.aggregate_file.string()},
             {"history_file", config.history_file.string()},
         })
             .dump(2)
      << std::endl;

  service.run_forever();
  return 0;
}
