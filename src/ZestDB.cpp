#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <iostream>
#include <openssl/evp.h>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>

#include <string_view>

#include "IndexManager.hpp"
#include "Logger.hpp"
#include "ZestDB.hpp"
#include "lib/json.hpp"
#include "lib/node.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

std::string ZestDB::responseToJson(const ResultType& resp)
{
    json j;
    j["code"] = static_cast<unsigned int>(resp.code);
    j["message"] = resp.message;
    j["affectedRows"] = resp.affectedRows;
    return j.dump();
}

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

    this->shardManager = std::make_unique<ShardManager>(this->settings, NUM_SHARDS);
    this->socket = std::make_unique<Server>(this->ioCtx, this->settings.DBPort, *this);

    this->replayWAL();

    ZestLog(LogLevel::INFO, std::format("ZestDB initialized with {} shards", NUM_SHARDS));

    this->initialized.store(true);

    std::thread flushThread = std::thread([this]() {
        while (this->settings.isRunning && !this->isFlushing.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(this->settings.FlushInterval));
            this->flush();
        }
    });
    flushThread.detach();

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

ZestDB::~ZestDB()
{
    if (this->settings.isRunning)
        this->stop();
}

void ZestDB::boot()
{
    this->settings = std::move(this->loadConfig());
    setLoggerDebugMode(this->settings.isDebug);

    ZestLog(LogLevel::DEBUG, std::format("Database path : {}", this->settings.DbPath.string()));
    ZestLog(LogLevel::DEBUG, std::format("SegSize : {}", this->settings.SegSize));
    ZestLog(LogLevel::DEBUG, std::format("MaxKeySize : {}", this->settings.MaxKeySize));
    ZestLog(LogLevel::DEBUG, std::format("MaxValueSize : {}", this->settings.MaxValueSize));
    ZestLog(LogLevel::DEBUG, std::format("CacheSize : {}", this->settings.CacheSize));
    ZestLog(LogLevel::DEBUG, std::format("CompactingInterval : {}", this->settings.CompactingInterval));
    ZestLog(LogLevel::DEBUG, std::format("FlushInterval : {}", this->settings.FlushInterval));
    ZestLog(LogLevel::DEBUG, std::format("isDebug : {}", this->settings.isDebug));
    ZestLog(LogLevel::DEBUG, std::format("isJson : {}", this->settings.jsonOnly));
    ZestLog(LogLevel::DEBUG, std::format("readOnly : {}", this->settings.readOnly));
    ZestLog(LogLevel::DEBUG, std::format("DBPort : {}", this->settings.DBPort));
    ZestLog(LogLevel::DEBUG, std::format("WebPort : {}", this->settings.WebPort));

    if (!fs::exists(this->settings.DbPath)) {
        fs::create_directories(this->settings.DbPath);
    }
}

Settings ZestDB::loadConfig()
{
    Settings result;

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

        result.DbPath = node["DbPath"].get_value_or<std::string>(current_path.string());
        result.SegSize = node["SegSize"].get_value_or<unsigned long>(128000);
        result.MaxKeySize = node["MaxKeySize"].get_value_or<unsigned int>(64);
        result.MaxValueSize = node["MaxValueSize"].get_value_or<unsigned int>(10000);
        result.CacheSize = node["CacheSize"].get_value_or<unsigned int>(1000);
        result.CompactingInterval = node["CompactingInterval"].get_value_or<unsigned int>(3600);
        result.FlushInterval = node["FlushInterval"].get_value_or<unsigned int>(120);
        result.DBPort = node["DBPort"].get_value_or<short>(7321);
        result.WebPort = node["WebPort"].get_value_or<short>(1237);

        result.NetworkValidationStr = node["NetworkValidation"].get_value_or<std::string>("");

        result.isDebug = node["isDebug"].get_value_or<bool>(false);
        result.jsonOnly = node["jsonOnly"].get_value_or<bool>(false);
        result.readOnly = node["readOnly"].get_value_or<bool>(false);

        result.useSSL = node["useSSL"].get_value_or<bool>(false);
        result.SSLCertPath = node["SSLCertPath"].get_value_or<std::string>("");
        result.SSLKeyPath = node["SSLKeyPath"].get_value_or<std::string>("");

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
            for (auto& user_node : node["users"]) {
                std::string username = user_node["user"].get_value<std::string>();
                std::string password = user_node["password"].get_value<std::string>();

                this->users[username] = sha256(username + password);
                ZestLog(LogLevel::DEBUG, std::format("Loaded user: {} with token: {}", username, this->users[username]));
            }
        }
    } catch (const std::exception& e) {
        ZestLog(LogLevel::ERROR, "Failed to parse config : " + std::string(e.what()));
        throw std::runtime_error("Failed to parse config: " + std::string(e.what()));
    }

    if (result.MaxValueSize >= result.SegSize) {
        ZestLog(LogLevel::CRITICAL, std::format("MaxValueSize is higher than SegSize ! MaxValueSize : {} | SegSize : {}", result.MaxValueSize, result.SegSize));
        throw std::runtime_error("MaxValueSize >= SegSize");
    }

    if (result.MaxKeySize > MAX_KEY_SIZE) {
        ZestLog(LogLevel::WARNING, std::format("MaxKeySize ({}) exceeds internal limit ({}), clamping...", result.MaxKeySize, MAX_KEY_SIZE));
        result.MaxKeySize = MAX_KEY_SIZE;
    }

    if (result.DBPort < 0 || result.WebPort < 0) {
        ZestLog(LogLevel::CRITICAL, "Ports cant be lower that zero");
        throw std::runtime_error("Port < 0");
    }

    if (result.DBPort == result.WebPort) {
        ZestLog(LogLevel::CRITICAL, "DBPort and WebPort cannot be the same");
        throw std::runtime_error("DBPort == WebPort");
    }

    return result;
}

ResultType ZestDB::reloadConfig()
{
    try {
        Settings oldSettings = this->settings;
        Settings s = this->loadConfig();

        if (s.DbPath != oldSettings.DbPath || s.IndexPath != oldSettings.IndexPath || s.WalPath != oldSettings.WalPath || s.SSLCertPath != oldSettings.SSLCertPath || s.SSLKeyPath != oldSettings.SSLKeyPath) {
            ZestLog(LogLevel::ERROR, "Cannot change path settings during hot-reload. Paths are immutable.");
            return { ResultType::Code::ERROR, "Cannot change path settings during hot-reload. Paths are immutable. Restart the database to apply changes" };
        }

        this->settings = std::move(s);
        setLoggerDebugMode(this->settings.isDebug);
        this->shardManager->reloadSettings(this->settings);
        
        return { ResultType::Code::SUCCESS, "Reload successful !" };
    } catch (const std::exception& e) {
        ZestLog(LogLevel::ERROR, std::format("An error occured during the reload of the configuration : {}. Aborting...", e.what()));
    }
    return { ResultType::Code::ERROR, "Reload failed !" };
}

void ZestDB::setConfig()
{
    const fs::path current_path = fs::current_path();
    const std::string configName = "config.yaml";
    fs::path configPath = current_path / configName;

    std::fstream configFile(configPath);
    std::stringstream buffer;
    buffer << configFile.rdbuf();
    configFile.close();

    auto node = fkyaml::node::deserialize(buffer.str());

    node["SegSize"] = this->settings.SegSize;
    node["MaxKeySize"] = this->settings.MaxKeySize;
    node["MaxValueSize"] = this->settings.MaxValueSize;
    node["CacheSize"] = this->settings.CacheSize;
    node["CompactingInterval"] = this->settings.CompactingInterval;
    node["FlushInterval"] = this->settings.FlushInterval;
    node["DBPort"] = this->settings.DBPort;
    node["WebPort"] = this->settings.WebPort;
    node["isDebug"] = this->settings.isDebug;
    node["jsonOnly"] = this->settings.jsonOnly;
    node["readOnly"] = this->settings.readOnly;

    std::ofstream outFile(configPath);
    outFile << node;
    outFile.close();

    ZestLog(LogLevel::INFO, "Configuration saved to config.yaml");
}

bool ZestDB::validateToken(const std::string& username, const std::string& token) const
{
    if (this->users.find(username) == this->users.end()) {
        ZestLog(LogLevel::DEBUG, std::format("ZestDB::validateToken - user not found: {}", username));
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
        ZestLog(LogLevel::ERROR, std::format("ZestDB::validateKey - {} MaxKeySize : {}", Messages::KEY_TOO_LONG, this->settings.MaxKeySize));
        return false;
    }
    return true;
}

bool ZestDB::validateValue(const std::string& value) const
{
    if (value.size() > this->settings.MaxValueSize) {
        ZestLog(LogLevel::ERROR, std::format("ZestDB::validateValue - {} MaxValueSize : {}", Messages::VALUE_TOO_LONG, this->settings.MaxValueSize));
        return false;
    }
    return true;
}

bool ZestDB::isJsonValid(const std::string& value) const
{
    return nlohmann::json::accept(value);
}

ResultType ZestDB::get(const std::string& key)
{
    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::INVALID_KEY };
    }

    ZestLog(LogLevel::DEBUG, std::format("ZestDB::get - looking for key: {}", key));

    return this->shardManager->get(key);
}

ResultType ZestDB::set(const std::string& key, const std::string& value)
{
    if (this->settings.readOnly) {
        return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
    }

    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::INVALID_KEY };
    }

    if (value.empty()) {
        return { ResultType::Code::ERROR, Messages::VALUE_EMPTY };
    }

    if (!this->validateValue(value)) {
        return { ResultType::Code::ERROR, Messages::INVALID_VALUE };
    }

    if (this->settings.jsonOnly) {
        if (!this->isJsonValid(value)) {
            return { ResultType::Code::ERROR, Messages::JSON_ONLY_ERROR };
        }
    }

    ZestLog(LogLevel::DEBUG, std::format("ZestDB::set - key: {}, value size: {}", key, value.size()));

    return this->shardManager->set(key, value);
}

ResultType ZestDB::del(const std::string& key)
{
    if (this->settings.readOnly) {
        return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
    }

    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::INVALID_KEY };
    }

    ZestLog(LogLevel::DEBUG, std::format("ZestDB::del - deleting key: {}", key));

    return this->shardManager->del(key);
}

ResultType ZestDB::getBy(ValidationRule valid)
{
    return this->shardManager->getBy(valid);
}

ResultType ZestDB::setBy(ValidationRule valid, const std::string& value)
{
    if (this->settings.readOnly) {
        return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
    }

    if (value.empty()) {
        return { ResultType::Code::ERROR, Messages::VALUE_EMPTY };
    }

    if (!this->validateValue(value)) {
        return { ResultType::Code::ERROR, Messages::INVALID_VALUE };
    }

    if (this->settings.jsonOnly) {
        if (!this->isJsonValid(value)) {
            return { ResultType::Code::ERROR, Messages::JSON_ONLY_ERROR };
        }
    }

    return this->shardManager->setBy(valid, value);
}

ResultType ZestDB::delBy(ValidationRule valid)
{
    if (this->settings.readOnly) {
        return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
    }

    return this->shardManager->delBy(valid);
}

CreationValidationRuleResult ZestDB::createValidationRule(const std::string& mode, const std::string& pattern) const
{
    ValidationRule valid;
    bool validMode = true;

    if (mode == "re") {
        try {
            std::regex keyRegex(pattern);
            valid.func = [reg = std::move(keyRegex)](const std::string& key) {
                return std::regex_match(key, reg);
            };
        } catch (const std::regex_error&) {
            validMode = false;
        }
    } else if (mode == "sw") {
        valid.func = [pattern](const std::string& key) {
            return key.find(pattern) == 0;
        };
    } else if (mode == "ct") {
        valid.func = [pattern](const std::string& key) {
            return key.find(pattern) != std::string::npos;
        };
    } else if (mode == "ew") {
        valid.func = [pattern](const std::string& key) {
            if (key.size() >= pattern.size()) {
                return key.compare(key.size() - pattern.size(), pattern.size(), pattern) == 0;
            }
            return false;
        };
    } else {
        validMode = false;
    }

    return { valid, validMode };
}

void ZestDB::appendToWAL(const std::string& key, const std::string& command)
{
    if (this->replaying.load()) {
        return;
    }

    this->shardManager->appendToWAL(key, command);
}

std::string toLowerStr(const std::string& str)
{
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
    return result;
}

ResultType ZestDB::execCmd(const std::string& command)
{
    std::string_view sv(command);
    auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return { ResultType::Code::ERROR, Messages::NO_COMMAND_GIVEN };
    }
    sv.remove_prefix(start);

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

    std::string cmd = toLowerStr(std::string(words[0]));

    if (cmd == "g") {
        if (words.size() < 2) {
            result.message = Messages::MISSING_KEY;
        } else {
            std::string key(words[1]);
            result = this->get(key);
        }
    } else if (cmd == "s") {
        if (words.size() < 2) {
            result.message = Messages::MISSING_KEY;
        } else if (words.size() < 3) {
            result.message = Messages::MISSING_VALUE;
        } else {
            std::string key(words[1]);
            size_t keyPos = command.find(words[1]);
            size_t valPos = command.find_first_not_of(" \t\r\n", keyPos + words[1].size());
            std::string value = command.substr(valPos);

            result = this->set(key, value);
        }
    } else if (cmd == "d") {
        if (words.size() < 2) {
            result.message = Messages::MISSING_KEY;
        } else {
            std::string key(words[1]);
            result = this->del(key);
        }
    } else if (cmd == "gb") {
        if (words.size() < 3) {
            result.message = Messages::MISSING_ARGUMENTS;
        } else {
            std::string mode = toLowerStr(std::string(words[1]));
            std::string pattern(words[2]);
            unsigned int limit = UINT_MAX;
            for (size_t i = 3; i < words.size(); i++) {
                if (toLowerStr(std::string(words[i])) == "lim" && i + 1 < words.size()) {
                    limit = static_cast<unsigned int>(std::stoi(std::string(words[i + 1])));
                }
            }
            auto [valid, validMode] = this->createValidationRule(mode, pattern);
            if (!validMode) {
                result.message = Messages::INVALID_REGEX;
            } else {
                valid.limit = limit;
                result = this->getBy(valid);
            }
        }
    } else if (cmd == "sb") {
        if (words.size() < 4) {
            result.message = Messages::MISSING_VALUE;
        } else {
            std::string mode = toLowerStr(std::string(words[1]));
            std::string pattern(words[2]);
            unsigned int limit = UINT_MAX;

            size_t valStartIdx = 3;
            if (words.size() >= 5) {
                for (size_t i = 3; i < words.size(); i++) {
                    if (toLowerStr(std::string(words[i])) == "lim" && i + 1 < words.size()) {
                        limit = static_cast<unsigned int>(std::stoi(std::string(words[i + 1])));
                        valStartIdx = i + 2;
                        break;
                    }
                }
            }

            if (valStartIdx >= words.size()) {
                result.message = Messages::MISSING_VALUE;
            } else {
                size_t valPosInCmd = command.find(words[valStartIdx]);
                std::string value = command.substr(valPosInCmd);

                auto [valid, validMode] = this->createValidationRule(mode, pattern);
                if (!validMode) {
                    result.message = Messages::INVALID_REGEX;
                } else {
                    valid.limit = limit;
                    result = this->setBy(valid, value);
                }
            }
        }
    } else if (cmd == "db") {
        if (words.size() < 3) {
            result.message = Messages::MISSING_PATTERN;
        } else {
            std::string mode = toLowerStr(std::string(words[1]));
            std::string pattern(words[2]);
            unsigned int limit = UINT_MAX;
            for (size_t i = 3; i < words.size(); i++) {
                if (toLowerStr(std::string(words[i])) == "lim" && i + 1 < words.size()) {
                    limit = static_cast<unsigned int>(std::stoi(std::string(words[i + 1])));
                }
            }
            auto [valid, validMode] = this->createValidationRule(mode, pattern);
            if (!validMode) {
                result.message = Messages::INVALID_MODE;
            } else {
                valid.limit = limit;
                result = this->delBy(valid);
            }
        }
    } else if (cmd == "h") {
        result.code = ResultType::Code::SUCCESS;
        result.message = this->help();
    } else if (cmd == "f") {
        this->flush();
        result.code = ResultType::Code::SUCCESS;
        result.message = Messages::FLUSH_SUCCESSFUL;
    } else if (cmd == "r") {
        result = this->reloadConfig();
    } else if (cmd == "scfg") {
        if (words.size() < 3) {
            result.message = Messages::MISSING_ARGUMENTS;
            return result;
        }

        std::string param = toLowerStr(std::string(words[1]));
        std::string valueStr(words[2]);

        bool valueSet = false;

        if (param == "segsize") {
            try {
                unsigned long val = std::stoul(valueStr);
                if (val <= this->settings.MaxValueSize) {
                    result.message = "SegSize must be greater than MaxValueSize";
                    return result;
                }
                this->settings.SegSize = val;
                valueSet = true;
            } catch (...) {
                result.message = "Invalid SegSize value";
                return result;
            }
        } else if (param == "maxkeysize") {
            try {
                unsigned int val = std::stoul(valueStr);
                if (val > MAX_KEY_SIZE) {
                    result.message = std::format("MaxKeySize cannot exceed {}", MAX_KEY_SIZE);
                    return result;
                }
                this->settings.MaxKeySize = val;
                valueSet = true;
            } catch (...) {
                result.message = "Invalid MaxKeySize value";
                return result;
            }
        } else if (param == "maxvaluesize") {
            try {
                unsigned int val = std::stoul(valueStr);
                if (val >= this->settings.SegSize) {
                    result.message = "MaxValueSize must be less than SegSize";
                    return result;
                }
                this->settings.MaxValueSize = val;
                valueSet = true;
            } catch (...) {
                result.message = "Invalid MaxValueSize value";
                return result;
            }
        } else if (param == "cachesize") {
            try {
                this->settings.CacheSize = std::stoul(valueStr);
                valueSet = true;
            } catch (...) {
                result.message = "Invalid CacheSize value";
                return result;
            }
        } else if (param == "compactinginterval") {
            try {
                this->settings.CompactingInterval = std::stoul(valueStr);
                valueSet = true;
            } catch (...) {
                result.message = "Invalid CompactingInterval value";
                return result;
            }
        } else if (param == "flushinterval") {
            try {
                this->settings.FlushInterval = std::stoul(valueStr);
                valueSet = true;
            } catch (...) {
                result.message = "Invalid FlushInterval value";
                return result;
            }
        } else if (param == "networkvalidationstr") {
            try {
                std::regex testRegex(valueStr);
                this->settings.NetworkValidationStr = valueStr;
                this->settings.NetworkValidation = std::move(testRegex);
                valueSet = true;
            } catch (const std::regex_error& e) {
                result.message = std::format("Invalid regex: {}", e.what());
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
                result.message = "Invalid isDebug value (use true/false or 0/1)";
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
                result.message = "Invalid useSSL value (use true/false or 0/1)";
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
                result.message = "Invalid jsonOnly value (use true/false or 0/1)";
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
                result.message = "Invalid readOnly value (use true/false or 0/1)";
                return result;
            }
        } else {
            result.message = "Unknown parameter: " + param;
            return result;
        }

        if (valueSet) {
            result.code = ResultType::Code::SUCCESS;
            result.message = Messages::UPDATE_SUCCESSFUL;
            this->setConfig();
            this->shardManager->reloadSettings(this->settings);
        }
    } else {
        result.message = Messages::CMD_NOT_FOUND;
    }

    if (!this->replaying.load()) {
        if (result.code == ResultType::Code::SUCCESS && (cmd == "s" || cmd == "d" || cmd == "sb" || cmd == "db")) {
            if (words.size() >= 2) {
                this->appendToWAL(std::string(words[1]), command);
            }
        }
    }

    return result;
}

void ZestDB::stop()
{
    ZestLog(LogLevel::INFO, "Exiting ZestDB...");

    this->settings.isRunning = false;

    this->ioCtx.stop();
    if (this->srv)
        this->srv->stop();

    this->shardManager->stop();
}

std::string ZestDB::help() const
{
    std::ostringstream oss;
    oss << "ZestDB Commands:" << "\n";
    oss << "---------------" << "\n";
    oss << "get <key>                              - Get value by key" << "\n";
    oss << "set <key> <value>                      - Set key-value pair" << "\n";
    oss << "del <key>                              - Delete key" << "\n";
    oss << "getby <mode> <pattern> [lim <n>]       - Get keys matching pattern" << "\n";
    oss << "setby <mode> <pattern> <val> [lim <n>] - Set value for keys matching pattern" << "\n";
    oss << "delby <mode> <pattern> [lim <n>]       - Delete keys matching pattern" << "\n";
    oss << "flush                                  - Flush all data in memory to the disk" << "\n";
    oss << "reload                                 - Reload configuration from config.yaml" << "\n";
    oss << "scfg <param> <value>                   - Set config parameter" << "\n";
    oss << "help                                   - Show this help" << "\n";
    oss << "\n";
    oss << "Config Parameters:" << "\n";
    oss << "  SegSize(int), MaxKeySize(int), MaxValueSize(int), CacheSize(int), CompactingInterval(int), FlushInterval(int), isDebug(bool), useSSL(bool), jsonOnly(bool), readOnly(bool), NetworkValidationStr(string), SSLCertPath(string), SSLKeyPath(string)" << "\n";
    oss << "\n";
    oss << "Modes:" << "\n";
    oss << "  re  - regex" << "\n";
    oss << "  sw  - starts with" << "\n";
    oss << "  ct  - contains" << "\n";
    oss << "  ew  - ends with" << "\n";
    oss << "  lim - limit (optional)" << "\n";
    oss << "\n";
    oss << "Examples:" << "\n";
    oss << "  gb re .* lim 10                      - Get keys matching regex '.*' with limit 10" << "\n";
    oss << "  gb sw ca lim 20                      - Get keys starting with 'ca' with limit 20" << "\n";
    oss << "  gb ct ac lim 1                       - Get keys containing 'ac' with limit 1" << "\n";
    oss << "  gb ew ca                             - Get keys ending with 'ca' (no limit)" << "\n";
    oss << "\n";
    oss << "Shortcuts: g=get, s=set, d=del, gb=getby, sb=setby, db=delby, f=flush, r=reload, scfg=setconfig, h=help" << "\n";
    return oss.str();
}

void ZestDB::flush()
{
    ZestLog(LogLevel::DEBUG, "Flushing all shards...");
    this->isFlushing.store(true);
    this->shardManager->flush();
    this->shardManager->clearAllWAL();
    this->isFlushing.store(false);
}

void ZestDB::replayWAL()
{
    ZestLog(LogLevel::INFO, "Replaying WAL...");
    this->replaying.store(true);
    this->shardManager->replayAllWAL([this](const std::string& cmd) {
        auto resp = this->execCmd(cmd);
        return responseToJson(resp);
    });
    this->replaying.store(false);
}