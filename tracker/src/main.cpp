#include "tracker/api.hpp"
#include "tracker/config.hpp"
#include "tracker/queue.hpp"
#include "tracker/state.hpp"
#include "tracker/sync.hpp"
#include "tracker/ws.hpp"

#include <filesystem>
#include <iostream>

namespace {

std::filesystem::path find_tracker_dir() {
  std::filesystem::path cwd = std::filesystem::current_path();
  if (std::filesystem::exists(cwd / "address.txt")) return cwd;
  if (std::filesystem::exists(cwd / "tracker" / "address.txt")) return cwd / "tracker";
  assert(false && "cannot find tracker directory");
  return cwd;
}

} // namespace

int main() {
  tracker::AppConfig cfg = tracker::AppConfig::load(find_tracker_dir());

  tracker::AppState state;
  tracker::EventQueue queue;

  tracker::WsThread ws_thread(cfg, queue);
  tracker::SyncThread sync_thread(cfg, state, queue, ws_thread);
  tracker::ApiThread api_thread(cfg, state,
                                [&sync_thread] { sync_thread.request_resync(); });

  ws_thread.start();
  api_thread.start();

  std::cout << tracker::json({
      {"backend_url", "http://localhost:" + std::to_string(cfg.backend_port)},
      {"frontend_url", "http://localhost:" + std::to_string(cfg.frontend_port)},
      {"address_file", cfg.address_file.string()},
      {"snapshot_file", cfg.snapshot_file.string()},
      {"history_file", cfg.history_file.string()},
      {"aggregate_file", cfg.aggregate_file.string()},
      {"meta_file", cfg.meta_file.string()},
  }).dump(2) << std::endl;

  sync_thread.run();
}
