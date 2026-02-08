#pragma once

// ============================================================================
// API Server - HTTP 服务器
// ============================================================================

#include <iostream>
#include <memory>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../core/database.hpp"
#include "../sync/sync_token_filler.hpp"
#include "api_session.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

// ============================================================================
// ApiServer - HTTP 服务器
// ============================================================================
class ApiServer {
public:
  ApiServer(asio::io_context &ioc, Database &db, SyncTokenFiller &token_filler, unsigned short port)
      : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), db_(db), token_filler_(token_filler) {
    std::cout << "[HTTP] 监听端口 " << port << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
          if (!ec) {
            std::make_shared<ApiSession>(std::move(socket), db_, token_filler_)
                ->run();
          }
          do_accept();
        });
  }

  asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  Database &db_;
  SyncTokenFiller &token_filler_;
};
