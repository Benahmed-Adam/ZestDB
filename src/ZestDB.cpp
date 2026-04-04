#include <filesystem>
#include <fstream>
#include <iostream>

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "node.hpp"

namespace fs = std::filesystem;

ZestDB::ZestDB() : cache(100000)
{
    this->boot();
    this->indexManager = new IndexManager(this->settings);
}

ZestDB::~ZestDB() {
    delete this->indexManager;
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
}

std::string ZestDB::get(const std::string& key) {
    IndexEntry entry = this->cache.get(key);

    if (entry.segmentId == -1) {
        entry = this->indexManager->search(key);
    }

    if (entry.segmentId != -1) {
        std::string res = "gaaaaaaaaah";
        //TODO reechercher dans le disque
        return res;
    }

    return "nope";
}

void ZestDB::set(const std::string& key, const std::string& value) {
    // TODO Enreegistrer sur le disque & récupérer le IndexEntry, insérer dans indexManager puis dans le cache
}

void ZestDB::del(const std::string key) {
    IndexEntry entry = this->cache.get(key);

    if (entry.segmentId == -1) {
        entry = this->indexManager->search(key);
    } 
    
    if (entry.segmentId != -1) {
        entry.isTombstone = true;
        this->indexManager->update(key, entry);
        this->cache.remove(key);
    }
}