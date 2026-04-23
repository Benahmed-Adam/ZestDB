#pragma once

#include <asio.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using asio::ip::tcp;

class ZestDB;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, ZestDB& db);
    void start();

private:
    void do_read();
    void do_write(const std::string& message, bool closeAfter = false);

    tcp::socket socket_;
    char data_[4096];
    ZestDB& db_;
    bool authenticated_ = false;
};

class Server {
public:
    Server(asio::io_context& io_context, short port, ZestDB& db);

private:
    void do_accept();

    tcp::acceptor acceptor_;
    ZestDB& db_;
};