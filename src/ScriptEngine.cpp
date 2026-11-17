#include "ScriptEngine.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sol/sol.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "Logger.hpp"
#include "ZestDB.hpp"
#include "lib/json.hpp"

namespace Zest {

    namespace fs = std::filesystem;

    namespace {

        constexpr uint64_t MAX_INSTRUCTIONS = 20'000'000;
        constexpr int HOOK_STEP = 1000;
        constexpr size_t MAX_MEMORY = size_t{ 64 } << 20;
        constexpr int MAX_DEPTH = 16;
        constexpr const char* LIMIT_MARK = "ZEST_LIMIT";

        std::atomic<uint64_t> nextEngineId{ 1 };

        struct Limits {
            uint64_t instructions = 0;
            size_t memory = 0;
        };

        void* limitedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
            auto* limits = static_cast<Limits*>(ud);
            const size_t old = ptr != nullptr ? osize : 0;
            if (nsize == 0) {
                std::free(ptr);
                limits->memory -= old;
                return nullptr;
            }
            if (nsize > old && limits->memory - old + nsize > MAX_MEMORY) {
                return nullptr;
            }
            void* block = std::realloc(ptr, nsize);
            if (block != nullptr) {
                limits->memory = limits->memory - old + nsize;
            }
            return block;
        }

        void countHook(lua_State* L, lua_Debug*) {
            Limits* limits = *static_cast<Limits**>(lua_getextraspace(L));
            limits->instructions += static_cast<uint64_t>(HOOK_STEP);
            if (limits->instructions > MAX_INSTRUCTIONS) {
                luaL_error(L, "%s: instruction limit reached", LIMIT_MARK);
            }
        }

        [[noreturn]] void fail(const ResultType& result) {
            if (result.code == ResultType::Code::ScriptLimitExceeded) {
                throw std::runtime_error(LIMIT_MARK);
            }
            throw std::runtime_error(std::format("zest: the command failed with code {}", static_cast<int>(result.code)));
        }

        ResultType::Code failureCode(sol::protected_function_result& result, ResultType::Code fallback, std::string_view what) {
            const bool outOfMemory = result.status() == sol::call_status::memory;
            const sol::error err = result.get<sol::error>();
            const std::string message = err.what();
            ZestLog(LogLevel::Debug, std::format("Script {}: {}", what, message));
            return outOfMemory || message.find(LIMIT_MARK) != std::string::npos ? ResultType::Code::ScriptLimitExceeded : fallback;
        }

        bool conflicts(const std::vector<std::string>& taken, const std::vector<std::string>& added) {
            return std::any_of(added.begin(), added.end(), [&taken](const std::string& name) { return std::find(taken.begin(), taken.end(), name) != taken.end(); });
        }

    } // namespace

    struct ScriptEngine::LuaContext {
        enum class Mode {
            Load,
            Run
        };

        struct Scope {
            LuaContext& ctx;
            bool ok;

            explicit Scope(LuaContext& c)
                : ctx(c), ok(c.enter()) {
            }

            ~Scope() {
                this->ctx.depth--;
            }
        };

        Limits limits;
        sol::state lua{ sol::default_at_panic, &limitedAlloc, &limits };
        Mode mode = Mode::Load;
        std::string script;
        Registry* registry = nullptr;
        std::unordered_map<std::string, sol::protected_function> functions;
        std::unordered_map<std::string, sol::protected_function> triggers;
        sol::protected_function tostring;
        int depth = 0;

        bool enter() {
            if (this->depth == 0) {
                this->limits.instructions = 0;
            }
            return ++this->depth <= MAX_DEPTH;
        }

        sol::protected_function_result run(const std::string& name, const std::string& code) {
            this->script = name;
            this->limits.instructions = 0;
            return this->lua.safe_script(code, sol::script_pass_on_error, "@" + name + ".lua", sol::load_mode::text);
        }
    };

    struct ScriptEngine::ThreadState {
        uint64_t generation = 0;
        std::unique_ptr<LuaContext> ctx;
    };

    ScriptEngine::ScriptEngine(ZestDB& database, std::filesystem::path dir)
        : db(database), id(nextEngineId.fetch_add(1)), scriptsDir(std::move(dir)) {
    }

    ScriptEngine::~ScriptEngine() = default;

    static std::vector<std::string> names(const std::vector<ScriptFunction>& functions, const std::string& except = "") {
        std::vector<std::string> out;
        for (const auto& f : functions) {
            if (f.script != except) {
                out.push_back(f.name);
            }
        }
        return out;
    }

    static std::vector<std::string> names(const std::vector<Trigger>& triggers, const std::string& except = "") {
        std::vector<std::string> out;
        for (const auto& t : triggers) {
            if (t.script != except) {
                out.push_back(t.name);
            }
        }
        return out;
    }

    void ScriptEngine::loadAll() {
        std::vector<fs::path> files;
        std::error_code ec;
        for (auto it = fs::directory_iterator(this->scriptsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".lua") {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());

        Registry fresh;
        for (const fs::path& file : files) {
            const std::string name = file.stem().string();
            std::ifstream in(file, std::ios::binary);
            std::stringstream code;
            code << in.rdbuf();

            Registry one;
            if (!isValidName(name) || this->validate(name, code.str(), one).code != ResultType::Code::Success) {
                ZestLog(LogLevel::Error, std::format("Scripts: {} is invalid and was not loaded", file.string()));
                continue;
            }
            if (conflicts(names(fresh.functions), names(one.functions)) || conflicts(names(fresh.triggers), names(one.triggers))) {
                ZestLog(LogLevel::Error, std::format("Scripts: {} registers a name another script already uses, it was not loaded", file.string()));
                continue;
            }
            fresh.scripts.merge(one.scripts);
            std::move(one.functions.begin(), one.functions.end(), std::back_inserter(fresh.functions));
            std::move(one.triggers.begin(), one.triggers.end(), std::back_inserter(fresh.triggers));
        }

        std::unique_lock<std::shared_mutex> lock(this->registryMtx);
        this->registry = std::move(fresh);
        this->anyTrigger = !this->registry.triggers.empty();
        this->generation++;
        ZestLog(LogLevel::Info, std::format("Scripts: {} loaded", this->registry.scripts.size()));
    }

    ResultType ScriptEngine::load(const std::string& name, const std::string& code) {
        if (!isValidName(name)) {
            return { ResultType::Code::ScriptError };
        }
        return this->install(name, code, true);
    }

    ResultType ScriptEngine::reloadFile(const std::string& name) {
        if (!isValidName(name)) {
            return { ResultType::Code::ScriptError };
        }
        std::ifstream in(this->scriptsDir / (name + ".lua"), std::ios::binary);
        if (!in) {
            return { ResultType::Code::ScriptNotFound };
        }
        std::stringstream code;
        code << in.rdbuf();
        return this->install(name, code.str(), false);
    }

    ResultType ScriptEngine::install(const std::string& name, const std::string& code, bool writeFile) {
        Registry added;
        ResultType validated = this->validate(name, code, added);
        if (validated.code != ResultType::Code::Success) {
            return validated;
        }

        std::unique_lock<std::shared_mutex> lock(this->registryMtx);
        if (conflicts(names(this->registry.functions, name), names(added.functions)) || conflicts(names(this->registry.triggers, name), names(added.triggers))) {
            ZestLog(LogLevel::Debug, std::format("Script {}: registers a name another script already uses", name));
            return { ResultType::Code::ScriptError };
        }

        if (writeFile) {
            std::error_code ec;
            fs::create_directories(this->scriptsDir, ec);
            const fs::path target = this->scriptsDir / (name + ".lua");
            const fs::path tmp = this->scriptsDir / (name + ".lua.tmp");
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!(out << code) || !out.flush()) {
                    fs::remove(tmp, ec);
                    return { ResultType::Code::WriteFailed };
                }
            }
            fs::rename(tmp, target, ec);
            if (ec) {
                fs::remove(tmp, ec);
                return { ResultType::Code::WriteFailed };
            }
        }

        std::erase_if(this->registry.functions, [&name](const ScriptFunction& f) { return f.script == name; });
        std::erase_if(this->registry.triggers, [&name](const Trigger& t) { return t.script == name; });
        std::move(added.functions.begin(), added.functions.end(), std::back_inserter(this->registry.functions));
        std::move(added.triggers.begin(), added.triggers.end(), std::back_inserter(this->registry.triggers));
        this->registry.scripts[name] = code;
        this->anyTrigger = !this->registry.triggers.empty();
        this->generation++;
        return { ResultType::Code::Success };
    }

    ResultType ScriptEngine::drop(const std::string& name) {
        std::unique_lock<std::shared_mutex> lock(this->registryMtx);
        if (!this->registry.scripts.contains(name)) {
            return { ResultType::Code::ScriptNotFound };
        }

        std::error_code ec;
        fs::remove(this->scriptsDir / (name + ".lua"), ec);
        if (ec) {
            return { ResultType::Code::WriteFailed };
        }

        this->registry.scripts.erase(name);
        std::erase_if(this->registry.functions, [&name](const ScriptFunction& f) { return f.script == name; });
        std::erase_if(this->registry.triggers, [&name](const Trigger& t) { return t.script == name; });
        this->anyTrigger = !this->registry.triggers.empty();
        this->generation++;
        return { ResultType::Code::Success };
    }

    ResultType ScriptEngine::list() const {
        std::shared_lock<std::shared_mutex> lock(this->registryMtx);
        std::vector<std::string> scripts;
        for (const auto& [name, code] : this->registry.scripts) {
            scripts.push_back(name);
        }
        std::vector<std::string> functions = names(this->registry.functions);
        std::vector<std::string> triggers = names(this->registry.triggers);
        std::sort(functions.begin(), functions.end());
        std::sort(triggers.begin(), triggers.end());

        nlohmann::json j = { { "scripts", scripts }, { "functions", functions }, { "triggers", triggers } };
        return { ResultType::Code::Success, j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), static_cast<long long>(scripts.size()) };
    }

    void ScriptEngine::reload() {
        this->generation++;
    }

    bool ScriptEngine::hasTriggers() const {
        return this->anyTrigger.load(std::memory_order_relaxed);
    }

    ResultType ScriptEngine::before(TriggerEvent event, const std::string& key, std::string& value) {
        if (!this->hasTriggers()) {
            return { ResultType::Code::Success };
        }
        LuaContext& ctx = *this->stateForThisThread().ctx;
        const std::vector<std::string> matching = this->matchingTriggers(TriggerPhase::Before, event, key);
        if (matching.empty()) {
            return { ResultType::Code::Success };
        }

        LuaContext::Scope scope(ctx);
        if (!scope.ok) {
            return { ResultType::Code::ScriptLimitExceeded };
        }
        for (const std::string& name : matching) {
            auto it = ctx.triggers.find(name);
            if (it == ctx.triggers.end()) {
                return { ResultType::Code::ScriptError };
            }
            if (event == TriggerEvent::Del) {
                sol::protected_function_result result = it->second(key);
                if (!result.valid()) {
                    return { failureCode(result, ResultType::Code::TriggerRejected, name) };
                }
                continue;
            }
            sol::protected_function_result result = it->second(key, value);
            if (!result.valid()) {
                return { failureCode(result, ResultType::Code::TriggerRejected, name) };
            }
            if (result.get_type() != sol::type::string) {
                ZestLog(LogLevel::Debug, std::format("Script {}: a before set trigger must return the value to write", name));
                return { ResultType::Code::ScriptError };
            }
            value = result.get<std::string>();
        }
        return { ResultType::Code::Success };
    }

    ResultType ScriptEngine::after(TriggerEvent event, const std::string& key, const std::string& value) {
        if (!this->hasTriggers()) {
            return { ResultType::Code::Success };
        }
        LuaContext& ctx = *this->stateForThisThread().ctx;
        const std::vector<std::string> matching = this->matchingTriggers(TriggerPhase::After, event, key);
        if (matching.empty()) {
            return { ResultType::Code::Success };
        }

        LuaContext::Scope scope(ctx);
        if (!scope.ok) {
            return { ResultType::Code::ScriptLimitExceeded };
        }
        for (const std::string& name : matching) {
            auto it = ctx.triggers.find(name);
            if (it == ctx.triggers.end()) {
                return { ResultType::Code::ScriptError };
            }
            sol::protected_function_result result = event == TriggerEvent::Set ? it->second(key, value) : it->second(key);
            if (!result.valid()) {
                return { failureCode(result, ResultType::Code::ScriptError, name) };
            }
        }
        return { ResultType::Code::Success };
    }

    ResultType ScriptEngine::call(const std::string& name, const std::vector<std::string>& args) {
        LuaContext& ctx = *this->stateForThisThread().ctx;
        auto it = ctx.functions.find(name);
        if (it == ctx.functions.end()) {
            return { ResultType::Code::FunctionNotFound };
        }

        LuaContext::Scope scope(ctx);
        if (!scope.ok) {
            return { ResultType::Code::ScriptLimitExceeded };
        }
        sol::protected_function_result result = it->second(sol::as_args(args));
        if (!result.valid()) {
            return { failureCode(result, ResultType::Code::ScriptError, name) };
        }

        std::string response;
        if (result.return_count() > 0 && result.get_type() != sol::type::lua_nil && result.get_type() != sol::type::none) {
            sol::object first = result.get<sol::object>();
            sol::protected_function_result text = ctx.tostring(first);
            if (text.valid() && text.get_type() == sol::type::string) {
                response = text.get<std::string>();
            }
        }
        return { ResultType::Code::Success, response };
    }

    ScriptEngine::ThreadState& ScriptEngine::stateForThisThread() {
        thread_local std::unordered_map<uint64_t, ThreadState> states;
        ThreadState& state = states[this->id];

        if (state.ctx && (state.ctx->depth > 0 || state.generation == this->generation.load())) {
            return state;
        }

        std::map<std::string, std::string> sources;
        uint64_t gen = 0;
        {
            std::shared_lock<std::shared_mutex> lock(this->registryMtx);
            sources = this->registry.scripts;
            gen = this->generation.load();
        }

        auto ctx = std::make_unique<LuaContext>();
        this->setupState(*ctx);
        for (const auto& [name, code] : sources) {
            sol::protected_function_result result = ctx->run(name, code);
            if (!result.valid()) {
                static_cast<void>(failureCode(result, ResultType::Code::ScriptError, name));
            }
        }
        ctx->mode = LuaContext::Mode::Run;
        state.ctx = std::move(ctx);
        state.generation = gen;
        return state;
    }

    ResultType ScriptEngine::validate(const std::string& name, const std::string& code, Registry& out) const {
        LuaContext ctx;
        ctx.registry = &out;
        this->setupState(ctx);

        sol::protected_function_result result = ctx.run(name, code);
        if (!result.valid()) {
            return { failureCode(result, ResultType::Code::ScriptError, name) };
        }
        out.scripts[name] = code;
        return { ResultType::Code::Success };
    }

    void ScriptEngine::setupState(LuaContext& ctx) const {
        sol::state& lua = ctx.lua;
        lua_State* L = lua.lua_state();
        *static_cast<Limits**>(lua_getextraspace(L)) = &ctx.limits;
        lua_sethook(L, &countHook, LUA_MASKCOUNT, HOOK_STEP);

        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table, sol::lib::math, sol::lib::utf8);
        for (const char* removed : { "dofile", "loadfile", "collectgarbage" }) {
            lua[removed] = sol::lua_nil;
        }
        lua["string"]["dump"] = sol::lua_nil;
        static_cast<void>(lua.safe_script(R"lua(
            local rawload = load
            load = function(chunk, chunkname, _, env)
                if env == nil then return rawload(chunk, chunkname, "t") end
                return rawload(chunk, chunkname, "t", env)
            end
        )lua",
            sol::script_pass_on_error));
        ctx.tostring = lua["tostring"];

        lua.set_function("print", [](sol::variadic_args args, sol::this_state ts) {
            std::string line;
            bool first = true;
            for (const auto& arg : args) {
                size_t len = 0;
                const char* text = luaL_tolstring(ts, arg.stack_index(), &len);
                if (!first) {
                    line += '\t';
                }
                first = false;
                line.append(text, len);
                lua_pop(ts, 1);
            }
            ZestLog(LogLevel::Info, "Lua: " + line);
        });

        auto requireLoad = [&ctx](const char* what) {
            if (ctx.mode != LuaContext::Mode::Load) {
                throw std::runtime_error(std::format("zest.{} can only be called while the script loads", what));
            }
        };
        auto requireRun = [&ctx](const char* what) {
            if (ctx.mode != LuaContext::Mode::Run) {
                throw std::runtime_error(std::format("zest.{} cannot be called while the script loads", what));
            }
        };
        auto ruleFrom = [this](const sol::optional<std::string>& mode, const sol::optional<std::string>& pattern) {
            if (mode.has_value() != pattern.has_value()) {
                throw std::runtime_error("mode and pattern go together");
            }
            if (!mode) {
                return ValidationRule{};
            }
            CreationValidationRuleResult created = this->db.createValidationRule(*mode, *pattern);
            if (!created.result) {
                throw std::runtime_error("invalid mode or pattern");
            }
            return std::move(created.rule);
        };

        sol::table zest = lua.create_named_table("zest");

        zest.set_function("log", [](const std::string& message, sol::optional<std::string> level) {
            static const std::unordered_map<std::string, LogLevel> levels = { { "debug", LogLevel::Debug }, { "info", LogLevel::Info }, { "warning", LogLevel::Warning }, { "error", LogLevel::Error } };
            auto it = levels.find(level.value_or("info"));
            if (it == levels.end()) {
                throw std::runtime_error("zest.log: the level must be debug, info, warning or error");
            }
            ZestLog(it->second, "Lua: " + message);
        });

        zest.set_function("register_function", [&ctx, requireLoad](const std::string& name, sol::protected_function fn) {
            requireLoad("register_function");
            if (name.empty() || !ctx.functions.emplace(name, std::move(fn)).second) {
                throw std::runtime_error(std::format("register_function: invalid or duplicate name '{}'", name));
            }
            if (ctx.registry) {
                ctx.registry->functions.push_back({ name, ctx.script });
            }
        });

        zest.set_function("register_trigger", [&ctx, requireLoad, ruleFrom](const sol::table& def) {
            requireLoad("register_trigger");
            const auto name = def.get<sol::optional<std::string>>("name");
            const auto phase = def.get<sol::optional<std::string>>("phase");
            const auto event = def.get<sol::optional<std::string>>("event");
            const auto fn = def.get<sol::object>("fn");
            if (!name || name->empty()) {
                throw std::runtime_error("register_trigger: name is required");
            }
            if (!phase || (*phase != "before" && *phase != "after")) {
                throw std::runtime_error("register_trigger: phase must be \"before\" or \"after\"");
            }
            if (!event || (*event != "set" && *event != "del")) {
                throw std::runtime_error("register_trigger: event must be \"set\" or \"del\"");
            }
            if (fn.get_type() != sol::type::function) {
                throw std::runtime_error("register_trigger: fn must be a function");
            }
            ValidationRule rule = ruleFrom(def.get<sol::optional<std::string>>("mode"), def.get<sol::optional<std::string>>("pattern"));
            if (!ctx.triggers.emplace(*name, fn.as<sol::protected_function>()).second) {
                throw std::runtime_error(std::format("register_trigger: duplicate name '{}'", *name));
            }
            if (ctx.registry) {
                ctx.registry->triggers.push_back({ *name, ctx.script, *phase == "before" ? TriggerPhase::Before : TriggerPhase::After, *event == "set" ? TriggerEvent::Set : TriggerEvent::Del, std::move(rule) });
            }
        });

        zest.set_function("get", [this, requireRun](const std::string& key, sol::this_state ts) -> sol::object {
            requireRun("get");
            ResultType r = this->db.get(key);
            if (r.code == ResultType::Code::KeyNotFound) {
                return sol::make_object(ts, sol::lua_nil);
            }
            if (r.code != ResultType::Code::Success) {
                fail(r);
            }
            return sol::make_object(ts, r.response);
        });

        zest.set_function("set", [this, requireRun](const std::string& key, const std::string& value, sol::optional<int64_t> ttl) {
            requireRun("set");
            uint64_t expiresAt = 0;
            if (ttl) {
                if (*ttl <= 0) {
                    throw std::runtime_error("zest.set: the ttl must be a positive number of seconds");
                }
                expiresAt = nowMs() + static_cast<uint64_t>(*ttl) * 1000;
            }
            ResultType r = this->db.set(key, value, expiresAt);
            if (r.code != ResultType::Code::Success) {
                fail(r);
            }
        });

        zest.set_function("del", [this, requireRun](const std::string& key) {
            requireRun("del");
            ResultType r = this->db.del(key);
            if (r.code != ResultType::Code::Success && r.code != ResultType::Code::KeyNotFound) {
                fail(r);
            }
            return r.code == ResultType::Code::Success;
        });

        zest.set_function("exists", [this, requireRun](const std::string& key) {
            requireRun("exists");
            ResultType r = this->db.exists(key);
            if (r.code != ResultType::Code::Success) {
                fail(r);
            }
            return r.response == "true";
        });

        zest.set_function("ttl", [this, requireRun](const std::string& key) {
            requireRun("ttl");
            ResultType r = this->db.getTTL(key);
            if (r.code != ResultType::Code::Success) {
                fail(r);
            }
            return std::stoll(r.response);
        });

        zest.set_function("expire", [this, requireRun](const std::string& key, int64_t seconds) {
            requireRun("expire");
            if (seconds <= 0) {
                throw std::runtime_error("zest.expire: seconds must be positive");
            }
            ResultType r = this->db.setTTL(key, nowMs() + static_cast<uint64_t>(seconds) * 1000);
            if (r.code != ResultType::Code::Success && r.code != ResultType::Code::KeyNotFound) {
                fail(r);
            }
            return r.code == ResultType::Code::Success;
        });

        zest.set_function("persist", [this, requireRun](const std::string& key) {
            requireRun("persist");
            ResultType r = this->db.setTTL(key, 0);
            if (r.code != ResultType::Code::Success && r.code != ResultType::Code::KeyNotFound) {
                fail(r);
            }
            return r.code == ResultType::Code::Success;
        });

        zest.set_function("incr", [this, requireRun](const std::string& key, sol::optional<int64_t> delta) {
            requireRun("incr");
            ResultType r = this->db.incr(key, delta.value_or(1));
            if (r.code != ResultType::Code::Success) {
                fail(r);
            }
            return std::stoll(r.response);
        });

        zest.set_function("setnx", [this, requireRun](const std::string& key, const std::string& value) {
            requireRun("setnx");
            ResultType r = this->db.setnx(key, value);
            if (r.code != ResultType::Code::Success && r.code != ResultType::Code::KeyExists) {
                fail(r);
            }
            return r.code == ResultType::Code::Success;
        });

        zest.set_function("cas", [this, requireRun](const std::string& key, const std::string& expected, const std::string& value) {
            requireRun("cas");
            ResultType r = this->db.cas(key, expected, value);
            if (r.code != ResultType::Code::Success && r.code != ResultType::Code::ValueMismatch && r.code != ResultType::Code::KeyNotFound) {
                fail(r);
            }
            return r.code == ResultType::Code::Success;
        });

        zest.set_function("count", [this, requireRun, ruleFrom](sol::optional<std::string> mode, sol::optional<std::string> pattern) {
            requireRun("count");
            ResultType r = this->db.count(ruleFrom(mode, pattern));
            if (r.code != ResultType::Code::Success) {
                fail(r);
            }
            return static_cast<int64_t>(r.affectedRows);
        });

        zest.set_function("keys", [this, requireRun, ruleFrom](sol::optional<std::string> mode, sol::optional<std::string> pattern, sol::optional<int64_t> lim) {
            requireRun("keys");
            ValidationRule rule = ruleFrom(mode, pattern);
            if (lim) {
                if (*lim <= 0) {
                    throw std::runtime_error("zest.keys: lim must be positive");
                }
                rule.limit = static_cast<unsigned int>(std::min<int64_t>(*lim, UINT_MAX));
            }
            ResultType r = this->db.keys(rule);
            if (r.code != ResultType::Code::Success) {
                fail(r);
            }
            return sol::as_table(nlohmann::json::parse(r.response).get<std::vector<std::string>>());
        });
    }

    std::vector<std::string> ScriptEngine::matchingTriggers(TriggerPhase phase, TriggerEvent event, std::string_view key) const {
        std::shared_lock<std::shared_mutex> lock(this->registryMtx);
        std::vector<std::string> matching;
        for (const Trigger& t : this->registry.triggers) {
            if (t.phase == phase && t.event == event && (!t.rule.func || t.rule.func(key))) {
                matching.push_back(t.name);
            }
        }
        return matching;
    }

    bool ScriptEngine::isValidName(std::string_view name) {
        return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
    }

} // namespace Zest
