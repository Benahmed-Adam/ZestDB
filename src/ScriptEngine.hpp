#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    class ZestDB;

    enum class TriggerPhase {
        Before,
        After
    };

    enum class TriggerEvent {
        Set,
        Del
    };

    struct Trigger {
        std::string name;
        std::string script;
        TriggerPhase phase;
        TriggerEvent event;
        ValidationRule rule;
    };

    struct ScriptFunction {
        std::string name;
        std::string script;
    };

    class ScriptEngine {
    public:
        ScriptEngine(ZestDB& db, std::filesystem::path scriptsDir);
        ~ScriptEngine();

        ScriptEngine(const ScriptEngine&) = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;

        void loadAll();
        ResultType load(const std::string& name, const std::string& code);
        ResultType drop(const std::string& name);
        ResultType list() const;
        void reload();

        bool hasTriggers() const;
        ResultType before(TriggerEvent event, const std::string& key, std::string& value);
        ResultType after(TriggerEvent event, const std::string& key, const std::string& value);

        ResultType call(const std::string& name, const std::vector<std::string>& args);

    private:
        struct Registry {
            std::map<std::string, std::string> scripts;
            std::vector<Trigger> triggers;
            std::vector<ScriptFunction> functions;
        };

        struct LuaContext;
        struct ThreadState;

        ThreadState& stateForThisThread();
        void setupState(LuaContext& ctx) const;
        ResultType validate(const std::string& name, const std::string& code, Registry& out) const;
        std::vector<std::string> matchingTriggers(TriggerPhase phase, TriggerEvent event, std::string_view key) const;
        static bool isValidName(std::string_view name);

        ZestDB& db;
        const uint64_t id;
        std::filesystem::path scriptsDir;

        mutable std::shared_mutex registryMtx;
        Registry registry;

        std::atomic<uint64_t> generation{ 0 };
        std::atomic<bool> anyTrigger{ false };
    };

} // namespace Zest
