#include "ZestDB.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <libzippp/libzippp.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#include "IndexManager.hpp"
#include "Logger.hpp"
#include "lib/json.hpp"
#include "lib/node.hpp"

namespace Zest {

    namespace fs = std::filesystem;

    using json = nlohmann::json;

    std::string ZestDB::responseToJson(const ResultType& resp) {
        std::string out = "{\"affectedRows\":";
        out += std::to_string(resp.affectedRows);
        out += ",\"code\":";
        out += std::to_string(static_cast<int>(resp.code));
        out += ",\"response\":";
        const bool looksLikeJson = !resp.response.empty() && (resp.response.front() == '[' || resp.response.front() == '{');
        if (looksLikeJson && json::accept(resp.response)) {
            out += resp.response;
        } else {
            out += json(resp.response).dump(-1, ' ', false, json::error_handler_t::replace);
        }
        out += '}';
        return out;
    }

    namespace {
        thread_local bool applyingReplicated = false; // this thread runs a write received from the primary
    } // namespace

    ZestDB::ZestDB()
        : ZestDB(fs::current_path()) {
    }

    ZestDB::ZestDB(fs::path configFolder)
        : configDir(std::move(configFolder)),
          replaying(false) {
        ZestLog(LogLevel::Info, "Initializing ZestDB...");
        this->boot();

        this->shardManager = std::make_unique<ShardManager>(this->settings, NUM_SHARDS);
        this->scriptEngine = std::make_unique<ScriptEngine>(*this, this->settings.DbPath / "scripts");
        this->scriptEngine->loadAll();

        this->replayWAL();
        this->socket = std::make_unique<Server>(this->ioCtx, this->settings.DBPort, *this);
        if (this->settings.replication.replica) {
            this->replicator = std::make_unique<Replicator>(*this, this->settings);
        }

        ZestLog(LogLevel::Info, std::format("ZestDB initialized with {} shards", NUM_SHARDS));

        this->flushThread = std::jthread([this](const std::stop_token& stopToken) {
            std::unique_lock<std::mutex> lock(this->flushThreadMtx);

            while (this->isRunning() && !stopToken.stop_requested()) {
                unsigned int interval;
                {
                    std::shared_lock<std::shared_mutex> settingsLock(this->writeMtx);
                    interval = this->settings.FlushInterval;
                }
                this->threadCV.wait_for(lock, stopToken, std::chrono::seconds(interval), [this, stopToken] { return stopToken.stop_requested() || !this->isRunning(); });

                if (stopToken.stop_requested() || !this->isRunning()) {
                    break;
                }

                lock.unlock();
                this->flush();
                lock.lock();
            }
        });

        this->saveThread = std::jthread([this](const std::stop_token& stopToken) {
            std::unique_lock<std::mutex> lock(this->saveThreadMtx);

            // Copied under writeMtx: a `reload` may replace the settings while this thread starts
            unsigned int archiveCreationDelay;
            fs::path archiveDir;
            {
                std::shared_lock<std::shared_mutex> settingsLock(this->writeMtx);
                archiveCreationDelay = this->settings.ArchiveCreationDelay;
                archiveDir = this->settings.ArchiveStoragePath;
            }
            unsigned int delay = 0;
            std::optional<fs::file_time_type> most_recent_time;

            if (!fs::exists(archiveDir) || !fs::is_directory(archiveDir)) {
                ZestLog(LogLevel::Warning, "Archive storage path does not exist or is not a directory.");
                fs::create_directories(archiveDir);
            }

            for (const auto& entry : fs::directory_iterator(archiveDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".zip") {
                    try {
                        auto current_time = fs::last_write_time(entry.path());
                        if (!most_recent_time.has_value() || current_time > *most_recent_time) {
                            most_recent_time = current_time;
                        }
                    } catch (const fs::filesystem_error&) {
                        ZestLog(LogLevel::Warning, "Failed to get last write time for file: " + entry.path().string());
                    }
                }
            }

            // After a restart, wait only for what is left of the delay since the most recent archive
            if (most_recent_time.has_value()) {
                auto most_recent_system = std::chrono::clock_cast<std::chrono::system_clock>(*most_recent_time);

                auto now = std::chrono::system_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - most_recent_system).count();

                if (elapsed >= 0 && static_cast<unsigned int>(elapsed) < archiveCreationDelay) {
                    delay = archiveCreationDelay - static_cast<unsigned int>(elapsed);
                }
            }

            while (this->isRunning() && !stopToken.stop_requested()) {

                this->threadCV.wait_for(lock, stopToken, std::chrono::seconds(delay), [this, stopToken] { return stopToken.stop_requested() || !this->isRunning(); });

                if (stopToken.stop_requested() || !this->isRunning()) {
                    break;
                }

                bool autoSave;
                {
                    std::shared_lock<std::shared_mutex> settingsLock(this->writeMtx);
                    autoSave = this->settings.AutoArchiveSaving;
                    delay = this->settings.ArchiveCreationDelay;
                }

                if (autoSave) {
                    lock.unlock();
                    ZestLog(LogLevel::Info, this->createArchive() ? "Autosave completed" : "An error occured during zip creation");
                    lock.lock();
                }
            }
        });

        if (this->settings.useSSL) {
            this->srv = std::make_unique<httplib::SSLServer>(this->settings.SSLCertPath.string().c_str(), this->settings.SSLKeyPath.string().c_str());
            ZestLog(LogLevel::Info, "Server mode: Encrypted");
        } else {
            this->srv = std::make_unique<httplib::Server>();
            ZestLog(LogLevel::Info, "Server mode: Plain");
        }

        this->srv->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            if (this->isAddressAllowed(req.remote_addr)) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            res.status = 403;
            return httplib::Server::HandlerResponse::Handled;
        });

        this->srv->Get("/", [this](const httplib::Request&, httplib::Response& res) {
            try {
                std::shared_lock<std::shared_mutex> lock(this->writeMtx);
                res.status = 200;

                json result = json::object();

                const auto& shards = this->shardManager->getShards();
                for (size_t i = 0; i < shards.size(); ++i) {
                    std::string shardKey = "shard" + std::to_string(i);
                    result["stats"][shardKey] = shards[i]->getPerfMonitoring().getPerformances();
                }

                res.set_content(result.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
            } catch (const std::exception& e) {
                ZestLog(LogLevel::Critical, e.what());
            }
        });

        this->srv->Get("/metrics", [this](const httplib::Request&, httplib::Response& res) {
            try {
                res.set_content(this->prometheusMetrics(), "text/plain; version=0.0.4; charset=utf-8");
            } catch (const std::exception& e) {
                res.status = 500;
                ZestLog(LogLevel::Critical, e.what());
            }
        });
    }

    ZestDB::~ZestDB() {
        if (this->isRunning())
            this->stop();
    }

    bool ZestDB::isRunning() const {
        return this->running.load();
    }

    void ZestDB::stop() {
        if (!this->running.exchange(false)) {
            return;
        }
        ZestLog(LogLevel::Info, "Exiting ZestDB...");

        this->ioCtx.stop();
        if (this->replicator) {
            this->replicator->stop();
        }
        if (this->socket)
            this->socket->stop();
        if (this->srv)
            this->srv->stop();

        {
            std::unique_lock<std::shared_mutex> lock(this->writeMtx);
            this->shardManager->stop();
        }

        this->threadCV.notify_all();
    }

    void ZestDB::boot() {
        this->settings = this->loadConfig(this->users);
        this->replicationLog = std::make_unique<ReplicationLog>(this->settings.replication.historySize);
        // A new id for each run: the sequence numbers restart once the WAL was cleared, so a replica's position in the
        // previous run means nothing in this one
        this->newHistory();
        {
            std::unique_lock<std::shared_mutex> usersLock(this->usersMtx);
            this->rebuildLimiters();
        }
        setLoggerDebugMode(this->settings.isDebug);

        if (!this->settings.useSSL && !this->users.empty()) {
            ZestLog(LogLevel::Warning, "useSSL is false: authentication tokens and data travel in clear and a captured token can be replayed. Enable useSSL outside a trusted network.");
        }

        ZestLog(LogLevel::Debug, std::format("Boot config: path={}, cache={}, segSize={}, dbPort={}, webPort={}", this->settings.DbPath.string(), this->settings.CacheSize, this->settings.SegSize, this->settings.DBPort, this->settings.WebPort));

        if (!fs::exists(this->settings.DbPath)) {
            fs::create_directories(this->settings.DbPath);
        }
    }

    ResultType ZestDB::loadScript(const std::string& name, const std::string& code) {
        return this->scriptEngine->load(name, code);
    }

    ResultType ZestDB::dropScript(const std::string& name) {
        return this->scriptEngine->drop(name);
    }

    ResultType ZestDB::reloadScript(const std::string& name) {
        return this->scriptEngine->reloadFile(name);
    }

    ResultType ZestDB::reloadScripts() {
        return this->scriptEngine->loadAll();
    }

    ResultType ZestDB::listScripts() const {
        return this->scriptEngine->list();
    }

    ResultType ZestDB::call(const std::string& name, const std::vector<std::string>& args) {
        return this->scriptEngine->call(name, args);
    }

    bool ZestDB::validateKey(std::string_view key) const {
        if (key.size() > this->settings.MaxKeySize) {
            ZestLog(LogLevel::Error, std::format("ZestDB::validateKey - The key is too long ! MaxKeySize : {}", this->settings.MaxKeySize));
            return false;
        }
        return true;
    }

    bool ZestDB::validateValue(std::string_view value) const {
        if (value.size() > this->settings.MaxValueSize) {
            ZestLog(LogLevel::Error, std::format("ZestDB::validateValue - The value is too long ! MaxValueSize : {}", this->settings.MaxValueSize));
            return false;
        }
        return true;
    }

    bool ZestDB::isJsonValid(std::string_view value) const {
        return nlohmann::json::accept(value);
    }

    ResultType ZestDB::get(const std::string& key) {
        if (!this->validateKey(key)) {
            return { ResultType::Code::InvalidKey };
        }

        return this->shardManager->get(key);
    }

    ResultType ZestDB::checkWrite(const std::string& key, const std::string& value) const {
        const bool enforce = !this->unchecked();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::ReadOnly };
        }

        if (enforce && !this->validateKey(key)) {
            return { ResultType::Code::InvalidKey };
        }

        if (value.empty()) {
            return { ResultType::Code::EmptyValue };
        }

        if (enforce && !this->validateValue(value)) {
            return { ResultType::Code::InvalidValue };
        }

        if (enforce && this->settings.jsonOnly) {
            if (!this->isJsonValid(value)) {
                return { ResultType::Code::JsonOnly };
            }
        }

        return { ResultType::Code::Success };
    }

    /// `value` between double quotes with `"` and `\` escaped: the form a command (and so a WAL record) expects.
    static std::string quoteValue(std::string_view value) {
        std::string out = "\"";
        for (char c : value) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        out += '"';
        return out;
    }

    /// The WAL record of a write: `s key "value"`, or `setexat key <ms> "value"` when it has an expiry date.
    static std::string setCommand(const std::string& key, const std::string& value, uint64_t expiresAt) {
        return expiresAt != 0 ? std::format("setexat {} {} {}", key, expiresAt, quoteValue(value)) : std::format("s {} {}", key, quoteValue(value));
    }

    bool ZestDB::scripted() const {
        return !this->unchecked() && this->scriptEngine && this->scriptEngine->hasTriggers();
    }

    bool ZestDB::unchecked() const {
        return this->replaying.load() || applyingReplicated;
    }

    ResultType ZestDB::applyReplicated(const std::string& command) {
        applyingReplicated = true;
        ResultType result;
        try {
            result = this->execCmd(command, Role::Admin, false);
        } catch (...) {
            applyingReplicated = false;
            throw;
        }
        applyingReplicated = false;
        return result;
    }

    bool ZestDB::logsRows() const {
        return this->scripted() || (!this->replaying.load() && this->replicationLog && this->replicationLog->active());
    }

    void ZestDB::newHistory() {
        std::random_device rd;
        {
            std::lock_guard<std::mutex> lock(this->historyMtx);
            this->historyId = std::format("{:08x}{:08x}{:08x}{:08x}", rd(), rd(), rd(), rd());
        }
        if (this->socket) {
            this->socket->closeReplicas();
        }
    }

    ResultType ZestDB::replicationHandshake(const std::string& remoteIp, const std::string& knownHistoryId, uint64_t fromSeq, bool* streamOut) {
        if (this->settings.replication.replica) {
            return { ResultType::Code::NotPrimary };
        }
        if (!std::regex_match(remoteIp, this->settings.AllowedReplicas)) {
            ZestLog(LogLevel::Warning, std::format("Replication: {} is not in allowedReplicas", remoteIp));
            return { ResultType::Code::ReplicaNotAllowed };
        }
        this->startReplicationLog();
        std::vector<ReplicationEvent> none;
        const std::string current = this->getHistoryId();
        const bool stream = knownHistoryId == current && this->replicationLog->since(fromSeq, none, 0);
        if (streamOut) {
            *streamOut = stream;
        }
        json reply = { { "mode", stream ? "stream" : "fullsync" }, { "historyId", current }, { "nextSeq", this->replicationLog->nextSeq() } };
        ZestLog(LogLevel::Info, std::format("Replication: {} connected, {}", remoteIp, stream ? std::format("streaming from {}", fromSeq) : "full sync needed"));
        return { ResultType::Code::Success, reply.dump(), 1 };
    }

    void ZestDB::startReplicationLog() {
        if (this->settings.replication.replica) {
            return;
        }
        // With every write order lock held, no write is between its change and its WAL record: the log misses none
        std::shared_lock<std::shared_mutex> lock(this->writeMtx);
        auto order = this->shardManager->lockAllWrites();
        this->replicationLog->activate(WAL::nextSequence());
    }

    ResultType ZestDB::beforeWrite(const std::string& key, std::string& value) {
        if (!this->scripted()) {
            return { ResultType::Code::Success };
        }
        ResultType before = this->scriptEngine->before(TriggerEvent::Set, key, value);
        if (before.code != ResultType::Code::Success) {
            return before;
        }
        return this->checkWrite(key, value);
    }

    ResultType ZestDB::logWrite(const std::string& key, const std::string& walCommand, ResultType result) {
        if (result.code == ResultType::Code::Success && !this->appendToWAL(key, walCommand)) {
            return { ResultType::Code::WalWriteFailed, "", result.affectedRows };
        }
        return result;
    }

    ResultType ZestDB::afterWrite(TriggerEvent event, const std::string& key, const std::string& value, ResultType result) {
        if (result.code != ResultType::Code::Success) {
            return result;
        }
        if (this->scripted()) {
            ResultType after = this->scriptEngine->after(event, key, value);
            if (after.code != ResultType::Code::Success) {
                return after;
            }
        }
        return result;
    }

    ResultType ZestDB::set(const std::string& key, const std::string& value, uint64_t expiresAt) {
        ResultType check = this->checkWrite(key, value);
        if (check.code != ResultType::Code::Success) {
            return check;
        }
        std::string stored = value;
        ResultType before = this->beforeWrite(key, stored);
        if (before.code != ResultType::Code::Success) {
            return before;
        }
        std::unique_lock<std::mutex> order = this->shardManager->lockWrites(key);
        ResultType result = this->shardManager->set(key, stored, expiresAt);
        result = this->logWrite(key, setCommand(key, stored, expiresAt), result);
        order.unlock();
        return this->afterWrite(TriggerEvent::Set, key, stored, result);
    }

    ResultType ZestDB::replace(const std::string& key, const std::string& value) {
        ResultType check = this->checkWrite(key, value);
        if (check.code != ResultType::Code::Success) {
            return check;
        }
        std::string stored = value;
        ResultType before = this->beforeWrite(key, stored);
        if (before.code != ResultType::Code::Success) {
            return before;
        }
        uint64_t expiresAt = 0;
        std::unique_lock<std::mutex> order = this->shardManager->lockWrites(key);
        ResultType result = this->shardManager->replace(key, stored, expiresAt);
        result = this->logWrite(key, setCommand(key, stored, expiresAt), result);
        order.unlock();
        return this->afterWrite(TriggerEvent::Set, key, stored, result);
    }

    ResultType ZestDB::setnx(const std::string& key, const std::string& value) {
        ResultType check = this->checkWrite(key, value);
        if (check.code != ResultType::Code::Success) {
            return check;
        }
        std::string stored = value;
        ResultType before = this->beforeWrite(key, stored);
        if (before.code != ResultType::Code::Success) {
            return before;
        }
        std::unique_lock<std::mutex> order = this->shardManager->lockWrites(key);
        ResultType result = this->shardManager->setnx(key, stored);
        result = this->logWrite(key, setCommand(key, stored, 0), result);
        order.unlock();
        return this->afterWrite(TriggerEvent::Set, key, stored, result);
    }

    ResultType ZestDB::cas(const std::string& key, const std::string& expected, const std::string& value) {
        uint64_t expiresAt = 0;
        return this->casBy(key, expected, value, expiresAt);
    }

    ResultType ZestDB::casBy(const std::string& key, const std::string& expected, const std::string& value, uint64_t& expiresAt) {
        ResultType check = this->checkWrite(key, value);
        if (check.code != ResultType::Code::Success) {
            return check;
        }
        std::string stored = value;
        ResultType before = this->beforeWrite(key, stored);
        if (before.code != ResultType::Code::Success) {
            return before;
        }
        std::unique_lock<std::mutex> order = this->shardManager->lockWrites(key);
        ResultType result = this->shardManager->cas(key, expected, stored, expiresAt);
        result = this->logWrite(key, setCommand(key, stored, expiresAt), result);
        order.unlock();
        return this->afterWrite(TriggerEvent::Set, key, stored, result);
    }

    ResultType ZestDB::setTTL(const std::string& key, uint64_t expiresAt) {
        const bool enforce = !this->unchecked();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::ReadOnly };
        }

        if (enforce && !this->validateKey(key)) {
            return { ResultType::Code::InvalidKey };
        }

        std::unique_lock<std::mutex> order = this->shardManager->lockWrites(key);
        ResultType result = this->shardManager->setTTL(key, expiresAt);
        return this->logWrite(key, expiresAt != 0 ? std::format("expireat {} {}", key, expiresAt) : std::format("persist {}", key), result);
    }

    ResultType ZestDB::getTTL(const std::string& key) {
        if (!this->validateKey(key)) {
            return { ResultType::Code::InvalidKey };
        }

        return this->shardManager->getTTL(key);
    }

    ResultType ZestDB::del(const std::string& key) {
        const bool enforce = !this->unchecked();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::ReadOnly };
        }

        if (enforce && !this->validateKey(key)) {
            return { ResultType::Code::InvalidKey };
        }

        if (this->scripted()) {
            std::string unused;
            ResultType before = this->scriptEngine->before(TriggerEvent::Del, key, unused);
            if (before.code != ResultType::Code::Success) {
                return before;
            }
        }
        std::unique_lock<std::mutex> order = this->shardManager->lockWrites(key);
        ResultType result = this->shardManager->del(key);
        result = this->logWrite(key, std::format("d {}", key), result);
        order.unlock();
        return this->afterWrite(TriggerEvent::Del, key, "", result);
    }

    ResultType ZestDB::getBy(ValidationRule& valid) {
        return this->shardManager->getBy(valid);
    }

    ResultType ZestDB::setBy(ValidationRule& valid, const std::string& value) {
        return this->setBy(valid, value, this->logsRows());
    }

    ResultType ZestDB::setBy(ValidationRule& valid, const std::string& value, bool perRow) {
        const bool enforce = !this->unchecked();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::ReadOnly };
        }

        if (value.empty()) {
            return { ResultType::Code::EmptyValue };
        }

        if (enforce && !this->validateValue(value)) {
            return { ResultType::Code::InvalidValue };
        }

        if (enforce && this->settings.jsonOnly) {
            if (!this->isJsonValid(value)) {
                return { ResultType::Code::JsonOnly };
            }
        }

        if (!perRow) {
            return this->shardManager->setBy(valid, value);
        }
        long long updated = 0;
        for (const std::string& key : this->shardManager->firstMatchingKeys(valid)) {
            updated += this->replace(key, value).code == ResultType::Code::Success;
        }
        return { ResultType::Code::Success, "", updated };
    }

    ResultType ZestDB::delBy(ValidationRule& valid) {
        return this->delBy(valid, this->logsRows());
    }

    ResultType ZestDB::delBy(ValidationRule& valid, bool perRow) {
        if (!this->unchecked() && this->settings.readOnly) {
            return { ResultType::Code::ReadOnly };
        }

        if (!perRow) {
            return this->shardManager->delBy(valid);
        }
        long long deleted = 0;
        for (const std::string& key : this->shardManager->firstMatchingKeys(valid)) {
            deleted += this->del(key).code == ResultType::Code::Success;
        }
        return { ResultType::Code::Success, "", deleted };
    }

    ResultType ZestDB::incr(const std::string& key, int64_t delta) {
        uint64_t expiresAt = 0;
        return this->incrBy(key, delta, expiresAt);
    }

    ResultType ZestDB::incrBy(const std::string& key, int64_t delta, uint64_t& expiresAt) {
        if (this->settings.readOnly) {
            return { ResultType::Code::ReadOnly };
        }

        if (!this->validateKey(key)) {
            return { ResultType::Code::InvalidKey };
        }

        std::unique_lock<std::mutex> order = this->shardManager->lockWrites(key);
        ResultType result = this->shardManager->incr(key, delta, expiresAt);
        result = this->logWrite(key, setCommand(key, result.response, expiresAt), result);
        order.unlock();
        return this->afterWrite(TriggerEvent::Set, key, result.response, result);
    }

    ResultType ZestDB::exists(const std::string& key) {
        if (!this->validateKey(key)) {
            return { ResultType::Code::InvalidKey };
        }

        return this->shardManager->exists(key);
    }

    ResultType ZestDB::count(const ValidationRule& valid) {
        return this->shardManager->count(valid);
    }

    ResultType ZestDB::keys(const ValidationRule& valid) {
        return this->shardManager->keys(valid);
    }

    bool ZestDB::appendToWAL(const std::string& key, const std::string& command) {
        if (this->replaying.load()) {
            return true;
        }
        // Every caller holds a write order lock, which startReplicationLog() takes too: `active` cannot change here
        if (!this->replicationLog->active()) {
            return this->shardManager->appendToWAL(key, command);
        }
        {
            // The log's lock spans the numbering of the record and its push, so the log stays in sequence order
            std::lock_guard<std::mutex> lock(this->replicationLog->lock());
            uint64_t seq = 0;
            if (!this->shardManager->appendToWAL(key, command, &seq)) {
                return false;
            }
            this->replicationLog->push(seq, command);
        }
        this->replicationLog->notify();
        return true;
    }

    bool ZestDB::syncWAL() {
        std::shared_lock<std::shared_mutex> lock(this->writeMtx);
        return this->shardManager->syncWAL();
    }

    void ZestDB::flush() {
        std::unique_lock<std::shared_mutex> lock(this->writeMtx);
        this->flushLocked();
    }

    void ZestDB::flushLocked() {
        ZestLog(LogLevel::Debug, "Flushing all shards...");
        this->isFlushing.store(true);
        this->shardManager->flush();
        this->isFlushing.store(false);
    }

    void ZestDB::replayWAL() {
        ZestLog(LogLevel::Info, "Replaying WAL...");
        this->replaying.store(true);
        this->shardManager->replayAllWAL([this](const std::string& cmd) {
            auto resp = this->execCmd(cmd);
            if (resp.code != ResultType::Code::Success) {
                ZestLog(LogLevel::Warning, std::format("WAL replay: '{}' failed with code {}", cmd, static_cast<int>(resp.code)));
            }
            return responseToJson(resp);
        });
        this->replaying.store(false);
    }

} // namespace Zest
