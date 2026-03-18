#pragma once

#include "tracker/tracker_service.hpp"

#include <thread>

namespace tracker {

class ApiServer {
public:
  ApiServer(TrackerService &service, std::string host, uint16_t port);
  ~ApiServer();

  void start();
  void stop();

private:
  void serve_loop();

  TrackerService &service_;
  std::string host_;
  uint16_t port_;
  bool running_ = false;
  std::thread thread_;
};

} // namespace tracker
