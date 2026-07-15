#include <cstring>
#include <format>
#include <istream>

#include "Server.hpp"
#include "Logger.hpp"
#include "ZestDB.hpp"

namespace Zest {

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
            ZestLog(LogLevel::WARNING, std::format("Session: Unauthorized IP: {}", remote_ip));
            this->queue_write("unauthorized", true);
            return;
        }

        ZestLog(LogLevel::INFO, std::format("Session: client connected from {}:{}", remote_ip, remote_port));

        if (std::holds_alternative<asio::ssl::stream<tcp::socket>>(stream_)) {
            auto& ssl_stream = std::get<asio::ssl::stream<tcp::socket>>(stream_);
            ssl_stream.async_handshake(asio::ssl::stream_base::server,
                [self = this->shared_from_this()](std::error_code ec) {
                    if (!ec)
                        self->do_read_size();
                });
        } else {
            this->do_read_size();
        }
    }

    void Session::do_read_size()
    {
        auto self(this->shared_from_this());
        std::visit([this, self](auto& stream) {
            asio::async_read(stream, this->buffer_, asio::transfer_exactly(4),
                [this, self](std::error_code ec, std::size_t length) {
                    if (!ec && length == 4) {
                        std::istream is(&this->buffer_);
                        uint32_t payload_size;
                        is.read(reinterpret_cast<char*>(&payload_size), 4);
                        payload_size = ntohl(payload_size);
                        this->do_read_command(payload_size);
                    } else if (ec != asio::error::operation_aborted) {
                        ZestLog(LogLevel::INFO, "Client disconnected");
                    }
                });
        },
            stream_);
    }

    void Session::do_read_command(uint32_t payload_size)
    {
        auto self(this->shared_from_this());
        std::visit([this, self, payload_size](auto& stream) {
            asio::async_read(stream, this->buffer_, asio::transfer_exactly(payload_size),
                [this, self, payload_size](std::error_code ec, std::size_t length) {
                    if (!ec && length == payload_size) {
                        std::istream is(&this->buffer_);
                        std::string payload(payload_size, '\0');
                        is.read(&payload[0], payload_size);

                        std::string final_result;
                        bool should_close = false;
                        uint32_t offset = 0;

                        while (offset < payload_size && !should_close) {
                            if (offset + 4 > payload_size) {
                                final_result += (final_result.empty() ? "" : "\r\n\r\n") + ZestDB::responseToJson({ ResultType::Code::ERROR, Messages::MALFORMED_BATCH, 0 });
                                break;
                            }

                            uint32_t cmd_size;
                            std::memcpy(&cmd_size, &payload[offset], 4);
                            cmd_size = ntohl(cmd_size);
                            offset += 4;

                            if (offset + cmd_size > payload_size) {
                                final_result += (final_result.empty() ? "" : "\r\n\r\n") + ZestDB::responseToJson({ ResultType::Code::ERROR, Messages::MALFORMED_BATCH, 0 });
                                break;
                            }

                            std::string cmd = payload.substr(offset, cmd_size);
                            offset += cmd_size;

                            std::string cmd_result;

                            if (!this->authenticated_) {
                                std::string authPrefix = "Authorization: ";
                                if (cmd.compare(0, authPrefix.length(), authPrefix) == 0) {
                                    std::string authContent = cmd.substr(authPrefix.length());
                                    size_t dotPos = authContent.find(".");
                                    if (dotPos != std::string::npos) {
                                        std::string username = authContent.substr(0, dotPos);
                                        std::string token = authContent.substr(dotPos + 1);
                                        if (this->db_.validateToken(username, token)) {
                                            this->authenticated_ = true;
                                            cmd_result = ZestDB::responseToJson({ ResultType::Code::SUCCESS, Messages::AUTH_SUCCESS, 0 });
                                        } else {
                                            cmd_result = ZestDB::responseToJson({ ResultType::Code::ERROR, Messages::AUTH_FAILED, 0 });
                                            should_close = true;
                                        }
                                    }
                                } else {
                                    cmd_result = ZestDB::responseToJson({ ResultType::Code::ERROR, Messages::AUTH_FAILED, 0 });
                                    should_close = true;
                                }
                            } else {
                                auto resp = this->db_.execCmd(cmd);
                                cmd_result = ZestDB::responseToJson(resp);
                            }

                            if (!final_result.empty()) {
                                final_result += "\r\n\r\n";
                            }
                            final_result += cmd_result;
                        }

                        this->queue_write(final_result + "\n", should_close);

                        if (!should_close) {
                            this->do_read_size();
                        }
                    }
                });
        },
            stream_);
    }

    void Session::queue_write(const std::string& message, bool closeAfter)
    {
        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(message);
        closing_ = closeAfter;

        if (!write_in_progress) {
            this->do_write();
        }
    }

    void Session::do_write()
    {
        auto self(this->shared_from_this());
        std::visit([this, self](auto& stream) {
            asio::async_write(stream, asio::buffer(write_queue_.front()),
                [this, self](std::error_code ec, std::size_t) {
                    if (!ec) {
                        write_queue_.pop_front();
                        if (!write_queue_.empty()) {
                            this->do_write();
                        } else if (closing_) {
                            this->close_stream();
                        }
                    } else {
                        this->close_stream();
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
            this->stream_);
    }

    Server::Server(asio::io_context& io_context, short port, ZestDB& db)
        : ssl_context_(asio::ssl::context::sslv23)
        , acceptor_(io_context, tcp::endpoint(tcp::v4(), static_cast<asio::ip::port_type>(port)))
        , db_(db)
    {
        ZestLog(LogLevel::INFO, std::format("Server: listening on port {}", port));
        if (db.settings.useSSL) {
            ssl_context_.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::sslv23);
            ssl_context_.use_certificate_chain_file(this->db_.settings.SSLCertPath);
            ssl_context_.use_private_key_file(this->db_.settings.SSLKeyPath, asio::ssl::context::pem);
        }
        this->do_accept();
    }

    void Server::do_accept()
    {
        this->acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
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

} // namespace Zest
