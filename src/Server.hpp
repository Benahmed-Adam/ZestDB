#include <asio.hpp>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using asio::ip::tcp;

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket);
    void start();
private:
    void do_read();
    void do_write(const std::string& message);

    tcp::socket socket_;
    char data_[4096];
};

class Server {
public:
    Server(asio::io_context& io_context, short port);
private:
    void do_accept();

    tcp::acceptor acceptor_;
};