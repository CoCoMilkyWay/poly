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
  ApiServer(asio::io_context &ioc, Database &stage1_db, Database &stage2_db, stage3::StageSync &stage3_sync,
            unsigned short port, ApiSession::Stage1Getter stage1_getter,
            ApiSession::Stage2Getter stage2_getter, ApiSession::Stage3Getter stage3_getter)
      : ioc_(ioc), acceptor_(ioc, tcp::endpoint(tcp::v4(), port)), stage1_db_(stage1_db), stage2_db_(stage2_db),
        stage3_sync_(stage3_sync), stage1_getter_(std::move(stage1_getter)),
        stage2_getter_(std::move(stage2_getter)), stage3_getter_(std::move(stage3_getter)) {
    std::cout << "[API] 监听端口 " << port << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    TraceN("api/accept");
    acceptor_.async_accept([this](beast::error_code ec, tcp::socket socket) {
      if (!ec) {
        std::make_shared<ApiSession>(std::move(socket), stage1_db_, stage2_db_, stage3_sync_,
                                     stage1_getter_, stage2_getter_, stage3_getter_)->run();
      }
      do_accept();
    });
  }

  asio::io_context &ioc_;
  tcp::acceptor acceptor_;
  Database &stage1_db_;
  Database &stage2_db_;
  stage3::StageSync &stage3_sync_;
  ApiSession::Stage1Getter stage1_getter_;
  ApiSession::Stage2Getter stage2_getter_;
  ApiSession::Stage3Getter stage3_getter_;
};
