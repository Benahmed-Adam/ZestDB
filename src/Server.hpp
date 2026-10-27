#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <atomic>
#include <chrono>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace Zest {

    using asio::ip::tcp;

    class ZestDB;

    using ZestStream = std::variant<tcp::socket, asio::ssl::stream<tcp::socket>>;

    class Session : public std::enable_shared_from_this<Session> {
    public:
        struct Limits {
            std::chrono::seconds authTimeout;
            std::chrono::seconds idleTimeout;
        };

        Session(ZestStream stream, ZestDB& db, Limits limits, std::shared_ptr<std::atomic<size_t>> activeSessions);
        ~Session();
        void start();

    private:
        void do_read_size();
        void do_read_command(uint32_t size);
        std::string handle_command(const std::string& cmd, bool& should_close);

        void touch();
        void watch_deadline();
        void queue_write(const std::string& message, bool closeAfter = false);
        void do_write();

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
        bool closing_ = false;
    };

    class Server {
    public:
        Server(asio::io_context& io_context, uint16_t port, ZestDB& db);
        ~Server();
        void stop();

    private:
        void do_accept();
        static std::vector<std::unique_ptr<asio::io_context>> makeWorkers();

        std::vector<std::unique_ptr<asio::io_context>> workers_;
        std::vector<asio::executor_work_guard<asio::io_context::executor_type>> workGuards_;
        std::vector<std::thread> workerThreads_;
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
