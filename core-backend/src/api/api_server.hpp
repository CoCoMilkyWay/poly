#pragma once

#include <iostream>
#include <memory>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "../core/database.hpp"
#include "../stage3/pnl_replay.hpp"
#include "api_session.hpp"
#include "misc/profiler.hpp"

namespace asio = boost::asio;
namespace beast = boost::beast;
using tcp = asio::ip::tcp;

class ApiServer {
public:
  ApiServer(asio::io_context &ioc, Database &db, stage3::PnlEngine &pnl_engine,
            unsigned short port, ApiSession::Stage1SyncGetter stage1_getter = nullptr,
            ApiSession::Stage2SyncGetter stage2_getter = nullptr)
      : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), db_(db),
        pnl_engine_(pnl_engine), sync_getter_(std::move(stage1_getter)),
        stage2_getter_(std::move(stage2_getter)) {
    std::cout << "[API] 监听端口 " << port << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    TraceN("do_accept");
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
      if (!ec) {
        std::make_shared<ApiSession>(std::move(socket), db_, pnl_engine_, sync_getter_, stage2_getter_)->run();
      }
      do_accept();
    });
  }

  asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  Database &db_;
  stage3::PnlEngine &pnl_engine_;
  ApiSession::Stage1SyncGetter sync_getter_;
  ApiSession::Stage2SyncGetter stage2_getter_;
};
