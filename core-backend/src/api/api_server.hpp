#pragma once

// ============================================================================
// API Server - HTTP 服务器
// ============================================================================

#include <iostream>
#include <memory>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../core/config.hpp"
#include "../core/database.hpp"
#include "../infra/https_pool.hpp"
#include "../sync/sync_repair.hpp"
#include "api_session.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

// ============================================================================
// ApiServer - HTTP 服务器
// ============================================================================
class ApiServer {
public:
  ApiServer(asio::io_context &ioc, Database &db, HttpsPool &pool,
            const Config &config, unsigned short port)
      : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), db_(db),
        sync_repair_(db, pool, config) {
    std::cout << "[HTTP] 监听端口 " << port << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<ApiSession>(std::move(socket), db_, sync_repair_)
                ->run();
          }
          do_accept();
        });
  }

  asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  Database &db_;
  SyncRepair sync_repair_;
};
