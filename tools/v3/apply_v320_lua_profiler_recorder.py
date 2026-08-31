import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.20 profiler match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.20 profiler patched {rel} ({count} match(es))")


# Allow a runtime recording session to close and flush its channel without
# changing the existing nonblocking producer path used by all other diagnostics.
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''        std::atomic<std::size_t> mDroppedLines{ 0 };
        bool mOpenAttempted = false;''',
    '''        std::atomic<std::size_t> mDroppedLines{ 0 };
        std::atomic<bool> mCloseQueued{ false };
        bool mOpenAttempted = false;''',
)
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''                mQueue.push_back({ channel, {}, true });''',
    '''                mQueue.push_back({ channel, {}, true, false });''',
)
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''        void enqueue(const ChannelHandle& channel, const std::string& line)
        {
            if (!channel)
                return;''',
    '''        void enqueue(const ChannelHandle& channel, const std::string& line)
        {
            if (!channel || channel->mCloseQueued.load(std::memory_order_acquire))
                return;''',
)
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''            mQueue.push_back({ channel, line, false });
            lock.unlock();
            mCondition.notify_one();
        }

        ~DiagnosticWriterHub()''',
    '''            mQueue.push_back({ channel, line, false, false });
            lock.unlock();
            mCondition.notify_one();
        }

        void closeChannel(const ChannelHandle& channel)
        {
            if (!channel || channel->mCloseQueued.exchange(true, std::memory_order_acq_rel))
                return;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mStopping)
                    return;
                mQueue.push_back({ channel, {}, false, true });
            }
            mCondition.notify_one();
        }

        ~DiagnosticWriterHub()''',
)
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''            bool mOpenOnly = false;
        };''',
    '''            bool mOpenOnly = false;
            bool mClose = false;
        };''',
)
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''        void writerLoop()
        {''',
    '''        static void finalizeChannel(DiagnosticChannel& channel)
        {
            openChannel(channel);
            if (!channel.mStream.is_open())
                return;
            const std::size_t dropped = channel.mDroppedLines.load(std::memory_order_relaxed);
            if (dropped > 0)
                channel.mStream << "# v3_async_diagnostics_dropped_lines=" << dropped << '\\n';
            channel.mStream.flush();
            channel.mStream.close();
        }

        void writerLoop()
        {''',
)
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''                    openChannel(*item.mChannel);
                    if (!item.mOpenOnly && item.mChannel->mStream.is_open())
                        item.mChannel->mStream << item.mLine << '\\n';''',
    '''                    if (item.mClose)
                    {
                        finalizeChannel(*item.mChannel);
                        continue;
                    }
                    openChannel(*item.mChannel);
                    if (!item.mOpenOnly && item.mChannel->mStream.is_open())
                        item.mChannel->mStream << item.mLine << '\\n';''',
)
replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''                openChannel(*channel);
                if (!channel->mStream.is_open())
                    continue;
                const std::size_t dropped = channel->mDroppedLines.load(std::memory_order_relaxed);
                if (dropped > 0)
                    channel->mStream << "# v3_async_diagnostics_dropped_lines=" << dropped << '\\n';
                channel->mStream.flush();''',
    '''                if (!channel->mCloseQueued.load(std::memory_order_acquire))
                    finalizeChannel(*channel);''',
)

# Preserve the existing custom allocator for memory attribution, but make the
# instruction hook independently switchable on the Lua owner thread.
replace_exact(
    "components/lua/luastate.hpp",
    '''        bool mLogMemoryUsage = false;
    };''',
    '''        bool mLogMemoryUsage = false;
        bool mInstructionProfilerEnabled = false;
    };''',
)
replace_exact(
    "components/lua/luastate.hpp",
    '''        // Note: Lua profiler can not be re-enabled after disabling.
        static void disableProfiler() { sProfilerEnabled = false; }
        static bool isProfilerEnabled() { return sProfilerEnabled; }''',
    '''        // The tracking allocator is fixed when the Lua state is created, but
        // the count hook can be changed safely by the Lua owner thread.
        static void disableProfiler() { sProfilerEnabled = false; }
        static bool isProfilerEnabled() { return sProfilerEnabled; }
        void setInstructionProfilerEnabled(bool enabled);
        bool isInstructionProfilerEnabled() const { return mInstructionProfilerEnabled; }''',
)
replace_exact(
    "components/lua/luastate.hpp",
    '''        uint64_t mWatchdogInstructionCounter = 0;
        std::map<void*, AllocOwner> mBigAllocOwners;''',
    '''        uint64_t mWatchdogInstructionCounter = 0;
        bool mInstructionProfilerEnabled = false;
        std::map<void*, AllocOwner> mBigAllocOwners;''',
)
replace_exact(
    "components/lua/luastate.cpp",
    '''        if (sProfilerEnabled)
            lua_sethook(mLuaState.get(), &countHook, LUA_MASKCOUNT, countHookStep);

        protectedCall([&](LuaView& view) {''',
    '''        if (sProfilerEnabled && mSettings.mInstructionProfilerEnabled)
            setInstructionProfilerEnabled(true);

        protectedCall([&](LuaView& view) {''',
)
replace_exact(
    "components/lua/luastate.cpp",
    '''    void* LuaState::trackingAllocator(void* ud, void* ptr, size_t osize, size_t nsize)
    {''',
    '''    void LuaState::setInstructionProfilerEnabled(bool enabled)
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
    {''',
)

# Keep a raw instruction counter beside the existing 30-frame display average.
replace_exact(
    "components/lua/scriptscontainer.hpp",
    '''            float mAvgInstructionCount = 0; // averaged number of Lua instructions per frame
            int64_t mMemoryUsage = 0; // bytes''',
    '''            float mAvgInstructionCount = 0; // averaged number of Lua instructions per frame
            int64_t mFrameInstructionCount = 0; // exact current-frame hook increments
            int64_t mMemoryUsage = 0; // bytes''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''                if (script.mStats.mAvgInstructionCount < 5)
                    script.mStats.mAvgInstructionCount = 0; // speeding up converge to zero if newValue is zero''',
    '''                if (script.mStats.mAvgInstructionCount < 5)
                    script.mStats.mAvgInstructionCount = 0; // speeding up converge to zero if newValue is zero
                script.mStats.mFrameInstructionCount = 0;''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''            if (it != data->mScripts.end())
                it->second.mStats.mAvgInstructionCount += instructionCount * instructionCountAvgCoef;''',
    '''            if (it != data->mScripts.end())
            {
                it->second.mStats.mAvgInstructionCount += instructionCount * instructionCountAvgCoef;
                it->second.mStats.mFrameInstructionCount += instructionCount;
            }''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''                stats[id].mAvgInstructionCount += script.mStats.mAvgInstructionCount;
                stats[id].mMemoryUsage += script.mStats.mMemoryUsage;''',
    '''                stats[id].mAvgInstructionCount += script.mStats.mAvgInstructionCount;
                stats[id].mFrameInstructionCount += script.mStats.mFrameInstructionCount;
                stats[id].mMemoryUsage += script.mStats.mMemoryUsage;''',
)

# Register a startup capability gate. Normal V3.20 gameplay enables it so the
# existing tracking allocator is available for a runtime recording session;
# inherited/exact-P0 launcher modes force it off to preserve allocator identity.
replace_exact(
    "components/settings/categories/lua.hpp",
    '''        SettingValue<bool> mV320SoundQueryCoalescing{ mIndex, "Lua", "v3.20 sound query coalescing" };''',
    '''        SettingValue<bool> mV320SoundQueryCoalescing{ mIndex, "Lua", "v3.20 sound query coalescing" };
        SettingValue<bool> mV320LuaProfilerRecorderCapable{ mIndex, "Lua",
            "v3.20 lua profiler recorder capability" };''',
)
replace_exact(
    "files/settings-default.cfg",
    '''v3.20 sound query coalescing = false''',
    '''v3.20 sound query coalescing = false
# Keeps the stock engine-side Lua profiler allocator available so a recorder
# session can start and stop at runtime. Recording itself remains off by default.
v3.20 lua profiler recorder capability = true''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''    static LuaUtil::LuaStateSettings createLuaStateSettings()
    {
        if (!Settings::lua().mLuaProfiler)
            LuaUtil::LuaState::disableProfiler();
        return { .mInstructionLimit = Settings::lua().mInstructionLimitPerCall,
            .mMemoryLimit = Settings::lua().mMemoryLimit,
            .mSmallAllocMaxSize = Settings::lua().mSmallAllocMaxSize,
            .mLogMemoryUsage = Settings::lua().mLogMemoryUsage };
    }''',
    '''    static LuaUtil::LuaStateSettings createLuaStateSettings()
    {
        bool recorderCapable = Settings::lua().mV320LuaProfilerRecorderCapable;
        if (const char* value = std::getenv("OPENMW_V320_LUA_PROFILER_CAPABLE"); value && *value)
            recorderCapable = std::atoi(value) != 0;
        if (!Settings::lua().mLuaProfiler && !recorderCapable)
            LuaUtil::LuaState::disableProfiler();
        return { .mInstructionLimit = Settings::lua().mInstructionLimitPerCall,
            .mMemoryLimit = Settings::lua().mMemoryLimit,
            .mSmallAllocMaxSize = Settings::lua().mSmallAllocMaxSize,
            .mLogMemoryUsage = Settings::lua().mLogMemoryUsage,
            .mInstructionProfilerEnabled = Settings::lua().mLuaProfiler };
    }''',
)

replace_exact(
    "apps/openmw/mwlua/luamanagerimp.hpp",
    '''namespace MWLua
{
    // \\brief LuaManager is the central interface through which the engine invokes lua scripts.''',
    '''namespace MWLua
{
    class V320LuaProfilerRecorder;

    // \\brief LuaManager is the central interface through which the engine invokes lua scripts.''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.hpp",
    '''        void reloadAllScriptsImpl();
        void synchronizedUpdateUnsafe();''',
    '''        void reloadAllScriptsImpl();
        void synchronizedUpdateUnsafe();
        void processV320LuaProfilerBoundary();
        void recordV320LuaProfilerFrame();''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.hpp",
    '''        LuaUtil::ScriptsConfiguration mConfiguration;
        LuaUtil::LuaState mLua;
        LuaUi::ResourceManager mUiResourceManager;''',
    '''        LuaUtil::ScriptsConfiguration mConfiguration;
        LuaUtil::LuaState mLua;
        std::unique_ptr<V320LuaProfilerRecorder> mV320LuaProfilerRecorder;
        LuaUi::ResourceManager mUiResourceManager;''',
)

replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''#include <filesystem>

#include <MyGUI_InputManager.h>''',
    '''#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <system_error>

#include <MyGUI_InputManager.h>''',
)

recorder_class = r'''
    class V320LuaProfilerRecorder
    {
    public:
        using Stats = LuaUtil::ScriptsContainer::ScriptStats;

        ~V320LuaProfilerRecorder()
        {
            if (mChannel)
                Debug::V3Diagnostics::DiagnosticWriterHub::instance().closeChannel(mChannel);
        }

        std::string requestStart()
        {
            if (mRequested || mActive)
                return "Lua profiler recording is already requested/active: " + mPath;

            std::error_code error;
            std::filesystem::path directory;
            if (const char* raw = std::getenv("OPENMW_V320_LUA_PROFILE_DIR"); raw && *raw)
                directory = std::filesystem::u8path(raw);
            else
                directory = std::filesystem::current_path(error);
            if (error)
                return "Unable to resolve the Lua profiler output directory: " + error.message();
            std::filesystem::create_directories(directory, error);
            if (error)
                return "Unable to create the Lua profiler output directory: " + error.message();

            mPath = (directory / ("v320-lua-profiler-"
                + std::to_string(Debug::V3Diagnostics::epochMs()) + ".csv")).string();
            mChannel = Debug::V3Diagnostics::DiagnosticWriterHub::instance().registerChannel(mPath,
                "session,frame,epoch_ms,script_index,script_path,scope,frame_ops,active_memory_bytes,"
                "inactive_memory_bytes,total_memory_bytes");
            if (!mChannel)
            {
                mPath.clear();
                return "Unable to create a Lua profiler recording channel";
            }
            Debug::V3Diagnostics::DiagnosticWriterHub::instance().enqueue(mChannel,
                "# sparse rows: frame_total is written every frame; script rows are written for nonzero ops or "
                "memory changes; omitted script rows mean zero ops and unchanged memory");
            mRequested = true;
            return "Lua profiler recording start requested: " + mPath;
        }

        std::string requestStop()
        {
            if (!mRequested && !mActive)
                return "Lua profiler recording is already stopped";
            mRequested = false;
            return "Lua profiler recording stop requested; the current partial frame will be retained";
        }

        std::string status() const
        {
            const char* state = mActive ? "active" : (mRequested ? "start-pending" : "stopped");
            return std::string("Lua profiler recording is ") + state
                + (mPath.empty() ? std::string{} : ": " + mPath);
        }

        bool requested() const { return mRequested; }
        bool active() const { return mActive; }

        void activate()
        {
            if (!mChannel)
                return;
            ++mSession;
            mLastMemory.clear();
            mActive = true;
        }

        void deactivate()
        {
            mActive = false;
            if (mChannel)
                Debug::V3Diagnostics::DiagnosticWriterHub::instance().closeChannel(mChannel);
            mChannel.reset();
            mPath.clear();
            mLastMemory.clear();
        }

        void writeFrame(const LuaUtil::ScriptsConfiguration& configuration, const LuaUtil::LuaState& lua,
            const std::vector<Stats>& activeStats, const std::vector<Stats>& executionStats)
        {
            if (!mActive || !mChannel)
                return;
            const std::uint64_t frame = Debug::V3HitchTelemetry::currentFrame();
            const long long epoch = Debug::V3Diagnostics::epochMs();
            if (mLastMemory.size() != configuration.size())
                mLastMemory.assign(configuration.size(), { std::numeric_limits<std::uint64_t>::max(),
                    std::numeric_limits<std::uint64_t>::max() });

            std::uint64_t totalOps = 0;
            std::uint64_t totalActiveMemory = 0;
            std::uint64_t totalInactiveMemory = 0;
            for (std::size_t i = 0; i < configuration.size(); ++i)
            {
                const std::uint64_t ops = executionStats[i].mFrameInstructionCount > 0
                    ? static_cast<std::uint64_t>(executionStats[i].mFrameInstructionCount) : 0;
                const std::uint64_t activeMemory = activeStats[i].mMemoryUsage > 0
                    ? static_cast<std::uint64_t>(activeStats[i].mMemoryUsage) : 0;
                const std::uint64_t attributedMemory = lua.getMemoryUsageByScriptIndex(static_cast<unsigned>(i));
                const std::uint64_t inactiveMemory
                    = attributedMemory > activeMemory ? attributedMemory - activeMemory : 0;
                totalOps += ops;
                totalActiveMemory += activeMemory;
                totalInactiveMemory += inactiveMemory;

                const std::pair memory{ activeMemory, inactiveMemory };
                if (ops == 0 && memory == mLastMemory[i])
                    continue;
                mLastMemory[i] = memory;

                const bool global = configuration[i].mFlags & ESM::LuaScriptCfg::sGlobal;
                const bool menu = configuration[i].mFlags & ESM::LuaScriptCfg::sMenu;
                const std::string_view scope = global ? "global" : (menu ? "menu" : "local");
                std::ostringstream row;
                row << mSession << ',' << frame << ',' << epoch << ',' << i << ','
                    << Debug::V3Diagnostics::csvQuote(configuration[i].mScriptPath.value()) << ',' << scope << ','
                    << ops << ',' << activeMemory << ',' << inactiveMemory << ',' << attributedMemory;
                Debug::V3Diagnostics::DiagnosticWriterHub::instance().enqueue(mChannel, row.str());
            }

            std::ostringstream total;
            total << mSession << ',' << frame << ',' << epoch << ",-1,\"[frame_total]\",all," << totalOps << ','
                << totalActiveMemory << ',' << totalInactiveMemory << ',' << lua.getTotalMemoryUsage();
            Debug::V3Diagnostics::DiagnosticWriterHub::instance().enqueue(mChannel, total.str());
        }

    private:
        Debug::V3Diagnostics::DiagnosticWriterHub::ChannelHandle mChannel;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> mLastMemory;
        std::string mPath;
        std::uint64_t mSession = 0;
        bool mRequested = false;
        bool mActive = false;
    };
'''
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''    static LuaUtil::LuaStateSettings createLuaStateSettings()
    {''',
    recorder_class + '''
    static LuaUtil::LuaStateSettings createLuaStateSettings()
    {''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''    LuaManager::LuaManager(const VFS::Manager* vfs, const std::filesystem::path& libsDir)
        : mLua(vfs, &mConfiguration, createLuaStateSettings())
    {''',
    '''    LuaManager::LuaManager(const VFS::Manager* vfs, const std::filesystem::path& libsDir)
        : mLua(vfs, &mConfiguration, createLuaStateSettings())
        , mV320LuaProfilerRecorder(std::make_unique<V320LuaProfilerRecorder>())
    {''',
)

boundary_methods = r'''
    void LuaManager::recordV320LuaProfilerFrame()
    {
        using Stats = LuaUtil::ScriptsContainer::ScriptStats;
        std::vector<Stats> activeStats;
        mGlobalScripts.collectStats(activeStats);
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->collectStats(activeStats);
        }
        std::vector<Stats> executionStats = activeStats;
        mMenuScripts.collectStats(executionStats);
        mV320LuaProfilerRecorder->writeFrame(mConfiguration, mLua, activeStats, executionStats);
    }

    void LuaManager::processV320LuaProfilerBoundary()
    {
        if (mV320LuaProfilerRecorder->active())
            recordV320LuaProfilerFrame();

        mGlobalScripts.statsNextFrame();
        mMenuScripts.statsNextFrame();
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->statsNextFrame();
        }

        const bool requested = mV320LuaProfilerRecorder->requested();
        if (requested == mV320LuaProfilerRecorder->active())
            return;
        if (requested)
        {
            mLua.setInstructionProfilerEnabled(true);
            mV320LuaProfilerRecorder->activate();
        }
        else
        {
            mLua.setInstructionProfilerEnabled(Settings::lua().mLuaProfiler);
            mV320LuaProfilerRecorder->deactivate();
        }
    }
'''
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''    void LuaManager::update()
    {''',
    boundary_methods + '''
    void LuaManager::update()
    {''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        Debug::V33LuaTrace::FrameScope v33LuaTraceFrame;''',
    '''        auto phaseStart = startPhase();
        processV320LuaProfilerBoundary();
        statsMs = finishPhase(phaseStart);

        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        Debug::V33LuaTrace::FrameScope v33LuaTraceFrame;''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        auto phaseStart = startPhase();
        mObjectLists.update();''',
    '''        phaseStart = startPhase();
        mObjectLists.update();''',
)
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        phaseStart = startPhase();
        mGlobalScripts.statsNextFrame();
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
            asLocal(ptr)->statsNextFrame();
        statsMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        mLuaEvents.finalizeEventBatch();''',
    '''        phaseStart = startPhase();
        mLuaEvents.finalizeEventBatch();''',
)

console_prefix = r'''
        if (consoleMode.empty())
        {
            std::istringstream input(command);
            std::string recorderCommand;
            std::string action;
            input >> recorderCommand >> action;
            if (recorderCommand == "luaProfilerRecord")
            {
                std::string message;
                if (action == "start" || action == "on")
                {
                    if (!LuaUtil::LuaState::isProfilerEnabled())
                        message = "Lua profiler recording capability is disabled in this exact-control process";
                    else
                        message = mV320LuaProfilerRecorder->requestStart();
                }
                else if (action == "stop" || action == "off")
                    message = mV320LuaProfilerRecorder->requestStop();
                else if (action == "status")
                    message = mV320LuaProfilerRecorder->status();
                else
                    message = "Usage: luaProfilerRecord start|stop|status";
                MWBase::Environment::get().getWindowManager()->printToConsole(
                    message + "\n", MWBase::WindowManager::sConsoleColor_Success);
                return;
            }
        }

'''
replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''    {
        PlayerScripts* playerScripts = nullptr;
        if (!mPlayer.isEmpty())''',
    '''    {''' + console_prefix + '''        PlayerScripts* playerScripts = nullptr;
        if (!mPlayer.isEmpty())''',
)

# Make the lab profile directory available as the runtime-selected recorder
# destination without enabling the recorder itself.
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    "$V320SoundConversionCache = '0'\n"
    "$V320SoundQueryCoalescing = '0'\n"
    "$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }",
    "$V320SoundConversionCache = '0'\n"
    "$V320SoundQueryCoalescing = '0'\n"
    "$V320LuaProfilerRecorderCapable = '0'\n"
    "$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }",
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''}

if ([int]$choice -ge 24) {''',
    '''}

# Preserve inherited and exact-P0 allocator identity. Substantive V3.20 modes
# expose the runtime recorder capability, but recording remains off until the
# in-game console start command is issued.
if ([int]$choice -ge 110 -and $choice -notin @('114','118')) {
    $V320LuaProfilerRecorderCapable = '1'
}

if ([int]$choice -ge 24) {''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v320_sound_query_coalescing=$V320SoundQueryCoalescing",''',
    '''    "v320_sound_query_coalescing=$V320SoundQueryCoalescing",
    "v320_lua_profiler_recorder_capable=$V320LuaProfilerRecorderCapable",''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
    '''    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST','OPENMW_V320_LUA_PROFILE_DIR',
    'OPENMW_V320_LUA_PROFILER_CAPABLE'
)''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    "$env:OPENMW_V3_HITCH_FILE = Join-Path $ProfileDir 'v3-hitch.csv'",
    "$env:OPENMW_V320_LUA_PROFILE_DIR = $ProfileDir\n"
    "$env:OPENMW_V3_HITCH_FILE = Join-Path $ProfileDir 'v3-hitch.csv'",
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    $env:OPENMW_V320_SOUND_QUERY_COALESCING = $V320SoundQueryCoalescing''',
    '''    $env:OPENMW_V320_SOUND_QUERY_COALESCING = $V320SoundQueryCoalescing
    $env:OPENMW_V320_LUA_PROFILER_CAPABLE = $V320LuaProfilerRecorderCapable''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''finally {
    Remove-Item Env:OPENMW_V320_SOUND_QUERY_COALESCING''',
    '''finally {
    Remove-Item Env:OPENMW_V320_LUA_PROFILE_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V320_LUA_PROFILER_CAPABLE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V320_SOUND_QUERY_COALESCING''',
)

print("V3.20 runtime Lua profiler recorder layer applied")
