#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <variant>

using asio::ip::tcp;

class ZestDB;

using ZestStream = std::variant<tcp::socket, asio::ssl::stream<tcp::socket>>;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(ZestStream stream, ZestDB& db);
    void start();

private:
    void do_read();
    void do_write(const std::string& message, bool closeAfter = false);
    void close_stream();

    ZestStream stream_;
    char data_[4096];
    ZestDB& db_;
    bool authenticated_ = false;
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