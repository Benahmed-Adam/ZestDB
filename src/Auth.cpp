#include <algorithm>
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

    std::string sha256(const std::string& str) {
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

    bool ZestDB::validateToken(const std::string& username, const std::string& token) const {
        return this->authenticate(username, token).has_value();
    }

    std::optional<Role> ZestDB::authenticate(const std::string& username, const std::string& token) const {
        std::shared_lock<std::shared_mutex> lock(this->usersMtx);
        auto it = this->users.find(username);
        if (it == this->users.end()) {
            return std::nullopt;
        }

        const std::string& expected = it->second.tokenHash;
        const std::string actual = sha256(token);
        if (actual.size() != expected.size() || CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) != 0) {
            return std::nullopt;
        }
        return it->second.role;
    }

    std::optional<Role> ZestDB::roleOf(const std::string& username) const {
        std::shared_lock<std::shared_mutex> lock(this->usersMtx);
        auto it = this->users.find(username);
        if (it == this->users.end()) {
            return std::nullopt;
        }
        return it->second.role;
    }

    std::shared_ptr<UserLimiter> ZestDB::limiterOf(const std::string& username) const {
        std::shared_lock<std::shared_mutex> lock(this->usersMtx);
        auto it = this->limiters.find(username);
        return it == this->limiters.end() ? nullptr : it->second;
    }

    void ZestDB::rebuildLimiters() {
        std::unordered_map<std::string, std::shared_ptr<UserLimiter>> rebuilt;
        for (const auto& [name, user] : this->users) {
            if (user.limits.unlimited()) {
                continue;
            }
            auto it = this->limiters.find(name);
            if (it != this->limiters.end()) {
                it->second->reconfigure(user.limits);
                rebuilt.emplace(name, it->second);
            } else {
                rebuilt.emplace(name, std::make_shared<UserLimiter>(user.limits));
            }
        }
        this->limiters = std::move(rebuilt);
    }

    static constexpr unsigned int MAX_AUTH_FAILURES = 5;
    static constexpr std::chrono::seconds AUTH_LOCKOUT{ 60 };

    bool ZestDB::authAllowed(const std::string& username) {
        std::lock_guard<std::mutex> lock(this->authFailuresMtx);
        auto it = this->authFailures.find(username);
        return it == this->authFailures.end() || it->second.first < MAX_AUTH_FAILURES || std::chrono::steady_clock::now() >= it->second.second;
    }

    void ZestDB::recordAuthResult(const std::string& username, bool success) {
        std::lock_guard<std::mutex> lock(this->authFailuresMtx);
        if (success) {
            this->authFailures.erase(username);
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (this->authFailures.size() > 10000) {
            std::erase_if(this->authFailures, [now](const auto& entry) { return now >= entry.second.second; });
        }
        auto& [failures, until] = this->authFailures[username];
        if (now >= until) {
            failures = 0;
        }
        failures++;
        until = now + AUTH_LOCKOUT;
    }

    bool ZestDB::isAddressAllowed(const std::string& ip) {
        std::shared_lock<std::shared_mutex> lock(this->writeMtx);
        return std::regex_match(ip, this->settings.NetworkValidation);
    }

} // namespace Zest
