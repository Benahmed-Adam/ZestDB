#include "Server.hpp"
#include "Logger.hpp"
#include "ZestDB.hpp"

Session::Session(tcp::socket socket, ZestDB& db)
    : socket_(std::move(socket))
    , db_(db)
{
}

void Session::start()
{
    ZestLog(LogLevel::INFO, "Session: client connected");
    this->do_read();
}

void Session::do_read()
{
    auto self(this->shared_from_this());

    this->socket_.async_read_some(asio::buffer(this->data_), [this, self](std::error_code ec, std::size_t length) {
        (void)length;
        if (!ec) {
            ZestLog(LogLevel::DEBUG, "Session: received " + std::to_string(length) + " bytes");
            std::string result = this->db_.execCmd(this->data_);
            ZestLog(LogLevel::DEBUG, "Session: command result = " + result);
            this->do_write(result);
        } else {
            ZestLog(LogLevel::WARNING, "Session: read error: " + ec.message());
        }
    });
}

void Session::do_write(const std::string& message)
{
    auto self(this->shared_from_this());

    asio::async_write(this->socket_, asio::buffer(message), [this, self, message](std::error_code ec, std::size_t length) {
        (void)length;
        if (!ec) {
            ZestLog(LogLevel::DEBUG, "Session: sent " + std::to_string(message.size()) + " bytes");
            this->do_read();
        } else {
            ZestLog(LogLevel::WARNING, "Session: write error: " + ec.message());
        }
    });
}

Server::Server(asio::io_context& io_context, short port, ZestDB& db)
    : acceptor_(io_context, tcp::endpoint(asio::ip::make_address("0.0.0.0"), static_cast<asio::ip::port_type>(port)))
    , db_(db)
{
    ZestLog(LogLevel::INFO, "Server: listening on port " + std::to_string(port));
    this->do_accept();
}

void Server::do_accept()
{
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            ZestLog(LogLevel::INFO, "Server: new connection accepted");
            std::make_shared<Session>(std::move(socket), this->db_)->start();
        } else {
            ZestLog(LogLevel::ERROR, "Server: accept error: " + ec.message());
        }

        this->do_accept();
    });
}