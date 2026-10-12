#include <gtest/gtest.h>

#include <components/lua/luastate.hpp>
#include <components/lua/scriptscontainer.hpp>
#include <components/testing/expecterror.hpp>
#include <components/testing/util.hpp>

#include <functional>
#include <limits>

namespace LuaUtil
{
    struct PreparedScriptRootTestAccess
    {
        static bool admissionIdle(const LuaState& state)
        {
            return !state.mPreparingScriptRoot && !state.mPreparedAllocationActive
                && !state.mPreparedAllocationMayDeny;
        }
        static void exhaustEpoch(LuaState& state)
        {
            state.mScriptCacheEpoch = std::numeric_limits<std::uint64_t>::max();
        }
        static void beginAccounting(LuaState& state, std::uint64_t limit)
        {
            state.mPreparedAllocationGrowth = 0;
            state.mPreparedAllocationLimit = limit;
            state.mPreparedAllocationActive = state.mPreparedAllocationMayDeny = true;
        }
        static std::uint64_t endAccounting(LuaState& state)
        {
            state.mPreparedAllocationActive = state.mPreparedAllocationMayDeny = false;
            return state.mPreparedAllocationGrowth;
        }
    };
}

namespace
{
    using namespace LuaUtil;
    using namespace TestingOpenMW;

    constexpr VFS::Path::NormalizedView sRoot("root.lua");
    constexpr VFS::Path::NormalizedView sCaller("caller.lua");
    constexpr VFS::Path::NormalizedView sModule("test/module.lua");
    constexpr VFS::Path::NormalizedView sError("error.lua");
    constexpr VFS::Path::NormalizedView sInvalid("invalid.lua");
    constexpr VFS::Path::NormalizedView sGlobal("global.lua");
    constexpr VFS::Path::NormalizedView sContainer("container.lua");

    VFSTestFile rootFile(R"X(
local host = require('test.host')
host.enter()
x = 10
local n = 0
return {
    get = function() return x end,
    inc = function() x = x + 1; n = n + 1; return n end,
    mutable = { value = 1 },
    owner = host.id,
}
)X");
    VFSTestFile callerFile(R"X(
local co = coroutine.create(function()
    local module = require('test.module')
    return module.value
end)
return { step = function(...) return coroutine.resume(co, ...) end }
)X");
    VFSTestFile moduleFile("local value = coroutine.yield('module-yield'); return {value=value}");
    VFSTestFile errorFile("require('test.host').enter(); error('root_failure')");
    VFSTestFile invalidFile("this is not valid Lua");
    VFSTestFile globalFile("return true");
    VFSTestFile containerFile(R"X(
local host = require('test.host')
host.enter()
local n = 0
return { engineHandlers = {
    onInit = function() host.lifecycle('init') end,
    onLoad = function(saved) n = saved.n; host.lifecycle('load') end,
    onSave = function() host.lifecycle('save'); return {n=n} end,
    onUpdate = function() n = n + 1; host.seen(n) end,
} }
)X");

    struct LuaPreparedScriptRootsTest : testing::Test
    {
        std::unique_ptr<VFS::Manager> mVFS = createTestVFS({ { sRoot, &rootFile }, { sCaller, &callerFile },
            { sModule, &moduleFile }, { sError, &errorFile }, { sInvalid, &invalidFile }, { sGlobal, &globalFile },
            { sContainer, &containerFile } });
        ScriptsConfiguration mCfg;
        LuaState mLua{ mVFS.get(), &mCfg };
        // Registry references and callback targets outlive calls, and the
        // references are released before the owning LuaState is destroyed.
        std::vector<sol::function> mEscapedRoots;
        int mBodyCalls = 0, mFactoryCalls = 0;
        std::function<void()> mOnEnter;
        std::vector<std::string> mLifecycles;
        std::vector<int> mSeen;

        LuaPreparedScriptRootsTest()
        {
            ESM::LuaScriptsCfg cfg;
            parseOMWScripts(cfg, "CUSTOM: root.lua\nCUSTOM: caller.lua\nCUSTOM: test/module.lua\n"
                                "CUSTOM: error.lua\nCUSTOM: invalid.lua\nGLOBAL: global.lua\nCUSTOM: container.lua\n");
            mCfg.init(std::move(cfg), false);
            mLua.protectedCall([&](LuaView& view) {
                auto hostFactory = sol::make_object(view.sol(), [&](sol::object hidden) {
                    ++mFactoryCalls;
                    sol::table host(mLua.unsafeState(), sol::create);
                    host["id"] = hidden.is<sol::table>() ? hidden.as<sol::table>()["id"].get_or(0) : 0;
                    host["lifecycle"] = [&](std::string value) { mLifecycles.push_back(std::move(value)); };
                    host["seen"] = [&](int value) { mSeen.push_back(value); };
                    host["enter"] = [&](sol::this_state thisState) {
                        ++mBodyCalls;
                        lua_State* state = thisState;
                        lua_Debug frame{};
                        if (lua_getstack(state, 1, &frame) && lua_getinfo(state, "f", &frame))
                        {
                            // An adversarial native API can retain the executing
                            // root. Single-use preparation must still be safe.
                            mEscapedRoots.push_back(sol::stack::get<sol::function>(state, -1));
                            lua_pop(state, 1);
                        }
                        if (mOnEnter) mOnEnter();
                    };
                    return host;
                });
                mLua.addCommonPackage("test.host", hostFactory);
            });
        }

        void warm()
        {
            ASSERT_TRUE(mLua.diagnosticTrackingAllocator());
            EXPECT_EQ(mLua.precompileConfiguredScripts(), 6); // invalid optional source is skipped
            EXPECT_EQ(mBodyCalls, 0);
            EXPECT_EQ(mFactoryCalls, 0);
        }
        void SetUp() override
        {
#if LUA_VERSION_NUM != 501
            GTEST_SKIP() << "Preparation requires the actual Lua5.1 lua_cpcall API";
#endif
        }
        void enable(std::size_t roots = 2, std::uint64_t bytes = 2 * 1024 * 1024)
        {
            ASSERT_TRUE(mLua.configurePreparedScriptRoots({ true, roots, bytes }));
        }
    };

    TEST_F(LuaPreparedScriptRootsTest, DefaultOffAndMissingBytecodeKeepOrdinaryPath)
    {
        EXPECT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot), 2), 0);
        EXPECT_EQ(mLua.preparedScriptRootStats().mCreated, 0);
        enable();
        EXPECT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 0);
        EXPECT_EQ(mLua.preparedScriptRootStats().mMissingBytecodeRefusals, 1);
        sol::table script = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(script["get"]).get<int>(), 10);
        EXPECT_EQ(mLua.preparedScriptRootStats().mOrdinaryDirectLoads, 1);
        EXPECT_EQ(mBodyCalls, 1);
        EXPECT_TRUE(PreparedScriptRootTestAccess::admissionIdle(mLua));
    }

    TEST_F(LuaPreparedScriptRootsTest, FreshOneUseRootsPreserveEscapedFunctionsAndIndependentState)
    {
        warm();
        enable();
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot), 2), 2);
        EXPECT_EQ(mBodyCalls, 0);
        EXPECT_EQ(mFactoryCalls, 0);
        EXPECT_GT(mLua.preparedScriptRootStats().mPendingPositiveBytes, 0);
        EXPECT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 0);
        sol::table first = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(first["inc"]).get<int>(), 1);
        first["mutable"].get<sol::table>()["value"] = 9;
        sol::table second = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(second["get"]).get<int>(), 10);
        EXPECT_EQ(call(second["inc"]).get<int>(), 1);
        EXPECT_EQ(second["mutable"].get<sol::table>()["value"].get<int>(), 1);
        EXPECT_EQ(call(first["get"]).get<int>(), 11);
        sol::table fallback = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(fallback["get"]).get<int>(), 10);
        ASSERT_EQ(mEscapedRoots.size(), 3);
        EXPECT_FALSE(mEscapedRoots[0] == mEscapedRoots[1]);
        EXPECT_FALSE(mEscapedRoots[1] == mEscapedRoots[2]);
        const auto stats = mLua.preparedScriptRootStats();
        EXPECT_EQ(stats.mCreated, 2);
        EXPECT_EQ(stats.mConsumed, 2);
        EXPECT_EQ(stats.mOrdinaryDirectLoads, 1);
        EXPECT_EQ(stats.mOrdinaryBytecodeLoads, 1);
        EXPECT_EQ(stats.mPendingRoots, 0);
        EXPECT_EQ(stats.mPendingPositiveBytes, 0);
        EXPECT_EQ(stats.mTransferredPositiveBytes, stats.mPeakPendingPositiveBytes);
        EXPECT_EQ(mBodyCalls, 3);
        EXPECT_EQ(mFactoryCalls, 3);
    }

    TEST_F(LuaPreparedScriptRootsTest, ClearAndVfsEpochInvalidateBeforeConsumption)
    {
        warm();
        enable();
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot), 2), 2);
        sol::table old = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        mLua.dropScriptCache();
        EXPECT_EQ(mLua.preparedScriptRootStats().mDiscarded, 1);
        VFSTestFile replacement("return {get=function() return 900 end}");
        VFS::FileMap files;
        files.emplace(VFS::Path::Normalized(sRoot), &replacement);
        mVFS->addArchive(std::make_unique<VFSTestData>(std::move(files)));
        mVFS->buildIndex();
        sol::table changed = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(changed["get"]).get<int>(), 900);
        EXPECT_EQ(call(old["get"]).get<int>(), 10);
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 1);
        mVFS->buildIndex();
        sol::table sameBytecode = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(sameBytecode["get"]).get<int>(), 900);
        EXPECT_GT(mLua.preparedScriptRootStats().mStaleRefusals, 0);
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 0);
    }

    TEST_F(LuaPreparedScriptRootsTest, RealPackagePrototypeAndHiddenDataRemainPerSandbox)
    {
        warm();
        mLua.setPackagePrototypeReuse(true);
        mLua.setPackageDescriptorReuse(true);
        enable();
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot), 2), 2);
        std::optional<sol::main_table> prototype;
        sol::main_object firstHidden, secondHidden;
        const std::map<std::string, sol::main_object> packages;
        mLua.protectedCall([&](LuaView& view) {
            firstHidden = view.sol().create_table_with("id", 1);
            secondHidden = view.sol().create_table_with("id", 2);
            prototype.emplace(mLua.makePackagePrototype(packages));
        });
        sol::table first = mLua.runInNewSandbox(
            VFS::Path::Normalized(sRoot), "one", packages, firstHidden, &*prototype, {}, true);
        sol::table second = mLua.runInNewSandbox(
            VFS::Path::Normalized(sRoot), "two", packages, secondHidden, &*prototype, {}, true);
        EXPECT_EQ(first["owner"].get<int>(), 1);
        EXPECT_EQ(second["owner"].get<int>(), 2);
        EXPECT_EQ(mFactoryCalls, 2);
        EXPECT_EQ(mBodyCalls, 2);
        EXPECT_EQ(mLua.preparedScriptRootStats().mConsumed, 2);
    }

    TEST_F(LuaPreparedScriptRootsTest, ActualContainersKeepInitLoadSaveAndIndependentHandlers)
    {
        warm();
        enable(3);
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sContainer), 3), 3);
        ScriptsContainer first(&mLua, "first"), second(&mLua, "second");
        const int id = *mCfg.findId(sContainer);
        ASSERT_TRUE(first.addCustomScript(id));
        ASSERT_TRUE(second.addCustomScript(id));
        first.update(0.1f);
        second.update(0.1f);
        ESM::LuaScripts saved;
        first.save(saved);
        second.load(saved);
        second.update(0.1f);
        EXPECT_EQ(mSeen, (std::vector<int>{ 1, 1, 2 }));
        EXPECT_EQ(mLifecycles, (std::vector<std::string>{ "init", "init", "save", "load" }));
        EXPECT_EQ(mBodyCalls, 3);
        EXPECT_EQ(mLua.preparedScriptRootStats().mConsumed, 3);
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 0);
    }

    TEST_F(LuaPreparedScriptRootsTest, ReentrantClearAndPreparationDoNotRecycleActiveRoot)
    {
        warm();
        enable();
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot), 2), 2);
        std::size_t preparedInside = 99;
        mOnEnter = [&] {
            mLua.dropScriptCache();
            // This now has no cached bytecode; there is no body omission or
            // sharing of the current root regardless of observer eligibility.
            preparedInside = mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot));
        };
        sol::table active = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(preparedInside, 0);
        EXPECT_EQ(call(active["inc"]).get<int>(), 1);
        EXPECT_EQ(mLua.preparedScriptRootStats().mConsumed, 1);
        EXPECT_EQ(mLua.preparedScriptRootStats().mDiscarded, 1);
        EXPECT_TRUE(PreparedScriptRootTestAccess::admissionIdle(mLua));
    }

    TEST_F(LuaPreparedScriptRootsTest, ErrorsConsumeOnceAndLeaveAdmissionAndStackUsable)
    {
        warm();
        enable();
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sError), 2), 2);
        const int top = lua_gettop(mLua.unsafeState());
        EXPECT_ERROR(mLua.runInNewSandbox(VFS::Path::Normalized(sError)), "root_failure");
        EXPECT_ERROR(mLua.runInNewSandbox(VFS::Path::Normalized(sError)), "root_failure");
        EXPECT_EQ(mLua.preparedScriptRootStats().mConsumed, 2);
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 0);
        EXPECT_EQ(lua_gettop(mLua.unsafeState()), top);
        EXPECT_TRUE(PreparedScriptRootTestAccess::admissionIdle(mLua));
        ASSERT_EQ(mEscapedRoots.size(), 2);
        EXPECT_FALSE(mEscapedRoots[0] == mEscapedRoots[1]);
        EXPECT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sInvalid)), 0);
        EXPECT_ERROR(mLua.runInNewSandbox(VFS::Path::Normalized(sInvalid)), "Lua error:");
    }

    TEST_F(LuaPreparedScriptRootsTest, OrdinaryModuleYieldDoesNotConsumePreparedModule)
    {
        warm();
        enable();
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sCaller)), 1);
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sModule)), 1);
        sol::table caller = mLua.runInNewSandbox(VFS::Path::Normalized(sCaller));
        auto yielded = call(caller["step"]);
        EXPECT_TRUE(yielded.get<bool>(0));
        EXPECT_EQ(yielded.get<std::string>(1), "module-yield");
        auto resumed = call(caller["step"], 123);
        EXPECT_TRUE(resumed.get<bool>(0));
        EXPECT_EQ(resumed.get<int>(1), 123);
        const auto stats = mLua.preparedScriptRootStats();
        EXPECT_EQ(stats.mConsumed, 1);
        EXPECT_EQ(stats.mOrdinaryModuleLoads, 1);
        EXPECT_EQ(stats.mOrdinaryBytecodeLoads, 1);
        EXPECT_EQ(stats.mPendingRoots, 1);
    }

    TEST_F(LuaPreparedScriptRootsTest, BudgetDenialIsOptionalAndNeverChargesGcNetDelta)
    {
        warm();
        enable();
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 1);
        const auto nativeBytes = mLua.preparedScriptRootStats().mPreparationNativeBytes;
        ASSERT_GT(nativeBytes, 0);
        mLua.discardPreparedScriptRoots();
        enable(2, nativeBytes); // native pin fits exactly; no positive Lua room
        EXPECT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 0);
        EXPECT_GT(mLua.preparedScriptRootStats().mBudgetRefusals, 0);
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 0);
        EXPECT_TRUE(PreparedScriptRootTestAccess::admissionIdle(mLua));
        sol::table ordinary = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(ordinary["get"]).get<int>(), 10);
        std::uint64_t growth = 0;
        mLua.protectedCall([&](LuaView& view) {
            void* ud = nullptr;
            auto allocator = lua_getallocf(view.sol().lua_state(), &ud);
            PreparedScriptRootTestAccess::beginAccounting(mLua, 64);
            void* memory = allocator(ud, nullptr, 0, 64);
            EXPECT_NE(memory, nullptr);
            allocator(ud, memory, 64, 0);
            // The real tracked allocator's heap net change is now zero, but
            // successful positive growth remains charged and the next is denied.
            EXPECT_EQ(allocator(ud, nullptr, 0, 1), nullptr);
            growth = PreparedScriptRootTestAccess::endAccounting(mLua);
        });
        EXPECT_EQ(growth, 64);
    }

    TEST_F(LuaPreparedScriptRootsTest, UntrackedAllocatorAndEpochExhaustionRefuseConservatively)
    {
        warm();
        enable();
        struct AllocatorForwarder
        {
            lua_Alloc mAllocator;
            void* mUserData;
        } forwarder;
        auto state = mLua.unsafeState();
        forwarder.mAllocator = lua_getallocf(state, &forwarder.mUserData);
        lua_setallocf(state, [](void* ud, void* ptr, std::size_t oldSize, std::size_t newSize) -> void* {
            const auto* f = static_cast<AllocatorForwarder*>(ud);
            return f->mAllocator(f->mUserData, ptr, oldSize, newSize);
        }, &forwarder);
        EXPECT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 0);
        lua_setallocf(state, forwarder.mAllocator, forwarder.mUserData);
        EXPECT_EQ(mLua.preparedScriptRootStats().mUntrackedRefusals, 1);
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 1);
        PreparedScriptRootTestAccess::exhaustEpoch(mLua);
        mLua.dropScriptCache();
        EXPECT_FALSE(mLua.configurePreparedScriptRoots({ true, 2, 2 * 1024 * 1024 }));
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 0);
        sol::table ordinary = mLua.runInNewSandbox(VFS::Path::Normalized(sRoot));
        EXPECT_EQ(call(ordinary["get"]).get<int>(), 10);
    }

    TEST_F(LuaPreparedScriptRootsTest, ActualVmHardLimitContainsEntryOomAndPreservesFollowingOrdinaryCall)
    {
        // Fill the actual tracked global policy to its limit after a full GC.
        // The next C-entry closure allocation occurs before the callback body;
        // lua_cpcall must catch that failure rather than invoking a panic.
        constexpr std::uint64_t limit = 16 * 1024 * 1024;
        LuaStateSettings settings;
        settings.mMemoryLimit = limit;
        settings.mSmallAllocMaxSize = 0;
        LuaState bounded(mVFS.get(), &mCfg, settings);
        ASSERT_TRUE(bounded.diagnosticTrackingAllocator());
        EXPECT_EQ(bounded.precompileConfiguredScripts(), 6);
        ASSERT_TRUE(bounded.configurePreparedScriptRoots({ true, 2, limit }));
        lua_State* state = bounded.unsafeState();
        lua_gc(state, LUA_GCCOLLECT, 0);
        lua_gc(state, LUA_GCSTOP, 0);
        const auto before = bounded.getTotalMemoryUsage();
        ASSERT_LT(before, limit);
        void* ud = nullptr;
        auto allocator = lua_getallocf(state, &ud);
        const auto heldSize = static_cast<std::size_t>(limit - before);
        void* held = allocator(ud, nullptr, 0, heldSize);
        ASSERT_NE(held, nullptr);
        const int top = lua_gettop(state);
        std::size_t created = 99;
        EXPECT_NO_THROW(created = bounded.prepareScriptRoots(VFS::Path::Normalized(sRoot)));
        const auto refused = bounded.preparedScriptRootStats();
        // Release the external live allocation before any ordinary Lua work.
        allocator(ud, held, heldSize, 0);
        lua_gc(state, LUA_GCRESTART, 0);
        EXPECT_EQ(created, 0);
        EXPECT_GT(refused.mFailures, 0);
        EXPECT_EQ(refused.mPendingRoots, 0);
        EXPECT_EQ(lua_gettop(state), top);
        EXPECT_TRUE(PreparedScriptRootTestAccess::admissionIdle(bounded));
        EXPECT_TRUE(bounded.runInNewSandbox(VFS::Path::Normalized(sGlobal)).get<bool>());
    }

    TEST_F(LuaPreparedScriptRootsTest, SameAllocatorWithForeignVmUserdataCannotBorrowAdmission)
    {
        warm();
        enable();
        LuaState foreign(mVFS.get(), &mCfg);
        auto state = mLua.unsafeState();
        void* localOwner = nullptr;
        auto allocator = lua_getallocf(state, &localOwner);
        void* foreignOwner = nullptr;
        ASSERT_EQ(lua_getallocf(foreign.unsafeState(), &foreignOwner), allocator);
        ASSERT_NE(localOwner, foreignOwner);
        const auto localHeap = mLua.getTotalMemoryUsage();
        const auto foreignHeap = foreign.getTotalMemoryUsage();
        // Install the same actual function but a different owner's counters.
        // No Lua allocation is permitted while this deliberate mismatch exists.
        lua_setallocf(state, allocator, foreignOwner);
        const auto created = mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot));
        lua_setallocf(state, allocator, localOwner);
        EXPECT_EQ(created, 0);
        EXPECT_EQ(mLua.preparedScriptRootStats().mUntrackedRefusals, 1);
        EXPECT_EQ(mLua.getTotalMemoryUsage(), localHeap);
        EXPECT_EQ(foreign.getTotalMemoryUsage(), foreignHeap);
        EXPECT_EQ(foreign.preparedScriptRootStats().mCreated, 0);
        ASSERT_EQ(mLua.prepareScriptRoots(VFS::Path::Normalized(sRoot)), 1);
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 1);
        EXPECT_EQ(foreign.preparedScriptRootStats().mPendingRoots, 0);
    }

    TEST_F(LuaPreparedScriptRootsTest, ConfiguredFillIsBoundedAndSkipsGlobalStartupRoots)
    {
        warm();
        enable(2);
        EXPECT_EQ(mLua.prepareConfiguredScriptRoots(), 2);
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 2);
        EXPECT_EQ(mBodyCalls, 0);
        EXPECT_EQ(mFactoryCalls, 0);
        EXPECT_LE(mLua.preparedScriptRootStats().mPendingPositiveBytes, 2 * 1024 * 1024);
        EXPECT_GT(mLua.preparedScriptRootMetadataBytes(), 0);
        EXPECT_TRUE(mLua.runInNewSandbox(VFS::Path::Normalized(sGlobal)).get<bool>());
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 2);
        EXPECT_EQ(mLua.preparedScriptRootStats().mOrdinaryDirectLoads, 1);
        EXPECT_TRUE(mLua.configurePreparedScriptRoots({}));
        EXPECT_EQ(mLua.preparedScriptRootStats().mPendingRoots, 0);
        EXPECT_EQ(mLua.prepareConfiguredScriptRoots(), 0);
    }
}
