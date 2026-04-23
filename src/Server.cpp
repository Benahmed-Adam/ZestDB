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
        if (!ec) {
            ZestLog(LogLevel::DEBUG, "Session: received " + std::to_string(length) + " bytes");
            std::string cmd(this->data_);
            std::string result;

            if (!this->authenticated_) {
                std::string authCmd = cmd;
                if (authCmd.find("Authorization: ") == 0) {
                    authCmd = authCmd.substr(15);
                }

                unsigned int dotPos = authCmd.find(".");
                if (dotPos == std::string::npos) {
                    ZestLog(LogLevel::DEBUG, "Session: authentication required");
                    result = "ERROR: authentication required";
                    this->do_write(result, true);
                    return;
                } else {
                    std::string username = authCmd.substr(0, dotPos);
                    std::string token = authCmd.substr(dotPos + 1);

                    ZestLog(LogLevel::DEBUG, "Session: username: " + username + ", token: " + token);

                    if (this->db_.validateToken(username, token)) {
                        ZestLog(LogLevel::DEBUG, "Session: authentication success");
                        this->authenticated_ = true;
                        result = "OK: authenticated";
                    } else {
                        ZestLog(LogLevel::DEBUG, "Session: authentication failed");
                        result = "ERROR: authentication failed";
                        this->do_write(result, true);
                        return;
                    }
                }
            } else {
                result = this->db_.execCmd(cmd);
            }

            ZestLog(LogLevel::DEBUG, "Session: command result = " + result);
            this->do_write(result, false);
        } else {
            ZestLog(LogLevel::INFO, "Session: client disconnected: " + ec.message());
        }
    });
}

void Session::do_write(const std::string& message, bool closeAfter)
{
    auto self(this->shared_from_this());

    asio::async_write(this->socket_, asio::buffer(message), [this, self, message, closeAfter](std::error_code ec, std::size_t) {
        if (!ec) {
            ZestLog(LogLevel::DEBUG, "Session: sent " + std::to_string(message.size()) + " bytes");
            if (closeAfter) {
                ZestLog(LogLevel::INFO, "Session: closing connection");
                this->socket_.close();
            } else {
                this->do_read();
            }
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