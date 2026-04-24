#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <openssl/evp.h>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "node.hpp"

namespace fs = std::filesystem;

std::string sha256(const std::string& str)
{
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
    : initialized(false)
    , replaying(false)
{
    ZestLog(LogLevel::INFO, "Initializing ZestDB...");
    this->boot();

    this->indexManager = std::make_unique<IndexManager>(this->settings);
    this->storageManager = std::make_unique<StorageManager>(this->settings);
    this->cache = std::make_unique<LRUCache>(this->settings.CacheSize);
    this->compactor = std::make_unique<Compactor>(this->settings.CompactingInterval);
    this->wal = std::make_unique<WAL>(this->settings);
    this->socket = std::make_unique<Server>(this->ioCtx, this->settings.DBPort, *this);

    this->replayWAL();

    ZestLog(LogLevel::INFO, "ZestDB initialized successfully");

    std::promise<void> cachePromise;
    std::future<void> cacheFuture = cachePromise.get_future();

    std::thread cacheThread([this, promise = std::move(cachePromise)]() mutable {
        this->fillCache();
        promise.set_value();
    });

    std::thread compactorThread([this]() mutable {
        this->compactor->run(*this->indexManager, *this->storageManager, this->settings.isRunning);
    });

    this->initialized.store(true);
    cacheFuture.wait();
    cacheThread.detach();
    compactorThread.detach();

    std::thread flushThread = std::thread([this]() {
        while (this->settings.isRunning) {
            std::this_thread::sleep_for(std::chrono::seconds(this->settings.FlushInterval));
            this->flush();
        }
    });
    flushThread.detach();

    this->srv.WebSocket("/ws", [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        if (!std::regex_match(req.remote_addr, this->settings.NetworkValidation)) {
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "authentication failed");
            return;
        }

        ZestLog(LogLevel::INFO, "Session: client connected");

        bool authenticated = false;
        std::string cmd;

        while (ws.read(cmd)) {
            if (!authenticated) {
                std::string authCmd = cmd;
                if (authCmd.find("Authorization: ") == 0) {
                    authCmd = authCmd.substr(15);
                }

                unsigned int dotPos = authCmd.find(".");
                if (dotPos != std::string::npos) {
                    std::string username = authCmd.substr(0, dotPos);
                    std::string token = authCmd.substr(dotPos + 1);

                    ZestLog(LogLevel::DEBUG, "WS auth - username: " + username + ", token: " + token);

                    if (this->users.find(username) == this->users.end()) {
                        ZestLog(LogLevel::DEBUG, "WS auth - user not found");
                        ws.send("ERROR: authentication failed");
                        ws.close(httplib::ws::CloseStatus::PolicyViolation, "authentication failed");
                        break;
                    }

                    ZestLog(LogLevel::DEBUG, "WS auth - stored token: " + this->users.at(username));

                    if (this->validateToken(username, token)) {
                        authenticated = true;
                        ws.send("OK: authenticated");
                    } else {
                        ws.send("ERROR: authentication failed");
                        ws.close(httplib::ws::CloseStatus::PolicyViolation, "authentication failed");
                        break;
                    }
                } else {
                    ws.send("ERROR: authentication required");
                    ws.close(httplib::ws::CloseStatus::PolicyViolation, "authentication required");
                    break;
                }
            } else {
                std::string result = this->execCmd(cmd);
                ws.send(result);
            }
        }

        ZestLog(LogLevel::INFO, "Session: client disconnected");
    });

    this->srv.Get("/", [this](const httplib::Request&, httplib::Response& res) {
        fs::path indexPath = fs::current_path() / "public" / "index.html";
        if (fs::exists(indexPath)) {
            std::ifstream file(indexPath);
            std::stringstream buffer;
            buffer << file.rdbuf();
            res.set_content(buffer.str(), "text/html");
        } else {
            res.status = 404;
            res.set_content("Not Found", "text/plain");
        }
    });
}

ZestDB::~ZestDB()
{
    if (this->settings.isRunning)
        this->stop();
}

bool ZestDB::validateToken(const std::string& username, const std::string& token) const
{
    if (this->users.find(username) == this->users.end()) {
        ZestLog(LogLevel::DEBUG, "ZestDB::validateToken - user not found: " + username);
        return false;
    }

    if (this->users.at(username) != token) {
        ZestLog(LogLevel::DEBUG, "ZestDB::validateToken - token mismatch");
        return false;
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::validateToken - success");
    return true;
}

bool ZestDB::validateKey(const std::string& key) const
{
    if (key.size() > this->settings.MaxKeySize) {
        ZestLog(LogLevel::ERROR, "ZestDB::validateKey - " + std::string(Messages::KEY_TOO_LONG) + " MaxKeySize : " + std::to_string(this->settings.MaxKeySize));
        return false;
    }

    if (!this->settings.KeyValidationStr.empty()) {
        try {
            if (!std::regex_match(key, this->settings.KeyValidation)) {
                ZestLog(LogLevel::ERROR, "ZestDB::validateKey - " + std::string(Messages::KEY_VALIDATION_FAILED));
                return false;
            }
        } catch (const std::regex_error& e) {
            ZestLog(LogLevel::ERROR, "ZestDB::validateKey - invalid key regex: " + std::string(e.what()));
            return false;
        }
    }
    return true;
}

bool ZestDB::validateValue(const std::string& value) const
{
    if (value.size() > this->settings.MaxValueSize) {
        ZestLog(LogLevel::ERROR, "ZestDB::validateValue - " + std::string(Messages::VALUE_TOO_LONG) + " MaxValueSize : " + std::to_string(this->settings.MaxValueSize));
        return false;
    }

    if (!this->settings.ValueValidationStr.empty()) {
        try {
            if (!std::regex_match(value, this->settings.ValueValidation)) {
                ZestLog(LogLevel::ERROR, "ZestDB::validateValue - " + std::string(Messages::VALUE_VALIDATION_FAILED));
                return false;
            }
        } catch (const std::regex_error& e) {
            ZestLog(LogLevel::ERROR, "ZestDB::validateValue - invalid value regex: " + std::string(e.what()));
            return false;
        }
    }
    return true;
}

void ZestDB::boot()
{
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

        this->settings.DbPath = node["DbPath"].get_value_or<std::string>(current_path.string());
        this->settings.SegSize = node["SegSize"].get_value_or<unsigned long>(128000);
        this->settings.MaxKeySize = node["MaxKeySize"].get_value_or<unsigned int>(64);
        this->settings.MaxValueSize = node["MaxValueSize"].get_value_or<unsigned int>(10000);
        this->settings.CacheSize = node["CacheSize"].get_value_or<unsigned int>(1000);
        this->settings.CompactingInterval = node["CompactingInterval"].get_value_or<unsigned int>(3600);
        this->settings.FlushInterval = node["FlushInterval"].get_value_or<unsigned int>(120);
        this->settings.DBPort = node["DBPort"].get_value_or<short>(7321);
        this->settings.WebPort = node["WebPort"].get_value_or<short>(1237);

        this->settings.KeyValidationStr = node["KeyValidation"].get_value_or<std::string>("");
        this->settings.ValueValidationStr = node["ValueValidation"].get_value_or<std::string>("");
        this->settings.NetworkValidationStr = node["NetworkValidation"].get_value_or<std::string>("");

        if (!this->settings.KeyValidationStr.empty()) {
            try {
                this->settings.KeyValidation = std::regex(this->settings.KeyValidationStr);
            } catch (const std::regex_error& e) {
                ZestLog(LogLevel::CRITICAL, "Invalid KeyValidation regex: " + std::string(e.what()));
                throw std::runtime_error("Invalid KeyValidation regex");
            }
        }

        if (!this->settings.ValueValidationStr.empty()) {
            try {
                this->settings.ValueValidation = std::regex(this->settings.ValueValidationStr);
            } catch (const std::regex_error& e) {
                ZestLog(LogLevel::CRITICAL, "Invalid ValueValidation regex: " + std::string(e.what()));
                throw std::runtime_error("Invalid ValueValidation regex");
            }
        }

        if (!this->settings.NetworkValidationStr.empty()) {
            try {
                this->settings.NetworkValidation = std::regex(this->settings.NetworkValidationStr);
            } catch (const std::regex_error& e) {
                ZestLog(LogLevel::CRITICAL, "Invalid NetworkValidation regex: " + std::string(e.what()));
                throw std::runtime_error("Invalid NetworkValidation regex");
            }
        }

        this->settings.isDebug = node["isDebug"].get_value_or<bool>(false);
        setLoggerDebugMode(this->settings.isDebug);

        if (node.contains("users") && node["users"].is_sequence()) {
            for (auto& user_node : node["users"]) {
                std::string username = user_node["user"].get_value<std::string>();
                std::string password = user_node["password"].get_value<std::string>();

                this->users[username] = sha256(username + password);
                ZestLog(LogLevel::DEBUG, "Loaded user: " + username + " with token: " + this->users[username]);
            }
        }
    } catch (const std::exception& e) {
        ZestLog(LogLevel::ERROR, "Failed to parse config : " + std::string(e.what()));
        throw std::runtime_error("Failed to parse config: " + std::string(e.what()));
    }

    if (this->settings.MaxValueSize >= this->settings.SegSize) {
        ZestLog(LogLevel::CRITICAL, "MaxValueSize is higher than SegSize ! MaxValueSize : " + std::to_string(this->settings.MaxValueSize) + " | SegSize : " + std::to_string(this->settings.SegSize));
        throw std::runtime_error("MaxValueSize >= SegSize");
    }

    if (this->settings.MaxKeySize > IndexEntry::MAX_KEY_SIZE) {
        ZestLog(LogLevel::WARNING, "MaxKeySize (" + std::to_string(this->settings.MaxKeySize) + ") exceeds internal limit (" + std::to_string(IndexEntry::MAX_KEY_SIZE) + "), clamping...");
        this->settings.MaxKeySize = IndexEntry::MAX_KEY_SIZE;
    }

    if (this->settings.DBPort < 0 || this->settings.WebPort < 0) {
        ZestLog(LogLevel::CRITICAL, "Ports cant be lower that zero");
        throw std::runtime_error("Port < 0");
    }

    if (this->settings.DBPort == this->settings.WebPort) {
        ZestLog(LogLevel::CRITICAL, "DBPort and WebPort cannot be the same");
        throw std::runtime_error("DBPort == WebPort");
    }

    ZestLog(LogLevel::DEBUG, "Database path : " + this->settings.DbPath.string());
    ZestLog(LogLevel::DEBUG, "SegSize : " + std::to_string(this->settings.SegSize));
    ZestLog(LogLevel::DEBUG, "MaxKeySize : " + std::to_string(this->settings.MaxKeySize));
    ZestLog(LogLevel::DEBUG, "MaxValueSize : " + std::to_string(this->settings.MaxValueSize));
    ZestLog(LogLevel::DEBUG, "CacheSize : " + std::to_string(this->settings.CacheSize));
    ZestLog(LogLevel::DEBUG, "CompactingInterval : " + std::to_string(this->settings.CompactingInterval));
    ZestLog(LogLevel::DEBUG, "FlushInterval : " + std::to_string(this->settings.FlushInterval));
    ZestLog(LogLevel::DEBUG, "isDebug : " + std::to_string(this->settings.isDebug));
    ZestLog(LogLevel::DEBUG, "DBPort : " + std::to_string(this->settings.DBPort));
    ZestLog(LogLevel::DEBUG, "WebPort : " + std::to_string(this->settings.WebPort));

    if (!fs::exists(this->settings.DbPath / "INDEX")) {
        fs::path indexPath = this->settings.DbPath / "INDEX";
        ZestLog(LogLevel::INFO, "Creating the INDEX at " + indexPath.string());

        if (auto parent = indexPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(indexPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, "Failed to create INDEX at " + indexPath.string());
            throw std::runtime_error("Failed to create INDEX");
        }
        this->settings.IndexPath = indexPath;
    } else {
        this->settings.IndexPath = this->settings.DbPath / "INDEX";
    }

    if (!fs::exists(this->settings.DbPath / "WAL")) {
        fs::path walPath = this->settings.DbPath / "WAL";
        ZestLog(LogLevel::INFO, "Creating the WAL at " + walPath.string());

        if (auto parent = walPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(walPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, "Failed to create WAL at " + walPath.string());
            throw std::runtime_error("Failed to create WAL");
        }
        this->settings.walPath = walPath;
    } else {
        this->settings.walPath = this->settings.DbPath / "WAL";
    }

    ZestLog(LogLevel::DEBUG, "INDEX path : " + this->settings.IndexPath.string());
    ZestLog(LogLevel::DEBUG, "WAL path : " + this->settings.walPath.string());

    if (!fs::exists(this->settings.DbPath / "seg")) {
        ZestLog(LogLevel::WARNING, "Creating seg folder at " + (this->settings.DbPath / "seg").string());
        fs::create_directory(this->settings.DbPath / "seg");
    }
}

void ZestDB::fillCache()
{
    while (!this->initialized.load()) {
        ZestLog(LogLevel::WARNING, "ZestDB::fillCache - not initialized yet, waiting...");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ZestLog(LogLevel::INFO, "Filling up the cache...");

    std::vector<IndexEntry> entries = this->indexManager->getAll();
    int numKeysInserted = 0;

    std::unordered_set<std::string> seenKeys;
    unsigned int entriesCount = static_cast<unsigned int>(entries.size());
    unsigned int cacheLimit = (this->settings.CacheSize < entriesCount) ? this->settings.CacheSize : entriesCount;

    for (unsigned int i = 0; i < cacheLimit; i++) {
        if (entries[i].segmentId == -1 || entries[i].isTombstone) {
            continue;
        }
        std::string key(entries[i].key);
        if (seenKeys.find(key) != seenKeys.end()) {
            continue;
        }
        seenKeys.insert(key);

        ZestLog(LogLevel::DEBUG, "Inserting the key : " + key + " in the cache");
        std::string value = this->storageManager->read(entries[i]);
        this->cache->put(entries[i], value);
        numKeysInserted++;
    }
    ZestLog(LogLevel::INFO, "Cache filled successfully with " + std::to_string(numKeysInserted) + " keys");
}

ResultType ZestDB::get(const std::string& key)
{
    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::get - looking for key: " + key);

    CacheEntry cacheEntry = this->cache->get(key);

    if (cacheEntry.index.segmentId != -1 && !cacheEntry.index.isTombstone) {
        ZestLog(LogLevel::DEBUG, "ZestDB::get - found in cache");
        return { ResultType::Code::SUCCESS, cacheEntry.value };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::get - key not in cache, searching index");

    std::shared_lock<std::shared_mutex> lock(this->readMtx);
    IndexEntry entry;
    entry = this->indexManager->search(key);

    if (entry.segmentId != -1 && !entry.isTombstone) {
        ZestLog(LogLevel::DEBUG, "ZestDB::get - found in segment: " + std::to_string(entry.segmentId));
        std::string value = this->storageManager->read(entry);

        this->cache->put(entry, value);

        return { ResultType::Code::SUCCESS, value };
    }

    ZestLog(LogLevel::WARNING, "ZestDB::get - key not found: " + key);
    return { ResultType::Code::ERROR, Messages::KEY_NOT_FOUND };
}

ResultType ZestDB::set(const std::string& key, const std::string& value)
{
    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    if (!this->validateValue(value)) {
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::set - key: " + key + ", value size: " + std::to_string(value.size()));

    std::lock_guard<std::shared_mutex> lock(this->readMtx);
    IndexEntry entry = this->storageManager->append(value);
    ZestLog(LogLevel::DEBUG, "ZestDB::set - appended to segment: " + std::to_string(entry.segmentId) + ", offset: " + std::to_string(entry.offset));

    memset(entry.key, 0, sizeof(entry.key));
    size_t copySize = (key.size() < sizeof(entry.key) - 1) ? key.size() : sizeof(entry.key) - 1;
    memcpy(entry.key, key.c_str(), copySize);
    entry.key[copySize] = '\0';

    this->indexManager->insert(entry);

    this->cache->put(entry, value);

    ZestLog(LogLevel::DEBUG, "ZestDB::set - successfully set key: " + key);
    return { ResultType::Code::SUCCESS, std::string(Messages::SUCCESS_SET) + key };
}

ResultType ZestDB::del(const std::string& key)
{
    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::del - deleting key: " + key);

    std::lock_guard<std::shared_mutex> lock(this->readMtx);
    IndexEntry entry;

    CacheEntry cacheEntry = this->cache->get(key);

    if (cacheEntry.index.segmentId != -1 && !cacheEntry.index.isTombstone) {
        entry = cacheEntry.index;
    }

    if (entry.segmentId == -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::del - key not in cache, searching index");
        entry = this->indexManager->search(key);
    }

    if (entry.segmentId != -1 && !entry.isTombstone) {
        entry.isTombstone = true;

        this->indexManager->update(key, entry);

        this->cache->remove(key);

        ZestLog(LogLevel::DEBUG, "ZestDB::del - successfully deleted key: " + key);
        return { ResultType::Code::SUCCESS, std::string(Messages::SUCCESS_DEL) + key };
    } else {
        ZestLog(LogLevel::WARNING, "ZestDB::del - key not found: " + key);
        return { ResultType::Code::ERROR, std::string(Messages::KEY_NOT_FOUND) + ": " + key };
    }
}

ResultType ZestDB::getBy(const std::string& patern)
{
    ZestLog(LogLevel::DEBUG, "ZestDB::getBy - searching with pattern: " + patern);

    if (patern.empty()) {
        ZestLog(LogLevel::ERROR, "ZestDB::getBy - pattern cannot be empty");
        return { ResultType::Code::ERROR, Messages::PATTERN_EMPTY };
    }

    std::regex reg;
    try {
        reg = std::regex(patern);
    } catch (const std::regex_error& e) {
        ZestLog(LogLevel::ERROR, "ZestDB::getBy - invalid regex pattern: " + std::string(e.what()));
        return { ResultType::Code::ERROR, Messages::INVALID_REGEX };
    }

    std::vector<IndexEntry> entries;
    entries = this->indexManager->getAll();

    std::ostringstream oss;
    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }
        std::string key(entry.key);
        if (std::regex_match(key, reg)) {
            ZestLog(LogLevel::DEBUG, "ZestDB::getBy - match found: " + key);
            std::string value = this->storageManager->read(entry);
            oss << key << ":" << value << ";";
            matchCount++;
        }
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::getBy - total matches: " + std::to_string(matchCount));

    return { ResultType::Code::SUCCESS, oss.str() };
}

ResultType ZestDB::setBy(const std::string& patern, const std::string& value)
{
    ZestLog(LogLevel::DEBUG, "ZestDB::setBy - searching with pattern: " + patern);

    if (patern.empty()) {
        ZestLog(LogLevel::ERROR, "ZestDB::setBy - pattern cannot be empty");
        return { ResultType::Code::ERROR, Messages::PATTERN_EMPTY };
    }

    if (!this->validateValue(value)) {
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
    }

    std::regex reg;
    try {
        reg = std::regex(patern);
    } catch (const std::regex_error& e) {
        ZestLog(LogLevel::ERROR, "ZestDB::setBy - invalid regex pattern: " + std::string(e.what()));
        return { ResultType::Code::ERROR, Messages::INVALID_REGEX };
    }

    std::vector<IndexEntry> entries;
    entries = this->indexManager->getAll();

    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }
        std::string key(entry.key);
        if (std::regex_match(key, reg)) {
            ZestLog(LogLevel::DEBUG, "ZestDB::setBy - match found: " + key);
            IndexEntry newEntry = this->storageManager->append(value);
            memset(newEntry.key, 0, sizeof(newEntry.key));
            size_t copySize = (key.size() < sizeof(newEntry.key) - 1) ? key.size() : sizeof(newEntry.key) - 1;
            memcpy(newEntry.key, key.c_str(), copySize);
            newEntry.key[copySize] = '\0';

            this->indexManager->insert(newEntry);

            this->cache->put(newEntry, value);

            matchCount++;
        }
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::setBy - successfully updated " + std::to_string(matchCount) + " entries");

    return { ResultType::Code::SUCCESS, "Value successfully modified for " + std::to_string(matchCount) + " entries" };
}

ResultType ZestDB::delBy(const std::string& patern)
{
    if (patern.empty()) {
        ZestLog(LogLevel::ERROR, "ZestDB::delBy - pattern cannot be empty");
        return { ResultType::Code::ERROR, Messages::PATTERN_EMPTY };
    }

    std::regex reg;
    try {
        reg = std::regex(patern);
    } catch (const std::regex_error& e) {
        ZestLog(LogLevel::ERROR, "ZestDB::delBy - invalid regex pattern: " + std::string(e.what()));
        return { ResultType::Code::ERROR, Messages::INVALID_REGEX };
    }

    std::vector<IndexEntry> entries;
    entries = this->indexManager->getAll();

    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
        if (entry.isTombstone || entry.segmentId == -1) {
            continue;
        }
        std::string key(entry.key);
        if (std::regex_match(key, reg)) {
            ZestLog(LogLevel::DEBUG, "ZestDB::delBy - match found: " + key);
            IndexEntry tombstoneEntry = entry;
            tombstoneEntry.isTombstone = true;

            this->indexManager->update(key, tombstoneEntry);
            this->cache->remove(key);

            matchCount++;
        }
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::delBy - total matches: " + std::to_string(matchCount));

    return { ResultType::Code::SUCCESS, "Successfully deleted " + std::to_string(matchCount) + " entries" };
}

std::string ZestDB::execCmd(const std::string& command)
{
    std::istringstream iss(command);
    std::string cmd;
    iss >> cmd;

    std::ostringstream oss;
    ResultType result = { ResultType::Code::ERROR, "" };

    if (cmd == "g" || cmd == "get") {
        std::string key;
        if (!(iss >> key)) {
            oss << Messages::MISSING_KEY << std::endl;
            oss << Messages::USAGE_GET << std::endl;
        }
        result = this->get(key);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << result.message << std::endl;
        } else {
            oss << "(not found): " << result.message << std::endl;
        }
    } else if (cmd == "s" || cmd == "set") {
        std::string key, value;
        if (!(iss >> key)) {
            oss << Messages::MISSING_KEY << std::endl;
            oss << Messages::USAGE_SET << std::endl;
        }
        if (!(iss >> value)) {
            oss << Messages::MISSING_VALUE << std::endl;
            oss << Messages::USAGE_SET << std::endl;
        }
        std::string rest;
        while (iss >> rest) {
            value += " " + rest;
        }
        result = this->set(key, value);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << "OK: " << result.message << std::endl;
        } else {
            oss << "ERROR: " << result.message << std::endl;
        }
    } else if (cmd == "d" || cmd == "del") {
        std::string key;
        if (!(iss >> key)) {
            oss << Messages::MISSING_KEY << std::endl;
            oss << Messages::USAGE_GET << std::endl;
        }
        result = this->del(key);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << "OK: " << result.message << std::endl;
        } else {
            oss << "ERROR: " << result.message << std::endl;
        }
    } else if (cmd == "gb" || cmd == "getby") {
        std::string pattern;
        if (!(iss >> pattern)) {
            oss << Messages::MISSING_PATTERN << std::endl;
            oss << Messages::USAGE_GETBY << std::endl;
        }
        result = this->getBy(pattern);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << result.message << std::endl;
        } else {
            oss << "(not found): " << result.message << std::endl;
        }
    } else if (cmd == "sb" || cmd == "setby") {
        std::string pattern, value;
        if (!(iss >> pattern)) {
            oss << Messages::MISSING_PATTERN << std::endl;
            oss << Messages::USAGE_SETBY << std::endl;
        }
        if (!(iss >> value)) {
            oss << Messages::MISSING_VALUE << std::endl;
            oss << Messages::USAGE_SETBY << std::endl;
        }
        std::string rest;
        while (iss >> rest) {
            value += " " + rest;
        }
        result = this->setBy(pattern, value);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << "OK: " << result.message << std::endl;
        } else {
            oss << "ERROR: " << result.message << std::endl;
        }
    } else if (cmd == "db" || cmd == "delby") {
        std::string pattern;
        if (!(iss >> pattern)) {
            oss << Messages::MISSING_PATTERN << std::endl;
            oss << Messages::USAGE_DELBY << std::endl;
        }
        result = this->delBy(pattern);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << "OK: " << result.message << std::endl;
        } else {
            oss << "ERROR: " << result.message << std::endl;
        }
    } else if (cmd == "h" || cmd == "help") {
        oss << this->help();
    } else if (cmd == "f" || cmd == "flush") {
        this->flush();
        oss << Messages::FLUSH_SUCCESSFUL << std::endl;
    } else {
        oss << "ERROR: " << Messages::CMD_NOT_FOUND << std::endl;
        oss << Messages::TYPE_HELP << std::endl;
    }

    if (!this->replaying.load()) {
        if (result.code == ResultType::Code::SUCCESS && cmd != "h" && cmd != "help" && cmd != "g" && cmd != "get" && cmd != "gb" && cmd != "getby" && cmd != "f" && cmd != "flush") {
            this->wal->append(command);
        }
    }

    return oss.str();
}

void ZestDB::stop()
{
    ZestLog(LogLevel::INFO, "Exiting ZestDB...");

    this->settings.isRunning = false;
    this->ioCtx.stop();
    this->srv.stop();

    this->flush();
}

std::string ZestDB::help() const
{
    std::ostringstream oss;
    oss << "ZestDB Commands:" << std::endl;
    oss << "---------------" << std::endl;
    oss << "get <key>             - Get value by key" << std::endl;
    oss << "set <key> <value>     - Set key-value pair" << std::endl;
    oss << "del <key>             - Delete key" << std::endl;
    oss << "getby <pattern>       - Get keys matching regex pattern" << std::endl;
    oss << "setby <pattern> <val> - Set value for keys matching pattern" << std::endl;
    oss << "delby <pattern>       - Delete keys matching pattern" << std::endl;
    oss << "flush                 - Flush all data in memory to the disk" << std::endl;
    oss << "help                  - Show this help" << std::endl;
    oss << std::endl;
    oss << "Shortcuts: g=get, s=set, d=del, gb=getby, sb=setby, db=delby, f=flush, h=help" << std::endl;
    return oss.str();
}

void ZestDB::flush()
{
    ZestLog(LogLevel::DEBUG, "Flushing the content of the INDEX and the data segments into the disk...");
    this->indexManager->flush();
    this->storageManager->flush();
    this->wal->clear();
}

void ZestDB::replayWAL()
{
    ZestLog(LogLevel::INFO, "Replaying WAL...");
    this->replaying.store(true);
    std::vector<std::string> cmds = this->wal->getCmds();

    for (const std::string& cmd : cmds) {
        ZestLog(LogLevel::INFO, "WAL replay: " + cmd);
        std::string result = this->execCmd(cmd);
        ZestLog(LogLevel::INFO, "WAL replay result: " + result);
    }

    this->flush();
    this->replaying.store(false);
    ZestLog(LogLevel::INFO, "WAL replay complete, processed " + std::to_string(cmds.size()) + " commands");
}