#include "ScriptEngine.hpp"

#include "ZestDB.hpp"

namespace Zest {

    struct ScriptEngine::ThreadState {
        uint64_t generation = 0;
        // sol::state lua;
        // functions registered by the scripts, by name
    };

    ScriptEngine::ScriptEngine(ZestDB& database, std::filesystem::path dir)
        : db(database), scriptsDir(std::move(dir)) {
    }

    ScriptEngine::~ScriptEngine() = default;

    void ScriptEngine::loadAll() {
    }

    ResultType ScriptEngine::load(const std::string&, const std::string&) {
        return { ResultType::Code::InternalError };
    }

    ResultType ScriptEngine::drop(const std::string&) {
        return { ResultType::Code::InternalError };
    }

    ResultType ScriptEngine::list() const {
        return { ResultType::Code::InternalError };
    }

    void ScriptEngine::reload() {
    }

    bool ScriptEngine::hasTriggers() const {
        return false;
    }

    ResultType ScriptEngine::before(TriggerEvent, const std::string&, std::string&) {
        return { ResultType::Code::InternalError };
    }

    ResultType ScriptEngine::after(TriggerEvent, const std::string&, const std::string&) {
        return { ResultType::Code::InternalError };
    }

    ResultType ScriptEngine::call(const std::string&, const std::vector<std::string>&) {
        return { ResultType::Code::InternalError };
    }

    ScriptEngine::ThreadState& ScriptEngine::stateForThisThread() {
        thread_local ThreadState state;
        return state;
    }

    ResultType ScriptEngine::validate(const std::string&, const std::string&, Registry&) const {
        return { ResultType::Code::InternalError };
    }

    std::vector<const Trigger*> ScriptEngine::matchingTriggers(TriggerPhase, TriggerEvent, std::string_view) const {
        return {};
    }

    bool ScriptEngine::isValidName(std::string_view) {
        return false;
    }

} // namespace Zest
