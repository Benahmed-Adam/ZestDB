#include "Server.hpp"

#include <cstring>
#include <format>
#include <future>
#include <istream>

#include "Logger.hpp"
#include "ZestDB.hpp"

namespace Zest {

    Session::Session(ZestStream stream, ZestDB& db, Limits limits, std::shared_ptr<std::atomic<size_t>> activeSessions)
        : stream_(std::move(stream)),
          timer_(std::visit([](auto& s) { return s.get_executor(); }, stream_)),
          db_(db),
          limits_(limits),
          activeSessions_(std::move(activeSessions)) {
        this->activeSessions_->fetch_add(1);
    }

    Session::~Session() {
        this->activeSessions_->fetch_sub(1);
    }

    void Session::start() {
        std::error_code endpointError;
        tcp::endpoint remote = std::visit([&endpointError](auto& s) { return s.lowest_layer().remote_endpoint(endpointError); }, stream_);
        if (endpointError) {
            ZestLog(LogLevel::Info, std::format("Session: client left before the session started: {}", endpointError.message()));
            this->close_stream();
            return;
        }
        this->remote_ip_ = remote.address().to_string();

        if (!this->db_.isAddressAllowed(this->remote_ip_)) {
            ZestLog(LogLevel::Warning, std::format("Session: Unauthorized IP: {}", this->remote_ip_));
            this->queue_write("unauthorized", true);
            return;
        }

        ZestLog(LogLevel::Info, std::format("Session: client connected from {}:{}", this->remote_ip_, remote.port()));

        this->touch();
        this->watch_deadline();
        if (std::holds_alternative<asio::ssl::stream<tcp::socket>>(stream_)) {
            auto& ssl_stream = std::get<asio::ssl::stream<tcp::socket>>(stream_);
            ssl_stream.async_handshake(asio::ssl::stream_base::server, [self = this->shared_from_this()](std::error_code ec) {
                if (ec) {
                    self->close_stream();
                } else {
                    self->do_read_size();
                }
            });
        } else {
            this->do_read_size();
        }
    }

    void Session::touch() {
        this->deadline_ = std::chrono::steady_clock::now() + (this->authenticated_ ? this->limits_.idleTimeout : this->limits_.authTimeout);
    }

    // A request only moves deadline_ forward: the timer is armed once and re-waits until the deadline really passes
    void Session::watch_deadline() {
        this->timer_.expires_at(this->deadline_);
        this->timer_.async_wait([self = this->shared_from_this()](std::error_code ec) {
            if (ec) {
                return;
            }
            if (std::chrono::steady_clock::now() >= self->deadline_) {
                ZestLog(LogLevel::Info, std::format("Session: {} timed out", self->remote_ip_));
                self->close_stream();
            } else {
                self->watch_deadline();
            }
        });
    }

    void Session::do_read_size() {
        this->touch();
        auto self(this->shared_from_this());
        std::visit(
            [this, self](auto& stream) {
                asio::async_read(stream, this->buffer_, asio::transfer_exactly(4), [this, self](std::error_code ec, std::size_t length) {
                    if (!ec && length == 4) {
                        std::istream is(&this->buffer_);
                        uint32_t payload_size;
                        is.read(reinterpret_cast<char*>(&payload_size), 4);
                        payload_size = ntohl(payload_size);
                        if (payload_size > (this->authenticated_ ? MAX_PAYLOAD_SIZE : MAX_UNAUTHENTICATED_PAYLOAD_SIZE)) {
                            ZestLog(LogLevel::Warning, std::format("Session: payload of {} bytes refused", payload_size));
                            this->queue_write(ZestDB::responseToJson({ ResultType::Code::Error, Messages::PAYLOAD_TOO_LARGE, 0 }) + "\n", true);
                            return;
                        }
                        this->do_read_command(payload_size);
                    } else {
                        if (ec != asio::error::operation_aborted) {
                            ZestLog(LogLevel::Info, "Client disconnected");
                        }
                        this->close_stream();
                    }
                });
            },
            stream_);
    }

    void Session::do_read_command(uint32_t payload_size) {
        auto self(this->shared_from_this());
        std::visit(
            [this, self, payload_size](auto& stream) {
                asio::async_read(stream, this->buffer_, asio::transfer_exactly(payload_size), [this, self, payload_size](std::error_code ec, std::size_t length) {
                    if (ec || length != payload_size) {
                        this->close_stream();
                        return;
                    }

                    std::istream is(&this->buffer_);
                    std::string payload(payload_size, '\0');
                    is.read(&payload[0], payload_size);

                    std::string final_result;
                    bool should_close = false;
                    uint32_t offset = 0;

                    while (offset < payload_size && !should_close) {
                        if (offset + 4 > payload_size) {
                            final_result += (final_result.empty() ? "" : "\r\n\r\n") + ZestDB::responseToJson({ ResultType::Code::Error, Messages::MALFORMED_BATCH, 0 });
                            break;
                        }

                        uint32_t cmd_size;
                        std::memcpy(&cmd_size, &payload[offset], 4);
                        cmd_size = ntohl(cmd_size);
                        offset += 4;

                        if (offset + cmd_size > payload_size) {
                            final_result += (final_result.empty() ? "" : "\r\n\r\n") + ZestDB::responseToJson({ ResultType::Code::Error, Messages::MALFORMED_BATCH, 0 });
                            break;
                        }

                        std::string cmd_result = this->handle_command(payload.substr(offset, cmd_size), should_close);
                        offset += cmd_size;

                        if (!final_result.empty()) {
                            final_result += "\r\n\r\n";
                        }
                        final_result += cmd_result;
                    }

                    // The batch's WAL records reach the OS (or the disk with fsyncOnWrite) before anything is acknowledged
                    if (!this->db_.syncWAL()) {
                        ZestLog(LogLevel::Error, "Session: the WAL could not be written, closing without acknowledging the batch");
                        this->close_stream();
                        return;
                    }

                    final_result += '\n';
                    this->queue_write(final_result, should_close);

                    if (!should_close) {
                        this->do_read_size();
                    }
                });
            },
            stream_);
    }

    std::string Session::handle_command(const std::string& cmd, bool& should_close) {
        if (!this->authenticated_) {
            const std::string authPrefix = "Authorization: ";
            const size_t dotPos = cmd.find('.', authPrefix.size());
            should_close = true;
            if (cmd.compare(0, authPrefix.size(), authPrefix) != 0 || dotPos == std::string::npos) {
                return ZestDB::responseToJson({ ResultType::Code::Error, Messages::AUTH_FAILED, 0 });
            }

            std::string username = cmd.substr(authPrefix.size(), dotPos - authPrefix.size());
            if (!this->db_.authAllowed(username)) {
                ZestLog(LogLevel::Warning, std::format("Session: user {} is locked out after too many failed authentications ({})", username, this->remote_ip_));
                return ZestDB::responseToJson({ ResultType::Code::Error, Messages::AUTH_FAILED, 0 });
            }
            if (!this->db_.authenticate(username, cmd.substr(dotPos + 1))) {
                this->db_.recordAuthResult(username, false);
                return ZestDB::responseToJson({ ResultType::Code::Error, Messages::AUTH_FAILED, 0 });
            }

            this->db_.recordAuthResult(username, true);
            this->authenticated_ = true;
            this->username_ = std::move(username);
            should_close = false;
            return ZestDB::responseToJson({ ResultType::Code::Success, Messages::AUTH_SUCCESS, 0 });
        }

        std::optional<Role> role = this->db_.roleOf(this->username_);
        if (!role) {
            should_close = true;
            return ZestDB::responseToJson({ ResultType::Code::Error, Messages::SESSION_REVOKED, 0 });
        }

        try {
            return ZestDB::responseToJson(this->db_.execCmd(cmd, *role, false));
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("Session: command failed with an exception: {}", e.what()));
            return ZestDB::responseToJson({ ResultType::Code::Error, Messages::INTERNAL_ERROR, 0 });
        }
    }

    void Session::queue_write(const std::string& message, bool closeAfter) {
        bool write_in_progress = !write_queue_.empty();
        write_queue_.push_back(message);
        closing_ = closeAfter;

        if (!write_in_progress) {
            this->do_write();
        }
    }

    void Session::do_write() {
        auto self(this->shared_from_this());
        std::visit(
            [this, self](auto& stream) {
                asio::async_write(stream, asio::buffer(write_queue_.front()), [this, self](std::error_code ec, std::size_t) {
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

    void Session::close_stream() {
        this->timer_.cancel();
        std::visit(
            [](auto& s) {
                std::error_code ec;
                s.lowest_layer().close(ec);
            },
            this->stream_);
    }

    std::vector<std::unique_ptr<asio::io_context>> Server::makeWorkers() {
        std::vector<std::unique_ptr<asio::io_context>> workers;
        const unsigned int threads = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned int i = 0; i < threads; i++) {
            workers.push_back(std::make_unique<asio::io_context>(1));
        }
        return workers;
    }

    Server::Server(asio::io_context&, uint16_t port, ZestDB& db)
        : workers_(makeWorkers()),
          ssl_context_(asio::ssl::context::tls_server),
          acceptor_(*workers_[0], tcp::endpoint(tcp::v4(), port)),
          accept_retry_timer_(*workers_[0]),
          db_(db),
          useSSL_(db.settings.useSSL),
          maxConnections_(db.settings.MaxConnections),
          limits_{ std::chrono::seconds(db.settings.AuthTimeout), std::chrono::seconds(db.settings.IdleTimeout) } {
        for (auto& worker : this->workers_) {
            this->workGuards_.push_back(asio::make_work_guard(*worker));
        }
        ZestLog(LogLevel::Info, std::format("Server: listening on port {} with {} IO threads", port, this->workers_.size()));
        if (this->useSSL_) {
            ssl_context_.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 |
                                     asio::ssl::context::single_dh_use);
            ssl_context_.use_certificate_chain_file(this->db_.settings.SSLCertPath.string());
            ssl_context_.use_private_key_file(this->db_.settings.SSLKeyPath.string(), asio::ssl::context::pem);
        }
        this->do_accept();
        for (auto& worker : this->workers_) {
            this->workerThreads_.emplace_back([&worker] { worker->run(); });
        }
    }

    Server::~Server() {
        this->stop();
    }

    void Server::stop() {
        if (this->stopped_.exchange(true)) {
            return;
        }
        // The acceptor is not thread-safe: close it from its own thread, and wait for that before stopping the workers
        std::promise<void> closed;
        asio::post(this->acceptor_.get_executor(), [this, &closed] {
            std::error_code ignored;
            this->acceptor_.close(ignored);
            this->accept_retry_timer_.cancel();
            closed.set_value();
        });
        closed.get_future().wait();
        for (auto& guard : this->workGuards_) {
            guard.reset();
        }
        for (auto& worker : this->workers_) {
            worker->stop();
        }
        for (auto& t : this->workerThreads_) {
            if (t.joinable() && t.get_id() != std::this_thread::get_id()) {
                t.join();
            }
        }
    }

    void Server::do_accept() {
        auto& worker = *this->workers_[this->nextWorker_++ % this->workers_.size()];
        this->acceptor_.async_accept(worker.get_executor(), [this](std::error_code ec, tcp::socket socket) {
            if (ec == asio::error::operation_aborted) {
                return;
            }
            if (ec) {
                ZestLog(LogLevel::Warning, std::format("Server: accept failed: {}", ec.message()));
                this->accept_retry_timer_.expires_after(std::chrono::milliseconds(100));
                this->accept_retry_timer_.async_wait([this](std::error_code timerError) {
                    if (!timerError) {
                        this->do_accept();
                    }
                });
                return;
            }

            if (this->activeSessions_->load() >= this->maxConnections_) {
                ZestLog(LogLevel::Warning, std::format("Server: refusing a connection, {} sessions already open", this->maxConnections_));
                std::error_code ignored;
                socket.close(ignored);
            } else {
                auto executor = socket.get_executor();
                std::shared_ptr<Session> session;
                if (this->useSSL_) {
                    session = std::make_shared<Session>(asio::ssl::stream<tcp::socket>(std::move(socket), this->ssl_context_), this->db_, this->limits_, this->activeSessions_);
                } else {
                    session = std::make_shared<Session>(std::move(socket), this->db_, this->limits_, this->activeSessions_);
                }
                asio::dispatch(executor, [session] { session->start(); });
            }
            this->do_accept();
        });
    }

} // namespace Zest
