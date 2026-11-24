#include <algorithm>
#include <cctype>
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
#include "ZestDB.hpp"
#include "lib/json.hpp"
#include "lib/node.hpp"

namespace Zest {

    namespace fs = std::filesystem;

    using json = nlohmann::json;

    /// Value of `key` in the YAML node, or `def` when it is absent or null.
    template <typename T>
    static T getOr(fkyaml::node& node, const std::string& key, T def) {
        return node.contains(key) && !node[key].is_null() ? node[key].get_value<T>() : std::move(def);
    }

    /// A byte count: a positive integer, or a string with a `KB`, `MB` or `GB` suffix (powers of 1024), such as `10MB`.
    static uint64_t parseSize(const fkyaml::node& value, const std::string& where) {
        if (value.is_integer()) {
            const auto bytes = value.get_value<int64_t>();
            if (bytes <= 0) {
                throw std::runtime_error(std::format("{}: bandwidthLimit must be positive", where));
            }
            return static_cast<uint64_t>(bytes);
        }
        const std::string text = value.is_string() ? value.get_value<std::string>() : "";
        uint64_t number = 0;
        const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), number);
        std::string unit(end, text.data() + text.size());
        std::erase(unit, ' ');
        std::ranges::transform(unit, unit.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        const std::pair<const char*, uint64_t> units[] = { { "", 1 }, { "B", 1 }, { "KB", 1ULL << 10 }, { "MB", 1ULL << 20 }, { "GB", 1ULL << 30 } };
        const auto* found = std::ranges::find_if(units, [&unit](const auto& u) { return unit == u.first; });
        if (ec != std::errc() || end == text.data() || number == 0 || found == std::end(units) || number > UINT64_MAX / found->second) {
            throw std::runtime_error(std::format("{}: invalid bandwidthLimit '{}' (use a byte count or a KB, MB or GB suffix)", where, text));
        }
        return number * found->second;
    }

    /// The rateLimit, burst and bandwidthLimit set in `node` (the top level, or one user), each absent when not set.
    static UserLimits parseLimits(const fkyaml::node& node, const std::string& where) {
        const auto number = [&](const char* key) -> std::optional<double> {
            if (!node.contains(key) || node[key].is_null()) {
                return std::nullopt;
            }
            const fkyaml::node& value = node[key];
            double n = 0;
            if (value.is_integer()) {
                n = static_cast<double>(value.get_value<int64_t>());
            } else if (value.is_float_number()) {
                n = value.get_value<double>();
            } else {
                throw std::runtime_error(std::format("{}: {} must be a number", where, key));
            }
            if (!(n > 0)) {
                throw std::runtime_error(std::format("{}: {} must be positive", where, key));
            }
            return n;
        };
        UserLimits limits;
        limits.rateLimit = number("rateLimit");
        limits.burst = number("burst");
        if (limits.burst && *limits.burst < 1) {
            throw std::runtime_error(std::format("{}: burst must be at least 1", where));
        }
        if (node.contains("bandwidthLimit") && !node["bandwidthLimit"].is_null()) {
            limits.bandwidthLimit = parseSize(node["bandwidthLimit"], where);
        }
        return limits;
    }

    /// `read`, `write` or `admin` as a Role, nullopt for anything else.
    static std::optional<Role> parseRole(const std::string& name) {
        if (name == "read") {
            return Role::Read;
        }
        if (name == "write") {
            return Role::Write;
        }
        if (name == "admin") {
            return Role::Admin;
        }
        return std::nullopt;
    }

    Settings ZestDB::loadConfig(std::unordered_map<std::string, UserEntry>& usersOut) {
        Settings result;
        std::unordered_map<std::string, UserEntry> parsedUsers;

        const fs::path current_path = fs::current_path().string();
        const std::string configName = "config.yaml";

        fs::path configPath = current_path / configName;

        if (!fs::exists(configPath)) {
            ZestLog(LogLevel::Critical, configName + " doesn't exist !");
            throw std::runtime_error(configName + " doesn't exist !");
        }

        ZestLog(LogLevel::Info, "Reading " + configName);

        try {
            std::ifstream configFile(configPath);
            std::stringstream buffer;
            buffer << configFile.rdbuf();

            auto node = fkyaml::node::deserialize(buffer.str());

            const Settings defaults;
            result.DbPath = getOr<std::string>(node, "DbPath", current_path.string());
            result.SegSize = getOr<unsigned long>(node, "SegSize", defaults.SegSize);
            result.MaxKeySize = getOr<unsigned int>(node, "MaxKeySize", defaults.MaxKeySize);
            result.MaxValueSize = getOr<unsigned int>(node, "MaxValueSize", defaults.MaxValueSize);
            result.CacheSize = getOr<unsigned int>(node, "CacheSize", defaults.CacheSize);
            result.CompactingInterval = getOr<unsigned int>(node, "CompactingInterval", defaults.CompactingInterval);
            result.FlushInterval = getOr<unsigned int>(node, "FlushInterval", defaults.FlushInterval);
            int dbPort = getOr<int>(node, "DBPort", defaults.DBPort);
            int webPort = getOr<int>(node, "WebPort", defaults.WebPort);
            if (dbPort < 1 || dbPort > 65535 || webPort < 1 || webPort > 65535) {
                ZestLog(LogLevel::Critical, "DBPort and WebPort must be between 1 and 65535");
                throw std::runtime_error("Port out of range");
            }
            result.DBPort = static_cast<uint16_t>(dbPort);
            result.WebPort = static_cast<uint16_t>(webPort);

            result.NetworkValidationStr = getOr<std::string>(node, "NetworkValidation", defaults.NetworkValidationStr);
            if (result.NetworkValidationStr.empty()) {
                result.NetworkValidationStr = ".*";
            }

            result.isDebug = getOr<bool>(node, "isDebug", defaults.isDebug);
            result.jsonOnly = getOr<bool>(node, "jsonOnly", defaults.jsonOnly);
            result.readOnly = getOr<bool>(node, "readOnly", defaults.readOnly);
            result.fsyncOnWrite = getOr<bool>(node, "fsyncOnWrite", defaults.fsyncOnWrite);

            result.useSSL = getOr<bool>(node, "useSSL", defaults.useSSL);
            result.SSLCertPath = getOr<std::string>(node, "SSLCertPath", "");
            result.SSLKeyPath = getOr<std::string>(node, "SSLKeyPath", "");

            result.ArchiveStoragePath = getOr<std::string>(node, "ArchiveStoragePath", (current_path / "archive/").string());
            result.ArchiveCreationDelay = getOr<unsigned int>(node, "ArchiveCreationDelay", defaults.ArchiveCreationDelay);
            result.AutoArchiveSaving = getOr<bool>(node, "AutoArchiveSaving", defaults.AutoArchiveSaving);
            result.ArchiveRetention = getOr<unsigned int>(node, "ArchiveRetention", defaults.ArchiveRetention);

            result.MaxConnections = getOr<unsigned int>(node, "MaxConnections", defaults.MaxConnections);
            result.AuthTimeout = getOr<unsigned int>(node, "AuthTimeout", defaults.AuthTimeout);
            result.IdleTimeout = getOr<unsigned int>(node, "IdleTimeout", defaults.IdleTimeout);

            if (result.useSSL && (result.SSLCertPath == "" || result.SSLKeyPath == "")) {
                ZestLog(LogLevel::Critical, "SSL enabled and not certificate or key path provided !");
                throw std::runtime_error("SSL with no cert or key");
            }

            if (!result.NetworkValidationStr.empty()) {
                try {
                    result.NetworkValidation = std::regex(result.NetworkValidationStr);
                } catch (const std::regex_error& e) {
                    ZestLog(LogLevel::Critical, "Invalid NetworkValidation regex: " + std::string(e.what()));
                    throw std::runtime_error("Invalid NetworkValidation regex");
                }
            }

            result.defaultLimits = parseLimits(node, "config");

            if (node.contains("users") && node["users"].is_sequence()) {
                for (const auto& user_node : node["users"]) {
                    std::string username = user_node["user"].get_value<std::string>();
                    std::string roleName = user_node.contains("role") ? user_node["role"].get_value<std::string>() : "write";
                    std::optional<Role> role = parseRole(roleName);
                    if (!role) {
                        throw std::runtime_error(std::format("user {}: unknown role '{}' (use read, write or admin)", username, roleName));
                    }
                    std::string tokenHash = user_node.contains("tokenHash") ? user_node["tokenHash"].get_value<std::string>() : sha256(sha256(username + user_node["password"].get_value<std::string>()));
                    // Each limit the user does not set falls back to the top-level one, then to unlimited. The top-level
                    // burst goes with the top-level rateLimit: a user who sets a rateLimit without a burst gets a burst
                    // equal to that rateLimit.
                    const UserLimits own = parseLimits(user_node, "user " + username);
                    const UserLimits& top = result.defaultLimits;
                    const std::optional<double> burst = own.burst ? own.burst : (own.rateLimit ? std::nullopt : top.burst);
                    const UserLimits limits{ own.rateLimit ? own.rateLimit : top.rateLimit, burst, own.bandwidthLimit ? own.bandwidthLimit : top.bandwidthLimit };
                    parsedUsers[username] = { std::move(tokenHash), *role, limits };
                }
            }
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, "Failed to parse config : " + std::string(e.what()));
            throw std::runtime_error("Failed to parse config: " + std::string(e.what()));
        }

        if (result.MaxValueSize >= result.SegSize) {
            ZestLog(LogLevel::Critical, std::format("MaxValueSize is higher than SegSize ! "
                                                    "MaxValueSize : {} | SegSize : {}",
                                                    result.MaxValueSize, result.SegSize));
            throw std::runtime_error("MaxValueSize >= SegSize");
        }

        if (result.MaxKeySize > MAX_KEY_SIZE) {
            ZestLog(LogLevel::Warning, std::format("MaxKeySize ({}) exceeds internal limit ({}), clamping...", result.MaxKeySize, MAX_KEY_SIZE));
            result.MaxKeySize = MAX_KEY_SIZE;
        }

        if (result.FlushInterval == 0 || result.CompactingInterval == 0 || result.ArchiveCreationDelay == 0 || result.AuthTimeout == 0 || result.IdleTimeout == 0 || result.MaxConnections == 0) {
            ZestLog(LogLevel::Critical, "FlushInterval, CompactingInterval, ArchiveCreationDelay, AuthTimeout, IdleTimeout and MaxConnections must be at least 1");
            throw std::runtime_error("Setting of 0");
        }

        if (result.DBPort == result.WebPort) {
            ZestLog(LogLevel::Critical, "DBPort and WebPort cannot be the same");
            throw std::runtime_error("DBPort == WebPort");
        }

        usersOut = std::move(parsedUsers);
        return result;
    }

    ResultType ZestDB::reloadConfig() {
        try {
            Settings oldSettings = this->settings;
            std::unordered_map<std::string, UserEntry> newUsers;
            Settings s = this->loadConfig(newUsers);

            if (s.DbPath != oldSettings.DbPath || s.ArchiveStoragePath != oldSettings.ArchiveStoragePath || s.IndexPath != oldSettings.IndexPath || s.WalPath != oldSettings.WalPath || s.SSLCertPath != oldSettings.SSLCertPath ||
                s.SSLKeyPath != oldSettings.SSLKeyPath) {
                ZestLog(LogLevel::Error, "Cannot change path settings during "
                                         "hot-reload. Paths are immutable.");
                return { ResultType::Code::ConfigNotReloadable };
            }

            if (s.useSSL != oldSettings.useSSL || s.DBPort != oldSettings.DBPort || s.WebPort != oldSettings.WebPort || s.MaxConnections != oldSettings.MaxConnections || s.AuthTimeout != oldSettings.AuthTimeout ||
                s.IdleTimeout != oldSettings.IdleTimeout) {
                ZestLog(LogLevel::Error, "Cannot change the ports, SSL or connection limits during hot-reload.");
                return { ResultType::Code::ConfigNotReloadable };
            }

            this->settings = std::move(s);
            {
                std::unique_lock<std::shared_mutex> usersLock(this->usersMtx);
                this->users = std::move(newUsers);
                this->rebuildLimiters();
            }
            setLoggerDebugMode(this->settings.isDebug);
            this->shardManager->reloadSettings(this->settings);
            this->scriptEngine->reload();
            this->threadCV.notify_all();

            return { ResultType::Code::Success };
        } catch (const std::exception& e) {
            ZestLog(LogLevel::Error, std::format("An error occured during the reload of the "
                                                 "configuration : {}. Aborting...",
                                                 e.what()));
        }
        return { ResultType::Code::ReloadFailed };
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
        node["ArchiveRetention"] = this->settings.ArchiveRetention;
        node["MaxConnections"] = this->settings.MaxConnections;
        node["AuthTimeout"] = this->settings.AuthTimeout;
        node["IdleTimeout"] = this->settings.IdleTimeout;

        std::ofstream outFile(configPath);
        outFile << node;
        outFile.close();

        ZestLog(LogLevel::Info, "Configuration saved to config.yaml");
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
        j["ArchiveRetention"] = this->settings.ArchiveRetention;
        j["MaxConnections"] = this->settings.MaxConnections;
        j["AuthTimeout"] = this->settings.AuthTimeout;
        j["IdleTimeout"] = this->settings.IdleTimeout;
        return j.dump(-1, ' ', false, json::error_handler_t::replace);
    }

} // namespace Zest
