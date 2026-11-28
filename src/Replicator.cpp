#include "Replicator.hpp"

#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "lib/json.hpp"

namespace Zest {

    namespace fs = std::filesystem;

    namespace {
        std::string be32(uint32_t v) {
            v = htonl(v);
            return std::string(reinterpret_cast<const char*>(&v), 4);
        }

        uint32_t readBe32(const std::string& bytes, size_t pos) {
            uint32_t v;
            std::memcpy(&v, bytes.data() + pos, 4);
            return ntohl(v);
        }

        /// `<seq> <rest>`, as the primary sends each item.
        std::pair<uint64_t, std::string> splitItem(const std::string& item) {
            const size_t space = item.find(' ');
            if (space == std::string::npos || space == 0) {
                throw std::runtime_error("malformed replication item");
            }
            return { std::stoull(item.substr(0, space)), item.substr(space + 1) };
        }
    } // namespace

    Replicator::Replicator(ZestDB& database, const Settings& settings)
        : db(database),
          config(settings.replication),
          useSSL(settings.useSSL),
          statePath(settings.DbPath / "replication.state"),
          incomingPath(settings.ArchiveStoragePath / ".replication" / "incoming.zip") {
        if (this->useSSL) {
            if (this->config.primaryCA.empty()) {
                ZestLog(LogLevel::Warning, "Replication: no primaryCA, the primary's certificate is not checked");
                this->sslContext.set_verify_mode(asio::ssl::verify_none);
            } else {
                this->sslContext.load_verify_file(this->config.primaryCA);
                this->sslContext.set_verify_mode(asio::ssl::verify_peer);
            }
        }
        this->loadPosition();
        this->thread = std::jthread([this](const std::stop_token& stop) { this->run(stop); });
    }

    Replicator::~Replicator() {
        this->stop();
    }

    void Replicator::stop() {
        if (!this->thread.joinable()) {
            return;
        }
        this->thread.request_stop();
        // A blocked read returns once the socket is closed, on the replicator's thread
        asio::post(this->io, [this] { this->closeLink(); });
        this->sleepCV.notify_all();
        this->thread.join();
    }

    void Replicator::run(const std::stop_token& stop) {
        auto backoff = std::chrono::seconds(1);
        while (!stop.stop_requested()) {
            try {
                this->replicate(stop);
            } catch (const std::exception& e) {
                if (!stop.stop_requested()) {
                    ZestLog(LogLevel::Warning, std::format("Replication: link to {}:{} lost ({}), retrying in {}s", this->config.primaryHost, this->config.primaryPort, e.what(), backoff.count()));
                }
            }
            if (this->isConnected.exchange(false)) {
                backoff = std::chrono::seconds(1); // it worked for a while: retry soon
            }
            this->closeLink();
            std::unique_lock<std::mutex> lock(this->sleepMtx);
            this->sleepCV.wait_for(lock, stop, backoff, [] { return false; });
            backoff = std::min(backoff * 2, MAX_BACKOFF);
        }
    }

    void Replicator::replicate(const std::stop_token& stop) {
        this->connect();
        this->request(std::format("Authorization: {}.{}", this->config.user, sha256(this->config.user + this->config.password)));
        const auto reply = nlohmann::json::parse(this->request(std::format("replicate {} {}", this->historyId, this->position.load())));
        const std::string mode = reply.at("mode");
        this->historyId = reply.at("historyId");
        this->isConnected = true;
        ZestLog(LogLevel::Info, std::format("Replication: connected to {}:{}, {}", this->config.primaryHost, this->config.primaryPort, mode == "stream" ? std::format("streaming from {}", this->position.load()) : "full sync"));

        if (mode != "stream") {
            const auto header = this->readFrame(SNAPSHOT_TIMEOUT);
            if (header.size() != 1 || !header.front().second.starts_with("snapshot ")) {
                throw std::runtime_error("expected a snapshot header");
            }
            this->fullSync(header.front());
        }
        while (!stop.stop_requested()) {
            const auto items = this->readFrame(LINK_TIMEOUT);
            if (!items.empty()) {
                this->apply(items);
            }
        }
    }

    void Replicator::fullSync(const Item& header) {
        const uint64_t bytes = std::stoull(header.second.substr(std::string_view("snapshot ").size()));
        fs::create_directories(this->incomingPath.parent_path());
        {
            std::ofstream out(this->incomingPath, std::ios::binary | std::ios::trunc);
            for (uint64_t left = bytes; left > 0;) {
                const size_t chunk = static_cast<size_t>(std::min<uint64_t>(left, uint64_t{ 1024 } * 1024));
                const std::string data = this->readExact(chunk, LINK_TIMEOUT);
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                left -= chunk;
            }
            if (!out.flush()) {
                throw std::runtime_error("cannot write the snapshot to " + this->incomingPath.string());
            }
        }
        const ResultType restored = this->db.restoreArchive(this->incomingPath);
        std::error_code ec;
        fs::remove(this->incomingPath, ec);
        if (restored.code != ResultType::Code::Success) {
            throw std::runtime_error(std::format("the snapshot could not be restored (code {})", static_cast<int>(restored.code)));
        }
        this->position = header.first;
        this->savePosition();
        ZestLog(LogLevel::Info, std::format("Replication: restored a snapshot of {} bytes, streaming from {}", bytes, header.first));
    }

    void Replicator::apply(const std::vector<Item>& items) {
        for (const auto& [seq, command] : items) {
            const ResultType result = this->db.applyReplicated(command);
            // A delete or a TTL change of a key that is already gone changes nothing: the states agree
            if (result.code != ResultType::Code::Success && result.code != ResultType::Code::KeyNotFound) {
                ZestLog(LogLevel::Error, std::format("Replication: write {} failed with code {}: {}", seq, static_cast<int>(result.code), command));
            }
        }
        if (!this->db.syncWAL()) {
            throw std::runtime_error("the WAL could not be synced");
        }
        this->position = items.back().first + 1;
        this->savePosition();
    }

    void Replicator::connect() {
        asio::ip::tcp::resolver resolver(this->io);
        asio::ip::tcp::resolver::results_type endpoints;
        this->wait(
            [&](auto handler) {
                resolver.async_resolve(this->config.primaryHost, std::to_string(this->config.primaryPort), [&endpoints, handler](std::error_code ec, auto results) mutable {
                    endpoints = std::move(results);
                    handler(ec, 0);
                });
            },
            LINK_TIMEOUT);
        asio::ip::tcp::socket socket(this->io);
        this->wait([&](auto handler) { asio::async_connect(socket, endpoints, [handler](std::error_code ec, const auto&) mutable { handler(ec, 0); }); }, LINK_TIMEOUT);
        this->pending.clear();
        if (!this->useSSL) {
            this->stream.emplace(std::move(socket));
            return;
        }
        this->stream.emplace(std::in_place_type<asio::ssl::stream<asio::ip::tcp::socket>>, std::move(socket), this->sslContext);
        auto& tls = std::get<asio::ssl::stream<asio::ip::tcp::socket>>(*this->stream);
        if (!this->config.primaryCA.empty()) {
            tls.set_verify_callback(asio::ssl::host_name_verification(this->config.primaryHost));
        }
        SSL_set_tlsext_host_name(tls.native_handle(), this->config.primaryHost.c_str());
        this->wait([&](auto handler) { tls.async_handshake(asio::ssl::stream_base::client, [handler](std::error_code ec) mutable { handler(ec, 0); }); }, LINK_TIMEOUT);
    }

    std::string Replicator::request(const std::string& command) {
        const std::string item = be32(static_cast<uint32_t>(command.size())) + command;
        this->write(be32(static_cast<uint32_t>(item.size())) + item);
        const auto reply = nlohmann::json::parse(this->readLine(LINK_TIMEOUT));
        if (reply.at("code") != 0) {
            throw std::runtime_error(std::format("`{}` refused with code {}", command.substr(0, command.find(' ')), reply.at("code").get<int>()));
        }
        const auto& response = reply.at("response");
        return response.is_string() ? response.get<std::string>() : response.dump();
    }

    std::vector<Replicator::Item> Replicator::readFrame(std::chrono::seconds timeout) {
        const uint32_t size = readBe32(this->readExact(4, timeout), 0);
        if (size > MAX_FRAME_SIZE) {
            throw std::runtime_error(std::format("frame of {} bytes refused", size));
        }
        const std::string frame = this->readExact(size, timeout);
        std::vector<Item> items;
        for (size_t pos = 0; pos < frame.size();) {
            if (frame.size() - pos < 4 || readBe32(frame, pos) > frame.size() - pos - 4) {
                throw std::runtime_error("malformed replication frame");
            }
            const uint32_t itemSize = readBe32(frame, pos);
            items.push_back(splitItem(frame.substr(pos + 4, itemSize)));
            pos += 4 + itemSize;
        }
        return items;
    }

    std::string Replicator::readLine(std::chrono::seconds timeout) {
        while (this->pending.find('\n') == std::string::npos) {
            char buf[4096];
            const size_t got = this->wait([&](auto handler) { std::visit([&](auto& s) { s.async_read_some(asio::buffer(buf), handler); }, *this->stream); }, timeout);
            this->pending.append(buf, got);
        }
        const size_t end = this->pending.find('\n') + 1;
        std::string line = this->pending.substr(0, end);
        this->pending.erase(0, end);
        return line;
    }

    std::string Replicator::readExact(size_t n, std::chrono::seconds timeout) {
        std::string out = this->pending.substr(0, std::min(n, this->pending.size()));
        this->pending.erase(0, out.size());
        if (out.size() < n) {
            const size_t have = out.size();
            out.resize(n);
            this->wait([&](auto handler) { std::visit([&](auto& s) { asio::async_read(s, asio::buffer(out.data() + have, n - have), handler); }, *this->stream); }, timeout);
        }
        return out;
    }

    void Replicator::write(const std::string& bytes) {
        this->wait([&](auto handler) { std::visit([&](auto& s) { asio::async_write(s, asio::buffer(bytes), handler); }, *this->stream); }, LINK_TIMEOUT);
    }

    template <typename Start>
    size_t Replicator::wait(Start&& start, std::chrono::seconds timeout) {
        std::optional<std::error_code> result;
        size_t bytes = 0;
        start([&result, &bytes](std::error_code ec, size_t n) {
            result = ec;
            bytes = n;
        });
        this->io.restart();
        this->io.run_for(timeout);
        if (!result) {
            // Timed out: closing the socket makes the operation complete with an error, so nothing refers to `result` after this
            this->closeLink();
            this->io.restart();
            this->io.run();
            throw std::runtime_error("timed out");
        }
        if (*result) {
            throw std::system_error(*result);
        }
        return bytes;
    }

    void Replicator::closeLink() {
        if (this->stream) {
            std::visit(
                [](auto& s) {
                    std::error_code ec;
                    s.lowest_layer().close(ec);
                },
                *this->stream);
        }
    }

    void Replicator::loadPosition() {
        std::ifstream in(this->statePath);
        std::string id;
        uint64_t seq = 0;
        if (in >> id >> seq) {
            this->historyId = id;
            this->position = seq;
        }
    }

    void Replicator::savePosition() {
        const fs::path tmp = this->statePath.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << this->historyId << ' ' << this->position.load() << '\n';
            if (!out.flush()) {
                throw std::runtime_error("cannot write " + tmp.string());
            }
        }
        fs::rename(tmp, this->statePath);
    }

} // namespace Zest
