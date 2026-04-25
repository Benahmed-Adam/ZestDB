#include "Server.hpp"
#include "Logger.hpp"
#include "ZestDB.hpp"

Session::Session(ZestStream stream, ZestDB& db)
    : stream_(std::move(stream))
    , db_(db)
{
}

void Session::start()
{
    std::string remote_ip = std::visit([](auto& s) {
        return s.lowest_layer().remote_endpoint().address().to_string();
    },
        stream_);

    unsigned short int remote_port = std::visit([](auto& s) {
        return s.lowest_layer().remote_endpoint().port();
    },
        stream_);

    if (!std::regex_match(remote_ip, this->db_.settings.NetworkValidation)) {
        ZestLog(LogLevel::WARNING, "Session: Unauthorized IP: " + remote_ip);
        this->do_write("unauthorized", true);
        return;
    }

    ZestLog(LogLevel::INFO, "Session: client connected from " + remote_ip + ":" + std::to_string(remote_port));

    if (std::holds_alternative<asio::ssl::stream<tcp::socket>>(stream_)) {
        auto& ssl_stream = std::get<asio::ssl::stream<tcp::socket>>(stream_);
        ssl_stream.async_handshake(asio::ssl::stream_base::server,
            [self = shared_from_this()](std::error_code ec) {
                if (!ec)
                    self->do_read();
            });
    } else {
        this->do_read();
    }
}

void Session::do_read()
{
    auto self(this->shared_from_this());

    auto handle_read = [this, self](auto& stream) {
        asio::async_read_until(stream, this->buffer_, "\n",
            [this, self](std::error_code ec, std::size_t) {
                if (!ec) {
                    std::istream is(&this->buffer_);
                    std::string cmd;
                    std::getline(is, cmd); 
                    
                    if (!cmd.empty() && cmd.back() == '\r') {
                        cmd.pop_back();
                    }

                    ZestLog(LogLevel::DEBUG, "Session: command received: " + cmd);
                    std::string result;

                    if (!this->authenticated_) {
                        std::string authPrefix = "Authorization: ";
                        if (cmd.compare(0, authPrefix.length(), authPrefix) == 0) {
                            std::string authContent = cmd.substr(authPrefix.length());
                            
                            size_t dotPos = authContent.find(".");
                            if (dotPos != std::string::npos) {
                                std::string username = authContent.substr(0, dotPos);
                                std::string token = authContent.substr(dotPos + 1);

                                if (this->db_.validateToken(username, token)) {
                                    ZestLog(LogLevel::DEBUG, "Session: auth success for " + username);
                                    this->authenticated_ = true;
                                    result = "OK: authenticated";
                                } else {
                                    result = "ERROR: authentication failed";
                                    this->do_write(result + "\n", true);
                                    return;
                                }
                            } else {
                                result = "ERROR: invalid auth format";
                                this->do_write(result + "\n", true);
                                return;
                            }
                        } else {
                            result = "ERROR: authentication required";
                            this->do_write(result + "\n", true);
                            return;
                        }
                    } else {
                        result = this->db_.execCmd(cmd);
                    }

                    this->do_write(result + "\n", false);
                } else if (ec != asio::error::operation_aborted) {
                    ZestLog(LogLevel::INFO, "Client disconnected");
                }
            });
    };

    std::visit(handle_read, stream_);
}

void Session::do_write(const std::string& message, bool closeAfter)
{
    auto self(this->shared_from_this());

    std::visit([this, self, message, closeAfter](auto& stream) {
        asio::async_write(stream, asio::buffer(message),
            [this, self, message, closeAfter](std::error_code ec, std::size_t) {
                if (!ec) {
                    ZestLog(LogLevel::DEBUG, "Session: sent " + std::to_string(message.size()) + " bytes");
                    if (closeAfter) {
                        this->close_stream();
                    } else {
                        this->do_read();
                    }
                } else {
                    ZestLog(LogLevel::WARNING, "Session: write error: " + ec.message());
                }
            });
    },
        stream_);
}

void Session::close_stream()
{
    ZestLog(LogLevel::DEBUG, "Session: closing connection");

    std::visit([](auto& s) {
        std::error_code ec;
        s.lowest_layer().close(ec);
    },
        stream_);
}

Server::Server(asio::io_context& io_context, short port, ZestDB& db)
    : ssl_context_(asio::ssl::context::sslv23)
    , acceptor_(io_context, tcp::endpoint(tcp::v4(), static_cast<asio::ip::port_type>(port)))
    , db_(db)
{
    ZestLog(LogLevel::INFO, "Server: listening on port " + std::to_string(port));
    if (db.settings.useSSL) {
        ssl_context_.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::sslv23);
        ssl_context_.use_certificate_chain_file(this->db_.settings.SSLCertPath);
        ssl_context_.use_private_key_file(this->db_.settings.SSLKeyPath, asio::ssl::context::pem);
    }
    this->do_accept();
}

void Server::do_accept()
{
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
        if (!ec) {
            if (this->db_.settings.useSSL) {
                asio::ssl::stream<tcp::socket> ssl_sock(std::move(socket), this->ssl_context_);
                std::make_shared<Session>(std::move(ssl_sock), this->db_)->start();
            } else {
                std::make_shared<Session>(std::move(socket), this->db_)->start();
            }
        }
        this->do_accept();
    });
}