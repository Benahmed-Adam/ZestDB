#include "ZestDB.hpp"
#include "Logger.hpp"
#include "node.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

ZestDB::ZestDB() {
    this->boot();
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
    } 
    catch (const std::exception& e) {
        ZestLog(LogLevel::ERROR, "Failed to parse config: " + std::string(e.what()));
        exit(-1);
    }

    ZestLog(LogLevel::DEBUG, "Database path : " + this->settings.DbPath);
}