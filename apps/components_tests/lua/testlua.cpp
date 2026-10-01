#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
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

    struct LuaStateTest : Test
    {
        std::unique_ptr<VFS::Manager> mVFS = TestingOpenMW::createTestVFS({
            { counterPath, &counterFile },
            { testsPath, &testsFile },
            { invalidPath, &invalidScriptFile },
            { bigPath, &bigScriptFile },
            { requireBigPath, &requireBigScriptFile },
            { metaIndexErrorPath, &metaIndexErrorFile },
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
