#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <thread>

#include <components/lua/luastate.hpp>
#include <components/testing/expecterror.hpp>
#include <components/testing/util.hpp>

namespace
{
    using namespace testing;

    constexpr VFS::Path::NormalizedView counterPath("aaa/counter.lua");

    TestingOpenMW::VFSTestFile counterFile(R"X(
x = 42
return {
    get = function() return x end,
    inc = function(v) x = x + v end,
    diagnosticAllocate = function()
        local t = {}
        for i = 1, 20000 do t[i] = string.rep(tostring(i), 64) end
        return #t
    end,
    diagnosticThrow = function()
        local t = {}
        for i = 1, 20000 do t[i] = string.rep(tostring(i), 64) end
        error("diagnostic_fixture_failure")
    end
}
)X");

    constexpr VFS::Path::NormalizedView invalidPath("invalid.lua");

    TestingOpenMW::VFSTestFile invalidScriptFile("Invalid script");

    constexpr VFS::Path::NormalizedView testsPath("bbb/tests.lua");

    TestingOpenMW::VFSTestFile testsFile(R"X(
return {
    -- should work
    sin = function(x) return math.sin(x) end,
    requireMathSin = function(x) return require('math').sin(x) end,
    useCounter = function()
        local counter = require('aaa.counter')
        counter.inc(1)
        return counter.get()
    end,
    callRawset = function()
        t = {a = 1, b = 2}
        rawset(t, 'b', 3)
        return t.b
    end,
    print = print,

    iteratePlain = function()
        local sum = 0
        for k, v in pairs({a = 1, b = 2, c = 3}) do sum = sum + v end
        for i, v in ipairs({10, 20, 30}) do sum = sum + v end
        return sum
    end,
    honoursCustomPairs = function()
        local t = setmetatable({}, { __pairs = function(v) return next, {z = 9}, nil end })
        local sum = 0
        for k, v in pairs(t) do sum = sum + v end
        return sum
    end,
    iterateHiddenMetatable = function()
        local t = setmetatable({a = 1, b = 2}, { __metatable = false })
        local sum = 0
        for k, v in pairs(t) do sum = sum + v end
        for i, v in ipairs(setmetatable({7, 8}, { __metatable = false })) do sum = sum + v end
        return sum
    end,
    iterateReadOnly = function(t)
        local keys, sum = {}, 0
        for k, v in pairs(t) do keys[#keys + 1] = k; sum = sum + v end
        table.sort(keys)
        return table.concat(keys, ','), sum, #t
    end,
    iterateReadOnlyArray = function(t)
        local sum = 0
        for i, v in ipairs(t) do sum = sum + v end
        return sum, #t
    end,
    readOnlyIterationStateIsGuarded = function(t)
        local _, state = pairs(t)
        return rawequal(state, t)
    end,

    -- should throw an error
    incorrectRequire = function() require('counter') end,
    modifySystemLib = function() math.sin = 5 end,
    modifySystemLib2 = function() math.__index.sin = 5 end,
    rawsetSystemLib = function() rawset(math, 'sin', 5) end,
    callLoadstring = function() loadstring('print(1)') end,
    setSqr = function() require('sqrlib').sqr = math.sin end,
    setOmwName = function() require('openmw').name = 'abc' end,

    -- should work if API is registered
    sqr = function(x) return require('sqrlib').sqr(x) end,
    apiName = function() return require('test.api').name end
}
)X");

    constexpr VFS::Path::NormalizedView metaIndexErrorPath("metaindexerror.lua");

    TestingOpenMW::VFSTestFile metaIndexErrorFile(
        "return setmetatable({}, { __index = function(t, key) error('meta index error') end })");

    std::string genBigScript()
    {
        std::stringstream buf;
        buf << "return function()\n";
        buf << "  x = {}\n";
        for (int i = 0; i < 1000; ++i)
            buf << "  x[" << i * 2 << "] = " << i << "\n";
        buf << "  return x\n";
        buf << "end\n";
        return buf.str();
    }

    constexpr VFS::Path::NormalizedView bigPath("big.lua");

    TestingOpenMW::VFSTestFile bigScriptFile(genBigScript());

    constexpr VFS::Path::NormalizedView requireBigPath("requirebig.lua");

    TestingOpenMW::VFSTestFile requireBigScriptFile("local x = require('big') ; return {x}");

    constexpr VFS::Path::NormalizedView descriptorPath("descriptors.lua");
    TestingOpenMW::VFSTestFile descriptorFile(R"X(
local actor = require('fixture.actor')
local common = require('fixture.shared')
local api = require('test.api')
local counter = require('aaa.counter')
local closure = 0
return {
    id = function() return actor.id end,
    actorInc = function() actor.n = actor.n + 1; return actor.n end,
    closureInc = function() closure = closure + 1; return closure end,
    moduleInc = function() counter.inc(1); return counter.get() end,
    commonName = function() return common.name end,
    apiName = function() return api.name end,
    mutateCommon = function() common.name = 'modified' end,
    mutateApi = function() api.name = 'modified' end,
}
)X");
    constexpr VFS::Path::NormalizedView descriptorFactoryPath("descriptorfactory.lua");
    TestingOpenMW::VFSTestFile descriptorFactoryFile("return {id = require('fixture.actor').id}");
    constexpr VFS::Path::NormalizedView descriptorEmptyPath("descriptorempty.lua");
    TestingOpenMW::VFSTestFile descriptorEmptyFile("return true");

    struct LuaStateTest : Test
    {
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({
            { counterPath, &counterFile },
            { testsPath, &testsFile },
            { invalidPath, &invalidScriptFile },
            { bigPath, &bigScriptFile },
            { requireBigPath, &requireBigScriptFile },
            { metaIndexErrorPath, &metaIndexErrorFile },
            { descriptorPath, &descriptorFile },
            { descriptorFactoryPath, &descriptorFactoryFile },
            { descriptorEmptyPath, &descriptorEmptyFile },
        });

        LuaUtil::ScriptsConfiguration mCfg;
        LuaUtil::LuaState mLua{ mVFS.get(), &mCfg };
    };

    void addSlowUnreachableFinalizer(LuaUtil::LuaState& lua, int& finalized)
    {
        lua_gc(lua.unsafeState(), LUA_GCSTOP, 0);
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            lua_State* state = view.sol().lua_state();
            auto* counter = static_cast<int**>(lua_newuserdata(state, sizeof(int*)));
            *counter = &finalized;
            lua_newtable(state);
            lua_pushcfunction(state, [](lua_State* callback) {
                ++**static_cast<int**>(lua_touserdata(callback, 1));
                // A real VM finalizer qualifies its explicit production GC
                // envelope without forced retention or synthetic GC metrics.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return 0;
            });
            lua_setfield(state, -2, "__gc");
            lua_setmetatable(state, -2);
            lua_pop(state, 1);
        });
        lua_gc(lua.unsafeState(), LUA_GCRESTART, 0);
    }

    TEST_F(LuaStateTest, ProtectedReuseKeepsNestedJobsAndStack)
    {
        mLua.setProtectedCallReuse(true);
        const auto before = mLua.protectedCallStats();
        const int outerTop = lua_gettop(mLua.unsafeState());
        std::vector<int> order;
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            lua_State* state = view.sol().lua_state();
            lua_pushinteger(state, 77);
            const int nestedTop = lua_gettop(state);
            order.push_back(1);
            mLua.protectedCall([&](LuaUtil::LuaView&) { order.push_back(2); });
            EXPECT_EQ(lua_gettop(state), nestedTop);
            EXPECT_EQ(lua_tointeger(state, -1), 77);
            order.push_back(3);
        });
        EXPECT_EQ(order, (std::vector<int>{ 1, 2, 3 }));
        EXPECT_EQ(lua_gettop(mLua.unsafeState()), outerTop);
        const auto after = mLua.protectedCallStats();
        EXPECT_EQ(after.mReusedCalls - before.mReusedCalls, 2);
        EXPECT_EQ(after.mClosureCreations, before.mClosureCreations);
    }

    TEST_F(LuaStateTest, ProtectedReusePreservesLuaErrorBoundary)
    {
        for (bool enabled : { false, true })
        {
            mLua.setProtectedCallReuse(enabled);
            const int top = lua_gettop(mLua.unsafeState());
            EXPECT_ERROR(mLua.protectedCall([](LuaUtil::LuaView& view) {
                luaL_error(view.sol().lua_state(), "protected_reuse_fixture_error");
            }), "protected_reuse_fixture_error");
            // Preserve the original API's error object on the caller's stack.
            EXPECT_EQ(lua_gettop(mLua.unsafeState()), top + 1);
            lua_pop(mLua.unsafeState(), 1);
            int calls = 0;
            mLua.protectedCall([&](LuaUtil::LuaView&) { ++calls; });
            EXPECT_EQ(calls, 1);
            EXPECT_EQ(lua_gettop(mLua.unsafeState()), top);
        }
    }

    TEST_F(LuaStateTest, ProtectedReusePreservesCallerEnvironment)
    {
        for (bool enabled : { false, true })
        {
            mLua.setProtectedCallReuse(enabled);
            mLua.protectedCall([&](LuaUtil::LuaView& view) {
                lua_State* state = view.sol().lua_state();
                lua_pushlightuserdata(state, &mLua);
                lua_pushcclosure(state, [](lua_State* callback) {
                    auto* owner = static_cast<LuaUtil::LuaState*>(lua_touserdata(callback, lua_upvalueindex(1)));
                    int value = 0;
                    owner->protectedCall([&](LuaUtil::LuaView& nested) {
                        lua_State* context = nested.sol().lua_state();
                        lua_pushvalue(context, LUA_ENVIRONINDEX);
                        lua_getfield(context, -1, "fixture_marker");
                        value = static_cast<int>(lua_tointeger(context, -1));
                        lua_pop(context, 2);
                    });
                    lua_pushinteger(callback, value);
                    return 1;
                }, 1);
                lua_newtable(state);
                lua_pushinteger(state, 1234);
                lua_setfield(state, -2, "fixture_marker");
                lua_setfenv(state, -2);
                ASSERT_EQ(lua_pcall(state, 0, 1, 0), 0);
                EXPECT_EQ(lua_tointeger(state, -1), 1234);
                lua_pop(state, 1);
            });
        }
        EXPECT_GT(mLua.protectedCallStats().mContextFallbacks, 0);
    }

    TEST_F(LuaStateTest, ProtectedReuseFallsBackOnRegistryReplacement)
    {
        mLua.setProtectedCallReuse(true);
        mLua.protectedCall([](LuaUtil::LuaView& view) {
            lua_State* state = view.sol().lua_state();
            lua_pushnil(state);
            while (lua_next(state, LUA_REGISTRYINDEX))
            {
                if (lua_tocfunction(state, -1) == &LuaUtil::invokeReusableProtectedCall)
                {
                    const int reference = static_cast<int>(lua_tointeger(state, -2));
                    lua_pop(state, 2);
                    lua_pushinteger(state, 9);
                    lua_rawseti(state, LUA_REGISTRYINDEX, reference);
                    return;
                }
                lua_pop(state, 1);
            }
            FAIL() << "Reusable trampoline was not registered";
        });
        const auto before = mLua.protectedCallStats();
        const int top = lua_gettop(mLua.unsafeState());
        int called = 0;
        mLua.protectedCall([&](LuaUtil::LuaView&) { ++called; });
        const auto after = mLua.protectedCallStats();
        EXPECT_EQ(called, 1);
        EXPECT_EQ(lua_gettop(mLua.unsafeState()), top);
        EXPECT_EQ(after.mRegistryFallbacks - before.mRegistryFallbacks, 1);
        EXPECT_EQ(after.mClosureCreations - before.mClosureCreations, 1);
        EXPECT_EQ(after.mReusedCalls, before.mReusedCalls);
    }

    TEST_F(LuaStateTest, ProtectedReuseOwnsRegistryPerStateAndCanDisable)
    {
        mLua.setProtectedCallReuse(true);
        auto fn = [](LuaUtil::LuaView&) {};
        mLua.protectedCall(fn);
        LuaUtil::LuaState independent{ mVFS.get(), &mCfg };
        independent.setProtectedCallReuse(true);
        const auto ownBefore = mLua.protectedCallStats();
        independent.protectedCall(fn);
        EXPECT_EQ(mLua.protectedCallStats().mReusedCalls, ownBefore.mReusedCalls);
        mLua.setProtectedCallReuse(false);
        mLua.protectedCall([](LuaUtil::LuaView&) {});
        EXPECT_EQ(mLua.protectedCallStats().mReusedCalls, ownBefore.mReusedCalls);
        EXPECT_EQ(mLua.protectedCallStats().mOrdinaryCalls - ownBefore.mOrdinaryCalls, 1);
    }

    TEST_F(LuaStateTest, ProtectedReuseAvoidsActualClosureAllocation)
    {
#ifdef NO_LUAJIT
        GTEST_SKIP() << "Closure allocation proof targets the pinned LuaJIT runtime";
#else
        const int top = lua_gettop(mLua.unsafeState());
        mLua.setProtectedCallReuse(true);
        lua_gc(mLua.unsafeState(), LUA_GCSTOP, 0);
        const auto heapBefore = mLua.getTotalMemoryUsage();
        const auto before = mLua.protectedCallStats();
        for (int i = 0; i < 512; ++i)
            mLua.protectedCall([](LuaUtil::LuaView&) {});
        const auto heapCached = mLua.getTotalMemoryUsage();
        const auto cached = mLua.protectedCallStats();
        mLua.setProtectedCallReuse(false);
        for (int i = 0; i < 512; ++i)
            mLua.protectedCall([](LuaUtil::LuaView&) {});
        const auto heapOrdinary = mLua.getTotalMemoryUsage();
        const auto ordinary = mLua.protectedCallStats();
        lua_gc(mLua.unsafeState(), LUA_GCRESTART, 0);
        EXPECT_EQ(cached.mReusedCalls - before.mReusedCalls, 512);
        EXPECT_EQ(cached.mClosureCreations, before.mClosureCreations);
        EXPECT_EQ(ordinary.mClosureCreations - cached.mClosureCreations, 512);
        EXPECT_LT(heapCached - heapBefore, heapOrdinary - heapCached);
        EXPECT_EQ(lua_gettop(mLua.unsafeState()), top);
#endif
    }

    TEST_F(LuaStateTest, DiagnosticVmSamplerIsExplicitAndKeepsScriptResult)
    {
        const char* request = std::getenv("OPENMW_LUA_VM_STATE_SAMPLER");
        if (!Debug::HitchTrace::requested() || !request || std::string_view(request) != "1")
        {
            EXPECT_FALSE(mLua.startDiagnosticVmSampler());
            EXPECT_EQ(mLua.diagnosticVmSamples().mCallbacks, 0);
            return;
        }
        if (!mLua.startDiagnosticVmSampler())
            GTEST_SKIP() << "Pinned runtime does not provide the optional diagnostic sampler";
        sol::protected_function fn;
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            fn = view.sol().script(R"X(
                return function()
                    local sum = 0
                    for i = 1, 2000000 do
                        local t = {i, i + 1, i + 2, i + 3}
                        sum = sum + t[1] - t[2] + t[3] - t[4]
                    end
                    return sum
                end
            )X").get<sol::protected_function>();
        });
        for (int i = 0; i < 4; ++i)
            EXPECT_EQ(LuaUtil::call(LuaUtil::ScriptId{}, fn).get<int>(), -4000000);
        const auto samples = mLua.diagnosticVmSamples();
        mLua.stopDiagnosticVmSampler();
        EXPECT_GT(samples.mCallbacks, 0);
        std::uint64_t count = 0;
        for (auto value : samples.mStates)
            count += value;
        EXPECT_GT(count, 0);
        EXPECT_FALSE(mLua.diagnosticVmSamplerStarted());
        EXPECT_EQ(LuaUtil::call(LuaUtil::ScriptId{}, fn).get<int>(), -4000000);
    }

    TEST_F(LuaStateTest, HitchAllocationCounters)
    {
        const auto before = mLua.diagnosticAllocations();
        const VFS::Path::Normalized path(counterPath);
        sol::table script = mLua.runInNewSandbox(path);
        LuaUtil::call(LuaUtil::ScriptId{}, script["inc"].get<sol::protected_function>(), 3);
        EXPECT_EQ(LuaUtil::call(script["get"]).get<int>(), 45);
        EXPECT_ERROR(mLua.runInNewSandbox(VFS::Path::Normalized(invalidPath)), "Lua error:");
        EXPECT_EQ(LuaUtil::call(LuaUtil::ScriptId{}, script["diagnosticAllocate"].get<sol::protected_function>()).get<int>(), 20000);
        EXPECT_ERROR(LuaUtil::call(LuaUtil::ScriptId{}, script["diagnosticThrow"].get<sol::protected_function>()),
            "diagnostic_fixture_failure");
        const auto after = mLua.diagnosticAllocations();
        if (Debug::HitchTrace::requested() && LuaUtil::LuaState::isProfilerEnabled())
        {
            EXPECT_GT(after.calls, before.calls);
            EXPECT_GT(after.growthBytes, before.growthBytes);
            EXPECT_GE(after.shrinkBytes, before.shrinkBytes);
            EXPECT_EQ(after.failures, before.failures);
            EXPECT_GT(after.serviceCalls, before.serviceCalls);
            EXPECT_GT(after.serviceNs, before.serviceNs);
            EXPECT_EQ(after.serviceFailures, before.serviceFailures);
            EXPECT_EQ(after.policyBlocked, before.policyBlocked);
        }
        else
        {
            EXPECT_EQ(after.calls, 0);
            EXPECT_EQ(after.growthBytes, 0);
            EXPECT_EQ(after.shrinkBytes, 0);
            EXPECT_EQ(after.failures, 0);
            EXPECT_EQ(after.serviceCalls, 0);
            EXPECT_EQ(after.serviceNs, 0);
            EXPECT_EQ(after.serviceFailures, 0);
            EXPECT_EQ(after.policyBlocked, 0);
            EXPECT_EQ(after.frees, 0);
            EXPECT_EQ(after.requestedSizes, (std::array<std::uint64_t, 6>{}));
        }
    }

    TEST_F(LuaStateTest, HitchAllocatorRequestSizesPreserveRealloc)
    {
        if (!mLua.diagnosticTrackingAllocator())
            GTEST_SKIP() << "Tracking allocator is not installed in this VM";
        void* context = nullptr;
        const lua_Alloc allocate = lua_getallocf(mLua.unsafeState(), &context);
        const auto before = mLua.diagnosticAllocations();
        const auto smallBefore = mLua.getSmallAllocMemoryUsage();
        constexpr std::array<std::size_t, 10> sizes{ 64, 65, 256, 257, 1024, 1025, 16384, 16385, 1048576, 1048577 };
        for (const auto size : sizes)
        {
            // Lua uses osize as a type tag when ptr is null. The real
            // allocator must not subtract that tag from logical bytes.
            void* pointer = allocate(context, nullptr, LUA_TSTRING, size);
            ASSERT_NE(pointer, nullptr);
            std::memset(pointer, 0x5a, size);
            EXPECT_EQ(static_cast<unsigned char*>(pointer)[size - 1], 0x5a);
            EXPECT_EQ(allocate(context, pointer, size, 0), nullptr);
        }
        void* pointer = allocate(context, nullptr, LUA_TUSERDATA, 48);
        ASSERT_NE(pointer, nullptr);
        std::memset(pointer, 0x37, 48);
        pointer = allocate(context, pointer, 48, 128);
        ASSERT_NE(pointer, nullptr);
        EXPECT_EQ(static_cast<unsigned char*>(pointer)[47], 0x37);
        pointer = allocate(context, pointer, 128, 32);
        ASSERT_NE(pointer, nullptr);
        EXPECT_EQ(static_cast<unsigned char*>(pointer)[31], 0x37);
        EXPECT_EQ(allocate(context, pointer, 32, 0), nullptr);
        EXPECT_EQ(mLua.getSmallAllocMemoryUsage(), smallBefore);
        const auto after = mLua.diagnosticAllocations();
        if (Debug::HitchTrace::requested())
        {
            EXPECT_EQ(after.calls - before.calls, 24);
            EXPECT_EQ(after.serviceCalls - before.serviceCalls, 24);
            EXPECT_EQ(after.frees - before.frees, 11);
            EXPECT_EQ(after.failures, before.failures);
            EXPECT_EQ(after.serviceFailures, before.serviceFailures);
            EXPECT_EQ(after.policyBlocked, before.policyBlocked);
            EXPECT_GT(after.serviceNs, before.serviceNs);
            EXPECT_EQ(after.growthBytes - before.growthBytes, after.shrinkBytes - before.shrinkBytes);
            constexpr std::array<std::uint64_t, 6> expected{ 3, 3, 2, 2, 2, 1 };
            for (std::size_t i = 0; i < expected.size(); ++i)
                EXPECT_EQ(after.requestedSizes[i] - before.requestedSizes[i], expected[i]);
        }
        else
        {
            EXPECT_EQ(after.calls, 0);
            EXPECT_EQ(after.serviceCalls, 0);
            EXPECT_EQ(after.serviceNs, 0);
            EXPECT_EQ(after.requestedSizes, (std::array<std::uint64_t, 6>{}));
        }
    }

    TEST_F(LuaStateTest, HitchMemoryLimitDenialPreservesPointer)
    {
        if (!LuaUtil::LuaState::isProfilerEnabled())
            GTEST_SKIP() << "Tracking allocator is not installed";
        LuaUtil::LuaStateSettings settings;
        settings.mSmallAllocMaxSize = 0;
        settings.mMemoryLimit = 16 * 1024 * 1024;
        LuaUtil::LuaState lua(mVFS.get(), &mCfg, settings);
        ASSERT_TRUE(lua.diagnosticTrackingAllocator());
        void* context = nullptr;
        const lua_Alloc allocate = lua_getallocf(lua.unsafeState(), &context);
        void* pointer = allocate(context, nullptr, LUA_TSTRING, 64);
        ASSERT_NE(pointer, nullptr);
        std::memset(pointer, 0x29, 64);
        const auto before = lua.diagnosticAllocations();
        EXPECT_EQ(allocate(context, pointer, 64, 64 * 1024 * 1024), nullptr);
        // Realloc refusal leaves the caller's old allocation and accounting
        // intact; it is not a failed CRT service invocation.
        EXPECT_EQ(static_cast<unsigned char*>(pointer)[0], 0x29);
        EXPECT_EQ(static_cast<unsigned char*>(pointer)[63], 0x29);
        const auto after = lua.diagnosticAllocations();
        if (Debug::HitchTrace::requested())
        {
            EXPECT_EQ(after.calls - before.calls, 1);
            EXPECT_EQ(after.failures - before.failures, 1);
            EXPECT_EQ(after.policyBlocked - before.policyBlocked, 1);
            EXPECT_EQ(after.requestedSizes[5] - before.requestedSizes[5], 1);
        }
        EXPECT_EQ(after.serviceCalls, before.serviceCalls);
        EXPECT_EQ(after.serviceNs, before.serviceNs);
        EXPECT_EQ(after.serviceFailures, before.serviceFailures);
        EXPECT_EQ(after.growthBytes, before.growthBytes);
        EXPECT_EQ(after.shrinkBytes, before.shrinkBytes);
        EXPECT_EQ(allocate(context, pointer, 64, 0), nullptr);
    }

    TEST_F(LuaStateTest, HitchGcStepCompletesCycleAndFinalizers)
    {
        int finalized = 0;
        const int stackBefore = lua_gettop(mLua.unsafeState());
        addSlowUnreachableFinalizer(mLua, finalized);
        bool completed = false;
        for (unsigned i = 0; i < 1000 && !completed; ++i)
            completed = mLua.gcStep(1024);
        EXPECT_TRUE(completed); // VM step completion, never inferred from heap size.
        EXPECT_EQ(finalized, 1);
        EXPECT_EQ(lua_gettop(mLua.unsafeState()), stackBefore);
        sol::table script = mLua.runInNewSandbox(VFS::Path::Normalized(counterPath));
        LuaUtil::call(LuaUtil::ScriptId{}, script["inc"].get<sol::protected_function>(), 1);
        EXPECT_EQ(LuaUtil::call(script["get"]).get<int>(), 43);
    }

    TEST(LuaDiagnosticsFallback, NativeAllocatorPreservesGcAndSemantics)
    {
        const char* enabled = std::getenv("OPENMW_P9_LUA_NATIVE_ALLOCATOR_FIXTURE");
        if (!enabled || std::string_view(enabled) != "1")
            GTEST_SKIP() << "Global profiler fallback is qualified in an isolated subprocess";
        LuaUtil::LuaState::disableProfiler();
        auto vfs = TestingOpenMW::createTestVFS({ { counterPath, &counterFile } });
        LuaUtil::ScriptsConfiguration configuration;
        LuaUtil::LuaState lua(vfs.get(), &configuration);
        ASSERT_FALSE(lua.diagnosticTrackingAllocator());
        sol::table script = lua.runInNewSandbox(VFS::Path::Normalized(counterPath));
        EXPECT_EQ(LuaUtil::call(LuaUtil::ScriptId{}, script["diagnosticAllocate"].get<sol::protected_function>()).get<int>(), 20000);
        EXPECT_EQ(LuaUtil::call(script["get"]).get<int>(), 42);
        EXPECT_ERROR(LuaUtil::call(LuaUtil::ScriptId{}, script["diagnosticThrow"].get<sol::protected_function>()),
            "diagnostic_fixture_failure");
        int finalized = 0;
        addSlowUnreachableFinalizer(lua, finalized);
        bool completed = false;
        for (unsigned i = 0; i < 1000 && !completed; ++i)
            completed = lua.gcStep(1024);
        EXPECT_TRUE(completed);
        EXPECT_EQ(finalized, 1);
        const auto stats = lua.diagnosticAllocations();
        EXPECT_EQ(stats.calls, 0);
        EXPECT_EQ(stats.serviceCalls, 0);
        EXPECT_EQ(stats.serviceNs, 0);
        EXPECT_EQ(stats.requestedSizes, (std::array<std::uint64_t, 6>{}));
    }

    TEST_F(LuaStateTest, Sandbox)
    {
        const VFS::Path::Normalized path(counterPath);
        sol::table script1 = mLua.runInNewSandbox(path);

        EXPECT_EQ(LuaUtil::call(script1["get"]).get<int>(), 42);
        LuaUtil::call(script1["inc"], 3);
        EXPECT_EQ(LuaUtil::call(script1["get"]).get<int>(), 45);

        sol::table script2 = mLua.runInNewSandbox(path);
        EXPECT_EQ(LuaUtil::call(script2["get"]).get<int>(), 42);
        LuaUtil::call(script2["inc"], 1);
        EXPECT_EQ(LuaUtil::call(script2["get"]).get<int>(), 43);

        EXPECT_EQ(LuaUtil::call(script1["get"]).get<int>(), 45);
    }

    TEST_F(LuaStateTest, ToString)
    {
        EXPECT_EQ(LuaUtil::toString(sol::make_object(mLua.unsafeState(), 3.14)), "3.14");
        EXPECT_EQ(LuaUtil::toString(sol::make_object(mLua.unsafeState(), true)), "true");
        EXPECT_EQ(LuaUtil::toString(sol::nil), "nil");
        EXPECT_EQ(LuaUtil::toString(sol::make_object(mLua.unsafeState(), "something")), "\"something\"");
    }

    TEST_F(LuaStateTest, Cast)
    {
        EXPECT_EQ(LuaUtil::cast<int>(sol::make_object(mLua.unsafeState(), 3.14)), 3);
        EXPECT_ERROR(LuaUtil::cast<int>(sol::make_object(mLua.unsafeState(), "3.14")),
            "Value \"\"3.14\"\" can not be casted to int");
        EXPECT_ERROR(LuaUtil::cast<std::string_view>(sol::make_object(mLua.unsafeState(), sol::nil)),
            "Value \"nil\" can not be casted to string");
        EXPECT_ERROR(LuaUtil::cast<std::string>(sol::make_object(mLua.unsafeState(), sol::nil)),
            "Value \"nil\" can not be casted to string");
        EXPECT_ERROR(LuaUtil::cast<sol::table>(sol::make_object(mLua.unsafeState(), sol::nil)),
            "Value \"nil\" can not be casted to sol::table");
        EXPECT_ERROR(LuaUtil::cast<sol::function>(sol::make_object(mLua.unsafeState(), "3.14")),
            "Value \"\"3.14\"\" can not be casted to sol::function");
        EXPECT_ERROR(LuaUtil::cast<sol::protected_function>(sol::make_object(mLua.unsafeState(), "3.14")),
            "Value \"\"3.14\"\" can not be casted to sol::function");
    }

    TEST_F(LuaStateTest, ErrorHandling)
    {
        const VFS::Path::Normalized path("invalid.lua");
        EXPECT_ERROR(mLua.runInNewSandbox(path), "[string \"invalid.lua\"]:1:");
    }

    TEST_F(LuaStateTest, CustomRequire)
    {
        const VFS::Path::Normalized path("bbb/tests.lua");
        sol::table script = mLua.runInNewSandbox(path);

        EXPECT_FLOAT_EQ(
            LuaUtil::call(script["sin"], 1).get<float>(), -LuaUtil::call(script["requireMathSin"], -1).get<float>());

        EXPECT_EQ(LuaUtil::call(script["useCounter"]).get<int>(), 43);
        EXPECT_EQ(LuaUtil::call(script["useCounter"]).get<int>(), 44);
        {
            sol::table script2 = mLua.runInNewSandbox(path);
            EXPECT_EQ(LuaUtil::call(script2["useCounter"]).get<int>(), 43);
        }
        EXPECT_EQ(LuaUtil::call(script["useCounter"]).get<int>(), 45);

        EXPECT_ERROR(LuaUtil::call(script["incorrectRequire"]), "module not found: counter");
    }

    TEST_F(LuaStateTest, ReadOnly)
    {
        const VFS::Path::Normalized path("bbb/tests.lua");
        sol::table script = mLua.runInNewSandbox(path);

        // rawset itself is allowed
        EXPECT_EQ(LuaUtil::call(script["callRawset"]).get<int>(), 3);

        // but read-only object can not be modified even with rawset
        EXPECT_ERROR(
            LuaUtil::call(script["rawsetSystemLib"]), "bad argument #1 to 'rawset' (table expected, got userdata)");
        EXPECT_ERROR(LuaUtil::call(script["modifySystemLib"]), "a userdata value");
        EXPECT_ERROR(LuaUtil::call(script["modifySystemLib2"]), "a nil value");

        EXPECT_EQ(LuaUtil::getMutableFromReadOnly(LuaUtil::makeReadOnly(script)), script);
    }

    TEST_F(LuaStateTest, Iteration)
    {
        const VFS::Path::Normalized path("bbb/tests.lua");
        sol::table script = mLua.runInNewSandbox(path);

        EXPECT_EQ(LuaUtil::call(script["iteratePlain"]).get<int>(), 66);
        EXPECT_EQ(LuaUtil::call(script["honoursCustomPairs"]).get<int>(), 9);
        EXPECT_EQ(LuaUtil::call(script["iterateHiddenMetatable"]).get<int>(), 18);

        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table hash = LuaUtil::makeReadOnly(view.sol().create_table_with("a", 1, "b", 2, "c", 3));
            sol::table array = LuaUtil::makeReadOnly(view.sol().create_table_with(1, 10, 2, 20, 3, 30));

            auto readOnly = LuaUtil::call(script["iterateReadOnly"], hash);
            EXPECT_EQ(readOnly.get<std::string>(0), "a,b,c");
            EXPECT_EQ(readOnly.get<int>(1), 6);
            EXPECT_EQ(readOnly.get<int>(2), 0);

            auto readOnlyArray = LuaUtil::call(script["iterateReadOnlyArray"], array);
            EXPECT_EQ(readOnlyArray.get<int>(0), 60);
            EXPECT_EQ(readOnlyArray.get<int>(1), 3);

            EXPECT_TRUE(LuaUtil::call(script["readOnlyIterationStateIsGuarded"], hash).get<bool>());
        });
    }

    TEST_F(LuaStateTest, Print)
    {
        const VFS::Path::Normalized path("bbb/tests.lua");
        {
            sol::table script = mLua.runInNewSandbox(path);
            testing::internal::CaptureStdout();
            LuaUtil::call(script["print"], 1, 2, 3);
            std::string output = testing::internal::GetCapturedStdout();
            EXPECT_EQ(output, "unnamed:\t1\t2\t3\n");
        }
        {
            sol::table script = mLua.runInNewSandbox(path, "prefix");
            testing::internal::CaptureStdout();
            LuaUtil::call(script["print"]); // print with no arguments
            std::string output = testing::internal::GetCapturedStdout();
            EXPECT_EQ(output, "prefix:\n");
        }
    }

    TEST_F(LuaStateTest, UnsafeFunction)
    {
        const VFS::Path::Normalized path("bbb/tests.lua");
        sol::table script = mLua.runInNewSandbox(path);
        EXPECT_ERROR(LuaUtil::call(script["callLoadstring"]), "a nil value");
    }

    TEST_F(LuaStateTest, ProvideAPI)
    {
        LuaUtil::LuaState lua(mVFS.get(), &mCfg);
        lua.protectedCall([&](LuaUtil::LuaView& view) {
            sol::table api1 = LuaUtil::makeReadOnly(view.sol().create_table_with("name", "api1"));
            sol::table api2 = LuaUtil::makeReadOnly(view.sol().create_table_with("name", "api2"));

            const VFS::Path::Normalized path("bbb/tests.lua");

            sol::table script1 = lua.runInNewSandbox(path, "", { { "test.api", api1 } });

            lua.addCommonPackage("sqrlib", view.sol().create_table_with("sqr", [](int x) { return x * x; }));

            sol::table script2 = lua.runInNewSandbox(path, "", { { "test.api", api2 } });

            EXPECT_ERROR(LuaUtil::call(script1["sqr"], 3), "module not found: sqrlib");
            EXPECT_EQ(LuaUtil::call(script2["sqr"], 3).get<int>(), 9);

            EXPECT_EQ(LuaUtil::call(script1["apiName"]).get<std::string>(), "api1");
            EXPECT_EQ(LuaUtil::call(script2["apiName"]).get<std::string>(), "api2");
        });
    }

    TEST_F(LuaStateTest, GetLuaVersion)
    {
        EXPECT_THAT(LuaUtil::getLuaVersion(), HasSubstr("Lua"));
    }

    TEST_F(LuaStateTest, PackageDescriptorsPreserveFreshSandboxAndFactoryState)
    {
        for (const bool candidate : { false, true })
            for (const bool prototype : { false, true })
            {
                SCOPED_TRACE(std::string("candidate=") + std::to_string(candidate)
                    + " prototype=" + std::to_string(prototype));
                LuaUtil::LuaState lua(mVFS.get(), &mCfg);
                lua.setPackageDescriptorReuse(candidate);
                lua.protectedCall([&](LuaUtil::LuaView& view) {
                    int factoryCalls = 0;
                    view.sol().set_function("fixtureActorFactory", [&](sol::table hidden) {
                        ++factoryCalls;
                        return view.sol().create_table_with("id", hidden["id"].get<int>(), "n", 0);
                    });
                    lua.addCommonPackage("fixture.actor", view.sol()["fixtureActorFactory"]);
                    lua.addCommonPackage("fixture.shared", view.sol().create_table_with("name", "shared"));
                    std::map<std::string, sol::main_object> api1{
                        { "test.api", LuaUtil::makeReadOnly(view.sol().create_table_with("name", "first")) } };
                    std::map<std::string, sol::main_object> api2{
                        { "test.api", LuaUtil::makeReadOnly(view.sol().create_table_with("name", "second")) } };
                    std::optional<sol::main_table> p1, p2;
                    if (prototype)
                    {
                        p1 = lua.makePackagePrototype(api1);
                        p2 = lua.makePackagePrototype(api2);
                    }
                    sol::table first = lua.runInNewSandbox(VFS::Path::Normalized(descriptorPath), "first", api1,
                        view.sol().create_table_with("id", 101), p1 ? &*p1 : nullptr, {}, true);
                    sol::table second = lua.runInNewSandbox(VFS::Path::Normalized(descriptorPath), "second", api2,
                        view.sol().create_table_with("id", 202), p2 ? &*p2 : nullptr, {}, true);
                    EXPECT_EQ(factoryCalls, 2);
                    EXPECT_EQ(LuaUtil::call(first["id"]).get<int>(), 101);
                    EXPECT_EQ(LuaUtil::call(second["id"]).get<int>(), 202);
                    EXPECT_EQ(LuaUtil::call(first["actorInc"]).get<int>(), 1);
                    EXPECT_EQ(LuaUtil::call(first["actorInc"]).get<int>(), 2);
                    EXPECT_EQ(LuaUtil::call(second["actorInc"]).get<int>(), 1);
                    EXPECT_EQ(LuaUtil::call(first["closureInc"]).get<int>(), 1);
                    EXPECT_EQ(LuaUtil::call(second["closureInc"]).get<int>(), 1);
                    EXPECT_EQ(LuaUtil::call(first["moduleInc"]).get<int>(), 43);
                    EXPECT_EQ(LuaUtil::call(first["moduleInc"]).get<int>(), 44);
                    EXPECT_EQ(LuaUtil::call(second["moduleInc"]).get<int>(), 43);
                    EXPECT_EQ(LuaUtil::call(first["commonName"]).get<std::string>(), "shared");
                    EXPECT_EQ(LuaUtil::call(first["apiName"]).get<std::string>(), "first");
                    EXPECT_EQ(LuaUtil::call(second["apiName"]).get<std::string>(), "second");
                    EXPECT_ERROR(LuaUtil::call(first["mutateCommon"]), "a userdata value");
                    EXPECT_ERROR(LuaUtil::call(first["mutateApi"]), "a userdata value");
                });
            }
    }

    TEST_F(LuaStateTest, PackageDescriptorsPreserveGenericAdditionalFactories)
    {
        for (const bool candidate : { false, true })
        {
            LuaUtil::LuaState lua(mVFS.get(), &mCfg);
            lua.setPackageDescriptorReuse(candidate);
            lua.protectedCall([&](LuaUtil::LuaView& view) {
                int calls = 0;
                view.sol().set_function("fixtureAdditionalFactory", [&](sol::table hidden) {
                    ++calls;
                    return view.sol().create_table_with("id", hidden["id"].get<int>());
                });
                std::map<std::string, sol::main_object> packages{
                    { "fixture.actor", view.sol()["fixtureAdditionalFactory"] } };
                const sol::main_table prototype = lua.makePackagePrototype(packages);
                for (const int id : { 17, 28 })
                {
                    // Generic maps deliberately retain the default false contract.
                    sol::table output = lua.runInNewSandbox(VFS::Path::Normalized(descriptorFactoryPath), "",
                        packages, view.sol().create_table_with("id", id), &prototype);
                    EXPECT_EQ(output["id"].get<int>(), id);
                }
                EXPECT_EQ(calls, 2);
            });
        }
    }

    TEST_F(LuaStateTest, PackageDescriptorsPreserveReentrantRegistrationAndDisable)
    {
        for (const bool candidate : { false, true })
            for (const bool disable : { false, true })
            {
                SCOPED_TRACE(std::string("candidate=") + std::to_string(candidate)
                    + " disable=" + std::to_string(disable));
                LuaUtil::LuaState lua(mVFS.get(), &mCfg);
                lua.setPackageDescriptorReuse(candidate);
                lua.protectedCall([&](LuaUtil::LuaView& view) {
                    std::vector<std::string> called;
                    view.sol().set_function("fixtureB", [&](sol::object) {
                        called.push_back("b"); return view.sol().create_table();
                    });
                    view.sol().set_function("fixtureZ", [&](sol::object) {
                        called.push_back("z"); return view.sol().create_table();
                    });
                    view.sol().set_function("fixtureA", [&](sol::object) {
                        called.push_back("a");
                        lua.addCommonPackage("a.factory", view.sol().create_table_with("now", "static"));
                        lua.addCommonPackage("b.factory", view.sol()["fixtureB"]);
                        if (disable) lua.setPackageDescriptorReuse(false);
                        return view.sol().create_table();
                    });
                    lua.addCommonPackage("a.factory", view.sol()["fixtureA"]);
                    lua.addCommonPackage("z.factory", view.sol()["fixtureZ"]);
                    const sol::main_table prototype = lua.makePackagePrototype({});
                    EXPECT_TRUE(lua.runInNewSandbox(VFS::Path::Normalized(descriptorEmptyPath), "", {}, {},
                        &prototype).get<bool>());
                    EXPECT_THAT(called, ElementsAre("a", "b", "z"));
                    called.clear();
                    EXPECT_TRUE(lua.runInNewSandbox(VFS::Path::Normalized(descriptorEmptyPath), "", {}, {},
                        &prototype).get<bool>());
                    EXPECT_THAT(called, ElementsAre("b", "z"));
                });
            }
    }

    TEST_F(LuaStateTest, PackageDescriptorProductionBenchmark)
    {
        if (Debug::HitchTrace::requested())
            GTEST_SKIP() << "Production sandbox benchmark requires inactive diagnostics";
        // This measures the actual sandbox path, not a synthetic map loop. It
        // checks factory calls but deliberately has no timing pass threshold.
        mLua.protectedCall([&](LuaUtil::LuaView& view) {
            mLua.setPackageDescriptorReuse(false);
            std::uint64_t factoryCalls = 0;
            view.sol().set_function("fixtureBenchmarkFactory", [&](sol::object) {
                ++factoryCalls;
                return view.sol().create_table();
            });
            mLua.addCommonPackage("fixture.factory", view.sol()["fixtureBenchmarkFactory"]);
            std::map<std::string, sol::main_object> packages;
            for (int i = 0; i < 24; ++i)
            {
                mLua.addCommonPackage("fixture.common." + std::to_string(i),
                    view.sol().create_table_with("value", i));
                packages.emplace("fixture.api." + std::to_string(i),
                    LuaUtil::makeReadOnly(view.sol().create_table_with("value", i)));
            }
            const sol::main_table prototype = mLua.makePackagePrototype(packages);
            const VFS::Path::Normalized path(descriptorEmptyPath);
            EXPECT_TRUE(mLua.runInNewSandbox(path, "", packages, {}, &prototype, {}, true).get<bool>());
            constexpr unsigned blocks = 8, iterations = 300;
            std::array<std::vector<double>, 2> nsPerSandbox;
            for (unsigned block = 0; block < blocks; ++block)
                for (unsigned step = 0; step < 2; ++step)
                {
                    const unsigned candidate = (block + step) % 2;
                    mLua.setPackageDescriptorReuse(candidate != 0);
                    // VM cleanup is outside the timed interval and is fixture
                    // setup, not a change to the production GC policy.
                    lua_gc(mLua.unsafeState(), LUA_GCCOLLECT, 0);
                    const auto start = std::chrono::steady_clock::now();
                    for (unsigned n = 0; n < iterations; ++n)
                    {
                        const auto result = mLua.runInNewSandbox(path, "", packages, {}, &prototype, {}, true);
                        if (!result.valid()) throw std::runtime_error("Benchmark sandbox failed");
                    }
                    const auto ns = std::chrono::duration<double, std::nano>(
                        std::chrono::steady_clock::now() - start).count();
                    nsPerSandbox[candidate].push_back(ns / iterations);
                }
            EXPECT_EQ(factoryCalls, 1 + 2 * blocks * iterations);
            for (unsigned candidate = 0; candidate < 2; ++candidate)
            {
                auto& values = nsPerSandbox[candidate];
                std::sort(values.begin(), values.end());
                const auto median = (values[3] + values[4]) * 0.5;
                std::cout << "NATIVE_LUA_PACKAGE_DESCRIPTOR_BENCHMARK candidate=" << candidate
                    << " ns_per_sandbox_median=" << median << " blocks=" << blocks
                    << " iterations=" << iterations << " common_static=24 api_static=24 factory=1\n";
            }
            mLua.setPackageDescriptorReuse(false);
        });
    }

    TEST_F(LuaStateTest, RemovedScriptsGarbageCollecting)
    {
        auto getMem = [&] {
            for (int i = 0; i < 5; ++i)
                lua_gc(mLua.unsafeState(), LUA_GCCOLLECT, 0);
            return mLua.getTotalMemoryUsage();
        };
        int64_t memWithScript;
        const VFS::Path::Normalized path("requireBig.lua");
        {
            sol::object s = mLua.runInNewSandbox(path);
            memWithScript = getMem();
        }
        for (int i = 0; i < 100; ++i) // run many times to make small memory leaks visible
            mLua.runInNewSandbox(path);
        int64_t memWithoutScript = getMem();
        // At this moment all instances of the script should be garbage-collected.
        EXPECT_LT(memWithoutScript, memWithScript);
    }

    TEST_F(LuaStateTest, SafeIndexMetamethod)
    {
        const VFS::Path::Normalized path("metaIndexError.lua");
        sol::lua_table t = mLua.runInNewSandbox(path);
        // without safe get we crash here
        EXPECT_ERROR(LuaUtil::safeGet(t, "any key"), "meta index error");
    }
}
