#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "Settings.hpp"

namespace Zest {

    class ZestDB;

    /// When a trigger runs: before the write (it can change or reject the value) or after it.
    enum class TriggerPhase {
        Before,
        After
    };

    /// Which write a trigger watches.
    enum class TriggerEvent {
        Set,
        Del
    };

    /// A trigger registered by a script. The Lua function itself lives in each thread's Lua state, found by `name`.
    struct Trigger {
        std::string name;
        std::string script; // the script that registered it, so reloading or dropping that script replaces it
        TriggerPhase phase;
        TriggerEvent event;
        ValidationRule rule; // the keys it watches; empty func means every key
    };

    /// A function registered by a script, called with `call <name>`.
    struct ScriptFunction {
        std::string name;
        std::string script;
    };

    /// Lua scripting: loads the scripts of `<DbPath>/scripts/`, keeps the registry of their functions and triggers,
    /// and runs them.
    ///
    /// Lua states are not thread-safe, so every thread that runs scripts (each server IO thread) has its own state.
    /// It is built on first use by running every script, and rebuilt when `generation` changes after a load, drop or
    /// reload. The registry (names, filters, which script owns what) is shared and built once, when a script is
    /// validated in a temporary state.
    ///
    /// A script runs in two modes: while loading it may only call `zest.register_*`; once loaded, its functions get the
    /// data API, which calls the public ZestDB methods. Scripts never run under a shard lock, and every call is bounded
    /// by an instruction, memory and nesting limit (ScriptLimitExceeded).
    class ScriptEngine {
    public:
        /// The client that sent the command being run, as `zest.caller()` shows it.
        struct Caller {
            std::string user;
            Role role;
        };

        /// Makes the client of the current command visible to the scripts it runs on this thread, for the lifetime of
        /// the scope; the previous caller is restored afterwards. ZestDB::execCmd opens one per command.
        class CallerScope {
        public:
            CallerScope(const std::string& user, Role role);
            ~CallerScope();

            CallerScope(const CallerScope&) = delete;
            CallerScope& operator=(const CallerScope&) = delete;

        private:
            std::optional<Caller> previous;
        };

        /// Creates `scriptsDir` if needed; call loadAll() to load the scripts it holds.
        ScriptEngine(ZestDB& db, std::filesystem::path scriptsDir);
        ~ScriptEngine();

        ScriptEngine(const ScriptEngine&) = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;

        /// Replaces the registry with the scripts of the folder. A file that fails (syntax error, name used by another
        /// file...) is logged and skipped, so one broken file does not stop the others or the server.
        /// @return `{"loaded": [...], "failed": [...]}` as the response, the number loaded as `affectedRows`.
        ResultType loadAll();
        /// Validates `code`, saves it as `<scriptsDir>/<name>.lua` (through a temporary file and a rename) and replaces
        /// the functions and triggers of the script `name`. Nothing changes if any step fails.
        ResultType load(const std::string& name, const std::string& code);
        /// Like load(), with the code read from `<scriptsDir>/<name>.lua` and the file left as it is.
        ResultType reloadFile(const std::string& name);
        /// Deletes the script's file and its functions and triggers. ScriptNotFound if no script has this name.
        ResultType drop(const std::string& name);
        /// `{"scripts": [...], "functions": [...], "triggers": [...]}`, each list sorted.
        ResultType list() const;
        /// Makes every thread rebuild its Lua state on its next call.
        void reload();

        /// True if at least one trigger is loaded. Cheap: lets writes skip the trigger machinery entirely.
        bool hasTriggers() const;
        /// Runs the `before` triggers of `event` that match `key`. For `set`, each one receives `value` and returns the
        /// value to store, which replaces `value`. An error raised by a trigger gives TriggerRejected with its message.
        ResultType before(TriggerEvent event, const std::string& key, std::string& value);
        /// Runs the `after` triggers of `event` that match `key`, with the value that was stored (empty for `del`).
        ResultType after(TriggerEvent event, const std::string& key, const std::string& value);

        /// Runs the function `name` with `args` as Lua strings. Its first return value, converted with `tostring`, is
        /// the response. FunctionNotFound if no script registered it.
        ResultType call(const std::string& name, const std::vector<std::string>& args);

    private:
        /// What the loaded scripts define, shared by every thread.
        struct Registry {
            std::map<std::string, std::string> scripts; // name -> source, used to build each thread's Lua state
            std::vector<Trigger> triggers;              // in registration order, which is the order they run in
            std::vector<ScriptFunction> functions;
        };

        struct LuaContext; // a Lua state with its limits and the Lua functions of the loaded scripts (ScriptEngine.cpp)
        struct ThreadState;

        /// The Lua state of the calling thread, rebuilt first if the scripts changed (never while a script runs on it).
        ThreadState& stateForThisThread();
        /// Prepares a fresh Lua state: limits, sandbox (no io, os, package, debug, bytecode) and the `zest` API.
        void setupState(LuaContext& ctx) const;
        /// Shared by load() and reloadFile(): validates, checks name conflicts, optionally writes the file, then swaps the
        /// script's registry entries.
        ResultType install(const std::string& name, const std::string& code, bool writeFile);
        /// Runs `code` in a temporary state to check it and collect what it registers into `out`.
        ResultType validate(const std::string& name, const std::string& code, Registry& out) const;
        /// Names of the triggers of `phase` and `event` whose filter matches `key`. Names rather than pointers, so the
        /// registry lock is released before any Lua runs (a trigger may write, and so call this again).
        std::vector<std::string> matchingTriggers(TriggerPhase phase, TriggerEvent event, std::string_view key) const;
        /// 1 to 64 letters, digits, `_` or `-`: a name that is safe as a file name.
        static bool isValidName(std::string_view name);

        ZestDB& db;
        const uint64_t id; // distinguishes this engine's per-thread states from those of another engine in the same thread
        std::filesystem::path scriptsDir;

        mutable std::shared_mutex registryMtx;
        Registry registry;

        std::atomic<uint64_t> generation{ 0 }; // bumped on every change; a thread whose state is older rebuilds it
        std::atomic<bool> anyTrigger{ false };
    };

} // namespace Zest
