#pragma once

#include <iostream>
#include <memory>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../core/database.hpp"
#include "../rebuild/rebuilder.hpp"
#include "api_session.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

class ApiServer {
public:
  ApiServer(asio::io_context &ioc, Database &db, rebuild::Engine &rebuilder,
            unsigned short port, ApiSession::SyncStatusGetter sync_getter = nullptr)
      : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), db_(db),
        rebuilder_(rebuilder), sync_getter_(std::move(sync_getter)) {
    std::cout << "[API] 监听端口 " << port << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
      if (!ec) {
        std::make_shared<ApiSession>(std::move(socket), db_, rebuilder_, sync_getter_)->run();
      }
      do_accept();
    });
  }

  asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  Database &db_;
  rebuild::Engine &rebuilder_;
  ApiSession::SyncStatusGetter sync_getter_;
};
