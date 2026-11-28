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

    static bool isRegexTooComplex(std::string_view pattern) {
        constexpr size_t maxLength = 1024;
        constexpr size_t maxDepth = 32;
        constexpr uint64_t maxStates = 100000;
        if (pattern.size() > maxLength) {
            return true;
        }

        std::vector<uint64_t> cost{ 0 };
        uint64_t lastAtom = 0;
        for (size_t i = 0; i < pattern.size(); i++) {
            const char c = pattern[i];
            if (c == '(') {
                if (cost.size() > maxDepth) {
                    return true;
                }
                cost.push_back(0);
                lastAtom = 0;
                continue;
            }

            if (c == ')' && cost.size() > 1) {
                lastAtom = cost.back() + 1;
                cost.pop_back();
                cost.back() += lastAtom;
            } else if (c == '{' && pattern.find('}', i) != std::string_view::npos) {
                const size_t close = pattern.find('}', i);
                uint64_t repeat = 0;
                for (std::string_view bounds = pattern.substr(i + 1, close - i - 1); !bounds.empty();) {
                    const size_t comma = bounds.find(',');
                    std::string_view part = bounds.substr(0, comma);
                    uint64_t value = 0;
                    if (!part.empty() && (std::from_chars(part.data(), part.data() + part.size(), value).ec != std::errc() || value > maxStates)) {
                        return true;
                    }
                    repeat = std::max(repeat, value);
                    bounds = comma == std::string_view::npos ? std::string_view() : bounds.substr(comma + 1);
                }
                cost.back() += lastAtom * (repeat > 0 ? repeat - 1 : 0);
                i = close;
            } else if (c == '[') {
                size_t j = i + 1;
                if (j < pattern.size() && pattern[j] == '^') {
                    j++;
                }
                if (j < pattern.size() && pattern[j] == ']') {
                    j++;
                }
                while (j < pattern.size() && pattern[j] != ']') {
                    j += pattern[j] == '\\' ? 2u : 1u;
                }
                i = j;
                lastAtom = 1;
                cost.back() += 1;
            } else {
                if (c == '\\') {
                    i++;
                }
                lastAtom = 1;
                cost.back() += 1;
            }

            if (cost.back() > maxStates) {
                return true;
            }
        }
        return false;
    }

    CreationValidationRuleResult ZestDB::createValidationRule(std::string_view mode, std::string_view pattern) const {
        ValidationRule valid;
        bool validMode = true;

        if (mode == "re") {
            try {
                std::string patStr(pattern);
                if (isRegexTooComplex(patStr) || std::regex_search(patStr, std::regex(R"(\\[1-9])"))) {
                    return { valid, false };
                }
#ifdef __GLIBCXX__
                std::regex keyRegex(patStr, std::regex::ECMAScript | std::regex_constants::__polynomial);
#else
                std::regex keyRegex(patStr, std::regex::ECMAScript);
#endif
                valid.func = [reg = std::move(keyRegex)](std::string_view key) {
                    try {
                        return std::regex_match(std::string(key), reg);
                    } catch (const std::regex_error&) {
                        return false;
                    }
                };
            } catch (const std::regex_error&) {
                validMode = false;
            }
        } else if (mode == "sw") {
            valid.func = [pat = std::string(pattern)](std::string_view key) { return key.starts_with(pat); };
        } else if (mode == "ct") {
            valid.func = [pat = std::string(pattern)](std::string_view key) { return key.find(pat) != std::string_view::npos; };
        } else if (mode == "ew") {
            valid.func = [pat = std::string(pattern)](std::string_view key) { return key.ends_with(pat); };
        } else {
            validMode = false;
        }

        return { valid, validMode };
    }

    /// ASCII lowercase copy of `str` (command names and modes are case-insensitive).
    std::string toLowerStr(std::string_view str) {
        std::string result(str);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::tolower(c); });
        return result;
    }

    /// Parses the whole of `str` as a number of type T; false on any extra character or overflow.
    template <typename T>
    static bool parseNumber(std::string_view str, T& out) {
        auto [end, ec] = std::from_chars(str.data(), str.data() + str.size(), out);
        return ec == std::errc() && end == str.data() + str.size();
    }

    /// Reads a quoted value starting at `pos` (spaces before it are skipped) into `out`, unescaping `\"` and `\\`.
    /// On success `pos` is moved past the closing quote. False if there is no opening quote or no closing one.
    static bool readQuoted(std::string_view sv, size_t& pos, std::string& out) {
        pos = sv.find_first_not_of(" \t", pos);
        if (pos == std::string_view::npos || sv[pos] != '"') {
            return false;
        }
        out.clear();
        for (size_t i = pos + 1; i < sv.size(); i++) {
            if (sv[i] == '\\' && i + 1 < sv.size() && (sv[i + 1] == '"' || sv[i + 1] == '\\')) {
                out += sv[++i];
            } else if (sv[i] == '"') {
                pos = i + 1;
                return true;
            } else {
                out += sv[i];
            }
        }
        return false;
    }

    /// True if nothing but spaces and tabs follows `pos`.
    static bool onlySpacesFrom(std::string_view sv, size_t pos) {
        return sv.find_first_not_of(" \t", pos) == std::string_view::npos;
    }

    /// Reads the quoted value that starts at `word` (a view into `sv`) and must end the command.
    static bool readLastValue(std::string_view sv, std::string_view word, std::string& out) {
        size_t pos = static_cast<size_t>(word.data() - sv.data());
        return readQuoted(sv, pos, out) && onlySpacesFrom(sv, pos);
    }

    /// Parses a TTL into an absolute expiry date in ms: `relative` reads seconds from now (`setex`, `expire`),
    /// otherwise a date in ms (`setexat`, `expireat`, the forms written to the WAL). Zero is refused.
    static bool parseTTL(std::string_view word, bool relative, uint64_t& expiresAt) {
        if (relative) {
            uint32_t seconds = 0;
            if (!parseNumber(word, seconds) || seconds == 0) {
                return false;
            }
            expiresAt = nowMs() + static_cast<uint64_t>(seconds) * 1000;
            return true;
        }
        return parseNumber(word, expiresAt) && expiresAt != 0;
    }

    bool ZestDB::parseCommand(const std::string& raw, Command& out) {
        std::string_view sv(raw);
        auto start = sv.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) {
            return false;
        }
        sv.remove_prefix(start);
        sv.remove_suffix(sv.size() - 1 - sv.find_last_not_of("\r\n"));

        size_t pos = 0;
        while (pos < sv.size()) {
            size_t end = sv.find_first_of(" \t\r\n", pos);
            out.words.push_back(sv.substr(pos, end - pos));
            if (end == std::string_view::npos)
                break;
            pos = sv.find_first_not_of(" \t\r\n", end);
            if (pos == std::string_view::npos)
                break;
        }
        if (out.words.empty()) {
            return false;
        }
        out.line = sv;

        static const std::unordered_map<std::string_view, std::string_view> aliases = {
            { "get", "g" }, { "set", "s" }, { "del", "d" }, { "getby", "gb" }, { "setby", "sb" }, { "delby", "db" }, { "flush", "f" }, { "reload", "r" }, { "help", "h" }, { "stats", "info" },
        };
        out.name = toLowerStr(out.words[0]);
        if (auto it = aliases.find(out.name); it != aliases.end()) {
            out.name = it->second;
        }
        return true;
    }

    const std::unordered_map<std::string_view, ZestDB::CommandSpec>& ZestDB::commandTable() {
        static const std::unordered_map<std::string_view, CommandSpec> table = {
            { "g", { &ZestDB::cmdGet, Access::Read, Locking::Shared } },
            { "s", { &ZestDB::cmdSet, Access::Write, Locking::Shared } },
            { "setex", { &ZestDB::cmdSetex, Access::Write, Locking::Shared } },
            { "setexat", { &ZestDB::cmdSetex, Access::Write, Locking::Shared } },
            { "expire", { &ZestDB::cmdExpire, Access::Write, Locking::Shared } },
            { "expireat", { &ZestDB::cmdExpire, Access::Write, Locking::Shared } },
            { "persist", { &ZestDB::cmdPersist, Access::Write, Locking::Shared } },
            { "ttl", { &ZestDB::cmdTtl, Access::Read, Locking::Shared } },
            { "setnx", { &ZestDB::cmdSetnx, Access::Write, Locking::Shared } },
            { "cas", { &ZestDB::cmdCas, Access::Write, Locking::Shared } },
            { "incr", { &ZestDB::cmdIncr, Access::Write, Locking::Shared } },
            { "decr", { &ZestDB::cmdIncr, Access::Write, Locking::Shared } },
            { "d", { &ZestDB::cmdDel, Access::Write, Locking::Shared } },
            { "gb", { &ZestDB::cmdGetBy, Access::Read, Locking::Shared } },
            { "sb", { &ZestDB::cmdSetBy, Access::Write, Locking::Shared } },
            { "db", { &ZestDB::cmdDelBy, Access::Write, Locking::Shared } },
            { "exists", { &ZestDB::cmdExists, Access::Read, Locking::Shared } },
            { "count", { &ZestDB::cmdCount, Access::Read, Locking::Shared } },
            { "keys", { &ZestDB::cmdKeys, Access::Read, Locking::Shared } },
            { "h", { &ZestDB::cmdHelp, Access::Read, Locking::Shared } },
            { "info", { &ZestDB::cmdInfo, Access::Read, Locking::Shared } },
            { "user", { &ZestDB::cmdUser, Access::Admin, Locking::Exclusive } },
            { "f", { &ZestDB::cmdFlush, Access::Admin, Locking::None } },
            { "r", { &ZestDB::cmdReload, Access::Admin, Locking::Exclusive } },
            { "scfg", { &ZestDB::cmdSetConfig, Access::Admin, Locking::Exclusive } },
            { "gcfg", { &ZestDB::cmdGetConfig, Access::Admin, Locking::Shared } },
            { "restore", { &ZestDB::cmdRestore, Access::Admin, Locking::None } },
            { "save", { &ZestDB::cmdSave, Access::Admin, Locking::None } },
            { "lsaves", { &ZestDB::cmdListSaves, Access::Admin, Locking::Shared } },
            { "script", { &ZestDB::cmdScript, Access::Admin, Locking::Shared } },
            { "call", { &ZestDB::cmdCall, Access::Write, Locking::Shared } },
        };
        return table;
    }

    ResultType ZestDB::execCmd(const std::string& command, Role role, bool syncWal, const std::string& user) {
        ScriptEngine::CallerScope caller(user, role);
        Command cmd;
        if (!parseCommand(command, cmd)) {
            return { ResultType::Code::NoCommand };
        }
        const auto& table = commandTable();
        const auto it = table.find(cmd.name);
        if (it == table.end()) {
            return { ResultType::Code::UnknownCommand };
        }
        const CommandSpec& spec = it->second;
        if (role == Role::Replica || (spec.access == Access::Write && role == Role::Read) || (spec.access == Access::Admin && role != Role::Admin)) {
            return { ResultType::Code::PermissionDenied };
        }

        // `reload` and `scfg` replace the settings: no other command may run meanwhile. `flush`, `save` and `restore`
        // take writeMtx themselves, exclusively, so they must not hold it already. Every other command shares it.
        std::shared_lock<std::shared_mutex> sharedLock(this->writeMtx, std::defer_lock);
        std::unique_lock<std::shared_mutex> uniqueLock(this->writeMtx, std::defer_lock);
        if (spec.locking == Locking::Exclusive) {
            uniqueLock.lock();
        } else if (spec.locking == Locking::Shared) {
            sharedLock.lock();
        }

        // Without triggers or replicas, setby / delby are logged as one record: no other write may come between them
        // and it. Otherwise each key they touch is logged on its own (cmd.rowsLogged).
        const bool setOrDelBy = cmd.name == "sb" || cmd.name == "db";
        bool loggedAsOne = setOrDelBy && !this->logsRows();
        std::vector<std::unique_lock<std::mutex>> order;
        if (loggedAsOne) {
            order = this->shardManager->lockAllWrites();
            if (this->logsRows()) { // the replication log started meanwhile
                order.clear();
                loggedAsOne = false;
            }
        }
        cmd.rowsLogged = setOrDelBy && !loggedAsOne;

        ResultType result = (this->*spec.handler)(cmd);

        if (!this->replaying.load() && spec.access == Access::Write) {
            bool walOk = true;
            if (result.code == ResultType::Code::Success && loggedAsOne) {
                walOk = this->appendToWAL(std::string(cmd.words[1]), command);
            }
            order.clear();
            if (syncWal) {
                walOk = this->shardManager->syncWAL() && walOk;
            }
            if (!walOk && result.code == ResultType::Code::Success) {
                result = { ResultType::Code::WalWriteFailed, "", result.affectedRows };
            }
        }

        return result;
    }

    ResultType ZestDB::cmdGet(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else {
            result = this->get(std::string(words[1]));
        }
        return result;
    }

    ResultType ZestDB::cmdSet(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        ResultType result = { ResultType::Code::InternalError };
        std::string value;
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else if (words.size() < 3) {
            result.code = ResultType::Code::MissingValue;
        } else if (!readLastValue(sv, words[2], value)) {
            result.code = ResultType::Code::InvalidValueFormat;
        } else {
            result = this->set(std::string(words[1]), value);
        }
        return result;
    }

    ResultType ZestDB::cmdSetex(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        const std::string& cmd = c.name;
        ResultType result = { ResultType::Code::InternalError };
        uint64_t ttl = 0;
        std::string value;
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else if (words.size() < 4) {
            result.code = ResultType::Code::MissingValue;
        } else if (!parseTTL(words[2], cmd == "setex", ttl)) {
            result.code = ResultType::Code::InvalidTtl;
        } else if (!readLastValue(sv, words[3], value)) {
            result.code = ResultType::Code::InvalidValueFormat;
        } else {
            result = this->set(std::string(words[1]), value, ttl);
        }
        return result;
    }

    ResultType ZestDB::cmdExpire(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string& cmd = c.name;
        ResultType result = { ResultType::Code::InternalError };
        uint64_t ttl = 0;
        if (words.size() < 3) {
            result.code = ResultType::Code::MissingArguments;
        } else if (!parseTTL(words[2], cmd == "expire", ttl)) {
            result.code = ResultType::Code::InvalidTtl;
        } else {
            result = this->setTTL(std::string(words[1]), ttl);
        }
        return result;
    }

    ResultType ZestDB::cmdPersist(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else {
            result = this->setTTL(std::string(words[1]), 0);
        }
        return result;
    }

    ResultType ZestDB::cmdTtl(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else {
            result = this->getTTL(std::string(words[1]));
        }
        return result;
    }

    ResultType ZestDB::cmdSetnx(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        ResultType result = { ResultType::Code::InternalError };
        std::string value;
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else if (words.size() < 3) {
            result.code = ResultType::Code::MissingValue;
        } else if (!readLastValue(sv, words[2], value)) {
            result.code = ResultType::Code::InvalidValueFormat;
        } else {
            result = this->setnx(std::string(words[1]), value);
        }
        return result;
    }

    ResultType ZestDB::cmdCas(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        ResultType result = { ResultType::Code::InternalError };
        std::string expected;
        std::string value;
        size_t valuePos = 0;
        bool expectedOk = false;
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else if (words.size() < 3) {
            result.code = ResultType::Code::MissingArguments;
        } else if (valuePos = static_cast<size_t>(words[2].data() - sv.data()), expectedOk = readQuoted(sv, valuePos, expected); expectedOk && onlySpacesFrom(sv, valuePos)) {
            result.code = ResultType::Code::MissingValue;
        } else if (!expectedOk || !readQuoted(sv, valuePos, value) || !onlySpacesFrom(sv, valuePos)) {
            result.code = ResultType::Code::InvalidValueFormat;
        } else {
            result = this->cas(std::string(words[1]), expected, value);
        }
        return result;
    }

    ResultType ZestDB::cmdIncr(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string& cmd = c.name;
        ResultType result = { ResultType::Code::InternalError };
        int64_t delta = 1;
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else if (words.size() > 2 && !parseNumber(words[2], delta)) {
            result.code = ResultType::Code::InvalidIncrement;
        } else if (cmd == "decr" && delta == INT64_MIN) {
            result.code = ResultType::Code::IntegerOverflow;
        } else {
            result = this->incr(std::string(words[1]), cmd == "incr" ? delta : -delta);
        }
        return result;
    }

    ResultType ZestDB::cmdDel(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else {
            result = this->del(std::string(words[1]));
        }
        return result;
    }

    ResultType ZestDB::cmdGetBy(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 3) {
            result.code = ResultType::Code::MissingArguments;
        } else {
            std::string mode = toLowerStr(words[1]);
            std::string_view pattern = words[2];
            unsigned int limit = UINT_MAX;
            bool limitOk = true;
            for (size_t i = 3; i < words.size(); i++) {
                if (toLowerStr(words[i]) == "lim" && i + 1 < words.size()) {
                    limitOk = parseNumber(words[i + 1], limit);
                }
            }
            auto [valid, validMode] = this->createValidationRule(mode, pattern);
            if (!limitOk) {
                result.code = ResultType::Code::InvalidLimit;
            } else if (!validMode) {
                result.code = ResultType::Code::InvalidRegex;
            } else {
                valid.limit = limit;
                result = this->getBy(valid);
            }
        }
        return result;
    }

    ResultType ZestDB::cmdSetBy(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 4) {
            result.code = ResultType::Code::MissingValue;
        } else {
            std::string mode = toLowerStr(words[1]);
            std::string_view pattern = words[2];
            unsigned int limit = UINT_MAX;
            std::string value;
            size_t valuePos = static_cast<size_t>(words[3].data() - sv.data());

            const bool valueOk = readQuoted(sv, valuePos, value);
            std::string_view rest;
            if (valueOk && !onlySpacesFrom(sv, valuePos)) {
                rest = sv.substr(sv.find_first_not_of(" \t", valuePos));
                rest = rest.substr(0, rest.find_last_not_of(" \t") + 1);
            }
            const bool hasLimit = rest.size() > 3 && toLowerStr(rest.substr(0, 3)) == "lim" && (rest[3] == ' ' || rest[3] == '\t');

            if (!valueOk || (!rest.empty() && !hasLimit)) {
                result.code = ResultType::Code::InvalidValueFormat;
            } else if (hasLimit && !parseNumber(rest.substr(rest.find_first_not_of(" \t", 3)), limit)) {
                result.code = ResultType::Code::InvalidLimit;
            } else {
                auto [valid, validMode] = this->createValidationRule(mode, pattern);
                if (!validMode) {
                    result.code = ResultType::Code::InvalidRegex;
                } else {
                    valid.limit = limit;
                    result = this->setBy(valid, value, c.rowsLogged);
                }
            }
        }
        return result;
    }

    ResultType ZestDB::cmdDelBy(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 3) {
            result.code = ResultType::Code::MissingPattern;
        } else {
            std::string mode = toLowerStr(words[1]);
            std::string_view pattern = words[2];
            unsigned int limit = UINT_MAX;
            bool limitOk = true;
            for (size_t i = 3; i < words.size(); i++) {
                if (toLowerStr(words[i]) == "lim" && i + 1 < words.size()) {
                    limitOk = parseNumber(words[i + 1], limit);
                }
            }
            auto [valid, validMode] = this->createValidationRule(mode, pattern);
            if (!limitOk) {
                result.code = ResultType::Code::InvalidLimit;
            } else if (!validMode) {
                result.code = ResultType::Code::InvalidMode;
            } else {
                valid.limit = limit;
                result = this->delBy(valid, c.rowsLogged);
            }
        }
        return result;
    }

    ResultType ZestDB::cmdExists(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingKey;
        } else {
            result = this->exists(std::string(words[1]));
        }
        return result;
    }

    ResultType ZestDB::cmdCount(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() == 1) {
            result = this->count(ValidationRule{});
        } else if (words.size() == 2) {
            result.code = ResultType::Code::MissingPattern;
        } else {
            auto [valid, validMode] = this->createValidationRule(toLowerStr(words[1]), words[2]);
            if (!validMode) {
                result.code = ResultType::Code::InvalidRegex;
            } else {
                result = this->count(valid);
            }
        }
        return result;
    }

    ResultType ZestDB::cmdKeys(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        ValidationRule valid;
        size_t next = 1;
        bool ok = true;
        if (words.size() > 1 && toLowerStr(words[1]) != "lim") {
            if (words.size() < 3) {
                result.code = ResultType::Code::MissingPattern;
                ok = false;
            } else if (auto [rule, validMode] = this->createValidationRule(toLowerStr(words[1]), words[2]); !validMode) {
                result.code = ResultType::Code::InvalidRegex;
                ok = false;
            } else {
                valid = std::move(rule);
                next = 3;
            }
        }
        if (ok && words.size() > next && (words.size() != next + 2 || toLowerStr(words[next]) != "lim" || !parseNumber(words[next + 1], valid.limit))) {
            result.code = ResultType::Code::InvalidLimit;
            ok = false;
        }
        if (ok) {
            result = this->keys(valid);
        }
        return result;
    }

    ResultType ZestDB::cmdHelp(Command& c) {
        (void)c;
        ResultType result = { ResultType::Code::InternalError };
        result.code = ResultType::Code::Success;
        result.response = this->help();
        return result;
    }

    ResultType ZestDB::cmdUser(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        if (words.size() < 2) {
            return { ResultType::Code::MissingArguments };
        }
        const std::string sub = toLowerStr(words[1]);
        if (sub == "ls" && words.size() == 2) {
            return this->listUsers();
        }
        if (sub != "add" && sub != "passwd" && sub != "role" && sub != "del") {
            return { ResultType::Code::UnknownCommand };
        }
        if (words.size() < 3 || (sub != "del" && words.size() < 4)) {
            return { ResultType::Code::MissingArguments };
        }
        const std::string name(words[2]);
        if (sub == "del") {
            return words.size() == 3 ? this->deleteUser(name) : ResultType{ ResultType::Code::InvalidUserName };
        }
        if (sub == "role") {
            return words.size() == 4 ? this->setUserRole(name, std::string(words[3])) : ResultType{ ResultType::Code::InvalidRole };
        }

        // add / passwd: a quoted password, then for add an optional role
        auto pos = static_cast<size_t>(words[3].data() - sv.data());
        std::string password;
        if (!readQuoted(sv, pos, password)) {
            return { ResultType::Code::InvalidValueFormat };
        }
        if (password.empty()) {
            return { ResultType::Code::EmptyValue };
        }
        std::string_view rest = sv.substr(pos);
        const size_t first = rest.find_first_not_of(" \t");
        rest = first == std::string_view::npos ? std::string_view{} : rest.substr(first, rest.find_last_not_of(" \t") - first + 1);
        if (sub == "passwd") {
            return rest.empty() ? this->setUserPassword(name, password) : ResultType{ ResultType::Code::InvalidValueFormat };
        }
        return this->addUser(name, password, std::string(rest));
    }

    ResultType ZestDB::cmdInfo(Command& c) {
        (void)c;
        return this->info();
    }

    ResultType ZestDB::cmdFlush(Command& c) {
        (void)c;
        ResultType result = { ResultType::Code::InternalError };
        this->flush();
        result.code = ResultType::Code::Success;
        return result;
    }

    ResultType ZestDB::cmdReload(Command& c) {
        (void)c;
        ResultType result = { ResultType::Code::InternalError };
        result = this->reloadConfig();
        return result;
    }

    ResultType ZestDB::cmdSetConfig(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 3) {
            result.code = ResultType::Code::MissingArguments;
            return result;
        }

        std::string param = toLowerStr(words[1]);
        std::string_view valueStr = words[2];

        bool valueSet = false;

        unsigned long ulongVal = 0;
        unsigned int uintVal = 0;

        if (param == "segsize") {
            if (!parseNumber(valueStr, ulongVal)) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            if (ulongVal <= this->settings.MaxValueSize) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            this->settings.SegSize = ulongVal;
            valueSet = true;
        } else if (param == "maxkeysize") {
            if (!parseNumber(valueStr, uintVal)) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            if (uintVal > MAX_KEY_SIZE) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            this->settings.MaxKeySize = uintVal;
            valueSet = true;
        } else if (param == "maxvaluesize") {
            if (!parseNumber(valueStr, uintVal)) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            if (uintVal >= this->settings.SegSize) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            this->settings.MaxValueSize = uintVal;
            valueSet = true;
        } else if (param == "cachesize") {
            if (!parseNumber(valueStr, uintVal)) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            this->settings.CacheSize = uintVal;
            valueSet = true;
        } else if (param == "compactinginterval") {
            if (!parseNumber(valueStr, uintVal) || uintVal == 0) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            this->settings.CompactingInterval = uintVal;
            valueSet = true;
        } else if (param == "flushinterval") {
            if (!parseNumber(valueStr, uintVal) || uintVal == 0) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            this->settings.FlushInterval = uintVal;
            valueSet = true;
        } else if (param == "networkvalidationstr") {
            try {
                std::string valueStrStr(valueStr);
                std::regex testRegex(valueStrStr);
                this->settings.NetworkValidationStr = std::move(valueStrStr);
                this->settings.NetworkValidation = std::move(testRegex);
                valueSet = true;
            } catch (const std::regex_error&) {
                result.code = ResultType::Code::InvalidConfigValue;
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
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            setLoggerDebugMode(this->settings.isDebug);
        } else if (param == "usessl") {
            result.code = ResultType::Code::ConfigNotReloadable;
            return result;
        } else if (param == "jsononly") {
            if (valueStr == "true" || valueStr == "1") {
                this->settings.jsonOnly = true;
                valueSet = true;
            } else if (valueStr == "false" || valueStr == "0") {
                this->settings.jsonOnly = false;
                valueSet = true;
            } else {
                result.code = ResultType::Code::InvalidConfigValue;
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
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
        } else if (param == "archivecreationdelay") {
            if (!parseNumber(valueStr, uintVal) || uintVal == 0) {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
            this->settings.ArchiveCreationDelay = uintVal;
            valueSet = true;
        } else if (param == "autoarchivesaving") {
            if (valueStr == "true" || valueStr == "1") {
                this->settings.AutoArchiveSaving = true;
                valueSet = true;
            } else if (valueStr == "false" || valueStr == "0") {
                this->settings.AutoArchiveSaving = false;
                valueSet = true;
            } else {
                result.code = ResultType::Code::InvalidConfigValue;
                return result;
            }
        } else {
            result.code = ResultType::Code::UnknownConfigParam;
            return result;
        }

        if (valueSet) {
            result.code = ResultType::Code::Success;
            this->setConfig();
            this->shardManager->reloadSettings(this->settings);
        }
        return result;
    }

    ResultType ZestDB::cmdGetConfig(Command& c) {
        (void)c;
        ResultType result = { ResultType::Code::InternalError };
        result.code = ResultType::Code::Success;
        result.response = this->getConfig();
        return result;
    }

    ResultType ZestDB::cmdRestore(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingArguments;
        } else {
            result = this->restore(std::string(words[1]));
        }
        return result;
    }

    ResultType ZestDB::cmdSave(Command& c) {
        (void)c;
        ResultType result = { ResultType::Code::InternalError };
        if (this->createArchive()) {
            result.code = ResultType::Code::Success;
        } else {
            result.code = ResultType::Code::ArchiveFailed;
            return result;
        }
        return result;
    }

    ResultType ZestDB::cmdListSaves(Command& c) {
        (void)c;
        ResultType result = { ResultType::Code::InternalError };
        result = this->listSaves();
        return result;
    }

    ResultType ZestDB::cmdScript(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        ResultType result = { ResultType::Code::InternalError };
        const std::string sub = words.size() > 1 ? toLowerStr(words[1]) : "";
        std::string code;
        if (sub == "ls") {
            result = this->listScripts();
        } else if (sub == "reload" && words.size() == 2) {
            result = this->reloadScripts();
        } else if ((sub != "load" && sub != "del" && sub != "reload") || words.size() < 3) {
            result.code = words.size() < 2 || sub == "load" || sub == "del" || sub == "reload" ? ResultType::Code::MissingArguments : ResultType::Code::UnknownCommand;
        } else if (sub == "del") {
            result = this->dropScript(std::string(words[2]));
        } else if (sub == "reload") {
            result = this->reloadScript(std::string(words[2]));
        } else if (words.size() < 4) {
            result.code = ResultType::Code::MissingValue;
        } else if (!readLastValue(sv, words[3], code)) {
            result.code = ResultType::Code::InvalidValueFormat;
        } else {
            result = this->loadScript(std::string(words[2]), code);
        }
        return result;
    }

    ResultType ZestDB::cmdCall(Command& c) {
        const std::vector<std::string_view>& words = c.words;
        const std::string_view sv = c.line;
        ResultType result = { ResultType::Code::InternalError };
        if (words.size() < 2) {
            result.code = ResultType::Code::MissingArguments;
        } else {
            std::vector<std::string> args;
            size_t argPos = static_cast<size_t>(words[1].data() - sv.data()) + words[1].size();
            bool argsOk = true;
            while (argsOk && !onlySpacesFrom(sv, argPos)) {
                std::string arg;
                argsOk = readQuoted(sv, argPos, arg);
                args.push_back(std::move(arg));
            }
            result = argsOk ? this->call(std::string(words[1]), args) : ResultType{ ResultType::Code::InvalidValueFormat };
        }
        return result;
    }

    std::string ZestDB::help() const {
        std::ostringstream oss;
        oss << "ZestDB Commands:" << "\n";
        oss << "---------------" << "\n";
        oss << "get <key>                              - Get value by key"
            << "\n";
        oss << "set <key> \"<value>\"                    - Set key-value pair"
            << "\n";
        oss << "del <key>                              - Delete key" << "\n";
        oss << "setex <key> <seconds> \"<value>\"        - Set a value that expires after <seconds>" << "\n";
        oss << "expire <key> <seconds>                 - Make an existing key expire after <seconds>" << "\n";
        oss << "ttl <key>                              - Seconds before expiry (-1: no TTL, -2: no key)" << "\n";
        oss << "exists <key>                           - true if the key exists and has not expired, false otherwise" << "\n";
        oss << "count [<mode> <pattern>]               - Number of live keys, or of those matching the pattern" << "\n";
        oss << "keys [<mode> <pattern>] [lim <n>]      - Sorted live keys, or those matching the pattern; lim keeps the n smallest" << "\n";
        oss << "persist <key>                          - Remove the TTL of a key" << "\n";
        oss << "incr <key> [n]                         - Add n (default 1) to an integer value, 0 if absent" << "\n";
        oss << "decr <key> [n]                         - Subtract n (default 1) from an integer value" << "\n";
        oss << "cas <key> \"<expected>\" \"<new>\"         - Replace the value only if it equals <expected>" << "\n";
        oss << "setnx <key> \"<value>\"                  - Set the value only if the key does not exist" << "\n";
        oss << "getby <mode> <pattern> [lim <n>]       - Get keys matching "
               "pattern"
            << "\n";
        oss << "setby <mode> <pattern> \"<val>\" [lim <n>] - Set value for keys "
               "matching pattern"
            << "\n";
        oss << "Values are written between double quotes; use \\\" for a quote and \\\\ for a backslash inside them" << "\n";
        oss << "delby <mode> <pattern> [lim <n>]       - Delete keys matching "
               "pattern"
            << "\n";
        oss << "flush                                  - Flush all data in "
               "memory "
               "to the disk"
            << "\n";
        oss << "reload                                 - Reload configuration "
               "from "
               "config.yaml"
            << "\n";
        oss << "scfg <param> <value>                   - Set config parameter"
            << "\n";
        oss << "save                                   - Create a zip archive of the database" << "\n";
        oss << "restore <archive.zip>                  - Replace the database with an archive of ArchiveStoragePath" << "\n";
        oss << "lsaves                                 - List the archives of ArchiveStoragePath, oldest first" << "\n";
        oss << "info (or stats)                        - Number of keys, size on disk, cache hit ratio, last archive" << "\n";
        oss << "user add <name> \"<password>\" [role]    - Create a user (role: read, write (default) or admin)" << "\n";
        oss << "user passwd <name> \"<password>\"        - Change a password and close the user's connections" << "\n";
        oss << "user role <name> <role>                - Change the role of a user" << "\n";
        oss << "user del <name>                        - Delete a user and close its connections" << "\n";
        oss << "user ls                                - List the users and their roles" << "\n";
        oss << "script load <name> \"<lua code>\"        - Save a Lua script and register its functions and triggers" << "\n";
        oss << "script del <name>                      - Remove a script with its functions and triggers" << "\n";
        oss << "script reload <name>                   - Reload DbPath/scripts/<name>.lua after editing it on disk" << "\n";
        oss << "script reload                          - Reload every script of DbPath/scripts (removed files are unloaded)" << "\n";
        oss << "script ls                              - List the scripts, functions and triggers" << "\n";
        oss << "call <function> [\"<arg>\" ...]          - Run a function registered by a script" << "\n";
        oss << "help                                   - Show this help"
            << "\n";
        oss << "\n";
        oss << "Config Parameters:" << "\n";
        oss << "  SegSize(int), MaxKeySize(int), MaxValueSize(int), "
               "CacheSize(int), CompactingInterval(int), FlushInterval(int), "
               "isDebug(bool), jsonOnly(bool), readOnly(bool), "
               "NetworkValidationStr(string), ArchiveCreationDelay(int), "
               "AutoArchiveSaving(bool)"
            << "\n";
        oss << "\n";
        oss << "Modes:" << "\n";
        oss << "  re  - regex" << "\n";
        oss << "  sw  - starts with" << "\n";
        oss << "  ct  - contains" << "\n";
        oss << "  ew  - ends with" << "\n";
        oss << "  lim - limit (optional)" << "\n";
        oss << "\n";
        oss << "Examples:" << "\n";
        oss << "  gb re .* lim 10                      - Get keys matching "
               "regex "
               "'.*' with limit 10"
            << "\n";
        oss << "  gb sw ca lim 20                      - Get keys starting "
               "with "
               "'ca' with limit 20"
            << "\n";
        oss << "  gb ct ac lim 1                       - Get keys containing "
               "'ac' "
               "with limit 1"
            << "\n";
        oss << "  gb ew ca                             - Get keys ending with "
               "'ca' "
               "(no limit)"
            << "\n";
        oss << "\n";
        oss << "Shortcuts: g=get, s=set, d=del, gb=getby, sb=setby, db=delby, "
               "f=flush, r=reload, scfg=setconfig, h=help"
            << "\n";
        return oss.str();
    }

} // namespace Zest
