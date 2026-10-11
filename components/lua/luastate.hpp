#ifndef COMPONENTS_LUA_LUASTATE_H
#define COMPONENTS_LUA_LUASTATE_H

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <type_traits>
#include <typeinfo>

#include <sol/sol.hpp>

#include <components/vfs/pathutil.hpp>
#include <components/debug/hitchtrace.hpp>

#include "configuration.hpp"
#include "luastateptr.hpp"

namespace VFS
{
    class Manager;
}

namespace LuaUtil
{

    std::string getLuaVersion();

    class ScriptsContainer;
    struct ScriptId
    {
        ScriptsContainer* mContainer = nullptr;
        int mIndex = -1; // index in LuaUtil::ScriptsConfiguration
    };

    struct LuaStateSettings
    {
        uint64_t mInstructionLimit = 0; // 0 is unlimited
        uint64_t mMemoryLimit = 0; // 0 is unlimited
        uint64_t mSmallAllocMaxSize = 1024 * 1024; // big default value efficiently disables memory tracking
        bool mLogMemoryUsage = false;
        bool mInstructionProfilerEnabled = false;
    };

    class LuaState;
    class LuaView
    {
        sol::state_view mSol;

        LuaView(const LuaView&) = delete;

        LuaView(lua_State* state)
            : mSol(state)
        {
        }

    public:
        template <class Function>
        friend int invokeProtectedCall(lua_State*, Function&&);
        friend int invokeReusableProtectedCall(lua_State*);
        // Returns underlying sol::state.
        sol::state_view& sol() { return mSol; }

        // A shortcut to create a new Lua table.
        sol::table newTable() { return sol::table(mSol, sol::create); }
    };

    template <typename Key, typename Value>
    sol::table tableFromPairs(lua_State* state, std::initializer_list<std::pair<Key, Value>> list)
    {
        sol::table res(state, sol::create);
        for (const auto& [k, v] : list)
            res[k] = v;
        return res;
    }

    // Pushing to the stack from outside a Lua context crashes the engine if no memory can be allocated to grow the
    // stack
    template <class Function>
    [[nodiscard]] int invokeProtectedCall(lua_State* luaState, Function&& function)
    {
        if (!lua_checkstack(luaState, 2))
            return LUA_ERRMEM;
        lua_pushcfunction(luaState, [](lua_State* state) {
            void* f = lua_touserdata(state, 1);
            LuaView view(state);
            (*static_cast<std::remove_reference_t<Function>*>(f))(view);
            return 0;
        });
        lua_pushlightuserdata(luaState, const_cast<void*>(static_cast<const void*>(&function)));
        return lua_pcall(luaState, 1, 0, 0);
    }

    inline void checkProtectedCallResult(lua_State* luaState, int result)
    {
        switch (result)
        {
            case LUA_OK:
                break;
            case LUA_ERRMEM:
                throw std::runtime_error("Lua error: out of memory");
            case LUA_ERRRUN:
            {
                sol::optional<std::string> error = sol::stack::check_get<std::string>(luaState);
                if (error)
                    throw std::runtime_error(*error);
            }
                [[fallthrough]];
            default:
                throw std::runtime_error("Lua error: " + std::to_string(result));
        }
    }

    template <class Lambda>
    void protectedCall(lua_State* luaState, Lambda&& f)
    {
        checkProtectedCallResult(luaState, invokeProtectedCall(luaState, std::forward<Lambda>(f)));
    }

    // One registry-owned zero-upvalue C closure can serve every protected job.
    // The job and its callable stay on the C++ stack until the synchronous pcall
    // returns. Nested jobs therefore have separate storage and no shared capture.
    struct ProtectedCallFrame
    {
        void* mCallable;
        void (*mInvoke)(void*, LuaView&);
    };

    inline int invokeReusableProtectedCall(lua_State* state)
    {
        auto* frame = static_cast<ProtectedCallFrame*>(lua_touserdata(state, 1));
        LuaView view(state);
        frame->mInvoke(frame->mCallable, view);
        return 0;
    }

    // Holds Lua state.
    // Provides additional features:
    //   - Load scripts from the virtual filesystem;
    //   - Caching of loaded scripts;
    //   - Disable unsafe Lua functions;
    //   - Run every instance of every script in a separate sandbox;
    //   - Forbid any interactions between sandboxes except than via provided API;
    //   - Access to common read-only resources from different sandboxes;
    //   - Replace standard `require` with a safe version that allows to search
    //         Lua libraries (only source, no dll's) in the virtual filesystem;
    //   - Make `print` to add the script name to every message and
    //         write to the Log rather than directly to stdout;
    class LuaState
    {
    public:
        explicit LuaState(const VFS::Manager* vfs, const ScriptsConfiguration* conf,
            const LuaStateSettings& settings = LuaStateSettings{});
        ~LuaState();
        LuaState(const LuaState&) = delete;
        LuaState(LuaState&&) = delete;

        template <class Lambda>
        void protectedCall(Lambda&& f) const
        {
            lua_State* state = mSol.lua_state();
            if (!mProtectedCallReuse || mProtectedCallReference == LUA_NOREF)
            {
                ++mProtectedCallStats.mOrdinaryCalls;
                ++mProtectedCallStats.mClosureCreations;
                LuaUtil::protectedCall(state, std::forward<Lambda>(f));
                return;
            }
            if (!lua_checkstack(state, 3))
                checkProtectedCallResult(state, LUA_ERRMEM);
            // lua_pushcclosure inherits its current caller environment on Lua
            // 5.1/LuaJIT. Only reuse from top-level native code or directly from
            // our trampoline. A Lua/native API callback may carry a sandbox
            // environment, so it keeps the original freshly inherited closure.
            lua_Debug caller{};
            if (lua_getstack(state, 0, &caller))
            {
                const bool identified = lua_getinfo(state, "f", &caller) != 0;
                const bool ownCaller = identified && lua_tocfunction(state, -1) == &invokeReusableProtectedCall;
                if (identified)
                    lua_pop(state, 1);
                if (!ownCaller)
                {
                    ++mProtectedCallStats.mContextFallbacks;
                    ++mProtectedCallStats.mOrdinaryCalls;
                    ++mProtectedCallStats.mClosureCreations;
                    LuaUtil::protectedCall(state, std::forward<Lambda>(f));
                    return;
                }
            }
            lua_rawgeti(state, LUA_REGISTRYINDEX, mProtectedCallReference);
            if (lua_tocfunction(state, -1) != &invokeReusableProtectedCall)
            {
                // Fail closed if native code has invalidated the private registry
                // entry. Do not invoke a substituted function or carry extra stack
                // entries into the ordinary path.
                lua_pop(state, 1);
                ++mProtectedCallStats.mRegistryFallbacks;
                ++mProtectedCallStats.mOrdinaryCalls;
                ++mProtectedCallStats.mClosureCreations;
                LuaUtil::protectedCall(state, std::forward<Lambda>(f));
                return;
            }
            using Function = std::remove_reference_t<Lambda>;
            ProtectedCallFrame frame{ const_cast<void*>(static_cast<const void*>(&f)),
                [](void* callable, LuaView& view) { (*static_cast<Function*>(callable))(view); } };
            lua_pushlightuserdata(state, &frame);
            ++mProtectedCallStats.mReusedCalls;
            checkProtectedCallResult(state, lua_pcall(state, 1, 0, 0));
        }

        // Startup/owner-thread only. The registry reference belongs to this VM
        // and is released with its registry when the Lua state is destroyed.
        void setProtectedCallReuse(bool enabled);
        bool getProtectedCallReuse() const { return mProtectedCallReuse; }
        struct ProtectedCallStats
        {
            std::uint64_t mOrdinaryCalls = 0;
            std::uint64_t mReusedCalls = 0;
            std::uint64_t mClosureCreations = 0;
            std::uint64_t mRegistryFallbacks = 0;
            std::uint64_t mContextFallbacks = 0;
        };
        ProtectedCallStats protectedCallStats() const { return mProtectedCallStats; }

        // Explicit diagnostic owner only: process-global LuaJIT sampling never
        // starts for ordinary gameplay. Callbacks only update bounded counters.
        // Counts are delivered during a scope; timer-triggered VM states may
        // predate delivery and must not be treated as exact callback blame.
        bool startDiagnosticVmSampler();
        void stopDiagnosticVmSampler();
        struct DiagnosticVmSamples
        {
            std::array<std::uint64_t, 6> mStates{}; // native, interpreted, C, GC, JIT, unknown
            std::uint64_t mCallbacks = 0;
        };
        DiagnosticVmSamples diagnosticVmSamples() const { return mDiagnosticVmSamples; }
        bool diagnosticVmSamplerStarted() const { return mVmProfileStop != nullptr; }

        // Note that constructing a sol::state_view is only safe from a Lua context. Use protectedCall to get one
        lua_State* unsafeState() const { return mSol.lua_state(); }

        // Can be used by a C++ function that is called from Lua to get the Lua traceback.
        // Makes no sense if called not from Lua code.
        // Note: It is a slow function, should be used for debug purposes only.
        std::string debugTraceback() { return mSol["debug"]["traceback"]().get<std::string>(); }

        // Registers a package that will be available from every sandbox via `require(name)`.
        // The package can be either a sol::table with an API or a sol::function. If it is a function,
        // it will be evaluated (once per sandbox) the first time when requested. If the package
        // is a table, then `makeReadOnly` is applied to it automatically (but not to other tables it contains).
        void addCommonPackage(std::string packageName, sol::object package);

        // Creates a new sandbox, runs a script, and returns the result
        // (the result is expected to be an interface of the script).
        // Args:
        //     path: path to the script in the virtual filesystem;
        //     envName: sandbox name.
        //     packages: additional packages that should be available from the sandbox via `require`. Each package
        //         should be either a sol::table or a sol::function. If it is a function, it will be evaluated
        //         (once per sandbox) with the argument 'hiddenData' the first time when requested.
        sol::protected_function_result runInNewSandbox(const VFS::Path::Normalized& path,
            const std::string& envName = "unnamed", const std::map<std::string, sol::main_object>& packages = {},
            const sol::main_object& hiddenData = sol::nil, const sol::main_table* packagePrototype = nullptr,
            ScriptId scriptId = {}, bool additionalPackagesReadOnly = false);

        void dropScriptCache() { mCompiledScripts.clear(); }

        // V3.12: compile configured top-level scripts into the existing bytecode
        // cache without creating a sandbox or executing script code.
        std::size_t precompileConfiguredScripts();

        // V3.14: populate the same bytecode cache for literal require() dependencies.
        // Mode1 scans configured scripts, mode2 recursively scans discovered modules.
        std::size_t precompileConfiguredDependencies(int mode);

        // V3.14: static packages are immutable read-only objects. A container can reuse
        // one prototype table rather than inserting the same package objects for every sandbox.
        sol::main_table makePackagePrototype(const std::map<std::string, sol::main_object>& packages);
        void setPackagePrototypeReuse(bool enabled) { mPackagePrototypeReuse = enabled; }
        bool getPackagePrototypeReuse() const { return mPackagePrototypeReuse; }
        // Registered metadata only; every factory still runs for every sandbox.
        void setPackageDescriptorReuse(bool enabled);
        bool getPackageDescriptorReuse() const { return mPackageDescriptorReuse; }
        std::uint64_t commonPackageGeneration() const { return mCommonPackageGeneration; }

        const ScriptsConfiguration& getConfiguration() const { return *mConf; }

        // Load internal Lua library. All libraries are loaded in one sandbox and shouldn't be exposed to scripts
        // directly.
        void addInternalLibSearchPath(const std::filesystem::path& path) { mLibSearchPaths.push_back(path); }
        sol::function loadInternalLib(std::string_view libName);
        sol::function loadFromVFS(const VFS::Path::Normalized& path);
        sol::environment newInternalLibEnvironment();

        uint64_t getTotalMemoryUsage() const { return mSol.memory_used(); }
        // Performs the same single explicit VM step as the engine GC worker.
        // Diagnostics observe its return value; they do not change GC policy.
        bool gcStep(int steps);
        uint64_t getSmallAllocMemoryUsage() const { return mSmallAllocMemoryUsage; }
        uint64_t getMemoryUsageByScriptIndex(unsigned id) const
        {
            return id < mMemoryUsage.size() ? mMemoryUsage[id] : 0;
        }

        const LuaStateSettings& getSettings() const { return mSettings; }

        // The tracking allocator is fixed when the Lua state is created, but
        // the count hook can be changed safely by the Lua owner thread.
        static void disableProfiler() { sProfilerEnabled = false; }
        static bool isProfilerEnabled() { return sProfilerEnabled; }
        void setInstructionProfilerEnabled(bool enabled);
        bool isInstructionProfilerEnabled() const { return mInstructionProfilerEnabled; }

        struct DiagnosticAllocations
        {
            std::uint64_t calls = 0, growthBytes = 0, shrinkBytes = 0, failures = 0;
            std::uint64_t serviceCalls = 0, serviceNs = 0, serviceFailures = 0, policyBlocked = 0, frees = 0;
            // Attempted nonzero nsize: <=64, <=256, <=1KiB, <=16KiB,
            // <=1MiB, >1MiB. Failed requests are included; frees are separate.
            std::array<std::uint64_t, 6> requestedSizes{};
        };
        // Read only by the current Lua owner. These are logical allocator
        // requests, not OS allocation sizes or per-script retained memory.
        DiagnosticAllocations diagnosticAllocations() const { return mDiagnosticAllocations; }
        bool diagnosticTrackingAllocator() const;

        static sol::protected_function_result throwIfError(sol::protected_function_result&&);

    private:
        template <typename... Args>
        friend sol::protected_function_result call(const sol::protected_function& fn, Args&&... args);
        template <typename... Args>
        friend sol::protected_function_result call(
            ScriptId scriptId, const sol::protected_function& fn, Args&&... args);

        sol::function loadScriptAndCache(const VFS::Path::Normalized& path);
        static void countHook(lua_State* state, lua_Debug* ar);
        static void* trackingAllocator(void* ud, void* ptr, size_t osize, size_t nsize);
        static void vmProfileCallback(void* data, lua_State*, int samples, int state);

        static LuaStatePtr createLuaRuntime(LuaState* luaState);

        // Bounded summaries of an enclosing operation. Their wall envelopes
        // are not allocator service time; only service_ns times free/realloc.
        // No allocation emits a trace row or queries an OS thread CPU timer.
        class DiagnosticAllocationScope
        {
        public:
            explicit DiagnosticAllocationScope(LuaState* state, ScriptId scriptId = {});
            ~DiagnosticAllocationScope();

        private:
            LuaState* mState;
            DiagnosticAllocations mBefore;
            DiagnosticVmSamples mVmBefore;
            std::optional<Debug::HitchTrace::Scope> mService, mSmall, mLarge;
            std::optional<Debug::HitchTrace::Scope> mVmStates, mVmExtra;
        };
        std::pair<std::string_view, std::string_view> diagnosticAllocationContext(ScriptId scriptId) const;

        struct AllocOwner
        {
            std::shared_ptr<ScriptsContainer*> mContainer;
            int mScriptIndex;
        };

        const LuaStateSettings mSettings;
        DiagnosticAllocations mDiagnosticAllocations;
        bool mProtectedCallReuse = false;
        int mProtectedCallReference = LUA_NOREF;
        mutable ProtectedCallStats mProtectedCallStats;
        DiagnosticVmSamples mDiagnosticVmSamples;
        void (*mVmProfileStop)(lua_State*) = nullptr;

        // Needed to track resource usage per script, must be initialized before mLuaHolder.
        std::vector<ScriptId> mActiveScriptIdStack;
        uint64_t mWatchdogInstructionCounter = 0;
        bool mInstructionProfilerEnabled = false;
        std::map<void*, AllocOwner> mBigAllocOwners;
        uint64_t mTotalMemoryUsage = 0;
        uint64_t mSmallAllocMemoryUsage = 0;
        std::vector<int64_t> mMemoryUsage;

        // Must be declared before mSol and all sol-related objects. Then on exit it will be destructed the last.
        LuaStatePtr mLuaState;

        sol::state_view mSol;
        const ScriptsConfiguration* mConf;
        sol::table mSandboxEnv;
        std::map<VFS::Path::Normalized, sol::bytecode> mCompiledScripts;
        std::map<std::string, sol::object> mCommonPackages;
        // Keys and values refer to stable nodes owned by mCommonPackages. Keys
        // are never erased during a LuaState lifetime; registration invalidates
        // classification and advances generation. No actor factory output lives
        // here and no descriptor outlives this LuaState.
        std::map<std::string_view, const sol::object*, std::less<>> mCommonFactoryDescriptors;
        std::uint64_t mCommonPackageGeneration = 0;
        bool mPackageDescriptorReuse = false;
        const VFS::Manager* mVFS;
        bool mPackagePrototypeReuse = false;
        std::vector<std::filesystem::path> mLibSearchPaths;

        static bool sProfilerEnabled;
    };

    // LuaUtil::call should be used for every call of every Lua function.
    // 1) It is a workaround for a bug in `sol`. See https://github.com/ThePhD/sol2/issues/1078
    // 2) When called with ScriptId it tracks resource usage (scriptId refers to the script that is responsible for this
    // call).

    template <typename... Args>
    sol::protected_function_result call(const sol::protected_function& fn, Args&&... args)
    {
        try
        {
            auto res = LuaState::throwIfError(fn(std::forward<Args>(args)...));
            return res;
        }
        catch (std::exception&)
        {
            throw;
        }
        catch (...)
        {
            throw std::runtime_error("Unknown error");
        }
    }

    // Lua must be initialized through LuaUtil::LuaState, otherwise this function will segfault.
    template <typename... Args>
    sol::protected_function_result call(ScriptId scriptId, const sol::protected_function& fn, Args&&... args)
    {
        Debug::HitchTrace::Scope diagnosticCall("lua.call", Debug::HitchTrace::CpuTiming::None);
        LuaState* luaState = nullptr;
        if (LuaState::sProfilerEnabled && (scriptId.mContainer || Debug::HitchTrace::requested()))
        {
            (void)lua_getallocf(fn.lua_state(), reinterpret_cast<void**>(&luaState));
            if (scriptId.mContainer)
            {
                luaState->mActiveScriptIdStack.push_back(scriptId);
                luaState->mWatchdogInstructionCounter = 0;
            }
        }
        const bool diagnostic = Debug::HitchTrace::requested() && luaState;
        LuaState::DiagnosticAllocationScope allocationDiagnostic(diagnostic ? luaState : nullptr, scriptId);
        const auto before = diagnostic ? luaState->diagnosticAllocations() : LuaState::DiagnosticAllocations{};
        auto finishDiagnostic = [&] {
            if (!diagnostic) return;
            const auto after = luaState->diagnosticAllocations();
            diagnosticCall.setMetric(0, "alloc_calls", after.calls - before.calls);
            diagnosticCall.setMetric(1, "alloc_growth_bytes", after.growthBytes - before.growthBytes);
            diagnosticCall.setMetric(2, "alloc_shrink_bytes", after.shrinkBytes - before.shrinkBytes);
            diagnosticCall.setMetric(3, "alloc_failures", after.failures - before.failures);
        };
        try
        {
            auto res = LuaState::throwIfError(fn(std::forward<Args>(args)...));
            finishDiagnostic();
            if (luaState && scriptId.mContainer)
                luaState->mActiveScriptIdStack.pop_back();
            return res;
        }
        catch (std::exception&)
        {
            finishDiagnostic();
            if (luaState && scriptId.mContainer)
                luaState->mActiveScriptIdStack.pop_back();
            throw;
        }
        catch (...)
        {
            finishDiagnostic();
            if (luaState && scriptId.mContainer)
                luaState->mActiveScriptIdStack.pop_back();
            throw std::runtime_error("Unknown error");
        }
    }

    // work around for a (likely) sol3 bug
    // when the index meta method throws, simply calling table.get crashes instead of re-throwing the error
    template <class Key>
    sol::object safeGet(const sol::lua_table& table, const Key& key)
    {
        auto index = table.traverse_raw_get<sol::optional<sol::main_protected_function>>(
            sol::metatable_key, sol::meta_function::index);
        if (index)
        {
            sol::protected_function_result result = index.value()(table, key);
            if (result.valid())
                return result.get<sol::object>();
            else
                throw result.get<sol::error>();
        }
        else
            return table.raw_get<sol::object>(key);
    }

    // getFieldOrNil(table, "a", "b", "c") returns table["a"]["b"]["c"] or nil if some of the fields doesn't exist.
    template <class... Str>
    sol::object getFieldOrNil(const sol::object& table, std::string_view first, const Str&... str)
    {
        if (!table.is<sol::lua_table>())
            return sol::nil;
        sol::object value = safeGet(table.as<sol::lua_table>(), first);
        if constexpr (sizeof...(str) == 0)
            return value;
        else
            return getFieldOrNil(value, str...);
    }

    template <class... Str>
    void setDeepField(sol::table& table, const sol::object& value, std::string_view first, const Str&... str)
    {
        if constexpr (sizeof...(str) == 0)
            table[first] = value;
        else
        {
            if (table[first] == sol::nil)
                table[first] = sol::table(table.lua_state(), sol::create);
            sol::table nextTable = table[first];
            setDeepField(nextTable, value, str...);
        }
    }

    // String representation of a Lua object. Should be used for debugging/logging purposes only.
    std::string toString(const sol::object&);

    namespace Internal
    {
        std::string formatCastingError(const sol::object& obj, const std::type_info&);
    }

    template <class T>
    decltype(auto) cast(const sol::object& obj)
    {
        if (!obj.is<T>())
            throw std::runtime_error(Internal::formatCastingError(obj, typeid(T)));
        return obj.as<T>();
    }

    template <class T>
    T getValueOrDefault(const sol::object& obj, const T& defaultValue)
    {
        if (obj == sol::nil)
            return defaultValue;
        return cast<T>(obj);
    }

    // Makes a table read only (when accessed from Lua) by wrapping it with an empty userdata.
    // Needed to forbid any changes in common resources that can be accessed from different sandboxes.
    // `strictIndex = true` replaces default `__index` with a strict version that throws an error if key is not found.
    sol::table makeReadOnly(const sol::table&, bool strictIndex = false);
    inline sol::table makeStrictReadOnly(const sol::table& tbl)
    {
        return makeReadOnly(tbl, true);
    }
    sol::table getMutableFromReadOnly(const sol::userdata&);

    template <class T>
    void copyVectorToTable(const std::vector<T>& v, sol::table& out)
    {
        for (const T& t : v)
            out.add(t);
    }
}

#endif // COMPONENTS_LUA_LUASTATE_H
