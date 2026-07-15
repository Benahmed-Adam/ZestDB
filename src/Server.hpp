#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace Zest {

using asio::ip::tcp;

class ZestDB;

using ZestStream = std::variant<tcp::socket, asio::ssl::stream<tcp::socket>>;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(ZestStream stream, ZestDB& db);
    void start();

private:
    void do_read_size();
    void do_read_command(uint32_t size);

    void queue_write(const std::string& message, bool closeAfter = false);
    void do_write();

    void close_stream();

    ZestStream stream_;
    asio::streambuf buffer_;
    ZestDB& db_;
    bool authenticated_ = false;

    std::deque<std::string> write_queue_;
    bool closing_ = false;
};

class Server {
public:
    Server(asio::io_context& io_context, short port, ZestDB& db);

private:
    void do_accept();

    asio::ssl::context ssl_context_;
    tcp::acceptor acceptor_;
    ZestDB& db_;
};

} // namespace Zest
