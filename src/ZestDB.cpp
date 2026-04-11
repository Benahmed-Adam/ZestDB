#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "node.hpp"

namespace fs = std::filesystem;

ZestDB::ZestDB()
    : initialized(false)
{
    ZestLog(LogLevel::DEBUG, "Initializing ZestDB...");
    this->boot();

    this->indexManager = std::make_unique<IndexManager>(this->settings);
    this->storageManager = std::make_unique<StorageManager>(this->settings);
    this->cache = std::make_unique<LRUCache>(this->settings.CacheSize);

    initialized.store(true);
    ZestLog(LogLevel::INFO, "ZestDB initialized successfully");

    std::promise<void> promise;
    std::future<void> future = promise.get_future();

    std::thread t([this, promise = std::move(promise)]() mutable {
        this->fillCache();
        promise.set_value();
    });

    t.detach();
    future.wait();
}

ZestDB::~ZestDB()
{
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

        this->settings.KeyValidationStr = node["KeyValidation"].get_value_or<std::string>("");
        this->settings.ValueValidationStr = node["ValueValidation"].get_value_or<std::string>("");

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

        this->settings.isDebug = node["isDebug"].get_value_or<bool>(false);
        setLoggerDebugMode(this->settings.isDebug);
    } catch (const std::exception& e) {
        ZestLog(LogLevel::ERROR, "Failed to parse config : " + std::string(e.what()));
        throw std::runtime_error("Failed to parse config: " + std::string(e.what()));
    }

    if (this->settings.MaxValueSize >= this->settings.SegSize) {
        ZestLog(LogLevel::CRITICAL, "MaxValueSize is higher than SegSize ! MaxValueSize : " + std::to_string(this->settings.MaxValueSize) + " | SegSize : " + std::to_string(this->settings.SegSize));
        throw std::runtime_error("MaxValueSize >= SegSize");
    }

    ZestLog(LogLevel::DEBUG, "Database path : " + this->settings.DbPath.string());
    ZestLog(LogLevel::DEBUG, "SegSize : " + std::to_string(this->settings.SegSize));
    ZestLog(LogLevel::DEBUG, "MaxKeySize : " + std::to_string(this->settings.MaxKeySize));
    ZestLog(LogLevel::DEBUG, "MaxValueSize : " + std::to_string(this->settings.MaxValueSize));
    ZestLog(LogLevel::DEBUG, "CacheSize : " + std::to_string(this->settings.CacheSize));
    ZestLog(LogLevel::DEBUG, "isDebug : " + std::to_string(this->settings.isDebug));

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

    ZestLog(LogLevel::DEBUG, "INDEX path : " + this->settings.IndexPath.string());

    if (!fs::exists(this->settings.DbPath / "seg")) {
        ZestLog(LogLevel::WARNING, "Creating seg folder at " + (this->settings.DbPath / "seg").string());
        fs::create_directory(this->settings.DbPath / "seg");
    }
}

void ZestDB::fillCache()
{
    if (!initialized.load()) {
        ZestLog(LogLevel::WARNING, "ZestDB::fillCache - not initialized yet, waiting...");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ZestLog(LogLevel::INFO, "Filling up the cache...");
    std::vector<IndexEntry> entries = this->indexManager->getAll();
    int numKeysInserted = 0;

    unsigned int entriesCount = static_cast<unsigned int>(entries.size());
    unsigned int cacheLimit = (this->settings.CacheSize < entriesCount) ? this->settings.CacheSize : entriesCount;

    for (unsigned int i = 0; i < cacheLimit; i++) {
        if (entries[i].segmentId == -1 || entries[i].isTombstone) {
            continue;
        }
        ZestLog(LogLevel::DEBUG, "Inserting the key : " + std::string(entries[i].key) + " in the cache");
        std::string value = this->storageManager->read(entries[i]);
        this->cache->put(entries[i], value);
        numKeysInserted++;
    }
    ZestLog(LogLevel::INFO, "Cache filled successfully with " + std::to_string(numKeysInserted) + " keys");
}

ResultType ZestDB::get(const std::string& key)
{
    if (!validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::get - looking for key: " + key);

    std::lock_guard<std::mutex> lock(this->mtx);

    CacheEntry cacheEntry = this->cache->get(key);

    if (cacheEntry.index.segmentId != -1 && !cacheEntry.index.isTombstone) {
        ZestLog(LogLevel::DEBUG, "ZestDB::get - found in cache");
        return { ResultType::Code::SUCCESS, cacheEntry.value };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::get - key not in cache, searching index");
    IndexEntry entry = this->indexManager->search(key);

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
    if (!validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    if (!validateValue(value)) {
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::set - key: " + key + ", value size: " + std::to_string(value.size()));

    std::lock_guard<std::mutex> lock(this->mtx);

    IndexEntry entry = this->storageManager->append(value);
    ZestLog(LogLevel::DEBUG, "ZestDB::set - appended to segment: " + std::to_string(entry.segmentId) + ", offset: " + std::to_string(entry.offset));

    memset(entry.key, 0, sizeof(entry.key));
    size_t copySize = (key.size() < sizeof(entry.key) - 1) ? key.size() : sizeof(entry.key) - 1;
    memcpy(entry.key, key.c_str(), copySize);
    entry.key[copySize] = '\0';

    this->indexManager->insert(entry);
    this->cache->put(entry, value);
    ZestLog(LogLevel::INFO, "ZestDB::set - successfully set key: " + key);
    return { ResultType::Code::SUCCESS, std::string(Messages::SUCCESS_SET) + key };
}

ResultType ZestDB::del(const std::string& key)
{
    if (!validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::del - deleting key: " + key);

    std::lock_guard<std::mutex> lock(this->mtx);

    CacheEntry cacheEntry = this->cache->get(key);

    if (cacheEntry.index.segmentId == -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::del - key not in cache, searching index");
        cacheEntry.index = this->indexManager->search(key);
    }

    if (cacheEntry.index.segmentId != -1 && !cacheEntry.index.isTombstone) {
        cacheEntry.index.isTombstone = true;
        this->indexManager->update(key, cacheEntry.index);
        this->cache->remove(key);
        ZestLog(LogLevel::INFO, "ZestDB::del - successfully deleted key: " + key);
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
        return { ResultType::Code::ERROR, "Pattern cannot be empty" };
    }

    std::regex reg;
    try {
        reg = std::regex(patern);
    } catch (const std::regex_error& e) {
        ZestLog(LogLevel::ERROR, "ZestDB::getBy - invalid regex pattern: " + std::string(e.what()));
        return { ResultType::Code::ERROR, "Invalid regex pattern" };
    }

    std::lock_guard<std::mutex> lock(this->mtx);

    std::vector<IndexEntry> entries = this->indexManager->getAll();
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
        return { ResultType::Code::ERROR, "Pattern cannot be empty" };
    }

    if (!validateValue(value)) {
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
    }

    std::regex reg;
    try {
        reg = std::regex(patern);
    } catch (const std::regex_error& e) {
        ZestLog(LogLevel::ERROR, "ZestDB::setBy - invalid regex pattern: " + std::string(e.what()));
        return { ResultType::Code::ERROR, "Invalid regex pattern" };
    }

    std::lock_guard<std::mutex> lock(this->mtx);

    std::vector<IndexEntry> entries = this->indexManager->getAll();
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

    ZestLog(LogLevel::INFO, "ZestDB::setBy - successfully updated " + std::to_string(matchCount) + " entries");

    return { ResultType::Code::SUCCESS, "Value successfully modified for " + std::to_string(matchCount) + " entries" };
}

ResultType ZestDB::delBy(const std::string& patern)
{
    if (patern.empty()) {
        ZestLog(LogLevel::ERROR, "ZestDB::delBy - pattern cannot be empty");
        return { ResultType::Code::ERROR, "Pattern cannot be empty" };
    }

    std::regex reg;
    try {
        reg = std::regex(patern);
    } catch (const std::regex_error& e) {
        ZestLog(LogLevel::ERROR, "ZestDB::delBy - invalid regex pattern: " + std::string(e.what()));
        return { ResultType::Code::ERROR, "Invalid regex pattern" };
    }

    std::lock_guard<std::mutex> lock(this->mtx);

    std::vector<IndexEntry> entries = this->indexManager->getAll();
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