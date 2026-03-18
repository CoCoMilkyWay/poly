#include "tracker/http_server.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace tracker {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

std::pair<std::string, std::string> split_target(const std::string &target) {
  const size_t pos = target.find('?');
  if (pos == std::string::npos) {
    return {target, ""};
  }
  return {target.substr(0, pos), target.substr(pos + 1)};
}

std::string query_value(const std::string &query, const std::string &key) {
  size_t start = 0;
  while (start < query.size()) {
    const size_t end = query.find('&', start);
    const std::string part = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t eq = part.find('=');
    const std::string current_key = eq == std::string::npos ? part : part.substr(0, eq);
    if (current_key == key) {
      return eq == std::string::npos ? "" : part.substr(eq + 1);
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return "";
}

const char *reason_phrase(unsigned status) {
  if (status == 200) {
    return "OK";
  }
  if (status == 204) {
    return "No Content";
  }
  assert(false);
  return "";
}

std::string build_http_response(unsigned version,
                                unsigned status,
                                std::string_view content_type,
                                std::string_view body) {
  const unsigned major = version / 10;
  const unsigned minor = version % 10;
  std::string response;
  response += "HTTP/" + std::to_string(major) + "." + std::to_string(minor) + " ";
  response += std::to_string(status) + " ";
  response += reason_phrase(status);
  response += "\r\n";
  response += "Content-Type: ";
  response += std::string(content_type);
  response += "\r\n";
  response += "Access-Control-Allow-Origin: *\r\n";
  response += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
  response += "Access-Control-Allow-Headers: Content-Type\r\n";
  response += "Content-Length: ";
  response += std::to_string(body.size());
  response += "\r\n";
  response += "Connection: close\r\n\r\n";
  response += body;
  return response;
}

void write_response(tcp::socket &socket,
                    unsigned version,
                    unsigned status,
                    std::string_view content_type,
                    std::string_view body) {
  const std::string response = build_http_response(version, status, content_type, body);
  asio::write(socket, asio::buffer(response));
}

void write_json_response(tcp::socket &socket, unsigned version, const json &payload) {
  write_response(socket, version, 200, "application/json; charset=utf-8", payload.dump());
}

void write_empty_response(tcp::socket &socket, unsigned version, unsigned status) {
  write_response(socket, version, status, "text/plain; charset=utf-8", "");
}

void handle_session(TrackerService &service, tcp::socket socket) {
  beast::flat_buffer buffer;
  http::request<http::string_body> request;
  http::read(socket, buffer, request);

  if (request.method() == http::verb::options) {
    write_empty_response(socket, request.version(), 204);
    return;
  }

  const auto [path, query] = split_target(std::string(request.target()));
  if (request.method() == http::verb::get && (path == "/" || path == "/api/state")) {
    write_json_response(socket, request.version(), service.current_state_json());
    return;
  }
  if (request.method() == http::verb::get && path == "/api/meta") {
    write_json_response(socket, request.version(), service.meta_json());
    return;
  }
  if (request.method() == http::verb::get && path == "/api/history") {
    const std::string user = query_value(query, "user");
    assert(!user.empty());
    write_json_response(socket, request.version(), service.history_json_for_user(user));
    return;
  }
  if (request.method() == http::verb::get && path == "/api/health") {
    write_json_response(socket, request.version(), service.health_json());
    return;
  }
  if (request.method() == http::verb::post && path == "/api/resync") {
    service.request_resync();
    write_json_response(socket, request.version(), json{{"ok", true}});
    return;
  }

  assert(false);
}

} // namespace

ApiServer::ApiServer(TrackerService &service, std::string host, uint16_t port)
    : service_(service), host_(std::move(host)), port_(port) {}

ApiServer::~ApiServer() {
  stop();
}

void ApiServer::start() {
  assert(!running_);
  running_ = true;
  thread_ = std::thread([this]() { serve_loop(); });
}

void ApiServer::stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  if (thread_.joinable()) {
    thread_.detach();
  }
}

void ApiServer::serve_loop() {
  asio::io_context ioc;
  tcp::acceptor acceptor(ioc, tcp::endpoint(asio::ip::make_address(host_), port_));
  while (running_) {
    tcp::socket socket = acceptor.accept();
    std::thread(handle_session, std::ref(service_), std::move(socket)).detach();
  }
}

} // namespace tracker
