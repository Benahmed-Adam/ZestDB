#include "ZestDB.hpp"

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

    std::string ZestDB::responseToJson(const ResultType& resp) {
        json j;
        j["code"] = static_cast<unsigned int>(resp.code);
        j["response"] = resp.response;
        if (!resp.response.empty() && (resp.response.front() == '[' || resp.response.front() == '{')) {
            if (json parsed = json::parse(resp.response, nullptr, false); !parsed.is_discarded()) {
                j["response"] = std::move(parsed);
            }
        }
        j["affectedRows"] = resp.affectedRows;
        return j.dump();
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
        ZestLog(LogLevel::INFO, "Initializing ZestDB...");
        this->boot();

        this->shardManager = std::make_unique<ShardManager>(this->settings, NUM_SHARDS);
        this->socket = std::make_unique<Server>(this->ioCtx, this->settings.DBPort, *this);

        this->replayWAL();

        ZestLog(LogLevel::INFO, std::format("ZestDB initialized with {} shards", NUM_SHARDS));

        this->flushThread = std::jthread([this](std::stop_token stopToken) {
            std::unique_lock<std::mutex> lock(this->flushThreadMtx);

            while (this->settings.isRunning && !stopToken.stop_requested()) {
                unsigned int interval;
                {
                    std::shared_lock<std::shared_mutex> settingsLock(this->writeMtx);
                    interval = this->settings.FlushInterval;
                }
                this->threadCV.wait_for(lock, stopToken, std::chrono::seconds(interval), [this, stopToken] { return stopToken.stop_requested() || !this->settings.isRunning; });

                if (stopToken.stop_requested() || !this->settings.isRunning) {
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
                ZestLog(LogLevel::WARNING, "Archive storage path does not exist or is not a directory.");
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
                        ZestLog(LogLevel::WARNING, "Failed to get last write time for file: " + entry.path().string());
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

            while (this->settings.isRunning && !stopToken.stop_requested()) {

                this->threadCV.wait_for(lock, stopToken, std::chrono::seconds(delay), [this, stopToken] { return stopToken.stop_requested() || !this->settings.isRunning; });

                if (stopToken.stop_requested() || !this->settings.isRunning) {
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
                    ZestLog(LogLevel::INFO, this->createArchive() ? "Autosave completed" : "An error occured during zip creation");
                    lock.lock();
                }
            }
        });

        if (this->settings.useSSL) {
            this->srv = std::make_unique<httplib::SSLServer>(this->settings.SSLCertPath.string().c_str(), this->settings.SSLKeyPath.string().c_str());
            ZestLog(LogLevel::INFO, "Server mode: Encrypted");
        } else {
            this->srv = std::make_unique<httplib::Server>();
            ZestLog(LogLevel::INFO, "Server mode: Plain");
        }

        this->srv->Get("/", [this](const httplib::Request&, httplib::Response& res) {
            try {
                res.status = 200;

                json result = json::object();

                const auto& shards = this->shardManager->getShards();
                for (size_t i = 0; i < shards.size(); ++i) {
                    std::string shardKey = "shard" + std::to_string(i);
                    result["stats"][shardKey] = shards[i]->getPerfMonitoring().getPerformances();
                }

                res.set_content(result.dump(), "application/json");
            } catch (const std::exception& e) {
                ZestLog(LogLevel::CRITICAL, e.what());
            }
        });
    }

    ZestDB::~ZestDB() {
        if (this->settings.isRunning)
            this->stop();
    }

    void ZestDB::stop() {
        ZestLog(LogLevel::INFO, "Exiting ZestDB...");

        this->settings.isRunning = false;

        this->ioCtx.stop();
        if (this->srv)
            this->srv->stop();

        this->shardManager->stop();

        this->threadCV.notify_all();
    }

    void ZestDB::boot() {
        this->settings = this->loadConfig(this->users);
        setLoggerDebugMode(this->settings.isDebug);

        if (!this->settings.useSSL && !this->users.empty()) {
            ZestLog(LogLevel::WARNING, "useSSL is false: authentication tokens and data travel in clear and a captured token can be replayed. Enable useSSL outside a trusted network.");
        }

        ZestLog(LogLevel::DEBUG, std::format("Boot config: path={}, cache={}, segSize={}, dbPort={}, webPort={}", this->settings.DbPath.string(), this->settings.CacheSize, this->settings.SegSize, this->settings.DBPort, this->settings.WebPort));

        if (!fs::exists(this->settings.DbPath)) {
            fs::create_directories(this->settings.DbPath);
        }
    }

    template <typename T>
    static T getOr(fkyaml::node& node, const std::string& key, T def) {
        return node.contains(key) && !node[key].is_null() ? node[key].get_value<T>() : def;
    }

    Settings ZestDB::loadConfig(std::unordered_map<std::string, std::string>& usersOut) {
        Settings result;
        std::unordered_map<std::string, std::string> parsedUsers;

        const fs::path current_path = fs::current_path().string();
        const std::string configName = "config.yaml";

        fs::path configPath = current_path / configName;

        if (!fs::exists(configPath)) {
            ZestLog(LogLevel::CRITICAL, configName + " doesn't exist !");
            throw std::runtime_error(configName + " doesn't exist !");
        }

        ZestLog(LogLevel::INFO, "Reading " + configName);

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
                ZestLog(LogLevel::CRITICAL, "DBPort and WebPort must be between 1 and 65535");
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

            if (result.useSSL && (result.SSLCertPath == "" || result.SSLKeyPath == "")) {
                ZestLog(LogLevel::CRITICAL, "SSL enabled and not certificate or key path provided !");
                throw std::runtime_error("SSL with no cert or key");
            }

            if (!result.NetworkValidationStr.empty()) {
                try {
                    result.NetworkValidation = std::regex(result.NetworkValidationStr);
                } catch (const std::regex_error& e) {
                    ZestLog(LogLevel::CRITICAL, "Invalid NetworkValidation regex: " + std::string(e.what()));
                    throw std::runtime_error("Invalid NetworkValidation regex");
                }
            }

            if (node.contains("users") && node["users"].is_sequence()) {
                for (const auto& user_node : node["users"]) {
                    std::string username = user_node["user"].get_value<std::string>();
                    if (user_node.contains("tokenHash")) {
                        parsedUsers[username] = user_node["tokenHash"].get_value<std::string>();
                    } else {
                        parsedUsers[username] = sha256(sha256(username + user_node["password"].get_value<std::string>()));
                    }
                }
            }
        } catch (const std::exception& e) {
            ZestLog(LogLevel::ERROR, "Failed to parse config : " + std::string(e.what()));
            throw std::runtime_error("Failed to parse config: " + std::string(e.what()));
        }

        if (result.MaxValueSize >= result.SegSize) {
            ZestLog(LogLevel::CRITICAL, std::format("MaxValueSize is higher than SegSize ! "
                                                    "MaxValueSize : {} | SegSize : {}",
                                                    result.MaxValueSize, result.SegSize));
            throw std::runtime_error("MaxValueSize >= SegSize");
        }

        if (result.MaxKeySize > MAX_KEY_SIZE) {
            ZestLog(LogLevel::WARNING, std::format("MaxKeySize ({}) exceeds internal limit ({}), clamping...", result.MaxKeySize, MAX_KEY_SIZE));
            result.MaxKeySize = MAX_KEY_SIZE;
        }

        if (result.FlushInterval == 0 || result.CompactingInterval == 0 || result.ArchiveCreationDelay == 0) {
            ZestLog(LogLevel::CRITICAL, "FlushInterval, CompactingInterval and ArchiveCreationDelay must be at least 1 second");
            throw std::runtime_error("Interval of 0 seconds");
        }

        if (result.DBPort == result.WebPort) {
            ZestLog(LogLevel::CRITICAL, "DBPort and WebPort cannot be the same");
            throw std::runtime_error("DBPort == WebPort");
        }

        usersOut = std::move(parsedUsers);
        return result;
    }

    ResultType ZestDB::reloadConfig() {
        try {
            Settings oldSettings = this->settings;
            std::unordered_map<std::string, std::string> newUsers;
            Settings s = this->loadConfig(newUsers);

            if (s.DbPath != oldSettings.DbPath || s.ArchiveStoragePath != oldSettings.ArchiveStoragePath || s.IndexPath != oldSettings.IndexPath || s.WalPath != oldSettings.WalPath || s.SSLCertPath != oldSettings.SSLCertPath ||
                s.SSLKeyPath != oldSettings.SSLKeyPath) {
                ZestLog(LogLevel::ERROR, "Cannot change path settings during "
                                         "hot-reload. Paths are immutable.");
                return { ResultType::Code::ERROR, "Cannot change path settings during hot-reload. Paths are "
                                                  "immutable. Restart the database to apply changes." };
            }

            if (s.useSSL != oldSettings.useSSL || s.DBPort != oldSettings.DBPort || s.WebPort != oldSettings.WebPort) {
                ZestLog(LogLevel::ERROR, "Cannot change the ports during hot-reload.");
                return { ResultType::Code::ERROR, "Cannot change the ports settings during hot-reload. "
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

            return { ResultType::Code::SUCCESS, "Reload successful !" };
        } catch (const std::exception& e) {
            ZestLog(LogLevel::ERROR, std::format("An error occured during the reload of the "
                                                 "configuration : {}. Aborting...",
                                                 e.what()));
        }
        return { ResultType::Code::ERROR, "Reload failed !" };
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

        std::ofstream outFile(configPath);
        outFile << node;
        outFile.close();

        ZestLog(LogLevel::INFO, "Configuration saved to config.yaml");
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
        return j.dump();
    }

    bool ZestDB::createArchive() {
        std::unique_lock<std::shared_mutex> lock(this->writeMtx);
        this->flushLocked();

        std::string folder_to_zip = this->settings.DbPath.string();

        if (!fs::exists(folder_to_zip) || !fs::is_directory(folder_to_zip)) {
            ZestLog(LogLevel::ERROR, std::format("Source folder does not exist : {}", folder_to_zip));
            return false;
        }

        try {
            fs::create_directories(this->settings.ArchiveStoragePath);
        } catch (const std::exception& e) {
            ZestLog(LogLevel::ERROR, std::format("Error while creating archives folder : {}", e.what()));
            return false;
        }

        auto now = std::chrono::system_clock::now();
        fs::path archive_full_path = this->settings.ArchiveStoragePath / std::format("zestdb_archive_{:%Y-%m-%d_%H-%M-%S}.zip", now);

        libzippp::ZipArchive archive(archive_full_path.string());
        if (!archive.open(libzippp::ZipArchive::New)) {
            ZestLog(LogLevel::ERROR, std::format("Impossible to create the archive : {}", archive_full_path.string()));
            return false;
        }

        std::error_code ec;
        const fs::path archiveDir = fs::weakly_canonical(this->settings.ArchiveStoragePath, ec);
        for (auto it = fs::recursive_directory_iterator(folder_to_zip); it != fs::recursive_directory_iterator(); ++it) {
            const auto& entry = *it;
            if (entry.is_directory() && fs::weakly_canonical(entry.path(), ec) == archiveDir) {
                it.disable_recursion_pending();
                continue;
            }
            if (fs::is_regular_file(entry.path())) {
                fs::path relative_path = fs::relative(entry.path(), folder_to_zip);

                std::string zip_entry_name = relative_path.generic_string();

                if (!archive.addFile(zip_entry_name, entry.path().string())) {
                    ZestLog(LogLevel::ERROR, std::format("Error while adding : {}", relative_path.string()));
                    archive.close();
                    return false;
                }
            }
        }

        int close_result = archive.close();
        if (close_result != LIBZIPPP_OK) {
            ZestLog(LogLevel::ERROR, std::format("Error while saving the archive : Error code : {}", close_result));
            return false;
        }

        return true;
    }

    bool ZestDB::validateToken(const std::string& username, const std::string& token) const {
        std::shared_lock<std::shared_mutex> lock(this->usersMtx);
        auto it = this->users.find(username);
        if (it == this->users.end()) {
            return false;
        }

        const std::string& expected = it->second;
        const std::string actual = sha256(token);
        return actual.size() == expected.size() && CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) == 0;
    }

    bool ZestDB::isAddressAllowed(const std::string& ip) {
        std::shared_lock<std::shared_mutex> lock(this->writeMtx);
        return std::regex_match(ip, this->settings.NetworkValidation);
    }

    bool ZestDB::validateKey(std::string_view key) const {
        if (key.size() > this->settings.MaxKeySize) {
            ZestLog(LogLevel::ERROR, std::format("ZestDB::validateKey - {} MaxKeySize : {}", Messages::KEY_TOO_LONG, this->settings.MaxKeySize));
            return false;
        }
        return true;
    }

    bool ZestDB::validateValue(std::string_view value) const {
        if (value.size() > this->settings.MaxValueSize) {
            ZestLog(LogLevel::ERROR, std::format("ZestDB::validateValue - {} MaxValueSize : {}", Messages::VALUE_TOO_LONG, this->settings.MaxValueSize));
            return false;
        }
        return true;
    }

    bool ZestDB::isJsonValid(std::string_view value) const {
        return nlohmann::json::accept(value);
    }

    ResultType ZestDB::get(const std::string& key) {
        if (!this->validateKey(key)) {
            return { ResultType::Code::ERROR, Messages::INVALID_KEY };
        }

        return this->shardManager->get(key);
    }

    ResultType ZestDB::set(const std::string& key, const std::string& value) {
        const bool enforce = !this->replaying.load();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
        }

        if (enforce && !this->validateKey(key)) {
            return { ResultType::Code::ERROR, Messages::INVALID_KEY };
        }

        if (value.empty()) {
            return { ResultType::Code::ERROR, Messages::VALUE_EMPTY };
        }

        if (enforce && !this->validateValue(value)) {
            return { ResultType::Code::ERROR, Messages::INVALID_VALUE };
        }

        if (enforce && this->settings.jsonOnly) {
            if (!this->isJsonValid(value)) {
                return { ResultType::Code::ERROR, Messages::JSON_ONLY_ERROR };
            }
        }

        return this->shardManager->set(key, value);
    }

    ResultType ZestDB::del(const std::string& key) {
        const bool enforce = !this->replaying.load();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
        }

        if (enforce && !this->validateKey(key)) {
            return { ResultType::Code::ERROR, Messages::INVALID_KEY };
        }

        return this->shardManager->del(key);
    }

    ResultType ZestDB::getBy(ValidationRule& valid) {
        return this->shardManager->getBy(valid);
    }

    ResultType ZestDB::setBy(ValidationRule& valid, const std::string& value) {
        const bool enforce = !this->replaying.load();

        if (enforce && this->settings.readOnly) {
            return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
        }

        if (value.empty()) {
            return { ResultType::Code::ERROR, Messages::VALUE_EMPTY };
        }

        if (enforce && !this->validateValue(value)) {
            return { ResultType::Code::ERROR, Messages::INVALID_VALUE };
        }

        if (enforce && this->settings.jsonOnly) {
            if (!this->isJsonValid(value)) {
                return { ResultType::Code::ERROR, Messages::JSON_ONLY_ERROR };
            }
        }

        return this->shardManager->setBy(valid, value);
    }

    ResultType ZestDB::delBy(ValidationRule& valid) {
        if (!this->replaying.load() && this->settings.readOnly) {
            return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
        }

        return this->shardManager->delBy(valid);
    }

    CreationValidationRuleResult ZestDB::createValidationRule(std::string_view mode, std::string_view pattern) const {
        ValidationRule valid;
        bool validMode = true;

        if (mode == "re") {
            try {
                std::string patStr(pattern);
                if (std::regex_search(patStr, std::regex(R"(\\[1-9])"))) {
                    return { valid, false };
                }
                std::regex keyRegex(patStr, std::regex::ECMAScript | std::regex_constants::__polynomial);
                valid.func = [reg = std::move(keyRegex)](std::string_view key) { return std::regex_match(std::string(key), reg); };
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

    ResultType ZestDB::execCmd(const std::string& command) {
        std::string_view sv(command);
        auto start = sv.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) {
            return { ResultType::Code::ERROR, Messages::NO_COMMAND_GIVEN };
        }
        sv.remove_prefix(start);
        sv.remove_suffix(sv.size() - 1 - sv.find_last_not_of(" \t\r\n"));

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

        ResultType result = { ResultType::Code::ERROR, "" };

        if (words.empty()) {
            return { ResultType::Code::ERROR, Messages::NO_COMMAND_GIVEN };
        }

        std::string cmd = toLowerStr(words[0]);

        static const std::unordered_map<std::string_view, std::string_view> aliases = {
            { "get", "g" }, { "set", "s" }, { "del", "d" }, { "getby", "gb" }, { "setby", "sb" }, { "delby", "db" }, { "flush", "f" }, { "reload", "r" }, { "help", "h" },
        };
        if (auto it = aliases.find(cmd); it != aliases.end()) {
            cmd = it->second;
        }

        const bool isWrite = cmd == "s" || cmd == "d" || cmd == "sb" || cmd == "db";
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
            result.code = ResultType::Code::SUCCESS;
            result.response = this->help();
        } else if (cmd == "f") {
            this->flush();
            result.code = ResultType::Code::SUCCESS;
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
                if (valueStr == "true" || valueStr == "1") {
                    this->settings.useSSL = true;
                    valueSet = true;
                } else if (valueStr == "false" || valueStr == "0") {
                    this->settings.useSSL = false;
                    valueSet = true;
                } else {
                    result.response = "Invalid useSSL value (use true/false or 0/1)";
                    return result;
                }
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
                result.code = ResultType::Code::SUCCESS;
                result.response = Messages::UPDATE_SUCCESSFUL;
                this->setConfig();
                this->shardManager->reloadSettings(this->settings);
            }
        } else if (cmd == "gcfg") {
            result.code = ResultType::Code::SUCCESS;
            result.response = this->getConfig();
        } else if (cmd == "save") {
            if (this->createArchive()) {
                result.code = ResultType::Code::SUCCESS;
                result.response = "Archive successfully created.";
            } else {
                result.response = "Failed to create the archive";
                return result;
            }
        } else {
            result.response = Messages::CMD_NOT_FOUND;
        }

        if (!this->replaying.load()) {
            if (result.code == ResultType::Code::SUCCESS && isWrite) {
                if (words.size() >= 2 && !this->appendToWAL(std::string(words[1]), command)) {
                    result = { ResultType::Code::ERROR, Messages::WAL_WRITE_FAILED, result.affectedRows };
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
               "isDebug(bool), jsonOnly(bool), useSSL(bool), readOnly(bool), "
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
        ZestLog(LogLevel::DEBUG, "Flushing all shards...");
        this->isFlushing.store(true);
        this->shardManager->flush();
        this->isFlushing.store(false);
    }

    void ZestDB::replayWAL() {
        ZestLog(LogLevel::INFO, "Replaying WAL...");
        this->replaying.store(true);
        this->shardManager->replayAllWAL([this](const std::string& cmd) {
            auto resp = this->execCmd(cmd);
            if (resp.code == ResultType::Code::ERROR) {
                ZestLog(LogLevel::WARNING, std::format("WAL replay: '{}' failed: {}", cmd, resp.response));
            }
            return responseToJson(resp);
        });
        this->replaying.store(false);
    }

} // namespace Zest
