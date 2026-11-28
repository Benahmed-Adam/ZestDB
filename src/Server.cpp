#include "Server.hpp"

#include <cstring>
#include <format>
#include <future>
#include <istream>

#include "Logger.hpp"
#include "RateLimiter.hpp"
#include "ZestDB.hpp"

namespace Zest {

    namespace {
        /// The reply to a command refused by a rate limit, built once: refusing costs no more than copying it.
        const std::string& rateLimitedReply() {
            static const std::string reply = ZestDB::responseToJson({ ResultType::Code::RateLimited });
            return reply;
        }

        constexpr uint32_t DISCARD_CHUNK = 64 * 1024;
    } // namespace

    Session::Session(ZestStream stream, ZestDB& db, Limits limits, std::shared_ptr<std::atomic<size_t>> activeSessions)
        : stream_(std::move(stream)),
          timer_(std::visit([](auto& s) { return s.get_executor(); }, stream_)),
          heartbeat_(timer_.get_executor()),
          db_(db),
          limits_(limits),
          activeSessions_(std::move(activeSessions)) {
        this->activeSessions_->fetch_add(1);
    }

    Session::~Session() {
        if (this->subscription_) {
            this->db_.getReplicationLog().unsubscribe(this->subscription_);
        }
        if (!this->snapshotPath_.empty()) {
            std::error_code ec;
            this->snapshot_.close();
            std::filesystem::remove(this->snapshotPath_, ec);
        }
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
        if (this->replicating_) {
            this->deadline_ = std::chrono::steady_clock::time_point::max();
            return;
        }
        this->deadline_ = std::chrono::steady_clock::now() + (this->authenticated_ ? this->limits_.idleTimeout : this->limits_.authTimeout);
    }

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

    void Session::drain() {
        this->draining_ = true;
        // A request or the handshake is in progress: do_read_size() closes the session once it is answered
        if (!this->waitingForRequest_) {
            return;
        }
        if (this->write_queue_.empty()) {
            this->close_stream();
        } else {
            this->closing_ = true;
        }
    }

    void Session::closeIfReplica() {
        if (this->replicating_) {
            this->close_stream();
        }
    }

    void Session::revokeIfUser(const std::string& username) {
        if (this->authenticated_ && this->username_ == username) {
            this->drain();
        }
    }

    void Session::do_read_size() {
        if (this->draining_) {
            if (this->write_queue_.empty()) {
                this->close_stream();
            } else {
                this->closing_ = true;
            }
            return;
        }
        this->touch();
        this->waitingForRequest_ = true;
        auto self(this->shared_from_this());
        std::visit(
            [this, self](auto& stream) {
                asio::async_read(stream, this->buffer_, asio::transfer_exactly(4), [this, self](std::error_code ec, std::size_t length) {
                    this->waitingForRequest_ = false;
                    if (!ec && this->replicating_) {
                        ZestLog(LogLevel::Warning, std::format("Replication: {} sent a request on its replication link, closing it", this->remote_ip_));
                        this->close_stream();
                        return;
                    }
                    if (!ec && length == 4) {
                        std::istream is(&this->buffer_);
                        uint32_t payload_size;
                        is.read(reinterpret_cast<char*>(&payload_size), 4);
                        payload_size = ntohl(payload_size);
                        // Checked before reading the payload, so a client cannot make the server allocate gigabytes
                        // (the limit is lower before authentication)
                        if (payload_size > (this->authenticated_ ? MAX_PAYLOAD_SIZE : MAX_UNAUTHENTICATED_PAYLOAD_SIZE)) {
                            ZestLog(LogLevel::Warning, std::format("Session: payload of {} bytes refused", payload_size));
                            this->queue_write(ZestDB::responseToJson({ ResultType::Code::PayloadTooLarge }) + "\n", true);
                            return;
                        }
                        // Looked up for each request, so a `reload` that changes the user's limits applies at once.
                        // The bandwidth is checked before the payload is read, let alone allocated.
                        this->limiter_ = this->authenticated_ ? this->db_.limiterOf(this->username_) : nullptr;
                        if (this->limiter_ && !this->limiter_->allowBytes(4ULL + payload_size)) {
                            this->discard_request(payload_size);
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

                    // The tokens of the whole batch are taken at once: the first `allowed` commands run, the others are
                    // refused without being parsed. A batch sent before authentication is not limited, but it is capped
                    // at MAX_UNAUTHENTICATED_PAYLOAD_SIZE.
                    uint64_t allowed = UINT64_MAX;
                    if (this->limiter_) {
                        uint64_t commands = 0;
                        for (uint32_t pos = 0; payload_size - pos >= 4; commands++) {
                            uint32_t size;
                            std::memcpy(&size, &payload[pos], 4);
                            size = ntohl(size);
                            if (size > payload_size - pos - 4) {
                                break;
                            }
                            pos += 4 + size;
                        }
                        allowed = this->limiter_->allowCommands(commands);
                        this->limiter_->addRefused(commands - allowed);
                    }
                    uint64_t index = 0;

                    while (offset < payload_size && !should_close) {
                        if (offset + 4 > payload_size) {
                            final_result += (final_result.empty() ? "" : "\r\n\r\n") + ZestDB::responseToJson({ ResultType::Code::MalformedBatch });
                            break;
                        }

                        uint32_t cmd_size;
                        std::memcpy(&cmd_size, &payload[offset], 4);
                        cmd_size = ntohl(cmd_size);
                        offset += 4;

                        if (offset + cmd_size > payload_size) {
                            final_result += (final_result.empty() ? "" : "\r\n\r\n") + ZestDB::responseToJson({ ResultType::Code::MalformedBatch });
                            break;
                        }

                        std::string cmd_result = index++ < allowed ? this->handle_command(payload.substr(offset, cmd_size), should_close) : rateLimitedReply();
                        offset += cmd_size;

                        if (!final_result.empty()) {
                            final_result += "\r\n\r\n";
                        }
                        final_result += cmd_result;
                    }

                    // Group commit: every command of the batch appended to the WAL, one sync makes them all durable.
                    // A batch is never acknowledged before its writes are on disk.
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

    void Session::discard_request(uint32_t size) {
        this->discard_ = { .left = size };
        this->discard_next_chunk();
    }

    void Session::discard_next_chunk() {
        Discard& d = this->discard_;
        if (d.left == 0) {
            const bool malformed = d.malformed || d.header != 0 || d.skip != 0;
            this->limiter_->addRefused(d.commands);
            std::string reply;
            for (uint64_t i = 0; i < d.commands; i++) {
                reply += (reply.empty() ? "" : "\r\n\r\n") + rateLimitedReply();
            }
            if (malformed) {
                reply += (reply.empty() ? "" : "\r\n\r\n") + ZestDB::responseToJson({ ResultType::Code::MalformedBatch });
            }
            this->queue_write(reply + '\n');
            this->do_read_size();
            return;
        }

        const uint32_t chunk = std::min(d.left, DISCARD_CHUNK);
        auto self(this->shared_from_this());
        std::visit(
            [this, self, chunk](auto& stream) {
                asio::async_read(stream, this->buffer_, asio::transfer_exactly(chunk), [this, self, chunk](std::error_code ec, std::size_t length) {
                    if (ec || length != chunk) {
                        this->close_stream();
                        return;
                    }
                    Discard& state = this->discard_;
                    const auto* bytes = static_cast<const unsigned char*>(this->buffer_.data().data());
                    for (uint32_t i = 0; i < chunk;) {
                        if (state.skip > 0) {
                            const uint32_t n = std::min(state.skip, chunk - i);
                            state.skip -= n;
                            i += n;
                            continue;
                        }
                        state.length = (state.length << 8) | bytes[i++];
                        if (++state.header < 4) {
                            continue;
                        }
                        // The command's length is complete: the bytes after it in the request are what is left of this chunk
                        // and the chunks still to read
                        const uint32_t after = (chunk - i) + (state.left - chunk);
                        state.header = 0;
                        if (state.length > after || state.malformed) {
                            state.malformed = true;
                            state.skip = after;
                        } else {
                            state.commands++;
                            state.skip = state.length;
                        }
                        state.length = 0;
                    }
                    this->buffer_.consume(chunk);
                    state.left -= chunk;
                    this->discard_next_chunk();
                });
            },
            stream_);
    }

    std::string Session::handle_command(const std::string& cmd, bool& should_close) {
        if (!this->authenticated_) {
            const std::string authPrefix = "Authorization: ";
            const size_t dotPos = cmd.find('.', authPrefix.size());
            should_close = true;
            if (!cmd.starts_with(authPrefix) || dotPos == std::string::npos) {
                return ZestDB::responseToJson({ ResultType::Code::AuthFailed });
            }

            std::string username = cmd.substr(authPrefix.size(), dotPos - authPrefix.size());
            if (!this->db_.authAllowed(username)) {
                ZestLog(LogLevel::Warning, std::format("Session: user {} is locked out after too many failed authentications ({})", username, this->remote_ip_));
                return ZestDB::responseToJson({ ResultType::Code::AuthFailed });
            }
            const std::string token = cmd.substr(dotPos + 1);
            if (!this->db_.authenticate(username, token)) {
                this->db_.recordAuthResult(username, false);
                return ZestDB::responseToJson({ ResultType::Code::AuthFailed });
            }
            this->tokenHash_ = sha256(token);

            this->db_.recordAuthResult(username, true);
            this->authenticated_ = true;
            this->username_ = std::move(username);
            should_close = false;
            return ZestDB::responseToJson({ ResultType::Code::Success });
        }

        // The role is looked up on every command rather than kept from the login, so a `reload` that removes the user or
        // changes its role applies to the open session at once
        std::optional<Role> role = this->db_.roleOf(this->username_, this->tokenHash_);
        if (!role) {
            should_close = true;
            return ZestDB::responseToJson({ ResultType::Code::SessionRevoked });
        }

        if (cmd == "replicate" || cmd.starts_with("replicate ")) {
            return ZestDB::responseToJson(this->handle_replicate(cmd, *role));
        }

        try {
            return ZestDB::responseToJson(this->db_.execCmd(cmd, *role, false, this->username_));
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("Session: command failed with an exception: {}", e.what()));
            return ZestDB::responseToJson({ ResultType::Code::InternalError });
        }
    }

    ResultType Session::handle_replicate(const std::string& cmd, Role role) {
        if (role != Role::Replica || this->replicating_) {
            return { ResultType::Code::PermissionDenied };
        }
        std::istringstream in(cmd.substr(std::string_view("replicate").size()));
        std::string historyId;
        uint64_t fromSeq = 0;
        if (!(in >> historyId >> fromSeq) || !(in >> std::ws).eof()) {
            return { ResultType::Code::InvalidValueFormat };
        }
        bool stream = false;
        ResultType result = this->db_.replicationHandshake(this->remote_ip_, historyId, fromSeq, &stream);
        if (result.code == ResultType::Code::Success) {
            this->replicating_ = true;
            this->touch();
            // Posted, so what follows goes after the handshake reply, which is queued once the batch is done
            if (stream) {
                asio::post(this->executor(), [self = this->shared_from_this(), fromSeq] { self->start_stream(fromSeq); });
            } else {
                asio::post(this->executor(), [self = this->shared_from_this()] { self->start_fullsync(); });
            }
        }
        return result;
    }

    void Session::start_stream(uint64_t fromSeq) {
        this->cursor_ = fromSeq;
        this->streaming_ = true;
        std::weak_ptr<Session> weak = this->shared_from_this();
        // Called by every write: one post at most until the session has sent what is pending
        this->subscription_ = this->db_.getReplicationLog().subscribe([weak] {
            if (auto session = weak.lock(); session && !session->wakePending_.exchange(true)) {
                asio::post(session->executor(), [session] {
                    session->wakePending_ = false;
                    session->send_events();
                });
            }
        });
        this->send_events();
        this->schedule_heartbeat();
    }

    void Session::start_fullsync() {
        // shortcut: archived on the IO thread, which stalls its other sessions meanwhile (writes are blocked for all of
        // them anyway while the snapshot is taken); move it to a worker if large databases make that noticeable
        std::filesystem::path zip;
        uint64_t seq = 0;
        if (!this->db_.createReplicaSnapshot(zip, seq)) {
            this->close_stream();
            return;
        }
        this->snapshotPath_ = zip;
        this->snapshotSeq_ = seq;
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(zip, ec);
        this->snapshot_.open(zip, std::ios::binary);
        if (ec || !this->snapshot_) {
            ZestLog(LogLevel::Error, std::format("Replication: cannot read the snapshot {}", zip.string()));
            this->close_stream();
            return;
        }
        ZestLog(LogLevel::Info, std::format("Replication: sending a snapshot of {} bytes to {}, then the writes from {}", bytes, this->remote_ip_, seq));
        const std::string item = std::format("{} snapshot {}", seq, bytes);
        const uint32_t itemSize = htonl(static_cast<uint32_t>(item.size()));
        const uint32_t frameSize = htonl(static_cast<uint32_t>(item.size() + 4));
        std::string frame(reinterpret_cast<const char*>(&frameSize), 4);
        frame.append(reinterpret_cast<const char*>(&itemSize), 4);
        frame += item;
        this->queue_write(frame);
    }

    void Session::send_snapshot_chunk() {
        std::string chunk(SNAPSHOT_CHUNK, '\0');
        this->snapshot_.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        chunk.resize(static_cast<size_t>(this->snapshot_.gcount()));
        if (!chunk.empty()) {
            this->queue_write(chunk);
            return;
        }
        this->snapshot_.close();
        std::error_code ec;
        std::filesystem::remove(this->snapshotPath_, ec);
        this->snapshotPath_.clear();
        this->start_stream(this->snapshotSeq_);
    }

    void Session::send_events() {
        if (!this->streaming_ || !this->write_queue_.empty()) {
            return;
        }
        std::vector<ReplicationEvent> events;
        if (!this->db_.getReplicationLog().since(this->cursor_, events, MAX_EVENTS_PER_FRAME)) {
            ZestLog(LogLevel::Warning, std::format("Replication: {} fell more than historySize writes behind, closing its link (it will need a full sync)", this->remote_ip_));
            this->close_stream();
            return;
        }
        if (events.empty()) {
            return;
        }
        // Same framing as a request: a frame of length-prefixed items, each item `<seq> <command>`
        std::string frame(4, '\0');
        for (const ReplicationEvent& e : events) {
            const std::string item = std::format("{} {}", e.seq, e.cmd);
            const uint32_t size = htonl(static_cast<uint32_t>(item.size()));
            frame.append(reinterpret_cast<const char*>(&size), 4);
            frame += item;
        }
        const uint32_t total = htonl(static_cast<uint32_t>(frame.size() - 4));
        std::memcpy(frame.data(), &total, 4);
        this->cursor_ = events.back().seq + 1;
        this->queue_write(frame);
    }

    void Session::schedule_heartbeat() {
        this->heartbeat_.expires_after(HEARTBEAT_INTERVAL);
        this->heartbeat_.async_wait([self = this->shared_from_this()](std::error_code ec) {
            if (ec || !self->streaming_) {
                return;
            }
            if (self->write_queue_.empty()) {
                self->queue_write(std::string(4, '\0'));
            }
            self->schedule_heartbeat();
        });
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
                        } else if (this->snapshot_.is_open()) {
                            this->send_snapshot_chunk();
                        } else if (this->streaming_) {
                            this->send_events();
                        }
                    } else {
                        this->close_stream();
                    }
                });
            },
            stream_);
    }

    void Session::close_stream() {
        this->streaming_ = false;
        this->timer_.cancel();
        this->heartbeat_.cancel();
        std::visit(
            [](auto& s) {
                std::error_code ec;
                s.lowest_layer().close(ec);
            },
            this->stream_);
    }

    void Server::closeReplicas() {
        std::lock_guard<std::mutex> lock(this->sessionsMtx_);
        for (auto& weak : this->sessions_) {
            if (auto session = weak.lock()) {
                asio::post(session->executor(), [session] { session->closeIfReplica(); });
            }
        }
    }

    void Server::revokeUser(const std::string& username) {
        std::lock_guard<std::mutex> lock(this->sessionsMtx_);
        for (auto& weak : this->sessions_) {
            if (auto session = weak.lock()) {
                // The session checks its user on its own thread, where its members may be read
                asio::post(session->executor(), [session, username] { session->revokeIfUser(username); });
            }
        }
    }

    std::vector<std::unique_ptr<asio::io_context>> Server::makeWorkers() {
        std::vector<std::unique_ptr<asio::io_context>> workers;
        const unsigned int threads = std::max(1u, std::thread::hardware_concurrency());
        workers.reserve(threads);
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
          useSSL_(db.getSettings().useSSL),
          maxConnections_(db.getSettings().MaxConnections),
          limits_{ std::chrono::seconds(db.getSettings().AuthTimeout), std::chrono::seconds(db.getSettings().IdleTimeout) } {
        for (auto& worker : this->workers_) {
            this->workGuards_.push_back(asio::make_work_guard(*worker));
        }
        ZestLog(LogLevel::Info, std::format("Server: listening on port {} with {} IO threads", port, this->workers_.size()));
        if (this->useSSL_) {
            ssl_context_.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
            ssl_context_.use_certificate_chain_file(this->db_.getSettings().SSLCertPath.string());
            ssl_context_.use_private_key_file(this->db_.getSettings().SSLKeyPath.string(), asio::ssl::context::pem);
        }
        this->do_accept();
        for (auto& worker : this->workers_) {
            std::promise<void> finished;
            this->workersDone_.push_back(finished.get_future());
            this->workerThreads_.emplace_back([&worker, done = std::move(finished)]() mutable {
                worker->run();
                done.set_value();
            });
        }
    }

    Server::~Server() {
        try {
            this->stop();
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("Server: stop failed while destroying the server: {}", e.what()));
        }
    }

    void Server::stop() {
        if (this->stopped_.exchange(true)) {
            return;
        }
        std::promise<void> closed;
        asio::post(this->acceptor_.get_executor(), [this, &closed] {
            std::error_code ec;
            static_cast<void>(this->acceptor_.close(ec));
            if (ec) {
                ZestLog(LogLevel::Error, std::format("Server: stop failed while destroying the server: {}", ec.message()));
            }
            this->accept_retry_timer_.cancel();
            closed.set_value();
        });
        closed.get_future().wait();

        // Ask every session to finish its current request and close. Resetting the work guards lets each worker's run()
        // return once its sessions are closed, which is what the futures below wait for.
        size_t open = 0;
        {
            std::lock_guard<std::mutex> lock(this->sessionsMtx_);
            for (auto& weak : this->sessions_) {
                if (auto session = weak.lock()) {
                    open++;
                    asio::post(session->executor(), [session] { session->drain(); });
                }
            }
            this->sessions_.clear();
        }
        for (auto& guard : this->workGuards_) {
            guard.reset();
        }
        if (open > 0) {
            ZestLog(LogLevel::Info, std::format("Server: waiting for {} sessions to send their last reply", open));
        }
        const auto deadline = std::chrono::steady_clock::now() + DRAIN_TIMEOUT;
        bool drained = true;
        for (auto& done : this->workersDone_) {
            drained = done.wait_until(deadline) == std::future_status::ready && drained;
        }
        if (!drained) {
            ZestLog(LogLevel::Warning, std::format("Server: sessions still busy after {}s, closing them", DRAIN_TIMEOUT.count()));
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
                static_cast<void>(socket.close(ignored));
            } else {
                auto executor = socket.get_executor();
                std::shared_ptr<Session> session;
                if (this->useSSL_) {
                    session = std::make_shared<Session>(asio::ssl::stream<tcp::socket>(std::move(socket), this->ssl_context_), this->db_, this->limits_, this->activeSessions_);
                } else {
                    session = std::make_shared<Session>(std::move(socket), this->db_, this->limits_, this->activeSessions_);
                }
                {
                    // Pruned on each connection, so the list does not keep growing with closed sessions
                    std::lock_guard<std::mutex> lock(this->sessionsMtx_);
                    std::erase_if(this->sessions_, [](const std::weak_ptr<Session>& weak) { return weak.expired(); });
                    this->sessions_.push_back(session);
                }
                asio::dispatch(executor, [session] { session->start(); });
            }
            this->do_accept();
        });
    }

} // namespace Zest
