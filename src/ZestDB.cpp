#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "node.hpp"

namespace fs = std::filesystem;

ZestDB::ZestDB()
{
    ZestLog(LogLevel::DEBUG, "Initializing ZestDB...");
    this->boot();
    this->indexManager = new IndexManager(this->settings);
    this->storageManager = new StorageManager(this->settings);
    this->cache = new LRUCache(this->settings.CacheSize);
    ZestLog(LogLevel::INFO, "ZestDB initialized successfully");
}

ZestDB::~ZestDB()
{
    delete this->indexManager;
    delete this->storageManager;
    delete this->cache;
}

void ZestDB::boot()
{
    const fs::path current_path = fs::current_path().string();
    const std::string configName = "config.yaml";

    fs::path configPath = current_path / configName;

    if (!fs::exists(configPath)) {
        ZestLog(LogLevel::CRITICAL, configName + " doesn't exist !");
        exit(-1);
    }

    ZestLog(LogLevel::INFO, "Reading " + configName);

    try {
        std::ifstream configFile(configPath);
        std::stringstream buffer;
        buffer << configFile.rdbuf();

        auto node = fkyaml::node::deserialize(buffer.str());

        // Set settings values
        this->settings.DbPath = node["DbPath"].get_value_or<std::string>(current_path.string());
        this->settings.SegSize = node["SegSize"].get_value_or<unsigned long>(128000);
        this->settings.MaxKeySize = node["MaxKeySize"].get_value_or<unsigned int>(64);
        this->settings.MaxValueSize = node["MaxValueSize"].get_value_or<unsigned int>(10000);
        this->settings.CacheSize = node["CacheSize"].get_value_or<unsigned int>(1000);
        this->settings.KeyValidation = node["KeyValidation"].get_value_or<std::string>("");
        this->settings.ValueValidation = node["ValueValidation"].get_value_or<std::string>("");
    } catch (const std::exception& e) {
        ZestLog(LogLevel::ERROR, "Failed to parse config : " + std::string(e.what()));
        exit(-1);
    }

    if (this->settings.MaxValueSize >= this->settings.SegSize) {
        ZestLog(LogLevel::CRITICAL, "MaxValueSize is higher than SegSize ! MaxValueSize : " + std::to_string(this->settings.MaxValueSize) + " | SegSize : " + std::to_string(this->settings.SegSize));
        exit(-1);
    }

    ZestLog(LogLevel::DEBUG, "Database path : " + this->settings.DbPath.string());
    ZestLog(LogLevel::DEBUG, "SegSize : " + std::to_string(this->settings.SegSize));
    ZestLog(LogLevel::DEBUG, "MaxKeySize : " + std::to_string(this->settings.MaxKeySize));
    ZestLog(LogLevel::DEBUG, "MaxValueSize : " + std::to_string(this->settings.MaxValueSize));
    ZestLog(LogLevel::DEBUG, "CacheSize : " + std::to_string(this->settings.CacheSize));
    // ZestLog(LogLevel::DEBUG, "KeyValidation : " + this->settings.KeyValidation);
    // ZestLog(LogLevel::DEBUG, "ValueValidation : " + this->settings.ValueValidation);

    if (!fs::exists(this->settings.DbPath / "INDEX")) {
        fs::path indexPath = this->settings.DbPath / "INDEX";
        ZestLog(LogLevel::INFO, "Creating the INDEX at " + indexPath.string());

        if (auto parent = indexPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(indexPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, "Failed to create INDEX at " + indexPath.string());
            exit(-1);
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

ResultType ZestDB::get(const std::string& key)
{
    if (key.size() >= this->settings.MaxKeySize) {
        ZestLog(LogLevel::ERROR, "ZestDB::get - " + std::string(Messages::KEY_TOO_LONG) + " MaxKeySize : " + std::to_string(this->settings.MaxKeySize));
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    if (!std::regex_match(key, this->settings.KeyValidation)) {
        ZestLog(LogLevel::ERROR, "ZestDB::get - " + std::string(Messages::KEY_VALIDATION_FAILED));
        return { ResultType::Code::ERROR, Messages::KEY_VALIDATION_FAILED };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::get - looking for key: " + key);
    IndexEntry entry = this->cache->get(key);

    if (entry.segmentId == -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::get - key not in cache, searching index");
        entry = this->indexManager->search(key);
    }

    if (entry.segmentId != -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::get - found in segment: " + std::to_string(entry.segmentId));
        return { ResultType::Code::SUCCESS, this->storageManager->read(entry) };
    }

    ZestLog(LogLevel::WARNING, "ZestDB::get - key not found: " + key);
    return { ResultType::Code::ERROR, Messages::KEY_NOT_FOUND };
}

ResultType ZestDB::set(const std::string& key, const std::string& value)
{
    if (key.size() >= this->settings.MaxKeySize) {
        ZestLog(LogLevel::ERROR, "ZestDB::set - " + std::string(Messages::KEY_TOO_LONG) + " MaxKeySize : " + std::to_string(this->settings.MaxKeySize));
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    if (value.size() >= this->settings.MaxValueSize) {
        ZestLog(LogLevel::ERROR, "ZestDB::set - " + std::string(Messages::VALUE_TOO_LONG) + " MaxValueSize : " + std::to_string(this->settings.MaxValueSize));
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
    }

    if (!std::regex_match(key, this->settings.KeyValidation)) {
        ZestLog(LogLevel::ERROR, "ZestDB::get - " + std::string(Messages::KEY_VALIDATION_FAILED));
        return { ResultType::Code::ERROR, Messages::KEY_VALIDATION_FAILED };
    }

    if (!std::regex_match(value, this->settings.ValueValidation)) {
        ZestLog(LogLevel::ERROR, "ZestDB::set - " + std::string(Messages::VALUE_VALIDATION_FAILED));
        return { ResultType::Code::ERROR, Messages::VALUE_VALIDATION_FAILED };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::set - key: " + key + ", value size: " + std::to_string(value.size()));
    IndexEntry entry = this->storageManager->append(value);
    ZestLog(LogLevel::DEBUG, "ZestDB::set - appended to segment: " + std::to_string(entry.segmentId) + ", offset: " + std::to_string(entry.offset));
    memcpy(entry.key, key.c_str(), key.size());
    this->indexManager->insert(entry);
    this->cache->put(entry);
    ZestLog(LogLevel::INFO, "ZestDB::set - successfully set key: " + key);
    return { ResultType::Code::SUCCESS, std::string(Messages::SUCCESS_SET) + key };
}

ResultType ZestDB::del(const std::string& key)
{
    if (key.size() >= this->settings.MaxKeySize) {
        ZestLog(LogLevel::ERROR, "ZestDB::del - " + std::string(Messages::KEY_TOO_LONG) + " MaxKeySize : " + std::to_string(this->settings.MaxKeySize));
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    if (!std::regex_match(key, this->settings.KeyValidation)) {
        ZestLog(LogLevel::ERROR, "ZestDB::del - " + std::string(Messages::KEY_VALIDATION_FAILED));
        return { ResultType::Code::ERROR, Messages::KEY_VALIDATION_FAILED };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::del - deleting key: " + key);
    IndexEntry entry = this->cache->get(key);

    if (entry.segmentId == -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::del - key not in cache, searching index");
        entry = this->indexManager->search(key);
    }

    if (entry.segmentId != -1 && !entry.isTombstone) {
        entry.isTombstone = true;
        this->indexManager->update(key, entry);
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

    std::vector<IndexEntry> entries = this->indexManager->getAll();
    std::ostringstream oss;
    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
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

    if (value.size() >= this->settings.MaxValueSize) {
        ZestLog(LogLevel::ERROR, "ZestDB::setBy - " + std::string(Messages::VALUE_TOO_LONG) + " MaxValueSize : " + std::to_string(this->settings.MaxValueSize));
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
    }

    if (!std::regex_match(value, this->settings.ValueValidation)) {
        ZestLog(LogLevel::ERROR, "ZestDB::setBy - " + std::string(Messages::VALUE_VALIDATION_FAILED));
        return { ResultType::Code::ERROR, Messages::VALUE_VALIDATION_FAILED };
    }

    std::regex reg;
    try {
        reg = std::regex(patern);
    } catch (const std::regex_error& e) {
        ZestLog(LogLevel::ERROR, "ZestDB::setBy - invalid regex pattern: " + std::string(e.what()));
        return { ResultType::Code::ERROR, "Invalid regex pattern" };
    }

    std::vector<IndexEntry> entries = this->indexManager->getAll();
    int matchCount = 0;

    for (const IndexEntry& entry : entries) {
        std::string key(entry.key);
        if (std::regex_match(key, reg)) {
            ZestLog(LogLevel::DEBUG, "ZestDB::setBy - match found: " + key);
            IndexEntry newEntry = this->storageManager->append(value);
            memcpy(newEntry.key, entry.key, sizeof(entry.key));
            this->indexManager->update(key, newEntry);
            this->cache->put(newEntry);
            matchCount++;
        }
    }

    ZestLog(LogLevel::INFO, "ZestDB::setBy - successfully updated " + std::to_string(matchCount) + " entries");

    return { ResultType::Code::SUCCESS, "Value successfully modified for " + std::to_string(matchCount) + " entries" };
}

// ResultType ZestDB::delBy(const std::string& patern)
// {
// }