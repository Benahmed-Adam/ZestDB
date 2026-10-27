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

    // Built by hand (keys in nlohmann's sorted order): this runs once per command and building a json object was its main cost
    std::string ZestDB::responseToJson(const ResultType& resp) {
        std::string out = "{\"affectedRows\":";
        out += std::to_string(resp.affectedRows);
        out += ",\"code\":";
        out += resp.code == ResultType::Code::Success ? '0' : '1';
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

    std::string sha256(const std::string& str) {
        EVP_MD_CTX* context = EVP_MD_CTX_new();
        const EVP_MD* md = EVP_sha256();
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int lengthOfHash = 0;

        EVP_DigestInit_ex(context, md, nullptr);
        EVP_DigestUpdate(context, str.c_str(), str.size());
        EVP_DigestFinal_ex(context, hash, &lengthOfHash);

        EVP_MD_CTX_free(context);

        std::stringstream ss;
        for (unsigned int i = 0; i < lengthOfHash; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

    ZestDB::ZestDB()
        : replaying(false) {
        ZestLog(LogLevel::Info, "Initializing ZestDB...");
        this->boot();

        this->shardManager = std::make_unique<ShardManager>(this->settings, NUM_SHARDS);
        this->socket = std::make_unique<Server>(this->ioCtx, this->settings.DBPort, *this);

        this->replayWAL();

        ZestLog(LogLevel::Info, std::format("ZestDB initialized with {} shards", NUM_SHARDS));

        this->flushThread = std::jthread([this](std::stop_token stopToken) {
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

        this->saveThread = std::jthread([this](std::stop_token stopToken) {
            std::unique_lock<std::mutex> lock(this->saveThreadMtx);

            unsigned int archiveCreationDelay = this->settings.ArchiveCreationDelay;
            unsigned int delay = 0;
            std::optional<fs::file_time_type> most_recent_time;

            if (!fs::exists(this->settings.ArchiveStoragePath) || !fs::is_directory(this->settings.ArchiveStoragePath)) {
                ZestLog(LogLevel::Warning, "Archive storage path does not exist or is not a directory.");
                fs::create_directories(this->settings.ArchiveStoragePath);
            }

            for (const auto& entry : fs::directory_iterator(this->settings.ArchiveStoragePath)) {
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
    }

    ZestDB::~ZestDB() {
        if (this->isRunning())
            this->stop();
    }

    bool ZestDB::isRunning() const {
        return this->running.load();
    }

    void ZestDB::stop() {
        ZestLog(LogLevel::Info, "Exiting ZestDB...");

        this->running = false;

        this->ioCtx.stop();
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
        setLoggerDebugMode(this->settings.isDebug);

        if (!this->settings.useSSL && !this->users.empty()) {
            ZestLog(LogLevel::Warning, "useSSL is false: authentication tokens and data travel in clear and a captured token can be replayed. Enable useSSL outside a trusted network.");
        }

        ZestLog(LogLevel::Debug, std::format("Boot config: path={}, cache={}, segSize={}, dbPort={}, webPort={}", this->settings.DbPath.string(), this->settings.CacheSize, this->settings.SegSize, this->settings.DBPort, this->settings.WebPort));

        if (!fs::exists(this->settings.DbPath)) {
            fs::create_directories(this->settings.DbPath);
        }
    }

    template <typename T>
    static T getOr(fkyaml::node& node, const std::string& key, T def) {
        return node.contains(key) && !node[key].is_null() ? node[key].get_value<T>() : def;
    }

    static std::optional<Role> parseRole(const std::string& name) {
        if (name == "read") {
            return Role::Read;
        }
        if (name == "write") {
            return Role::Write;
        }
        if (name == "admin") {
            return Role::Admin;
        }
        return std::nullopt;
    }

    Settings ZestDB::loadConfig(std::unordered_map<std::string, UserEntry>& usersOut) {
        Settings result;
        std::unordered_map<std::string, UserEntry> parsedUsers;

        const fs::path current_path = fs::current_path().string();
        const std::string configName = "config.yaml";

        fs::path configPath = current_path / configName;

        if (!fs::exists(configPath)) {
            ZestLog(LogLevel::Critical, configName + " doesn't exist !");
            throw std::runtime_error(configName + " doesn't exist !");
        }

        ZestLog(LogLevel::Info, "Reading " + configName);

        try {
            std::ifstream configFile(configPath);
            std::stringstream buffer;
            buffer << configFile.rdbuf();

            auto node = fkyaml::node::deserialize(buffer.str());

            result.DbPath = getOr<std::string>(node, "DbPath", current_path.string());
            result.SegSize = getOr<unsigned long>(node, "SegSize", 128000);
            result.MaxKeySize = getOr<unsigned int>(node, "MaxKeySize", 64);
            result.MaxValueSize = getOr<unsigned int>(node, "MaxValueSize", 10000);
            result.CacheSize = getOr<unsigned int>(node, "CacheSize", 1000);
            result.CompactingInterval = getOr<unsigned int>(node, "CompactingInterval", 3600);
            result.FlushInterval = getOr<unsigned int>(node, "FlushInterval", 120);
            int dbPort = getOr<int>(node, "DBPort", 7321);
            int webPort = getOr<int>(node, "WebPort", 1237);
            if (dbPort < 1 || dbPort > 65535 || webPort < 1 || webPort > 65535) {
                ZestLog(LogLevel::Critical, "DBPort and WebPort must be between 1 and 65535");
                throw std::runtime_error("Port out of range");
            }
            result.DBPort = static_cast<uint16_t>(dbPort);
            result.WebPort = static_cast<uint16_t>(webPort);

            result.NetworkValidationStr = getOr<std::string>(node, "NetworkValidation", ".*");
            if (result.NetworkValidationStr.empty()) {
                result.NetworkValidationStr = ".*";
            }

            result.isDebug = getOr<bool>(node, "isDebug", false);
            result.jsonOnly = getOr<bool>(node, "jsonOnly", false);
            result.readOnly = getOr<bool>(node, "readOnly", false);
            result.fsyncOnWrite = getOr<bool>(node, "fsyncOnWrite", true);

            result.useSSL = getOr<bool>(node, "useSSL", false);
            result.SSLCertPath = getOr<std::string>(node, "SSLCertPath", "");
            result.SSLKeyPath = getOr<std::string>(node, "SSLKeyPath", "");

            result.ArchiveStoragePath = getOr<std::string>(node, "ArchiveStoragePath", (current_path / "archive/").string());
            result.ArchiveCreationDelay = getOr<unsigned int>(node, "ArchiveCreationDelay", 3600);
            result.AutoArchiveSaving = getOr<bool>(node, "AutoArchiveSaving", true);
            result.ArchiveRetention = getOr<unsigned int>(node, "ArchiveRetention", 10);

            result.MaxConnections = getOr<unsigned int>(node, "MaxConnections", 1024);
            result.AuthTimeout = getOr<unsigned int>(node, "AuthTimeout", 10);
            result.IdleTimeout = getOr<unsigned int>(node, "IdleTimeout", 300);

            if (result.useSSL && (result.SSLCertPath == "" || result.SSLKeyPath == "")) {
                ZestLog(LogLevel::Critical, "SSL enabled and not certificate or key path provided !");
                throw std::runtime_error("SSL with no cert or key");
            }

            if (!result.NetworkValidationStr.empty()) {
                try {
                    result.NetworkValidation = std::regex(result.NetworkValidationStr);
                } catch (const std::regex_error& e) {
                    ZestLog(LogLevel::Critical, "Invalid NetworkValidation regex: " + std::string(e.what()));
                    throw std::runtime_error("Invalid NetworkValidation regex");
                }
            }

            if (node.contains("users") && node["users"].is_sequence()) {
                for (const auto& user_node : node["users"]) {
                    std::string username = user_node["user"].get_value<std::string>();
                    std::string roleName = user_node.contains("role") ? user_node["role"].get_value<std::string>() : "write";
                    std::optional<Role> role = parseRole(roleName);
                    if (!role) {
                        throw std::runtime_error(std::format("user {}: unknown role '{}' (use read, write or admin)", username, roleName));
                    }
                    std::string tokenHash = user_node.contains("tokenHash") ? user_node["tokenHash"].get_value<std::string>() : sha256(sha256(username + user_node["password"].get_value<std::string>()));
                    parsedUsers[username] = { std::move(tokenHash), *role };
                }
            }
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, "Failed to parse config : " + std::string(e.what()));
            throw std::runtime_error("Failed to parse config: " + std::string(e.what()));
        }

        if (result.MaxValueSize >= result.SegSize) {
            ZestLog(LogLevel::Critical, std::format("MaxValueSize is higher than SegSize ! "
                                                    "MaxValueSize : {} | SegSize : {}",
                                                    result.MaxValueSize, result.SegSize));
            throw std::runtime_error("MaxValueSize >= SegSize");
        }

        if (result.MaxKeySize > MAX_KEY_SIZE) {
            ZestLog(LogLevel::Warning, std::format("MaxKeySize ({}) exceeds internal limit ({}), clamping...", result.MaxKeySize, MAX_KEY_SIZE));
            result.MaxKeySize = MAX_KEY_SIZE;
        }

        if (result.FlushInterval == 0 || result.CompactingInterval == 0 || result.ArchiveCreationDelay == 0 || result.AuthTimeout == 0 || result.IdleTimeout == 0 || result.MaxConnections == 0) {
            ZestLog(LogLevel::Critical, "FlushInterval, CompactingInterval, ArchiveCreationDelay, AuthTimeout, IdleTimeout and MaxConnections must be at least 1");
            throw std::runtime_error("Setting of 0");
        }

        if (result.DBPort == result.WebPort) {
            ZestLog(LogLevel::Critical, "DBPort and WebPort cannot be the same");
            throw std::runtime_error("DBPort == WebPort");
        }

        usersOut = std::move(parsedUsers);
        return result;
    }

    ResultType ZestDB::reloadConfig() {
        try {
            Settings oldSettings = this->settings;
            std::unordered_map<std::string, UserEntry> newUsers;
            Settings s = this->loadConfig(newUsers);

            if (s.DbPath != oldSettings.DbPath || s.ArchiveStoragePath != oldSettings.ArchiveStoragePath || s.IndexPath != oldSettings.IndexPath || s.WalPath != oldSettings.WalPath || s.SSLCertPath != oldSettings.SSLCertPath ||
                s.SSLKeyPath != oldSettings.SSLKeyPath) {
                ZestLog(LogLevel::Error, "Cannot change path settings during "
                                         "hot-reload. Paths are immutable.");
                return { ResultType::Code::Error, "Cannot change path settings during hot-reload. Paths are "
                                                  "immutable. Restart the database to apply changes." };
            }

            if (s.useSSL != oldSettings.useSSL || s.DBPort != oldSettings.DBPort || s.WebPort != oldSettings.WebPort || s.MaxConnections != oldSettings.MaxConnections || s.AuthTimeout != oldSettings.AuthTimeout ||
                s.IdleTimeout != oldSettings.IdleTimeout) {
                ZestLog(LogLevel::Error, "Cannot change the ports, SSL or connection limits during hot-reload.");
                return { ResultType::Code::Error, "Cannot change the ports, SSL or connection limits during hot-reload. "
                                                  "Restart the database to apply changes." };
            }

            this->settings = std::move(s);
            {
                std::unique_lock<std::shared_mutex> usersLock(this->usersMtx);
                this->users = std::move(newUsers);
            }
            setLoggerDebugMode(this->settings.isDebug);
            this->shardManager->reloadSettings(this->settings);
            this->threadCV.notify_all();

            return { ResultType::Code::Success, "Reload successful !" };
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("An error occured during the reload of the "
                                                 "configuration : {}. Aborting...",
                                                 e.what()));
        }
        return { ResultType::Code::Error, "Reload failed !" };
    }

    void ZestDB::setConfig() {
        const fs::path current_path = fs::current_path();
        const std::string configName = "config.yaml";
        fs::path configPath = current_path / configName;

        std::fstream configFile(configPath);
        std::stringstream buffer;
        buffer << configFile.rdbuf();
        configFile.close();

        auto node = fkyaml::node::deserialize(buffer.str());

        node["DbPath"] = this->settings.DbPath.string();
        node["ArchiveStoragePath"] = this->settings.ArchiveStoragePath.string();
        node["SegSize"] = this->settings.SegSize;
        node["MaxKeySize"] = this->settings.MaxKeySize;
        node["MaxValueSize"] = this->settings.MaxValueSize;
        node["CacheSize"] = this->settings.CacheSize;
        node["CompactingInterval"] = this->settings.CompactingInterval;
        node["FlushInterval"] = this->settings.FlushInterval;
        node["DBPort"] = static_cast<int>(this->settings.DBPort);
        node["WebPort"] = static_cast<int>(this->settings.WebPort);
        node["NetworkValidation"] = this->settings.NetworkValidationStr;
        node["isDebug"] = this->settings.isDebug;
        node["jsonOnly"] = this->settings.jsonOnly;
        node["readOnly"] = this->settings.readOnly;
        node["fsyncOnWrite"] = this->settings.fsyncOnWrite;
        node["useSSL"] = this->settings.useSSL;
        node["SSLCertPath"] = this->settings.SSLCertPath.string();
        node["SSLKeyPath"] = this->settings.SSLKeyPath.string();
        node["ArchiveCreationDelay"] = this->settings.ArchiveCreationDelay;
        node["AutoArchiveSaving"] = this->settings.AutoArchiveSaving;
        node["ArchiveRetention"] = this->settings.ArchiveRetention;
        node["MaxConnections"] = this->settings.MaxConnections;
        node["AuthTimeout"] = this->settings.AuthTimeout;
        node["IdleTimeout"] = this->settings.IdleTimeout;

        std::ofstream outFile(configPath);
        outFile << node;
        outFile.close();

        ZestLog(LogLevel::Info, "Configuration saved to config.yaml");
    }

    std::string ZestDB::getConfig() const {
        json j;
        j["DbPath"] = this->settings.DbPath.string();
        j["ArchiveStoragePath"] = this->settings.ArchiveStoragePath.string();
        j["SegSize"] = this->settings.SegSize;
        j["MaxKeySize"] = this->settings.MaxKeySize;
        j["MaxValueSize"] = this->settings.MaxValueSize;
        j["CacheSize"] = this->settings.CacheSize;
        j["CompactingInterval"] = this->settings.CompactingInterval;
        j["FlushInterval"] = this->settings.FlushInterval;
        j["DBPort"] = this->settings.DBPort;
        j["WebPort"] = this->settings.WebPort;
        j["NetworkValidation"] = this->settings.NetworkValidationStr;
        j["isDebug"] = this->settings.isDebug;
        j["jsonOnly"] = this->settings.jsonOnly;
        j["readOnly"] = this->settings.readOnly;
        j["fsyncOnWrite"] = this->settings.fsyncOnWrite;
        j["useSSL"] = this->settings.useSSL;
        j["SSLCertPath"] = this->settings.SSLCertPath.string();
        j["SSLKeyPath"] = this->settings.SSLKeyPath.string();
        j["ArchiveCreationDelay"] = this->settings.ArchiveCreationDelay;
        j["AutoArchiveSaving"] = this->settings.AutoArchiveSaving;
        j["ArchiveRetention"] = this->settings.ArchiveRetention;
        j["MaxConnections"] = this->settings.MaxConnections;
        j["AuthTimeout"] = this->settings.AuthTimeout;
        j["IdleTimeout"] = this->settings.IdleTimeout;
        return j.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    bool ZestDB::createArchive() {
        if (this->archiving.exchange(true)) {
            ZestLog(LogLevel::Warning, "An archive is already being created");
            return false;
        }
        struct ResetFlag {
            std::atomic<bool>& flag;
            ~ResetFlag() {
                flag = false;
            }
        } resetFlag{ this->archiving };

        fs::path snapshot;
        std::error_code ec;
        try {
            fs::path folder_to_zip;
            fs::path archivePath;
            unsigned int retention;
            {
                std::unique_lock<std::shared_mutex> lock(this->writeMtx);
                this->flushLocked();
                folder_to_zip = this->settings.DbPath;
                archivePath = this->settings.ArchiveStoragePath;
                retention = this->settings.ArchiveRetention;

                if (!fs::is_directory(folder_to_zip)) {
                    ZestLog(LogLevel::Error, std::format("Source folder does not exist : {}", folder_to_zip.string()));
                    return false;
                }
                fs::create_directories(archivePath);
                snapshot = archivePath / ".snapshot";
                fs::remove_all(snapshot);

                auto shardLocks = this->shardManager->lockAllShards();
                const fs::path archiveDir = fs::weakly_canonical(archivePath);
                for (auto it = fs::recursive_directory_iterator(folder_to_zip); it != fs::recursive_directory_iterator(); ++it) {
                    if (it->is_directory() && fs::weakly_canonical(it->path()) == archiveDir) {
                        it.disable_recursion_pending();
                        continue;
                    }
                    if (it->is_regular_file()) {
                        fs::path target = snapshot / fs::relative(it->path(), folder_to_zip);
                        fs::create_directories(target.parent_path());
                        fs::copy_file(it->path(), target);
                    }
                }
            }

            fs::path archive_full_path = archivePath / std::format("zestdb_archive_{:%Y-%m-%d_%H-%M-%S}.zip", std::chrono::system_clock::now());
            libzippp::ZipArchive archive(archive_full_path.string());
            if (!archive.open(libzippp::ZipArchive::New)) {
                ZestLog(LogLevel::Error, std::format("Impossible to create the archive : {}", archive_full_path.string()));
                fs::remove_all(snapshot, ec);
                return false;
            }
            if (fs::exists(snapshot)) {
                for (const auto& entry : fs::recursive_directory_iterator(snapshot)) {
                    if (entry.is_regular_file() && !archive.addFile(fs::relative(entry.path(), snapshot).generic_string(), entry.path().string())) {
                        ZestLog(LogLevel::Error, std::format("Error while adding : {}", entry.path().string()));
                        archive.close();
                        fs::remove_all(snapshot, ec);
                        return false;
                    }
                }
            }
            int close_result = archive.close();
            fs::remove_all(snapshot, ec);
            if (close_result != LIBZIPPP_OK) {
                ZestLog(LogLevel::Error, std::format("Error while saving the archive : Error code : {}", close_result));
                return false;
            }

            if (retention > 0) {
                std::vector<fs::path> archives;
                for (const auto& entry : fs::directory_iterator(archivePath)) {
                    const std::string name = entry.path().filename().string();
                    if (entry.is_regular_file() && name.starts_with("zestdb_archive_") && name.ends_with(".zip")) {
                        archives.push_back(entry.path());
                    }
                }
                std::sort(archives.begin(), archives.end());
                for (size_t i = 0; i + retention < archives.size(); i++) {
                    fs::remove(archives[i], ec);
                }
            }
            return true;
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("Error while creating the archive : {}", e.what()));
            if (!snapshot.empty()) {
                fs::remove_all(snapshot, ec);
            }
            return false;
        }
    }

    bool ZestDB::validateToken(const std::string& username, const std::string& token) const {
        return this->authenticate(username, token).has_value();
    }

    std::optional<Role> ZestDB::authenticate(const std::string& username, const std::string& token) const {
        std::shared_lock<std::shared_mutex> lock(this->usersMtx);
        auto it = this->users.find(username);
        if (it == this->users.end()) {
            return std::nullopt;
        }

        const std::string& expected = it->second.tokenHash;
        const std::string actual = sha256(token);
        if (actual.size() != expected.size() || CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) != 0) {
            return std::nullopt;
        }
        return it->second.role;
    }

    std::optional<Role> ZestDB::roleOf(const std::string& username) const {
        std::shared_lock<std::shared_mutex> lock(this->usersMtx);
        auto it = this->users.find(username);
        if (it == this->users.end()) {
            return std::nullopt;
        }
        return it->second.role;
    }

    static constexpr unsigned int MAX_AUTH_FAILURES = 5;
    static constexpr std::chrono::seconds AUTH_LOCKOUT{ 60 };

    bool ZestDB::authAllowed(const std::string& username) {
        std::lock_guard<std::mutex> lock(this->authFailuresMtx);
        auto it = this->authFailures.find(username);
        return it == this->authFailures.end() || it->second.first < MAX_AUTH_FAILURES || std::chrono::steady_clock::now() >= it->second.second;
    }

    void ZestDB::recordAuthResult(const std::string& username, bool success) {
        std::lock_guard<std::mutex> lock(this->authFailuresMtx);
        if (success) {
            this->authFailures.erase(username);
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (this->authFailures.size() > 10000) {
            std::erase_if(this->authFailures, [now](const auto& entry) { return now >= entry.second.second; });
        }
        auto& [failures, until] = this->authFailures[username];
        if (now >= until) {
            failures = 0;
        }
        failures++;
        until = now + AUTH_LOCKOUT;
    }

    bool ZestDB::isAddressAllowed(const std::string& ip) {
        std::shared_lock<std::shared_mutex> lock(this->writeMtx);
        return std::regex_match(ip, this->settings.NetworkValidation);
    }

    bool ZestDB::validateKey(std::string_view key) const {
        if (key.size() > this->settings.MaxKeySize) {
            ZestLog(LogLevel::Error, std::format("ZestDB::validateKey - {} MaxKeySize : {}", Messages::KEY_TOO_LONG, this->settings.MaxKeySize));
            return false;
        }
        return true;
    }

    bool ZestDB::validateValue(std::string_view value) const {
        if (value.size() > this->settings.MaxValueSize) {
            ZestLog(LogLevel::Error, std::format("ZestDB::validateValue - {} MaxValueSize : {}", Messages::VALUE_TOO_LONG, this->settings.MaxValueSize));
            return false;
        }
        return true;
    }

    bool ZestDB::isJsonValid(std::string_view value) const {
        return nlohmann::json::accept(value);
    }

    ResultType ZestDB::get(const std::string& key) {
        if (!this->validateKey(key)) {
            return { ResultType::Code::Error, Messages::INVALID_KEY };
        }

        return this->shardManager->get(key);
    }

    ResultType ZestDB::set(const std::string& key, const std::string& value) {
        const bool enforce = !this->replaying.load();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::Error, Messages::READ_ONLY_ERROR };
        }

        if (enforce && !this->validateKey(key)) {
            return { ResultType::Code::Error, Messages::INVALID_KEY };
        }

        if (value.empty()) {
            return { ResultType::Code::Error, Messages::VALUE_EMPTY };
        }

        if (enforce && !this->validateValue(value)) {
            return { ResultType::Code::Error, Messages::INVALID_VALUE };
        }

        if (enforce && this->settings.jsonOnly) {
            if (!this->isJsonValid(value)) {
                return { ResultType::Code::Error, Messages::JSON_ONLY_ERROR };
            }
        }

        return this->shardManager->set(key, value);
    }

    ResultType ZestDB::del(const std::string& key) {
        const bool enforce = !this->replaying.load();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::Error, Messages::READ_ONLY_ERROR };
        }

        if (enforce && !this->validateKey(key)) {
            return { ResultType::Code::Error, Messages::INVALID_KEY };
        }

        return this->shardManager->del(key);
    }

    ResultType ZestDB::getBy(ValidationRule& valid) {
        return this->shardManager->getBy(valid);
    }

    ResultType ZestDB::setBy(ValidationRule& valid, const std::string& value) {
        const bool enforce = !this->replaying.load();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::Error, Messages::READ_ONLY_ERROR };
        }

        if (value.empty()) {
            return { ResultType::Code::Error, Messages::VALUE_EMPTY };
        }

        if (enforce && !this->validateValue(value)) {
            return { ResultType::Code::Error, Messages::INVALID_VALUE };
        }

        if (enforce && this->settings.jsonOnly) {
            if (!this->isJsonValid(value)) {
                return { ResultType::Code::Error, Messages::JSON_ONLY_ERROR };
            }
        }

        return this->shardManager->setBy(valid, value);
    }

    ResultType ZestDB::delBy(ValidationRule& valid) {
        if (!this->replaying.load() && this->settings.readOnly) {
            return { ResultType::Code::Error, Messages::READ_ONLY_ERROR };
        }

        return this->shardManager->delBy(valid);
    }

    static bool isRegexTooComplex(std::string_view pattern) {
        constexpr size_t maxLength = 1024;
        constexpr size_t maxDepth = 32;
        constexpr uint64_t maxStates = 100000;
        if (pattern.size() > maxLength) {
            return true;
        }

        std::vector<uint64_t> cost{ 0 };
        uint64_t lastAtom = 0;
        for (size_t i = 0; i < pattern.size(); i++) {
            const char c = pattern[i];
            if (c == '(') {
                if (cost.size() > maxDepth) {
                    return true;
                }
                cost.push_back(0);
                lastAtom = 0;
                continue;
            }

            if (c == ')' && cost.size() > 1) {
                lastAtom = cost.back() + 1;
                cost.pop_back();
                cost.back() += lastAtom;
            } else if (c == '{' && pattern.find('}', i) != std::string_view::npos) {
                const size_t close = pattern.find('}', i);
                uint64_t repeat = 0;
                for (std::string_view bounds = pattern.substr(i + 1, close - i - 1); !bounds.empty();) {
                    const size_t comma = bounds.find(',');
                    std::string_view part = bounds.substr(0, comma);
                    uint64_t value = 0;
                    if (!part.empty() && (std::from_chars(part.data(), part.data() + part.size(), value).ec != std::errc() || value > maxStates)) {
                        return true;
                    }
                    repeat = std::max(repeat, value);
                    bounds = comma == std::string_view::npos ? std::string_view() : bounds.substr(comma + 1);
                }
                cost.back() += lastAtom * (repeat > 0 ? repeat - 1 : 0);
                i = close;
            } else if (c == '[') {
                size_t j = i + 1;
                if (j < pattern.size() && pattern[j] == '^') {
                    j++;
                }
                if (j < pattern.size() && pattern[j] == ']') {
                    j++;
                }
                while (j < pattern.size() && pattern[j] != ']') {
                    j += pattern[j] == '\\' ? 2u : 1u;
                }
                i = j;
                lastAtom = 1;
                cost.back() += 1;
            } else {
                if (c == '\\') {
                    i++;
                }
                lastAtom = 1;
                cost.back() += 1;
            }

            if (cost.back() > maxStates) {
                return true;
            }
        }
        return false;
    }

    CreationValidationRuleResult ZestDB::createValidationRule(std::string_view mode, std::string_view pattern) const {
        ValidationRule valid;
        bool validMode = true;

        if (mode == "re") {
            try {
                std::string patStr(pattern);
                if (isRegexTooComplex(patStr) || std::regex_search(patStr, std::regex(R"(\\[1-9])"))) {
                    return { valid, false };
                }
#ifdef __GLIBCXX__
                std::regex keyRegex(patStr, std::regex::ECMAScript | std::regex_constants::__polynomial);
#else
                std::regex keyRegex(patStr, std::regex::ECMAScript);
#endif
                valid.func = [reg = std::move(keyRegex)](std::string_view key) {
                    try {
                        return std::regex_match(std::string(key), reg);
                    } catch (const std::regex_error&) {
                        return false;
                    }
                };
            } catch (const std::regex_error&) {
                validMode = false;
            }
        } else if (mode == "sw") {
            valid.func = [pat = std::string(pattern)](std::string_view key) { return key.substr(0, pat.size()) == pat; };
        } else if (mode == "ct") {
            valid.func = [pat = std::string(pattern)](std::string_view key) { return key.find(pat) != std::string_view::npos; };
        } else if (mode == "ew") {
            valid.func = [pat = std::string(pattern)](std::string_view key) {
                if (key.size() >= pat.size()) {
                    return key.compare(key.size() - pat.size(), pat.size(), pat) == 0;
                }
                return false;
            };
        } else {
            validMode = false;
        }

        return { valid, validMode };
    }

    bool ZestDB::appendToWAL(const std::string& key, const std::string& command) {
        if (this->replaying.load()) {
            return true;
        }

        return this->shardManager->appendToWAL(key, command);
    }

    std::string toLowerStr(std::string_view str) {
        std::string result(str);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    template <typename T>
    static bool parseUInt(std::string_view str, T& out) {
        auto [end, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
        return ec == std::errc() && end == str.data() + str.size();
    }

    bool ZestDB::syncWAL() {
        return this->shardManager->syncWAL();
    }

    ResultType ZestDB::execCmd(const std::string& command, Role role, bool syncWal) {
        std::string_view sv(command);
        auto start = sv.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) {
            return { ResultType::Code::Error, Messages::NO_COMMAND_GIVEN };
        }
        sv.remove_prefix(start);
        sv.remove_suffix(sv.size() - 1 - sv.find_last_not_of("\r\n"));

        std::vector<std::string_view> words;
        size_t pos = 0;
        while (pos < sv.size()) {
            size_t end = sv.find_first_of(" \t\r\n", pos);
            words.push_back(sv.substr(pos, end - pos));
            if (end == std::string_view::npos)
                break;
            pos = sv.find_first_not_of(" \t\r\n", end);
            if (pos == std::string_view::npos)
                break;
        }

        ResultType result = { ResultType::Code::Error, "" };

        if (words.empty()) {
            return { ResultType::Code::Error, Messages::NO_COMMAND_GIVEN };
        }

        std::string cmd = toLowerStr(words[0]);

        static const std::unordered_map<std::string_view, std::string_view> aliases = {
            { "get", "g" }, { "set", "s" }, { "del", "d" }, { "getby", "gb" }, { "setby", "sb" }, { "delby", "db" }, { "flush", "f" }, { "reload", "r" }, { "help", "h" },
        };
        if (auto it = aliases.find(cmd); it != aliases.end()) {
            cmd = it->second;
        }

        const bool isWrite = cmd == "s" || cmd == "d" || cmd == "sb" || cmd == "db";
        const bool isAdmin = cmd == "f" || cmd == "r" || cmd == "scfg" || cmd == "gcfg" || cmd == "save";
        if ((isWrite && role == Role::Read) || (isAdmin && role != Role::Admin)) {
            return { ResultType::Code::Error, Messages::PERMISSION_DENIED };
        }

        std::shared_lock<std::shared_mutex> sharedLock(this->writeMtx, std::defer_lock);
        std::unique_lock<std::shared_mutex> uniqueLock(this->writeMtx, std::defer_lock);
        if (cmd == "r" || cmd == "scfg") {
            uniqueLock.lock();
        } else if (cmd != "f" && cmd != "save") {
            sharedLock.lock();
        }

        if (cmd == "g") {
            if (words.size() < 2) {
                result.response = Messages::MISSING_KEY;
            } else {
                result = this->get(std::string(words[1]));
            }
        } else if (cmd == "s") {
            if (words.size() < 2) {
                result.response = Messages::MISSING_KEY;
            } else if (words.size() < 3) {
                result.response = Messages::MISSING_VALUE;
            } else {
                result = this->set(std::string(words[1]), std::string(sv.substr(static_cast<size_t>(words[2].data() - sv.data()))));
            }
        } else if (cmd == "d") {
            if (words.size() < 2) {
                result.response = Messages::MISSING_KEY;
            } else {
                result = this->del(std::string(words[1]));
            }
        } else if (cmd == "gb") {
            if (words.size() < 3) {
                result.response = Messages::MISSING_ARGUMENTS;
            } else {
                std::string mode = toLowerStr(words[1]);
                std::string_view pattern = words[2];
                unsigned int limit = UINT_MAX;
                bool limitOk = true;
                for (size_t i = 3; i < words.size(); i++) {
                    if (toLowerStr(words[i]) == "lim" && i + 1 < words.size()) {
                        limitOk = parseUInt(words[i + 1], limit);
                    }
                }
                auto [valid, validMode] = this->createValidationRule(mode, pattern);
                if (!limitOk) {
                    result.response = Messages::INVALID_LIMIT;
                } else if (!validMode) {
                    result.response = Messages::INVALID_REGEX;
                } else {
                    valid.limit = limit;
                    result = this->getBy(valid);
                }
            }
        } else if (cmd == "sb") {
            if (words.size() < 4) {
                result.response = Messages::MISSING_VALUE;
            } else {
                std::string mode = toLowerStr(words[1]);
                std::string_view pattern = words[2];
                unsigned int limit = UINT_MAX;
                bool limitOk = true;

                size_t valStart = static_cast<size_t>(words[3].data() - sv.data());
                size_t valEnd = sv.size();
                if (words.size() >= 6 && toLowerStr(words[words.size() - 2]) == "lim") {
                    limitOk = parseUInt(words.back(), limit);
                    std::string_view lastValueWord = words[words.size() - 3];
                    valEnd = static_cast<size_t>(lastValueWord.data() + lastValueWord.size() - sv.data());
                }

                if (!limitOk) {
                    result.response = Messages::INVALID_LIMIT;
                } else {
                    std::string value(sv.substr(valStart, valEnd - valStart));

                    auto [valid, validMode] = this->createValidationRule(mode, pattern);
                    if (!validMode) {
                        result.response = Messages::INVALID_REGEX;
                    } else {
                        valid.limit = limit;
                        result = this->setBy(valid, value);
                    }
                }
            }
        } else if (cmd == "db") {
            if (words.size() < 3) {
                result.response = Messages::MISSING_PATTERN;
            } else {
                std::string mode = toLowerStr(words[1]);
                std::string_view pattern = words[2];
                unsigned int limit = UINT_MAX;
                bool limitOk = true;
                for (size_t i = 3; i < words.size(); i++) {
                    if (toLowerStr(words[i]) == "lim" && i + 1 < words.size()) {
                        limitOk = parseUInt(words[i + 1], limit);
                    }
                }
                auto [valid, validMode] = this->createValidationRule(mode, pattern);
                if (!limitOk) {
                    result.response = Messages::INVALID_LIMIT;
                } else if (!validMode) {
                    result.response = Messages::INVALID_MODE;
                } else {
                    valid.limit = limit;
                    result = this->delBy(valid);
                }
            }
        } else if (cmd == "h") {
            result.code = ResultType::Code::Success;
            result.response = this->help();
        } else if (cmd == "f") {
            this->flush();
            result.code = ResultType::Code::Success;
            result.response = Messages::FLUSH_SUCCESSFUL;
        } else if (cmd == "r") {
            result = this->reloadConfig();
        } else if (cmd == "scfg") {
            if (words.size() < 3) {
                result.response = Messages::MISSING_ARGUMENTS;
                return result;
            }

            std::string param = toLowerStr(words[1]);
            std::string_view valueStr = words[2];

            bool valueSet = false;

            unsigned long ulongVal = 0;
            unsigned int uintVal = 0;

            if (param == "segsize") {
                if (!parseUInt(valueStr, ulongVal)) {
                    result.response = "Invalid SegSize value";
                    return result;
                }
                if (ulongVal <= this->settings.MaxValueSize) {
                    result.response = "SegSize must be greater than MaxValueSize";
                    return result;
                }
                this->settings.SegSize = ulongVal;
                valueSet = true;
            } else if (param == "maxkeysize") {
                if (!parseUInt(valueStr, uintVal)) {
                    result.response = "Invalid MaxKeySize value";
                    return result;
                }
                if (uintVal > MAX_KEY_SIZE) {
                    result.response = std::format("MaxKeySize cannot exceed {}", MAX_KEY_SIZE);
                    return result;
                }
                this->settings.MaxKeySize = uintVal;
                valueSet = true;
            } else if (param == "maxvaluesize") {
                if (!parseUInt(valueStr, uintVal)) {
                    result.response = "Invalid MaxValueSize value";
                    return result;
                }
                if (uintVal >= this->settings.SegSize) {
                    result.response = "MaxValueSize must be less than SegSize";
                    return result;
                }
                this->settings.MaxValueSize = uintVal;
                valueSet = true;
            } else if (param == "cachesize") {
                if (!parseUInt(valueStr, uintVal)) {
                    result.response = "Invalid CacheSize value";
                    return result;
                }
                this->settings.CacheSize = uintVal;
                valueSet = true;
            } else if (param == "compactinginterval") {
                if (!parseUInt(valueStr, uintVal) || uintVal == 0) {
                    result.response = "Invalid CompactingInterval value (at least 1 second)";
                    return result;
                }
                this->settings.CompactingInterval = uintVal;
                valueSet = true;
            } else if (param == "flushinterval") {
                if (!parseUInt(valueStr, uintVal) || uintVal == 0) {
                    result.response = "Invalid FlushInterval value (at least 1 second)";
                    return result;
                }
                this->settings.FlushInterval = uintVal;
                valueSet = true;
            } else if (param == "networkvalidationstr") {
                try {
                    std::string valueStrStr(valueStr);
                    std::regex testRegex(valueStrStr);
                    this->settings.NetworkValidationStr = std::move(valueStrStr);
                    this->settings.NetworkValidation = std::move(testRegex);
                    valueSet = true;
                } catch (const std::regex_error& e) {
                    result.response = std::format("Invalid regex: {}", e.what());
                    return result;
                }
            } else if (param == "isdebug") {
                if (valueStr == "true" || valueStr == "1") {
                    this->settings.isDebug = true;
                    valueSet = true;
                } else if (valueStr == "false" || valueStr == "0") {
                    this->settings.isDebug = false;
                    valueSet = true;
                } else {
                    result.response = "Invalid isDebug value (use true/false or 0/1)";
                    return result;
                }
                setLoggerDebugMode(this->settings.isDebug);
            } else if (param == "usessl") {
                result.response = "useSSL cannot be changed at runtime: edit config.yaml and restart";
                return result;
            } else if (param == "jsononly") {
                if (valueStr == "true" || valueStr == "1") {
                    this->settings.jsonOnly = true;
                    valueSet = true;
                } else if (valueStr == "false" || valueStr == "0") {
                    this->settings.jsonOnly = false;
                    valueSet = true;
                } else {
                    result.response = "Invalid jsonOnly value (use true/false or 0/1)";
                    return result;
                }
            } else if (param == "readonly") {
                if (valueStr == "true" || valueStr == "1") {
                    this->settings.readOnly = true;
                    valueSet = true;
                } else if (valueStr == "false" || valueStr == "0") {
                    this->settings.readOnly = false;
                    valueSet = true;
                } else {
                    result.response = "Invalid readOnly value (use true/false or 0/1)";
                    return result;
                }
            } else if (param == "archivecreationdelay") {
                if (!parseUInt(valueStr, uintVal) || uintVal == 0) {
                    result.response = "Invalid ArchiveCreationDelay value (at least 1 second)";
                    return result;
                }
                this->settings.ArchiveCreationDelay = uintVal;
                valueSet = true;
            } else if (param == "autoarchivesaving") {
                if (valueStr == "true" || valueStr == "1") {
                    this->settings.AutoArchiveSaving = true;
                    valueSet = true;
                } else if (valueStr == "false" || valueStr == "0") {
                    this->settings.AutoArchiveSaving = false;
                    valueSet = true;
                } else {
                    result.response = "Invalid AutoArchiveSaving value (use "
                                      "true/false or 0/1)";
                    return result;
                }
            } else {
                result.response = std::string("Unknown parameter: ") + std::string(param);
                return result;
            }

            if (valueSet) {
                result.code = ResultType::Code::Success;
                result.response = Messages::UPDATE_SUCCESSFUL;
                this->setConfig();
                this->shardManager->reloadSettings(this->settings);
            }
        } else if (cmd == "gcfg") {
            result.code = ResultType::Code::Success;
            result.response = this->getConfig();
        } else if (cmd == "save") {
            if (this->createArchive()) {
                result.code = ResultType::Code::Success;
                result.response = "Archive successfully created.";
            } else {
                result.response = "Failed to create the archive";
                return result;
            }
        } else {
            result.response = Messages::CMD_NOT_FOUND;
        }

        if (!this->replaying.load()) {
            if (result.code == ResultType::Code::Success && isWrite) {
                if (words.size() >= 2 && (!this->appendToWAL(std::string(words[1]), command) || (syncWal && !this->syncWAL()))) {
                    result = { ResultType::Code::Error, Messages::WAL_WRITE_FAILED, result.affectedRows };
                }
            }
        }

        return result;
    }

    std::string ZestDB::help() const {
        std::ostringstream oss;
        oss << "ZestDB Commands:" << "\n";
        oss << "---------------" << "\n";
        oss << "get <key>                              - Get value by key"
            << "\n";
        oss << "set <key> <value>                      - Set key-value pair"
            << "\n";
        oss << "del <key>                              - Delete key" << "\n";
        oss << "getby <mode> <pattern> [lim <n>]       - Get keys matching "
               "pattern"
            << "\n";
        oss << "setby <mode> <pattern> <val> [lim <n>] - Set value for keys "
               "matching pattern"
            << "\n";
        oss << "delby <mode> <pattern> [lim <n>]       - Delete keys matching "
               "pattern"
            << "\n";
        oss << "flush                                  - Flush all data in "
               "memory "
               "to the disk"
            << "\n";
        oss << "reload                                 - Reload configuration "
               "from "
               "config.yaml"
            << "\n";
        oss << "scfg <param> <value>                   - Set config parameter"
            << "\n";
        oss << "help                                   - Show this help"
            << "\n";
        oss << "\n";
        oss << "Config Parameters:" << "\n";
        oss << "  SegSize(int), MaxKeySize(int), MaxValueSize(int), "
               "CacheSize(int), CompactingInterval(int), FlushInterval(int), "
               "isDebug(bool), jsonOnly(bool), readOnly(bool), "
               "NetworkValidationStr(string), ArchiveCreationDelay(int), "
               "AutoArchiveSaving(bool)"
            << "\n";
        oss << "\n";
        oss << "Modes:" << "\n";
        oss << "  re  - regex" << "\n";
        oss << "  sw  - starts with" << "\n";
        oss << "  ct  - contains" << "\n";
        oss << "  ew  - ends with" << "\n";
        oss << "  lim - limit (optional)" << "\n";
        oss << "\n";
        oss << "Examples:" << "\n";
        oss << "  gb re .* lim 10                      - Get keys matching "
               "regex "
               "'.*' with limit 10"
            << "\n";
        oss << "  gb sw ca lim 20                      - Get keys starting "
               "with "
               "'ca' with limit 20"
            << "\n";
        oss << "  gb ct ac lim 1                       - Get keys containing "
               "'ac' "
               "with limit 1"
            << "\n";
        oss << "  gb ew ca                             - Get keys ending with "
               "'ca' "
               "(no limit)"
            << "\n";
        oss << "\n";
        oss << "Shortcuts: g=get, s=set, d=del, gb=getby, sb=setby, db=delby, "
               "f=flush, r=reload, scfg=setconfig, h=help"
            << "\n";
        return oss.str();
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
            if (resp.code == ResultType::Code::Error) {
                ZestLog(LogLevel::Warning, std::format("WAL replay: '{}' failed: {}", cmd, resp.response));
            }
            return responseToJson(resp);
        });
        this->replaying.store(false);
    }

} // namespace Zest
