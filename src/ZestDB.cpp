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

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "lib/json.hpp"
#include "lib/node.hpp"

namespace fs = std::filesystem;

using json = nlohmann::json;

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
    this->wal = std::make_unique<WAL>(this->settings);
    this->socket = std::make_unique<Server>(this->ioCtx, this->settings.DBPort, *this);

    this->replayWAL();

    ZestLog(LogLevel::INFO, std::format("ZestDB initialized with {} shards", NUM_SHARDS));

    this->initialized.store(true);

    std::thread flushThread = std::thread([this]() {
        while (this->settings.isRunning) {
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
                result[shardKey] = shards[i]->getPerfMonitoring().getPerformances();
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

        this->settings.NetworkValidationStr = node["NetworkValidation"].get_value_or<std::string>("");

        this->settings.isDebug = node["isDebug"].get_value_or<bool>(false);
        setLoggerDebugMode(this->settings.isDebug);
        this->settings.jsonOnly = node["jsonOnly"].get_value_or<bool>(false);
        this->settings.readOnly = node["readOnly"].get_value_or<bool>(false);

        this->settings.useSSL = node["useSSL"].get_value_or<bool>(false);
        this->settings.SSLCertPath = node["SSLCertPath"].get_value_or<std::string>("");
        this->settings.SSLKeyPath = node["SSLKeyPath"].get_value_or<std::string>("");

        if (this->settings.useSSL && (this->settings.SSLCertPath == "" || this->settings.SSLKeyPath == "")) {
            ZestLog(LogLevel::CRITICAL, "SSL enabled and not certificate or key path provided !");
            throw std::runtime_error("SSL with no cert or key");
        }

        if (!this->settings.NetworkValidationStr.empty()) {
            try {
                this->settings.NetworkValidation = std::regex(this->settings.NetworkValidationStr);
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

    if (this->settings.MaxValueSize >= this->settings.SegSize) {
        ZestLog(LogLevel::CRITICAL, std::format("MaxValueSize is higher than SegSize ! MaxValueSize : {} | SegSize : {}", this->settings.MaxValueSize, this->settings.SegSize));
        throw std::runtime_error("MaxValueSize >= SegSize");
    }

    if (this->settings.MaxKeySize > IndexEntry::MAX_KEY_SIZE) {
        ZestLog(LogLevel::WARNING, std::format("MaxKeySize ({}) exceeds internal limit ({}), clamping...", this->settings.MaxKeySize, IndexEntry::MAX_KEY_SIZE));
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

    this->settings.WalPath = this->settings.DbPath / "WAL";

    if (!fs::exists(this->settings.DbPath / "WAL")) {
        fs::path WalPath = this->settings.DbPath / "WAL";
        ZestLog(LogLevel::INFO, std::format("Creating the WAL at {}", WalPath.string()));

        if (auto parent = WalPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(WalPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, std::format("Failed to create WAL at {}", WalPath.string()));
            throw std::runtime_error("Failed to create WAL");
        }
        this->settings.WalPath = WalPath;
    } else {
        this->settings.WalPath = this->settings.DbPath / "WAL";
    }
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
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, std::format("ZestDB::get - looking for key: {}", key));

    return this->shardManager->get(key);
    ;
}

ResultType ZestDB::set(const std::string& key, const std::string& value)
{
    if (this->settings.readOnly) {
        return { ResultType::Code::ERROR, Messages::READ_ONLY_ERROR };
    }

    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    if (value.empty()) {
        return { ResultType::Code::ERROR, Messages::VALUE_EMPTY };
    }

    if (!this->validateValue(value)) {
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
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
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
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
        return { ResultType::Code::ERROR, Messages::VALUE_TOO_LONG };
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
            oss << Messages::MISSING_KEY << "\n";
            oss << Messages::USAGE_GET << "\n";
        }
        result = this->get(key);
        if (result.code == ResultType::Code::SUCCESS && !result.message.empty()) {
            oss << result.message << "\n";
        } else {
            oss << "(not found): " << (result.message.empty() ? Messages::KEY_NOT_FOUND : result.message) << "\n";
        }
    } else if (cmd == "s" || cmd == "set") {
        std::string key, value;
        if (!(iss >> key)) {
            oss << Messages::MISSING_KEY << "\n";
            oss << Messages::USAGE_SET << "\n";
        }
        if (!(iss >> value)) {
            oss << Messages::MISSING_VALUE << "\n";
            oss << Messages::USAGE_SET << "\n";
        }
        std::string rest;
        while (iss >> rest) {
            value += " " + rest;
        }
        result = this->set(key, value);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << "OK: " << (result.message.empty() ? Messages::SUCCESS_SET : result.message) << "\n";
        } else {
            oss << "ERROR: " << (result.message.empty() ? "operation failed" : result.message) << "\n";
        }
    } else if (cmd == "d" || cmd == "del") {
        std::string key;
        if (!(iss >> key)) {
            oss << Messages::MISSING_KEY << "\n";
            oss << Messages::USAGE_GET << "\n";
        }
        result = this->del(key);
        if (result.code == ResultType::Code::SUCCESS) {
            oss << "OK: " << (result.message.empty() ? Messages::SUCCESS_DEL : result.message) << "\n";
        } else {
            oss << "ERROR: " << (result.message.empty() ? "operation failed" : result.message) << "\n";
        }
    } else if (cmd == "gb" || cmd == "getby") {
        std::string mode, pattern;
        unsigned int limit = UINT_MAX;
        if (!(iss >> mode)) {
            oss << Messages::MISSING_PATTERN << "\n";
            oss << Messages::USAGE_GETBY << "\n";
        }
        if (!(iss >> pattern)) {
            oss << Messages::MISSING_PATTERN << "\n";
            oss << Messages::USAGE_GETBY << "\n";
        }
        std::string word;
        while (iss >> word) {
            if (word == "lim" && (iss >> limit)) {
                break;
            } else {
                oss << "ERROR: unexpected token '" << word << "'" << "\n";
                break;
            }
        }
        auto [valid, validMode] = this->createValidationRule(mode, pattern);
        if (!validMode) {
            oss << "ERROR: invalid mode '" << mode << "'. Use re, sw, ct, or ew" << "\n";
        } else {
            valid.limit = limit;
            result = this->getBy(valid);
            if (result.code == ResultType::Code::SUCCESS && result.affectedRows > 0 && !result.message.empty()) {
                std::string msg = result.message;
                oss << msg << "\n";
            } else {
                oss << "(not found): no keys match the pattern" << "\n";
            }
        }
    } else if (cmd == "sb" || cmd == "setby") {
        std::string mode, pattern, value;
        unsigned int limit = UINT_MAX;
        if (!(iss >> mode)) {
            oss << Messages::MISSING_PATTERN << "\n";
            oss << Messages::USAGE_SETBY << "\n";
        }
        if (!(iss >> pattern)) {
            oss << Messages::MISSING_PATTERN << "\n";
            oss << Messages::USAGE_SETBY << "\n";
        }
        if (!(iss >> value)) {
            oss << Messages::MISSING_VALUE << "\n";
            oss << Messages::USAGE_SETBY << "\n";
        }
        std::string word;
        std::vector<std::string> words;
        while (iss >> word) {
            words.push_back(word);
        }
        for (size_t i = 0; i < words.size(); i++) {
            if ((words[i] == "lim") && i + 1 < words.size()) {
                limit = static_cast<unsigned int>(std::stoi(words[i + 1]));
                break;
            }
            if (i == 0 || (i > 0 && words[i - 1] != "limit")) {
                value += (value.empty() ? "" : " ") + words[i];
            }
        }
        auto [valid, validMode] = this->createValidationRule(mode, pattern);
        if (!validMode) {
            oss << "ERROR: invalid mode '" << mode << "'. Use re, sw, ct, or ew" << "\n";
        } else {
            valid.limit = limit;
            result = this->setBy(valid, value);
            if (result.code == ResultType::Code::SUCCESS && !result.message.empty()) {
                oss << "OK: " << result.message << "\n";
            } else {
                oss << "OK: value modified" << "\n";
            }
        }
    } else if (cmd == "db" || cmd == "delby") {
        std::string mode, pattern;
        unsigned int limit = UINT_MAX;
        if (!(iss >> mode)) {
            oss << Messages::MISSING_PATTERN << "\n";
            oss << Messages::USAGE_DELBY << "\n";
        }
        if (!(iss >> pattern)) {
            oss << Messages::MISSING_PATTERN << "\n";
            oss << Messages::USAGE_DELBY << "\n";
        }
        std::string word;
        while (iss >> word) {
            if (word == "lim" && (iss >> limit)) {
                break;
            } else {
                oss << "ERROR: unexpected token '" << word << "'" << "\n";
                break;
            }
        }
        auto [valid, validMode] = this->createValidationRule(mode, pattern);
        if (!validMode) {
            oss << "ERROR: invalid mode '" << mode << "'. Use re, sw, ct, or ew" << "\n";
        } else {
            valid.limit = limit;
            result = this->delBy(valid);
            if (result.code == ResultType::Code::SUCCESS && !result.message.empty()) {
                oss << "OK: " << result.message << "\n";
            } else if (result.code == ResultType::Code::SUCCESS) {
                oss << "OK: entries deleted" << "\n";
            } else {
                oss << "ERROR: " << (result.message.empty() ? "operation failed" : result.message) << "\n";
            }
        }
    } else if (cmd == "h" || cmd == "help") {
        oss << this->help();
    } else if (cmd == "f" || cmd == "flush") {
        this->flush();
        oss << Messages::FLUSH_SUCCESSFUL << "\n";
    } else {
        oss << "ERROR: " << Messages::CMD_NOT_FOUND << "\n";
        oss << Messages::TYPE_HELP << "\n";
    }

    if (!this->replaying.load()) {
        if (result.code == ResultType::Code::SUCCESS && (cmd == "s" || cmd == "set" || cmd == "d" || cmd == "del" || cmd == "sb" || cmd == "setby" || cmd == "db" || cmd == "delby")) {
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
    this->srv->stop();

    this->shardManager->stop();
    this->wal->clear();
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
    oss << "help                                   - Show this help" << "\n";
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
    oss << "Shortcuts: g=get, s=set, d=del, gb=getby, sb=setby, db=delby, f=flush, h=help" << "\n";
    return oss.str();
}

void ZestDB::flush()
{
    ZestLog(LogLevel::DEBUG, "Flushing all shards...");
    this->shardManager->flush();
    this->wal->clear();
}

void ZestDB::replayWAL()
{
    ZestLog(LogLevel::INFO, "Replaying WAL...");
    this->replaying.store(true);
    std::vector<std::string> cmds = this->wal->getCmds();

    for (const std::string& cmd : cmds) {
        ZestLog(LogLevel::INFO, std::format("WAL replay: {}", cmd));
        std::string result = this->execCmd(cmd);
        ZestLog(LogLevel::INFO, std::format("WAL replay result: {}", result));
    }

    this->shardManager->flush();
    this->wal->clear();
    this->replaying.store(false);
    ZestLog(LogLevel::INFO, std::format("WAL replay complete, processed {} commands", cmds.size()));
}