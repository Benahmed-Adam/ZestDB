#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace Zest {

    using asio::ip::tcp;

    class ZestDB;

    /// A client connection, plain or TLS; the session code is written once for both through std::visit.
    using ZestStream = std::variant<tcp::socket, asio::ssl::stream<tcp::socket>>;

    /// One client connection. It reads length-prefixed batches, runs their commands with ZestDB::execCmd and writes back
    /// one JSON reply per command.
    ///
    /// A session lives on one IO thread: its handlers never run concurrently, so its members need no lock. It keeps
    /// itself alive through the shared_ptr captured by each pending asynchronous operation, and is destroyed once none is left.
    class Session : public std::enable_shared_from_this<Session> {
    public:
        /// How long a client may stay silent: before authenticating, then once authenticated.
        struct Limits {
            std::chrono::seconds authTimeout;
            std::chrono::seconds idleTimeout;
        };

        /// `activeSessions` counts the open sessions for MaxConnections; this session adds itself and removes itself.
        Session(ZestStream stream, ZestDB& db, Limits limits, std::shared_ptr<std::atomic<size_t>> activeSessions);
        ~Session();
        /// Checks the client's address, starts the inactivity timer, runs the TLS handshake if needed, then reads.
        void start();
        /// Server shutdown: an idle session closes now; a session processing a request finishes it, sends the reply,
        /// then closes instead of reading the next one. Must run on the session's executor.
        void drain();

        /// The executor (IO thread) the session runs on.
        asio::any_io_executor executor() {
            return this->timer_.get_executor();
        }

    private:
        /// Reads the 4-byte length of the next request (or closes the session while draining).
        void do_read_size();
        /// Reads a request of `size` bytes, runs each of its commands, syncs the WAL once, then sends all the replies.
        void do_read_command(uint32_t size);
        /// Runs one command. Before authentication, only `Authorization: <user>.<token>` is accepted. `should_close` is
        /// set when the connection must be closed after the reply (failed authentication, revoked user).
        std::string handle_command(const std::string& cmd, bool& should_close);

        /// Pushes the inactivity deadline back (AuthTimeout before authentication, IdleTimeout after).
        void touch();
        /// Closes the session once the deadline passes; re-arms itself when touch() moved the deadline.
        void watch_deadline();
        /// Queues a reply and starts writing if no write is in progress (asio allows one write at a time per stream).
        /// `closeAfter` closes the connection once everything queued has been sent.
        void queue_write(const std::string& message, bool closeAfter = false);
        /// Writes the first queued reply, then the next ones in turn.
        void do_write();

        /// Cancels the timer and closes the socket, which aborts the pending reads and writes.
        void close_stream();

        static constexpr uint32_t MAX_UNAUTHENTICATED_PAYLOAD_SIZE = 1024 * 1024;
        static constexpr uint32_t MAX_PAYLOAD_SIZE = 64 * 1024 * 1024;

        ZestStream stream_;
        asio::steady_timer timer_;
        std::chrono::steady_clock::time_point deadline_;
        asio::streambuf buffer_;
        ZestDB& db_;
        Limits limits_;
        std::shared_ptr<std::atomic<size_t>> activeSessions_;

        std::string remote_ip_;
        std::string username_;
        bool authenticated_ = false;

        std::deque<std::string> write_queue_;
        bool closing_ = false;           // close once the queue is written
        bool waitingForRequest_ = false; // idle: waiting for the length of the next request
        bool draining_ = false;          // the server is stopping
    };

    /// The TCP server of `DBPort`. It runs one io_context per CPU core, each on its own thread; every new connection is
    /// given to the next one in turn and stays on it.
    class Server {
    public:
        /// Binds the port, loads the TLS certificate when `useSSL` is set, then starts accepting on the IO threads.
        Server(asio::io_context& io_context, uint16_t port, ZestDB& db);
        ~Server();
        /// Stops accepting, lets every session answer the request it is processing (DRAIN_TIMEOUT at most), closes the
        /// rest and joins the IO threads. Does nothing the second time.
        void stop();

    private:
        /// Accepts the next connection on the next IO thread, refusing it above MaxConnections; retries after an error.
        void do_accept();
        /// One io_context per hardware thread.
        static std::vector<std::unique_ptr<asio::io_context>> makeWorkers();

        static constexpr std::chrono::seconds DRAIN_TIMEOUT{ 5 };

        std::vector<std::unique_ptr<asio::io_context>> workers_;
        std::vector<asio::executor_work_guard<asio::io_context::executor_type>> workGuards_;
        std::vector<std::thread> workerThreads_;
        std::vector<std::future<void>> workersDone_; // ready once a worker's run() returned
        std::mutex sessionsMtx_;
        std::vector<std::weak_ptr<Session>> sessions_; // to drain them on stop; weak so they still die on their own
        size_t nextWorker_ = 0;
        std::atomic<bool> stopped_ = false;

        asio::ssl::context ssl_context_;
        tcp::acceptor acceptor_;
        asio::steady_timer accept_retry_timer_;
        ZestDB& db_;

        bool useSSL_;
        size_t maxConnections_;
        Session::Limits limits_;
        std::shared_ptr<std::atomic<size_t>> activeSessions_ = std::make_shared<std::atomic<size_t>>(0);
    };

} // namespace Zest
