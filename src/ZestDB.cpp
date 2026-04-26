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

    this->shardManager = std::make_unique<ShardManager>(this->settings, NUM_SHARDS);
    this->wal = std::make_unique<WAL>(this->settings);
    this->socket = std::make_unique<Server>(this->ioCtx, this->settings.DBPort, *this);

    this->replayWAL();

    ZestLog(LogLevel::INFO, "ZestDB initialized with " + std::to_string(NUM_SHARDS) + " shards");

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

    this->srv->WebSocket("/ws", [this](const httplib::Request& req, httplib::ws::WebSocket& ws) {
        if (!std::regex_match(req.remote_addr, this->settings.NetworkValidation)) {
            ZestLog(LogLevel::WARNING, "Session: Unauthorized IP: " + req.remote_addr);
            ws.close(httplib::ws::CloseStatus::PolicyViolation, "authentication failed");
            return;
        }

        ZestLog(LogLevel::INFO, "Session: client connected from " + req.remote_addr + ":" + std::to_string(req.remote_port));

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

    this->srv->Get("/", [this](const httplib::Request&, httplib::Response& res) {
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

        this->settings.isDebug = node["isDebug"].get_value_or<bool>(false);
        setLoggerDebugMode(this->settings.isDebug);

        this->settings.useSSL = node["useSSL"].get_value_or<bool>(false);
        this->settings.SSLCertPath = node["SSLCertPath"].get_value_or<std::string>("");
        this->settings.SSLKeyPath = node["SSLKeyPath"].get_value_or<std::string>("");

        if (this->settings.useSSL && (this->settings.SSLCertPath == "" || this->settings.SSLKeyPath == "")) {
            ZestLog(LogLevel::CRITICAL, "SSL enabled and not certificate or key path provided !");
            throw std::runtime_error("SSL with no cert or key");
        }

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

    if (!fs::exists(this->settings.DbPath)) {
        fs::create_directories(this->settings.DbPath);
    }

    this->settings.WalPath = this->settings.DbPath / "WAL";

    if (!fs::exists(this->settings.DbPath / "WAL")) {
        fs::path WalPath = this->settings.DbPath / "WAL";
        ZestLog(LogLevel::INFO, "Creating the WAL at " + WalPath.string());

        if (auto parent = WalPath.parent_path(); !fs::exists(parent)) {
            fs::create_directories(parent);
        }

        std::ofstream index(WalPath);
        if (!index) {
            ZestLog(LogLevel::ERROR, "Failed to create WAL at " + WalPath.string());
            throw std::runtime_error("Failed to create WAL");
        }
        this->settings.WalPath = WalPath;
    } else {
        this->settings.WalPath = this->settings.DbPath / "WAL";
    }
}

ResultType ZestDB::get(const std::string& key)
{
    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::get - looking for key: " + key);
    return this->shardManager->get(key);
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
    return this->shardManager->set(key, value);
}

ResultType ZestDB::del(const std::string& key)
{
    if (!this->validateKey(key)) {
        return { ResultType::Code::ERROR, Messages::KEY_TOO_LONG };
    }

    ZestLog(LogLevel::DEBUG, "ZestDB::del - deleting key: " + key);
    return this->shardManager->del(key);
}

ResultType ZestDB::getBy(const std::string& patern)
{
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

    return this->shardManager->getBy(patern);
}

ResultType ZestDB::setBy(const std::string& patern, const std::string& value)
{
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

    return this->shardManager->setBy(patern, value);
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

    return this->shardManager->delBy(patern);
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
    this->srv->stop();

    this->shardManager->stop();
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
        ZestLog(LogLevel::INFO, "WAL replay: " + cmd);
        std::string result = this->execCmd(cmd);
        ZestLog(LogLevel::INFO, "WAL replay result: " + result);
    }

    this->shardManager->flush();
    this->wal->clear();
    this->replaying.store(false);
    ZestLog(LogLevel::INFO, "WAL replay complete, processed " + std::to_string(cmds.size()) + " commands");
}