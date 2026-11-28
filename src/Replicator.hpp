#pragma once

#include <asio.hpp>
#include <asio/ssl.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    class ZestDB;

    /// The replica side of replication: one thread that connects to the primary, logs in as the replica user, sends
    /// the handshake, restores the snapshot of a full sync if it gets one, then applies the stream of writes. On any
    /// error it closes the link and tries again later; the database keeps serving reads meanwhile.
    ///
    /// Its position, the primary's historyId and the next sequence number, is kept in `<DbPath>/replication.state`. It
    /// is written after each frame, once the local WAL holds its writes, so a restart resumes the stream. A write
    /// applied twice after a crash does no harm: every replicated command sets an absolute state.
    class Replicator {
    public:
        Replicator(ZestDB& db, const Settings& settings);
        /// Calls stop().
        ~Replicator();
        /// Closes the link and joins the thread. Safe to call twice.
        void stop();

        /// True while the link is up, past its handshake.
        bool connected() const {
            return this->isConnected.load();
        }

        /// The sequence number of the next write it expects from the primary.
        uint64_t nextSeq() const {
            return this->position.load();
        }

    private:
        using Stream = std::variant<asio::ip::tcp::socket, asio::ssl::stream<asio::ip::tcp::socket>>;
        using Item = std::pair<uint64_t, std::string>;

        /// Connects, replicates until the link fails, waits, and again, until stop().
        void run(const std::stop_token& stop);
        /// One link, from the connection to its failure (an exception).
        void replicate(const std::stop_token& stop);
        /// Restores the snapshot that follows the header item of a full sync, then sets the position to its sequence number.
        void fullSync(const Item& header);
        /// Runs the writes of one frame, syncs the WAL, then saves the position.
        void apply(const std::vector<Item>& items);

        void connect();
        /// Sends one command as a request and returns the JSON object of its reply; throws unless its code is Success.
        std::string request(const std::string& command);
        /// The items of the next frame (none for a heartbeat).
        std::vector<Item> readFrame(std::chrono::seconds timeout);
        std::string readLine(std::chrono::seconds timeout);
        std::string readExact(size_t n, std::chrono::seconds timeout);
        void write(const std::string& bytes);

        /// Runs the asynchronous operation `start` begins, for `timeout` at most. Throws on error or timeout.
        template <typename Start>
        size_t wait(Start&& start, std::chrono::seconds timeout);
        /// Closes the socket; runs on this thread, or posted to `io` by stop().
        void closeLink();

        void loadPosition();
        void savePosition();

        static constexpr std::chrono::seconds LINK_TIMEOUT{ 5 };       // the primary sends a heartbeat every second
        static constexpr std::chrono::seconds SNAPSHOT_TIMEOUT{ 600 }; // the primary archives its database first
        static constexpr std::chrono::seconds MAX_BACKOFF{ 30 };
        static constexpr uint32_t MAX_FRAME_SIZE = 512 * 1024 * 1024;

        ZestDB& db;
        const ReplicationSettings config;
        const bool useSSL;
        const std::filesystem::path statePath;
        const std::filesystem::path incomingPath; // where the snapshot of a full sync is written

        asio::io_context io;
        asio::ssl::context sslContext{ asio::ssl::context::tls_client };
        std::optional<Stream> stream;
        std::string pending; // bytes read and not consumed yet

        std::string historyId = "-";
        std::atomic<uint64_t> position{ 0 };
        std::atomic<bool> isConnected{ false };

        std::mutex sleepMtx;
        std::condition_variable_any sleepCV;
        std::jthread thread; // last: started once every member above is ready
    };

} // namespace Zest
