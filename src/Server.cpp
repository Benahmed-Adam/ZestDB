#include "Server.hpp"

Session::Session(tcp::socket socket): socket_(std::move(socket)) {}

void Session::start() {
    this->do_read();
}

void Session::do_read() {
    auto self(this->shared_from_this());

    this->socket_.async_read_some(asio::buffer(this->data_), [this, self](std::error_code ec, std::size_t length) {
        if (!ec) {
            std::cout << "piaojudazijdazijda" << std::endl;
            this->do_write("recu !");
        }
    });
}

void Session::do_write(const std::string& message) {
    auto self(this->shared_from_this());

    asio::async_write(this->socket_, asio::buffer(message), [this, self, message](std::error_code ec, std::size_t length) {
        if (!ec) {
            this->do_read();
        }
    });
}

Server::Server(asio::io_context& io_context, short port): acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
    this->do_accept();
}

void Server::do_accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            std::make_shared<Session>(std::move(socket))->start();
        }

        this->do_accept();
    });
}