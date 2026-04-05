#include <filesystem>
#include <fstream>
#include <iostream>

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "node.hpp"

namespace fs = std::filesystem;

ZestDB::ZestDB()
    : cache(100000)
{
    ZestLog(LogLevel::DEBUG, "Initializing ZestDB...");
    this->boot();
    this->indexManager = new IndexManager(this->settings);
    this->storageManager = new StorageManager(this->settings);
    ZestLog(LogLevel::INFO, "ZestDB initialized successfully");
}

ZestDB::~ZestDB()
{
    delete this->indexManager;
    delete this->storageManager;
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
    } catch (const std::exception& e) {
        ZestLog(LogLevel::ERROR, "Failed to parse config : " + std::string(e.what()));
        exit(-1);
    }

    ZestLog(LogLevel::DEBUG, "Database path : " + this->settings.DbPath.string());

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
        ZestLog(LogLevel::WARNING, "Creating seeg folder at " + (this->settings.DbPath / "seg").string());
        fs::create_directory(this->settings.DbPath / "seg");
    }
}

std::string ZestDB::get(const std::string& key)
{
    ZestLog(LogLevel::DEBUG, "ZestDB::get - looking for key: " + key);
    IndexEntry entry = this->cache.get(key);

    if (entry.segmentId == -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::get - key not in cache, searching index");
        entry = this->indexManager->search(key);
    }

    if (entry.segmentId != -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::get - found in segment: " + std::to_string(entry.segmentId));
        return this->storageManager->read(entry);
    }

    ZestLog(LogLevel::WARNING, "ZestDB::get - key not found: " + key);
    return "nope";
}

void ZestDB::set(const std::string& key, const std::string& value)
{
    ZestLog(LogLevel::DEBUG, "ZestDB::set - key: " + key + ", value size: " + std::to_string(value.size()));
    IndexEntry entry = this->storageManager->append(value);
    ZestLog(LogLevel::DEBUG, "ZestDB::set - appended to segment: " + std::to_string(entry.segmentId) + ", offset: " + std::to_string(entry.offset));
    memcpy(entry.key, key.c_str(), key.size());
    this->indexManager->insert(entry);
    this->cache.put(key, entry);
    ZestLog(LogLevel::INFO, "ZestDB::set - successfully set key: " + key);
}

void ZestDB::del(const std::string key)
{
    ZestLog(LogLevel::DEBUG, "ZestDB::del - deleting key: " + key);
    IndexEntry entry = this->cache.get(key);

    if (entry.segmentId == -1) {
        ZestLog(LogLevel::DEBUG, "ZestDB::del - key not in cache, searching index");
        entry = this->indexManager->search(key);
    }

    if (entry.segmentId != -1 && !entry.isTombstone) {
        entry.isTombstone = true;
        this->indexManager->update(key, entry);
        this->cache.remove(key);
        ZestLog(LogLevel::INFO, "ZestDB::del - successfully deleted key: " + key);
    } else {
        ZestLog(LogLevel::WARNING, "ZestDB::del - key not found: " + key);
    }
}