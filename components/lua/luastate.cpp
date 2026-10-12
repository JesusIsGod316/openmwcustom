#include "luastate.hpp"

#ifndef NO_LUAJIT
#include <luajit.h>
#endif // NO_LUAJIT

#include <filesystem>
#include <fstream>
#include <charconv>
#include <regex>
#include <set>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>

#if defined(_WIN32) && !defined(NO_LUAJIT)
#include <windows.h>
#endif

#include <components/debug/debuglog.hpp>
#include <components/debug/diagnosticobserver.hpp>
#include <components/debug/v36luaaddscripttrace.hpp>
#include <components/files/conversion.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

#include "luastateptr.hpp"
#include "scriptscontainer.hpp"
#include "utf8.hpp"

#if SOL_IS_ON(SOL_PROPAGATE_EXCEPTIONS)
#error SOL_PROPAGATE_EXCEPTIONS inhibits error checking
#endif

namespace LuaUtil
{
    namespace
    {
        std::atomic<LuaState*> sDiagnosticVmSamplerOwner{ nullptr };
    }

    void LuaState::vmProfileCallback(void* data, lua_State*, int samples, int state)
    {
        // LuaJIT calls this on the Lua owner at a profile dispatch hook, not on
        // its timer thread. No stack access, allocation, logging or I/O here.
        auto* owner = static_cast<LuaState*>(data);
        unsigned index = 5;
        switch (state)
        {
            case 'N': index = 0; break;
            case 'I': index = 1; break;
            case 'C': index = 2; break;
            case 'G': index = 3; break;
            case 'J': index = 4; break;
        }
        if (samples > 0)
            owner->mDiagnosticVmSamples.mStates[index] += static_cast<std::uint64_t>(samples);
        ++owner->mDiagnosticVmSamples.mCallbacks;
    }

    bool LuaState::startDiagnosticVmSampler()
    {
        if (mVmProfileStop)
            return true;
        if (!Debug::HitchTrace::requested())
            return false;
        const char* requested = std::getenv("OPENMW_LUA_VM_STATE_SAMPLER");
        if (!requested || std::string_view(requested) != "1")
            return false;
        LuaState* expected = nullptr;
        if (!sDiagnosticVmSamplerOwner.compare_exchange_strong(expected, this))
        {
            Log(Debug::Info) << "OptimizedMW Lua VM sampling: refused_multiple_engine_owners";
            return false;
        }
#if defined(_WIN32) && !defined(NO_LUAJIT)
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<const char*>(&lua_pcall), &module);
        using Start = void (*)(lua_State*, const char*, luaJIT_profile_callback, void*);
        using Stop = void (*)(lua_State*);
        const auto start = module ? reinterpret_cast<Start>(GetProcAddress(module, "luaJIT_profile_start")) : nullptr;
        const auto stop = module ? reinterpret_cast<Stop>(GetProcAddress(module, "luaJIT_profile_stop")) : nullptr;
        if (start && stop)
        {
            // No f/l mode: do not flush JIT traces at start. Stop unavoidably
            // flushes this diagnostic VM's traces in the pinned LuaJIT source.
            // There is no public acceptance query; observed callbacks, not the
            // void return, qualify that the requested sampler actually ran.
            start(mSol.lua_state(), "i1", &vmProfileCallback, this);
            mVmProfileStop = stop;
            Log(Debug::Info) << "OptimizedMW Lua VM sampling: requested=1 api_available=1 interval_ms=1"
                << " acceptance_requires_callbacks=1 delivery_is_not_sample_time=1 stop_flushes_jit=1";
            return true;
        }
#endif
        sDiagnosticVmSamplerOwner.store(nullptr);
        Log(Debug::Info) << "OptimizedMW Lua VM sampling: requested=1 api_available=0 qualified=0";
        return false;
    }

    void LuaState::stopDiagnosticVmSampler()
    {
        if (!mVmProfileStop)
            return;
        mVmProfileStop(mSol.lua_state());
        mVmProfileStop = nullptr;
        sDiagnosticVmSamplerOwner.store(nullptr);
        const auto& s = mDiagnosticVmSamples;
        Log(Debug::Info) << "OptimizedMW Lua VM sampling: final_callbacks=" << s.mCallbacks
            << " native=" << s.mStates[0] << " interpreted=" << s.mStates[1] << " c=" << s.mStates[2]
            << " gc=" << s.mStates[3] << " jit=" << s.mStates[4] << " unknown=" << s.mStates[5]
            << " delivery_is_not_sample_time=1 stop_flushes_jit=1";
    }

    LuaState::~LuaState()
    {
        stopDiagnosticVmSampler();
    }

    static VFS::Path::Normalized packageNameToVfsPath(std::string_view packageName, const VFS::Manager& vfs)
    {
        std::string pathValue(packageName);
        std::replace(pathValue.begin(), pathValue.end(), '.', '/');
        VFS::Path::Normalized pathWithInit(pathValue + "/init.lua");
        pathValue.append(".lua");
        VFS::Path::Normalized path(pathValue);
        if (vfs.exists(path))
            return path;
        else if (vfs.exists(pathWithInit))
            return pathWithInit;
        else
            throw std::runtime_error("module not found: " + std::string(packageName));
    }

    static std::filesystem::path packageNameToPath(
        std::string_view packageName, const std::vector<std::filesystem::path>& searchDirs)
    {
        std::string path(packageName);
        std::replace(path.begin(), path.end(), '.', '/');
        std::string pathWithInit = path + "/init.lua";
        path.append(".lua");
        for (const auto& base : searchDirs)
        {
            std::filesystem::path p1 = base / path;
            if (std::filesystem::exists(p1))
                return p1;
            std::filesystem::path p2 = base / pathWithInit;
            if (std::filesystem::exists(p2))
                return p2;
        }
        throw std::runtime_error("module not found: " + std::string(packageName));
    }

    static const std::string safeFunctions[] = { "assert", "error", "ipairs", "next", "pairs", "pcall", "select",
        "tonumber", "tostring", "type", "unpack", "xpcall", "rawequal", "rawget", "rawset", "setmetatable" };
    static const std::string safePackages[] = { "coroutine", "math", "string", "table", "utf8" };

    static constexpr int64_t countHookStep = 1000;

    static bool isExplicitVulkanBackend()
    {
        return Settings::video().mRendererBackend.get() == "vulkan";
    }

    bool LuaState::sProfilerEnabled = true;

    void LuaState::countHook(lua_State* state, lua_Debug* /*ar*/)
    {
        LuaState* self;
        (void)lua_getallocf(state, reinterpret_cast<void**>(&self));
        if (self->mActiveScriptIdStack.empty())
            return;
        const ScriptId& activeScript = self->mActiveScriptIdStack.back();
        activeScript.mContainer->addInstructionCount(activeScript.mIndex, countHookStep);
        self->mWatchdogInstructionCounter += countHookStep;
        if (self->mSettings.mInstructionLimit > 0
            && self->mWatchdogInstructionCounter > self->mSettings.mInstructionLimit)
        {
            lua_pushstring(state,
                "Lua instruction count exceeded, probably an infinite loop in a script. "
                "To change the limit set \"[Lua] instruction limit per call\" in settings.cfg");
            lua_error(state);
        }
    }

    void LuaState::setInstructionProfilerEnabled(bool enabled)
    {
        if (!sProfilerEnabled)
            enabled = false;
        if (enabled == mInstructionProfilerEnabled)
            return;
        lua_sethook(mLuaState.get(), enabled ? &countHook : nullptr, enabled ? LUA_MASKCOUNT : 0,
            enabled ? countHookStep : 0);
        mInstructionProfilerEnabled = enabled;
    }

    void* LuaState::trackingAllocator(void* ud, void* ptr, size_t osize, size_t nsize)
    {
        LuaState* self = static_cast<LuaState*>(ud);
        const bool diagnostic = Debug::HitchTrace::requested();
        if (diagnostic)
        {
            Debug::DiagnosticObserver::Scope observer(Debug::DiagnosticObserver::Category::LuaAllocatorBookkeeping);
            auto& stats = self->mDiagnosticAllocations;
            ++stats.calls;
            if (!nsize)
                ++stats.frees;
            else
            {
                constexpr std::array<std::size_t, 5> limits{ 64, 256, 1024, 16384, 1048576 };
                const auto bucket = std::lower_bound(limits.begin(), limits.end(), nsize) - limits.begin();
                ++stats.requestedSizes[bucket];
            }
        }
        const uint64_t smallAllocSize = self->mSettings.mSmallAllocMaxSize;
        const uint64_t memoryLimit = self->mSettings.mMemoryLimit;

        if (!ptr)
            osize = 0;
        const std::uint64_t positiveGrowth = nsize > osize ? nsize - osize : 0;
        if (self->mPreparedAllocationActive && self->mPreparedAllocationMayDeny
            && positiveGrowth > self->mPreparedAllocationLimit
                - (std::min)(self->mPreparedAllocationGrowth, self->mPreparedAllocationLimit))
        {
            self->mPreparedAllocationDenied = true;
            return nullptr;
        }
        int64_t smallAllocDelta = 0, bigAllocDelta = 0;
        if (osize <= smallAllocSize)
            smallAllocDelta -= osize;
        else
            bigAllocDelta -= osize;
        if (nsize <= smallAllocSize)
            smallAllocDelta += nsize;
        else
            bigAllocDelta += nsize;

        if (bigAllocDelta > 0 && memoryLimit > 0 && self->mTotalMemoryUsage + nsize - osize > memoryLimit)
        {
            if (diagnostic)
            {
                Debug::DiagnosticObserver::Scope observer(Debug::DiagnosticObserver::Category::LuaAllocatorBookkeeping);
                ++self->mDiagnosticAllocations.failures;
                ++self->mDiagnosticAllocations.policyBlocked;
            }
            Log(Debug::Error) << "Lua realloc " << osize << "->" << nsize
                              << " is blocked because Lua memory limit (configurable in settings.cfg) is exceeded";
            return nullptr;
        }

        void* newPtr = nullptr;
        const auto serviceStart = diagnostic ? Debug::HitchTrace::Clock::now() : Debug::HitchTrace::Clock::time_point{};
        if (nsize == 0)
            free(ptr);
        else
            newPtr = realloc(ptr, nsize);
        if (diagnostic)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                Debug::HitchTrace::Clock::now() - serviceStart).count();
            // Count the existing pair of timer boundaries without recursively
            // timing clocks or charging free/realloc service to the observer.
            Debug::DiagnosticObserver::count(Debug::DiagnosticObserver::Category::LuaAllocatorTimerBoundary);
            Debug::DiagnosticObserver::count(Debug::DiagnosticObserver::Category::LuaAllocatorTimerBoundary);
            Debug::DiagnosticObserver::Scope observer(Debug::DiagnosticObserver::Category::LuaAllocatorBookkeeping);
            ++self->mDiagnosticAllocations.serviceCalls;
            self->mDiagnosticAllocations.serviceNs += static_cast<std::uint64_t>((std::max)(elapsed, std::int64_t{ 0 }));
        }
        if (nsize != 0 && !newPtr)
        {
            if (diagnostic)
            {
                Debug::DiagnosticObserver::Scope observer(Debug::DiagnosticObserver::Category::LuaAllocatorBookkeeping);
                ++self->mDiagnosticAllocations.failures;
                ++self->mDiagnosticAllocations.serviceFailures;
            }
            Log(Debug::Error) << "Lua realloc " << osize << "->" << nsize << " failed";
            return nullptr;
        }
        self->mTotalMemoryUsage += smallAllocDelta + bigAllocDelta;
        if (self->mPreparedAllocationActive)
        {
            // This scope includes successful temporary growth even if GC frees
            // unrelated objects. It is independent of Deep diagnostics/clocks.
            self->mPreparedAllocationGrowth = positiveGrowth > std::numeric_limits<std::uint64_t>::max()
                    - self->mPreparedAllocationGrowth
                ? std::numeric_limits<std::uint64_t>::max()
                : self->mPreparedAllocationGrowth + positiveGrowth;
        }
        if (diagnostic)
        {
            Debug::DiagnosticObserver::Scope observer(Debug::DiagnosticObserver::Category::LuaAllocatorBookkeeping);
            if (nsize >= osize) self->mDiagnosticAllocations.growthBytes += nsize - osize;
            else self->mDiagnosticAllocations.shrinkBytes += osize - nsize;
        }
        self->mSmallAllocMemoryUsage += smallAllocDelta;

        if (bigAllocDelta != 0)
        {
            auto it = osize > smallAllocSize ? self->mBigAllocOwners.find(ptr) : self->mBigAllocOwners.end();
            ScriptId id;
            if (it != self->mBigAllocOwners.end())
            {
                if (it->second.mContainer)
                    id = ScriptId{ *it->second.mContainer, it->second.mScriptIndex };
                if (ptr != newPtr || nsize <= smallAllocSize)
                    self->mBigAllocOwners.erase(it);
            }
            else if (bigAllocDelta > 0)
            {
                if (!self->mActiveScriptIdStack.empty())
                    id = self->mActiveScriptIdStack.back();
                bigAllocDelta = nsize;
            }
            if (id.mIndex >= 0)
            {
                if (static_cast<size_t>(id.mIndex) >= self->mMemoryUsage.size())
                    self->mMemoryUsage.resize(id.mIndex + 1);
                self->mMemoryUsage[id.mIndex] += bigAllocDelta;
            }
            if (id.mContainer)
            {
                id.mContainer->addMemoryUsage(id.mIndex, bigAllocDelta);
                if (newPtr && nsize > smallAllocSize)
                    self->mBigAllocOwners.emplace(newPtr, AllocOwner{ id.mContainer->mThis, id.mIndex });
            }
        }

        return newPtr;
    }

    bool LuaState::diagnosticTrackingAllocator() const
    {
        return lua_getallocf(mLuaState.get(), nullptr) == &trackingAllocator;
    }

    std::pair<std::string_view, std::string_view> LuaState::diagnosticAllocationContext(ScriptId scriptId) const
    {
        std::string_view path;
        if (mConf && scriptId.mIndex >= 0 && static_cast<std::size_t>(scriptId.mIndex) < mConf->size())
            path = (*mConf)[scriptId.mIndex].mScriptPath.value();
        return { scriptId.mContainer ? std::string_view(scriptId.mContainer->mNamePrefix) : "unassigned", path };
    }

    LuaState::DiagnosticAllocationScope::DiagnosticAllocationScope(LuaState* state, ScriptId scriptId)
        : mState(Debug::HitchTrace::requested() ? state : nullptr)
    {
        if (!mState)
            return;
        mBefore = mState->diagnosticAllocations();
        const auto [container, path] = mState->diagnosticAllocationContext(scriptId);
        mService.emplace("lua.alloc.service", Debug::HitchTrace::CpuTiming::WallEnvelope, container, path);
        mSmall.emplace("lua.alloc.request_small", Debug::HitchTrace::CpuTiming::WallEnvelope, container, path);
        mLarge.emplace("lua.alloc.request_large", Debug::HitchTrace::CpuTiming::WallEnvelope, container, path);
        if (mState->diagnosticVmSamplerStarted())
        {
            mVmBefore = mState->diagnosticVmSamples();
            mVmStates.emplace("lua.vm.delivered_states", Debug::HitchTrace::CpuTiming::WallEnvelope, container, path);
            mVmExtra.emplace("lua.vm.delivered_coverage", Debug::HitchTrace::CpuTiming::WallEnvelope, container, path);
        }
    }

    LuaState::DiagnosticAllocationScope::~DiagnosticAllocationScope()
    {
        if (!mState)
            return;
        const auto after = mState->diagnosticAllocations();
        mService->setMetric(0, "service_ns", after.serviceNs - mBefore.serviceNs);
        mService->setMetric(1, "service_calls", after.serviceCalls - mBefore.serviceCalls);
        mService->setMetric(2, "service_failures", after.serviceFailures - mBefore.serviceFailures);
        mService->setMetric(3, "policy_blocked", after.policyBlocked - mBefore.policyBlocked);
        constexpr std::array<std::string_view, 4> names{ "req_le64", "req_65_256", "req_257_1024", "req_1025_16384" };
        for (std::size_t i = 0; i < names.size(); ++i)
            mSmall->setMetric(i, names[i], after.requestedSizes[i] - mBefore.requestedSizes[i]);
        mLarge->setMetric(0, "req_16385_1048576", after.requestedSizes[4] - mBefore.requestedSizes[4]);
        mLarge->setMetric(1, "req_gt1048576", after.requestedSizes[5] - mBefore.requestedSizes[5]);
        mLarge->setMetric(2, "free_requests", after.frees - mBefore.frees);
        mLarge->setMetric(3, "tracking_available", mState->diagnosticTrackingAllocator());
        if (mVmStates)
        {
            const auto vm = mState->diagnosticVmSamples();
            mVmStates->setMetric(0, "sample_native", vm.mStates[0] - mVmBefore.mStates[0]);
            mVmStates->setMetric(1, "sample_interpreted", vm.mStates[1] - mVmBefore.mStates[1]);
            mVmStates->setMetric(2, "sample_c", vm.mStates[2] - mVmBefore.mStates[2]);
            mVmStates->setMetric(3, "sample_gc", vm.mStates[3] - mVmBefore.mStates[3]);
            mVmExtra->setMetric(0, "sample_jit", vm.mStates[4] - mVmBefore.mStates[4]);
            mVmExtra->setMetric(1, "sample_unknown", vm.mStates[5] - mVmBefore.mStates[5]);
            mVmExtra->setMetric(2, "delivered_callbacks", vm.mCallbacks - mVmBefore.mCallbacks);
            mVmExtra->setMetric(3, "delivery_not_sample_time", 1);
        }
    }

    bool LuaState::gcStep(int steps)
    {
        const bool diagnostic = Debug::HitchTrace::requested();
        const auto heapBefore = diagnostic ? getTotalMemoryUsage() : 0;
        std::array<char, 16> argument{};
        std::string_view requestedSteps;
        if (diagnostic)
        {
            const auto end = std::to_chars(argument.data(), argument.data() + argument.size(), steps);
            requestedSteps = std::string_view(argument.data(), end.ptr - argument.data());
        }
        DiagnosticAllocationScope allocationDiagnostic(diagnostic ? this : nullptr);
        Debug::HitchTrace::Scope detail("lua.gc.step_metrics", Debug::HitchTrace::CpuTiming::WallEnvelope,
            "explicit_step", requestedSteps);
        const bool completed = lua_gc(mSol.lua_state(), LUA_GCSTEP, steps) == 1;
        if (diagnostic)
        {
            detail.setMetric(0, "requested_steps", static_cast<std::uint64_t>((std::max)(steps, 0)));
            detail.setMetric(1, "cycle_completed", completed);
            detail.setMetric(2, "heap_before_bytes", heapBefore);
            detail.setMetric(3, "heap_after_bytes", getTotalMemoryUsage());
        }
        return completed;
    }

    void LuaState::setProtectedCallReuse(bool enabled)
    {
        if (enabled && mProtectedCallReference == LUA_NOREF)
        {
            // Create the reusable closure inside an ordinary protected boundary,
            // so registry growth retains the existing out-of-memory behavior.
            LuaUtil::protectedCall(mSol.lua_state(), [&](LuaView& view) {
                lua_State* state = view.sol().lua_state();
                lua_pushcfunction(state, &invokeReusableProtectedCall);
                mProtectedCallReference = luaL_ref(state, LUA_REGISTRYINDEX);
            });
            ++mProtectedCallStats.mClosureCreations;
        }
        mProtectedCallReuse = enabled;
    }

    LuaStatePtr LuaState::createLuaRuntime(LuaState* luaState)
    {
        if (isExplicitVulkanBackend() && !luaState->mSettings.mInstructionProfilerEnabled && sProfilerEnabled)
        {
            // V4 Vulkan correctness quarantine: the V3.20 recorder kept the tracking allocator enabled even when
            // ordinary Lua profiling was off. Run the native Lua allocator on the Vulkan path unless the user
            // explicitly requested the stock Lua profiler. The recorder is diagnostic-only and can be revisited
            // after Vulkan runtime acceptance.
            sProfilerEnabled = false;
            Log(Debug::Info) << "V4 Vulkan: disabling legacy V3 Lua tracking allocator";
        }

        if (sProfilerEnabled)
        {
            Log(Debug::Info) << "Initializing LuaUtil::LuaState with profiler";
            LuaStatePtr state(lua_newstate(&trackingAllocator, luaState));
            if (state != nullptr)
                return state;
            sProfilerEnabled = false;
            Log(Debug::Error) << "Failed to initialize LuaUtil::LuaState with custom allocator; disabling Lua profiler";
        }
        Log(Debug::Info) << "Initializing LuaUtil::LuaState without profiler";
        LuaStatePtr state(luaL_newstate());
        if (state == nullptr)
            throw std::runtime_error("Failed to create Lua runtime");
        return state;
    }

    LuaState::LuaState(const VFS::Manager* vfs, const ScriptsConfiguration* conf, const LuaStateSettings& settings)
        : mSettings(settings)
        , mLuaState([&] {
            LuaStatePtr state = createLuaRuntime(this);
            sol::set_default_state(state.get());
            return state;
        }())
        , mSol(mLuaState.get())
        , mConf(conf)
        , mVFS(vfs)
    {
        if (sProfilerEnabled && mSettings.mInstructionProfilerEnabled)
            setInstructionProfilerEnabled(true);

        if (Debug::HitchTrace::requested())
        {
            // The pinned LuaJIT sampling API has no public profiler ownership
            // query and stopping it flushes JIT traces. Preserve the existing
            // VM/profiler policy and make the automatic-GC coverage gap explicit.
#ifndef NO_LUAJIT
            constexpr std::string_view runtime = LUAJIT_VERSION;
#else
            constexpr std::string_view runtime = LUA_VERSION;
#endif
            Debug::HitchTrace::Scope observation("lua.gc.observation", Debug::HitchTrace::CpuTiming::WallEnvelope,
                "unavailable_without_vm_sampler", runtime);
            observation.setMetric(0, "automatic_gc_observed", 0);
            observation.setMetric(1, "tracking_available", diagnosticTrackingAllocator());
            observation.setMetric(2, "instruction_profiler", mInstructionProfilerEnabled);
            observation.setMetric(3, "vm_sampler_enabled", 0);
            observation.retain(); // One coverage record per VM, never per allocation/callback.
        }

        protectedCall([&](LuaView& view) {
            auto& sol = view.sol();
            sol.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::math, sol::lib::bit32, sol::lib::string,
                sol::lib::table, sol::lib::os, sol::lib::debug);

#ifndef NO_LUAJIT
            sol.open_libraries(sol::lib::jit);
#endif // NO_LUAJIT

            sol["math"]["randomseed"](static_cast<unsigned>(std::time(nullptr)));
            sol["math"]["randomseed"] = [] {};

            sol["utf8"] = LuaUtf8::initUtf8Package(sol);

            sol["writeToLog"] = [](std::string_view s) { Log(Debug::Level::Info) << s; };

            sol["setEnvironment"]
                = [](const sol::environment& env, const sol::function& fn) { sol::set_environment(env, fn); };
            sol["loadFromVFS"] = [this](std::string_view packageName) {
                Debug::V36LuaAddScriptTrace::PhaseScope v36ModuleLoad(
                    Debug::V36LuaAddScriptTrace::Phase::ModuleLoad);
                return loadScriptAndCache(packageNameToVfsPath(packageName, *mVFS));
            };
            sol["loadInternalLib"] = [this](std::string_view packageName) { return loadInternalLib(packageName); };

            // Some fixes for compatibility between different Lua versions
            if (sol["unpack"] == sol::nil)
                sol["unpack"] = sol["table"]["unpack"];
            else if (sol["table"]["unpack"] == sol::nil)
                sol["table"]["unpack"] = sol["unpack"];
            if (LUA_VERSION_NUM <= 501)
            {
                sol.script(R"(
                    local _pairs = pairs
                    local _ipairs = ipairs
                    -- script environments, sandboxed strings and ui.content set __metatable, so
                    -- getmetatable returns false here rather than nil or a table
                    pairs = function(v)
                        local meta = getmetatable(v)
                        local override = meta and rawget(meta, '__pairs')
                        return (override or _pairs)(v)
                    end
                    ipairs = function(v)
                        local meta = getmetatable(v)
                        local override = meta and rawget(meta, '__ipairs')
                        return (override or _ipairs)(v)
                    end
                )");
            }

            sol.script(R"(
                local printToLog = function(...)
                    local strs = {}
                    for i = 1, select('#', ...) do
                        strs[i] = tostring(select(i, ...))
                    end
                    return writeToLog(table.concat(strs, '\t'))
                end
                printGen = function(name) return function(...) return printToLog(name, ...) end end

                function requireGen(env, loaded, loadFn, hiddenData)
                    return function(packageName)
                        if packageName == 'openmw.animation' and hiddenData ~= nil then
                            hiddenData.openmw_v321_animation_consumer = true
                        end
                        local p = loaded[packageName]
                        if p == nil then
                            local loader = loadFn(packageName)
                            setEnvironment(env, loader)
                            p = loader(packageName)
                            loaded[packageName] = p
                        end
                        return p
                    end
                end

                function createStrictIndexFn(tbl)
                    return function(_, key)
                        local res = tbl[key]
                        if res ~= nil then
                            return res
                        else
                            error('Key not found: '..tostring(key), 2)
                        end
                    end
                end
                function pairsForReadOnly(v)
                    local nextFn, t, firstKey = pairs(getmetatable(v).t)
                    return function(_, k) return nextFn(t, k) end, v, firstKey
                end
                function ipairsForReadOnly(v)
                    local nextFn, t, firstKey = ipairs(getmetatable(v).t)
                    return function(_, k) return nextFn(t, k) end, v, firstKey
                end
                function lenForReadOnly(v)
                    return #getmetatable(v).t
                end
                local function nextForArray(array, index)
                    index = (index or 0) + 1
                    if index <= #array then
                        return index, array[index]
                    end
                end
                function ipairsForArray(array)
                    return nextForArray, array, 0
                end

                getmetatable('').__metatable = false
                getSafeMetatable = function(v)
                    if type(v) ~= 'table' then error('getmetatable is allowed only for tables', 2) end
                    return getmetatable(v)
                end
            )");

            mSandboxEnv = sol::table(sol, sol::create);
            mSandboxEnv["_VERSION"] = sol["_VERSION"];
            for (const std::string& s : safeFunctions)
            {
                if (sol[s] == sol::nil)
                    throw std::logic_error("Lua function not found: " + s);
                mSandboxEnv[s] = sol[s];
            }
            for (const std::string& s : safePackages)
            {
                if (sol[s] == sol::nil)
                    throw std::logic_error("Lua package not found: " + s);
                mCommonPackages[s] = mSandboxEnv[s] = makeReadOnly(sol[s]);
            }
            mSandboxEnv["getmetatable"] = sol["getSafeMetatable"];
            mCommonPackages["os"] = mSandboxEnv["os"]
                = makeReadOnly(tableFromPairs<std::string_view, sol::function>(sol,
                    { { "date", sol["os"]["date"] }, { "difftime", sol["os"]["difftime"] },
                        { "time", sol["os"]["time"] } }));
        });
        const char* descriptors = std::getenv("OPENMW_HITCH_LUA_PACKAGE_DESCRIPTORS");
        if (descriptors && std::string_view(descriptors) == "1")
            setPackageDescriptorReuse(true);
    }

    sol::table makeReadOnly(const sol::table& table, bool strictIndex)
    {
        if (table == sol::nil)
            return table;
        if (table.is<sol::userdata>())
            return table; // it is already userdata, no sense to wrap it again

        lua_State* luaState = table.lua_state();
        sol::state_view lua(luaState);
        sol::table meta(lua, sol::create);
        meta["t"] = table;
        if (strictIndex)
            meta[sol::meta_method::index] = lua["createStrictIndexFn"](table);
        else
            meta[sol::meta_method::index] = table;
        meta[sol::meta_method::pairs] = lua["pairsForReadOnly"];
        meta[sol::meta_method::ipairs] = lua["ipairsForReadOnly"];
        meta[sol::meta_method::length] = lua["lenForReadOnly"];
        meta[sol::meta_method::type] = "ReadOnlyTable";

        lua_newuserdata(luaState, 0);
        sol::stack::push(luaState, meta);
        lua_setmetatable(luaState, -2);
        return sol::stack::pop<sol::table>(luaState);
    }

    sol::table getMutableFromReadOnly(const sol::userdata& ro)
    {
        return ro[sol::metatable_key].get<sol::table>()["t"];
    }

    void LuaState::addCommonPackage(std::string packageName, sol::object package)
    {
        const bool factory = package.is<sol::function>();
        if (!factory)
            package = makeReadOnly(std::move(package));
        const auto it = mCommonPackages.insert_or_assign(std::move(packageName), std::move(package)).first;
        ++mCommonPackageGeneration;
        if (mPackageDescriptorReuse)
        {
            try
            {
                if (factory) mCommonFactoryDescriptors.insert_or_assign(it->first, &it->second);
                else mCommonFactoryDescriptors.erase(it->first);
            }
            catch (const std::bad_alloc&)
            {
                // Registration succeeded; metadata allocation must not make a
                // registered package unavailable or bypass the original path.
                mCommonFactoryDescriptors.clear();
                mPackageDescriptorReuse = false;
            }
        }
    }

    void LuaState::setPackageDescriptorReuse(bool enabled)
    {
        mPackageDescriptorReuse = false;
        mCommonFactoryDescriptors.clear();
        if (!enabled) return;
        try
        {
            for (const auto& [name, package] : mCommonPackages)
                if (package.is<sol::function>())
                    mCommonFactoryDescriptors.emplace(name, &package);
            mPackageDescriptorReuse = true;
        }
        catch (const std::bad_alloc&)
        {
            mCommonFactoryDescriptors.clear();
        }
    }

    sol::protected_function_result LuaState::runInNewSandbox(const VFS::Path::Normalized& path,
        const std::string& envName, const std::map<std::string, sol::main_object>& packages,
        const sol::main_object& hiddenData, const sol::main_table* packagePrototype, ScriptId scriptId,
        bool additionalPackagesReadOnly)
    {
        sol::protected_function script;
        {
            Debug::V36LuaAddScriptTrace::PhaseScope v36CachedChunk(
                Debug::V36LuaAddScriptTrace::Phase::CachedChunkLoad);
            script = loadDirectScriptAndCache(path);
        }

        const auto v36EnvironmentStart = Debug::V36LuaAddScriptTrace::startPhase();
        sol::environment env(mSol, sol::create, mSandboxEnv);
        env["print"] = mSol["printGen"](envName + ":");
        env["_G"] = env;
        env[sol::metatable_key]["__metatable"] = false;
        Debug::V36LuaAddScriptTrace::add(
            Debug::V36LuaAddScriptTrace::Phase::Environment, v36EnvironmentStart);

        // The owning ScriptsContainer passes ScriptId directly from C++. Do not
        // round-trip this engine pointer/index pair through Lua userdata here:
        // malformed or nil userdata can make sol's unchecked as_value getter
        // dereference a null pointer before Lua can report a normal type error.

        auto maybeRunLoader = [&hiddenData, scriptId](const sol::object& package) -> sol::object {
            if (package.is<sol::function>())
                return call(scriptId, package.as<sol::function>(), hiddenData);
            else
                return package;
        };
        sol::table loaded(mSol, sol::create);
        if (packagePrototype)
        {
            sol::table meta(mSol, sol::create);
            meta[sol::meta_method::index] = *packagePrototype;
            loaded[sol::metatable_key] = meta;
            {
                Debug::V36LuaAddScriptTrace::PhaseScope v36CommonPackages(
                    Debug::V36LuaAddScriptTrace::Phase::CommonPackages);
                if (mPackageDescriptorReuse)
                {
                    auto it = mCommonFactoryDescriptors.begin();
                    while (it != mCommonFactoryDescriptors.end())
                    {
                        // Names remain owned by mCommonPackages. A native loader
                        // may register/replace a package reentrantly; do not hold
                        // a classification iterator across that call. The next
                        // key follows the same ordered-map semantics as control.
                        const std::string_view key = it->first;
                        const sol::function loader = it->second->as<sol::function>();
                        loaded[key] = call(scriptId, loader, hiddenData);
                        if (!mPackageDescriptorReuse)
                        {
                            // Reentrant disable/allocation failure falls back
                            // for the remainder without dropping factory calls.
                            for (auto legacy = mCommonPackages.upper_bound(std::string(key));
                                legacy != mCommonPackages.end(); ++legacy)
                                if (legacy->second.is<sol::function>())
                                    loaded[legacy->first] = maybeRunLoader(legacy->second);
                            break;
                        }
                        it = mCommonFactoryDescriptors.upper_bound(key);
                    }
                }
                else
                {
                    for (const auto& [key, value] : mCommonPackages)
                        if (value.is<sol::function>())
                            loaded[key] = maybeRunLoader(value);
                }
            }
            {
                Debug::V36LuaAddScriptTrace::PhaseScope v36ContainerPackages(
                    Debug::V36LuaAddScriptTrace::Phase::ContainerPackages);
                if (!mPackageDescriptorReuse || !additionalPackagesReadOnly)
                    for (const auto& [key, value] : packages)
                        if (value.is<sol::function>())
                            loaded[key] = maybeRunLoader(value);
            }
        }
        else
        {
            {
                Debug::V36LuaAddScriptTrace::PhaseScope v36CommonPackages(
                    Debug::V36LuaAddScriptTrace::Phase::CommonPackages);
                for (const auto& [key, value] : mCommonPackages)
                    loaded[key] = maybeRunLoader(value);
            }
            {
                Debug::V36LuaAddScriptTrace::PhaseScope v36ContainerPackages(
                    Debug::V36LuaAddScriptTrace::Phase::ContainerPackages);
                for (const auto& [key, value] : packages)
                    loaded[key] = mPackageDescriptorReuse && additionalPackagesReadOnly ? sol::object(value)
                        : maybeRunLoader(value);
            }
        }
        {
            Debug::V36LuaAddScriptTrace::PhaseScope v36RequireSetup(
                Debug::V36LuaAddScriptTrace::Phase::RequireSetup);
            env["require"] = mSol["requireGen"](env, loaded, mSol["loadFromVFS"], hiddenData);
            sol::set_environment(env, script);
        }
        Debug::V36LuaAddScriptTrace::PhaseScope v36ScriptBody(
            Debug::V36LuaAddScriptTrace::Phase::ScriptBody);
        return call(scriptId, script);
    }

    sol::environment LuaState::newInternalLibEnvironment()
    {
        // TODO
        sol::environment env(mSol, sol::create, mSandboxEnv);
        sol::table loaded(mSol, sol::create);
        for (const std::string& s : safePackages)
            loaded[s] = static_cast<sol::object>(mSandboxEnv[s]);
        env["require"] = mSol["requireGen"](env, loaded, mSol["loadInternalLib"]);
        return env;
    }

    sol::protected_function_result LuaState::throwIfError(sol::protected_function_result&& res)
    {
        if (!res.valid())
            throw std::runtime_error(std::string("Lua error: ") += res.get<sol::error>().what());
        else
            return std::move(res);
    }

    std::size_t LuaState::precompileConfiguredScripts()
    {
        if (isExplicitVulkanBackend())
        {
            Log(Debug::Info) << "V4 Vulkan: bypassing V3.12 Lua configured-script precompile";
            return 0;
        }

        std::size_t compiled = 0;
        protectedCall([&](LuaView&) {
            for (std::size_t i = 0; i < mConf->size(); ++i)
            {
                const VFS::Path::Normalized& path = (*mConf)[i].mScriptPath;
                if (mCompiledScripts.contains(path))
                    continue;
                try
                {
                    sol::function function = loadFromVFS(path);
                    mCompiledScripts[path] = function.dump();
                    ++compiled;
                }
                catch (const std::exception& e)
                {
                    // Prewarming must not make an otherwise-unused bad script a
                    // startup-fatal error. Normal execution keeps upstream behavior.
                    Log(Debug::Warning) << "V3.12 Lua precompile skipped " << path << ": " << e.what();
                }
            }
        });
        return compiled;
    }

    std::size_t LuaState::precompileConfiguredDependencies(int mode)
    {
        if (mode <= 0)
            return 0;
        if (isExplicitVulkanBackend())
        {
            Log(Debug::Info) << "V4 Vulkan: bypassing V3.14 Lua dependency precompile";
            return 0;
        }

        std::size_t compiled = 0;
        protectedCall([&](LuaView&) {
            static const std::regex requirePattern(
                R"v314(\brequire\s*(?:\(\s*)?[\"']([^\"']+)[\"']\s*\)?)v314");

            std::vector<VFS::Path::Normalized> scanQueue;
            std::set<VFS::Path::Normalized> queued;
            for (std::size_t i = 0; i < mConf->size(); ++i)
            {
                const VFS::Path::Normalized& path = (*mConf)[i].mScriptPath;
                if (queued.insert(path).second)
                    scanQueue.push_back(path);
            }

            constexpr std::size_t MaxScannedModules = 4096;
            for (std::size_t index = 0; index < scanQueue.size() && index < MaxScannedModules; ++index)
            {
                const VFS::Path::Normalized path = scanQueue[index];
                try
                {
                    std::string source(std::istreambuf_iterator<char>(*mVFS->get(path)), {});
                    for (std::sregex_iterator it(source.begin(), source.end(), requirePattern), end; it != end; ++it)
                    {
                        const std::string packageName = (*it)[1].str();
                        try
                        {
                            const VFS::Path::Normalized dependency = packageNameToVfsPath(packageName, *mVFS);
                            const bool newlyQueued = queued.insert(dependency).second;
                            if (!mCompiledScripts.contains(dependency))
                            {
                                sol::function function = loadFromVFS(dependency);
                                mCompiledScripts[dependency] = function.dump();
                                ++compiled;
                            }
                            if (mode >= 2 && newlyQueued && scanQueue.size() < MaxScannedModules)
                                scanQueue.push_back(dependency);
                        }
                        catch (const std::exception&)
                        {
                            // Comments/dead branches/optional package text are intentionally nonfatal.
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "V3.14 Lua dependency scan skipped " << path << ": " << e.what();
                }
            }
        });
        return compiled;
    }

    sol::main_table LuaState::makePackagePrototype(const std::map<std::string, sol::main_object>& packages)
    {
        sol::table prototype(mSol, sol::create);
        for (const auto& [key, value] : mCommonPackages)
        {
            if (!value.is<sol::function>())
                prototype[key] = value;
        }
        for (const auto& [key, value] : packages)
        {
            if (!value.is<sol::function>())
                prototype[key] = value;
        }
        return sol::main_table(prototype);
    }

    bool LuaState::configurePreparedScriptRoots(PreparedScriptRootLimits limits)
    {
        if (mPreparingScriptRoot || !mActiveScriptIdStack.empty() || !mVFS || !mConf)
        {
            ++mPreparedScriptRootStats.mContextRefusals;
            return false;
        }
        discardPreparedScriptRoots();
        mPreparedScriptRootLimits = {};
        if (!limits.mEnabled)
            return true;
#if LUA_VERSION_NUM != 501
        // The preparation entry relies on the real 5.1 protected C call, not
        // a push-before-pcall compatibility emulation on another Lua runtime.
        ++mPreparedScriptRootStats.mUnsupportedVmRefusals;
        return false;
#endif
        if (mPreparedScriptRootEpochExhausted)
        {
            ++mPreparedScriptRootStats.mGenerationRefusals;
            return false;
        }
        if (!limits.mMaxRoots || limits.mMaxRoots > sMaxPreparedScriptRoots || !limits.mMaxPositiveBytes)
        {
            ++mPreparedScriptRootStats.mCapacityRefusals;
            return false;
        }
        mPreparedScriptRootLimits = limits;
        return true;
    }

    std::size_t LuaState::preparedScriptRootMetadataBytes() const
    {
        // Logical fixed native storage, not an estimate of OS allocation size.
        return sizeof(mPreparedScriptRoots);
    }

    void LuaState::discardPreparedScriptRoots()
    {
        if (!mPreparedScriptRootStats.mPendingRoots)
            return;
        for (auto& root : mPreparedScriptRoots)
        {
            if (!root.mFunction)
                continue;
            root.mFunction.reset();
            root.mPath = nullptr;
            root.mBytecode = nullptr;
            root.mCacheEpoch = root.mVfsEpoch = root.mPositiveBytes = 0;
            ++mPreparedScriptRootStats.mDiscarded;
        }
        mPreparedScriptRootStats.mPendingRoots = 0;
        mPreparedScriptRootStats.mPendingPositiveBytes = 0;
    }

    void LuaState::discardStalePreparedScriptRoots()
    {
        if (!mPreparedScriptRootStats.mPendingRoots)
            return;
        const auto vfsEpoch = mVFS->getIndexGeneration();
        for (const auto& root : mPreparedScriptRoots)
            if (root.mFunction && (root.mCacheEpoch != mScriptCacheEpoch || root.mVfsEpoch != vfsEpoch))
            {
                ++mPreparedScriptRootStats.mStaleRefusals;
                discardPreparedScriptRoots();
                return;
            }
    }

    void LuaState::dropScriptCache()
    {
        // Discard borrowed entry/key addresses BEFORE clearing their map. A
        // preparation in flight owns its input copy and fails the epoch check.
        discardPreparedScriptRoots();
        if (mScriptCacheEpoch == std::numeric_limits<std::uint64_t>::max())
        {
            mPreparedScriptRootEpochExhausted = true;
            mPreparedScriptRootLimits.mEnabled = false;
            ++mPreparedScriptRootStats.mGenerationRefusals;
        }
        else
            ++mScriptCacheEpoch;
        mCompiledScripts.clear();
    }

    std::size_t LuaState::prepareScriptRoots(const VFS::Path::Normalized& path, std::size_t count)
    {
        if (!mPreparedScriptRootLimits.mEnabled || !count)
            return 0;
        if (mPreparingScriptRoot || !mActiveScriptIdStack.empty())
        {
            ++mPreparedScriptRootStats.mContextRefusals;
            return 0;
        }
        void* allocatorOwner = nullptr;
        if (lua_getallocf(mLuaState.get(), &allocatorOwner) != &trackingAllocator || allocatorOwner != this)
        {
            ++mPreparedScriptRootStats.mUntrackedRefusals;
            return 0;
        }
        discardStalePreparedScriptRoots();
        std::size_t created = 0;
        // At most the fixed slot count, regardless of a caller's requested count.
        for (std::size_t attempt = 0; attempt < count && attempt < sMaxPreparedScriptRoots; ++attempt)
        {
            if (mPreparedScriptRootStats.mPendingRoots >= mPreparedScriptRootLimits.mMaxRoots)
            {
                ++mPreparedScriptRootStats.mCapacityRefusals;
                break;
            }
            const auto iter = mCompiledScripts.find(path);
            if (iter == mCompiledScripts.end())
            {
                ++mPreparedScriptRootStats.mMissingBytecodeRefusals;
                break;
            }
            const auto cacheEpoch = mScriptCacheEpoch;
            const auto vfsEpoch = mVFS->getIndexGeneration();
            const std::size_t codeSize = iter->second.size();
            const std::size_t nameSize = iter->first.value().size();
            const auto remaining = mPreparedScriptRootLimits.mMaxPositiveBytes
                - mPreparedScriptRootStats.mPendingPositiveBytes;
            if (!codeSize || nameSize == std::numeric_limits<std::size_t>::max()
                || codeSize > remaining || nameSize + 1 > remaining - codeSize)
            {
                ++mPreparedScriptRootStats.mBudgetRefusals;
                break;
            }
            const std::uint64_t nativeBytes = std::uint64_t(codeSize) + std::uint64_t(nameSize) + 1;
            std::unique_ptr<char[]> code, name;
            try
            {
                // Exact requested array sizes, including temporary overlap. The
                // ordinary bytecode cache remains unchanged. GC/native reentry
                // can clear that cache without invalidating this reader's bytes.
                code = std::make_unique<char[]>(codeSize);
                mPreparedScriptRootStats.mPreparationNativeBytes += codeSize;
                name = std::make_unique<char[]>(nameSize + 1);
                mPreparedScriptRootStats.mPreparationNativeBytes += nameSize + 1;
            }
            catch (const std::bad_alloc&)
            {
                ++mPreparedScriptRootStats.mFailures;
                break;
            }
            std::memcpy(code.get(), iter->second.data(), codeSize);
            std::memcpy(name.get(), iter->first.value().data(), nameSize);
            name[nameSize] = '\0';
            sol::function candidate;
            const int originalTop = lua_gettop(mLuaState.get());
            mPreparingScriptRoot = true;
            mPreparedAllocationLimit = remaining - nativeBytes;
            mPreparedAllocationGrowth = 0;
            mPreparedAllocationDenied = false;
            mPreparedAllocationActive = true;
            mPreparedAllocationMayDeny = true;
            bool failed = false;
#if LUA_VERSION_NUM == 501
            struct PreparationCall
            {
                const char* mCode;
                std::size_t mCodeSize;
                const char* mName;
                sol::function* mCandidate;
                std::uint64_t* mLoads;
                bool mCppFailure = false;
            } job{ code.get(), codeSize, name.get(), &candidate,
                &mPreparedScriptRootStats.mPreparationBytecodeLoads };
            try
            {
                // Unlike protectedCall, lua_cpcall also protects allocation of
                // its C entry closure and userdata argument. The exact 5.1 API
                // contains both optional-budget and global VM OOM failures.
                const int status = lua_cpcall(mLuaState.get(), [](lua_State* state) -> int {
                    auto* call = static_cast<PreparationCall*>(lua_touserdata(state, 1));
                    try
                    {
                        ++*call->mLoads;
                        if (luaL_loadbufferx(state, call->mCode, call->mCodeSize, call->mName, "b") == LUA_OK)
                            *call->mCandidate = sol::stack::get<sol::function>(state, -1);
                        lua_pop(state, 1); // loaded function or optional load error
                    }
                    catch (const std::exception&)
                    {
                        call->mCppFailure = true;
                    }
                    return 0;
                }, &job);
                failed = status != LUA_OK || job.mCppFailure;
            }
            catch (...)
            {
                failed = true;
            }
#else
            failed = true; // configuration refuses this unsupported VM above
#endif
            // These flags live outside the Lua C frame: a longjmp/OOM cannot
            // leave preparation admission active for ordinary script execution.
            mPreparedAllocationMayDeny = false;
            mPreparedAllocationActive = false;
            mPreparingScriptRoot = false;
            lua_settop(mLuaState.get(), originalTop);
            const auto luaBytes = mPreparedAllocationGrowth;
            mPreparedScriptRootStats.mPreparationPositiveLuaBytes += luaBytes;
            if (mPreparedAllocationDenied || luaBytes > remaining - nativeBytes)
            {
                ++mPreparedScriptRootStats.mBudgetRefusals;
                break;
            }
            if (failed || !candidate.valid())
            {
                ++mPreparedScriptRootStats.mFailures;
                break;
            }
            if (cacheEpoch != mScriptCacheEpoch || vfsEpoch != mVFS->getIndexGeneration()
                || !mPreparedScriptRootLimits.mEnabled)
            {
                ++mPreparedScriptRootStats.mStaleRefusals;
                break;
            }
            auto slot = std::find_if(mPreparedScriptRoots.begin(), mPreparedScriptRoots.end(),
                [](const PreparedScriptRoot& root) { return !root.mFunction; });
            // Moves transfer a registry reference without another Lua allocation.
            slot->mFunction.emplace(std::move(candidate));
            slot->mPath = &iter->first;
            slot->mBytecode = &iter->second;
            slot->mCacheEpoch = cacheEpoch;
            slot->mVfsEpoch = vfsEpoch;
            slot->mPositiveBytes = nativeBytes + luaBytes;
            mPreparedScriptRootStats.mPendingPositiveBytes += slot->mPositiveBytes;
            ++mPreparedScriptRootStats.mPendingRoots;
            ++mPreparedScriptRootStats.mCreated;
            mPreparedScriptRootStats.mPeakPendingPositiveBytes = (std::max)(
                mPreparedScriptRootStats.mPeakPendingPositiveBytes, mPreparedScriptRootStats.mPendingPositiveBytes);
            ++created;
        }
        return created;
    }

    std::size_t LuaState::prepareConfiguredScriptRoots(std::size_t rootsPerScript)
    {
        if (!mPreparedScriptRootLimits.mEnabled || mPreparingScriptRoot || !mActiveScriptIdStack.empty()
            || !mVFS || !mConf)
            return 0;
        std::size_t created = 0;
        const auto epoch = mScriptCacheEpoch;
        for (std::size_t i = 0; i < mConf->size(); ++i)
        {
            const auto& entry = (*mConf)[i];
            if (entry.mFlags & (ESM::LuaScriptCfg::sGlobal | ESM::LuaScriptCfg::sMenu
                    | ESM::LuaScriptCfg::sLoad | ESM::LuaScriptCfg::sPlayer))
                continue;
            created += prepareScriptRoots(entry.mScriptPath, rootsPerScript);
            if (epoch != mScriptCacheEpoch || mPreparedScriptRootEpochExhausted
                || mPreparedScriptRootStats.mPendingRoots >= mPreparedScriptRootLimits.mMaxRoots)
                break;
        }
        return created;
    }

    sol::function LuaState::loadDirectScriptAndCache(const VFS::Path::Normalized& path)
    {
        if (mPreparedScriptRootLimits.mEnabled)
        {
            discardStalePreparedScriptRoots();
            if (mPreparingScriptRoot || !mActiveScriptIdStack.empty())
                ++mPreparedScriptRootStats.mContextRefusals;
            else
            {
                const auto entry = mCompiledScripts.find(path);
                if (entry != mCompiledScripts.end())
                    for (auto& root : mPreparedScriptRoots)
                        if (root.mFunction && root.mBytecode == &entry->second && root.mPath == &entry->first)
                        {
                            sol::function result(std::move(*root.mFunction));
                            root.mFunction.reset();
                            --mPreparedScriptRootStats.mPendingRoots;
                            mPreparedScriptRootStats.mPendingPositiveBytes -= root.mPositiveBytes;
                            mPreparedScriptRootStats.mTransferredPositiveBytes += root.mPositiveBytes;
                            root.mPath = nullptr;
                            root.mBytecode = nullptr;
                            root.mCacheEpoch = root.mVfsEpoch = root.mPositiveBytes = 0;
                            ++mPreparedScriptRootStats.mConsumed;
                            return result;
                        }
            }
        }
        return loadScriptAndCache(path, true);
    }

    sol::function LuaState::loadScriptAndCache(const VFS::Path::Normalized& path, bool direct)
    {
        if (mPreparedScriptRootLimits.mEnabled)
        {
            if (direct) ++mPreparedScriptRootStats.mOrdinaryDirectLoads;
            else ++mPreparedScriptRootStats.mOrdinaryModuleLoads;
        }
        Debug::HitchTrace::Scope cached("lua.chunk_cache", {}, path.value());
        auto iter = mCompiledScripts.find(path);
        cached.setMetric(0, "cache_hit", iter != mCompiledScripts.end());
        if (iter != mCompiledScripts.end())
        {
            if (mPreparedScriptRootLimits.mEnabled)
                ++mPreparedScriptRootStats.mOrdinaryBytecodeLoads;
            sol::load_result res = mSol.load(iter->second.as_string_view(), path.value(), sol::load_mode::binary);
            // Unless we have memory corruption issues, the bytecode is valid at this point, but loading might still
            // fail because we've hit our Lua memory cap
            if (!res.valid())
                throw std::runtime_error("Lua error: " + res.get<std::string>());
            return res;
        }
        sol::function res = loadFromVFS(path);
        mCompiledScripts[path] = res.dump();
        return res;
    }

    sol::function LuaState::loadFromVFS(const VFS::Path::Normalized& path)
    {
        Debug::HitchTrace::Scope load("lua.source_read_compile", {}, path.value());
        std::string fileContent(std::istreambuf_iterator<char>(*mVFS->get(path)), {});
        load.setMetric(0, "source_bytes", fileContent.size());
        sol::load_result res = mSol.load(fileContent, path.value(), sol::load_mode::text);
        if (!res.valid())
            throw std::runtime_error(std::string("Lua error: ") += res.get<sol::error>().what());
        return res;
    }

    sol::function LuaState::loadInternalLib(std::string_view libName)
    {
        const auto path = packageNameToPath(libName, mLibSearchPaths);
        std::ifstream stream(path);
        std::string fileContent(std::istreambuf_iterator<char>(stream), {});
        sol::load_result res = mSol.load(fileContent, Files::pathToUnicodeString(path), sol::load_mode::text);
        if (!res.valid())
            throw std::runtime_error("Lua error: " + res.get<std::string>());
        return res;
    }

    std::string getLuaVersion()
    {
#ifdef NO_LUAJIT
        return LUA_RELEASE;
#else
        return LUA_RELEASE " (" LUAJIT_VERSION ")";
#endif
    }

    std::string toString(const sol::object& obj)
    {
        if (obj == sol::nil)
            return "nil";
        else if (obj.get_type() == sol::type::string)
            return "\"" + obj.as<std::string>() + "\"";
        else
            return call(sol::state_view(obj.lua_state())["tostring"], obj);
    }

    std::string Internal::formatCastingError(const sol::object& obj, const std::type_info& t)
    {
        const char* typeName = t.name();
        if (t == typeid(int))
            typeName = "int";
        else if (t == typeid(unsigned))
            typeName = "uint32";
        else if (t == typeid(size_t))
            typeName = "size_t";
        else if (t == typeid(float))
            typeName = "float";
        else if (t == typeid(double))
            typeName = "double";
        else if (t == typeid(sol::table))
            typeName = "sol::table";
        else if (t == typeid(sol::function) || t == typeid(sol::protected_function))
            typeName = "sol::function";
        else if (t == typeid(std::string) || t == typeid(std::string_view))
            typeName = "string";
        return std::string("Value \"") + toString(obj) + std::string("\" can not be casted to ") + typeName;
    }
}
