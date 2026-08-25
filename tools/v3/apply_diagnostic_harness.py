from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"patched {rel}")

def replace_all(rel, old, new, minimum=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count < minimum:
        raise RuntimeError(f"{rel}: expected at least {minimum} matches, found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"patched {rel} ({count} replacements)")

# ---------------------------------------------------------------------------
# Real settings.cfg RAM-cache preset.
# ---------------------------------------------------------------------------
replace_once(
    "components/settings/categories/cells.hpp",
    '''        using WithIndex::WithIndex;

        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
    '''        using WithIndex::WithIndex;

        SettingValue<std::string> mRamCacheMode{ mIndex, "Cells", "ram cache mode",
            makeEnumSanitizerString({ "normal", "aggressive", "extreme" }) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };'''
)

replace_once(
    "files/settings-default.cfg",
    '''[Cells]

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
    '''[Cells]

# V3 RAM/cache policy preset.
# normal = upstream OpenMW cache behavior.
# aggressive = longer-lived resource caches and a larger cell preload cache, aimed at systems with spare RAM.
# extreme = 32 GB-class preset. Keeps recently-used cells/resources around for up to 10 minutes and raises
#           preload cache capacity substantially to trade RAM for smoother revisits and cell transitions.
# Presets act as minimums: manually configured values that are already larger are preserved.
ram cache mode = normal

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.'''
)

# ---------------------------------------------------------------------------
# Give all diagnostic streams an exact engine-frame number.
# ---------------------------------------------------------------------------
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''#include <algorithm>
#include <array>
#include <chrono>''',
    '''#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>'''
)
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''namespace Debug::V3HitchTelemetry
{
    constexpr std::size_t StageCount = 10;''',
    '''namespace Debug::V3HitchTelemetry
{
    constexpr std::size_t StageCount = 10;

    inline std::atomic<unsigned> sCurrentFrame{ 0 };

    inline unsigned currentFrame()
    {
        return sCurrentFrame.load(std::memory_order_relaxed);
    }'''
)
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''        void beginFrame(unsigned frameNumber)
        {
            const auto now = Clock::now();''',
    '''        void beginFrame(unsigned frameNumber)
        {
            sCurrentFrame.store(frameNumber, std::memory_order_relaxed);
            const auto now = Clock::now();'''
)

# ---------------------------------------------------------------------------
# Apply effective RAM policy without rewriting the user's individual settings.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/engine.cpp",
    '''#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>''',
    '''#include <components/settings/ramcache.hpp>
#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>'''
)
replace_once(
    "apps/openmw/engine.cpp",
    '''    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder());''',
    '''    const float effectiveResourceCacheExpiry = Settings::RamCache::cacheExpiryDelay();
    Log(Debug::Info) << "V3 RAM cache mode: " << Settings::RamCache::name()
                     << " resource expiry=" << effectiveResourceCacheExpiry << "s"
                     << " preload min/max=" << Settings::RamCache::preloadCellCacheMin() << "/"
                     << Settings::RamCache::preloadCellCacheMax()
                     << " preload expiry=" << Settings::RamCache::preloadCellExpiryDelay() << "s";
    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), effectiveResourceCacheExpiry, &mEncoder.get()->getStatelessEncoder());'''
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>'''
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''#include <components/settings/values.hpp>''',
    '''#include <components/settings/ramcache.hpp>
#include <components/settings/values.hpp>'''
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        , mPredictionTime(Settings::cells().mPredictionTime)''',
    '''        , mPredictionTime(Settings::RamCache::predictionTime())'''
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        mPreloader->setExpiryDelay(Settings::cells().mPreloadCellExpiryDelay);
        mPreloader->setMinCacheSize(Settings::cells().mPreloadCellCacheMin);
        mPreloader->setMaxCacheSize(Settings::cells().mPreloadCellCacheMax);
        mPreloader->setPreloadInstances(Settings::cells().mPreloadInstances);''',
    '''        mPreloader->setExpiryDelay(Settings::RamCache::preloadCellExpiryDelay());
        mPreloader->setMinCacheSize(Settings::RamCache::preloadCellCacheMin());
        mPreloader->setMaxCacheSize(Settings::RamCache::preloadCellCacheMax());
        mPreloader->setPreloadInstances(Settings::RamCache::preloadInstances());'''
)
replace_all(
    "apps/openmw/mwworld/scene.cpp",
    '''mRendering.getResourceSystem()->setExpiryDelay(Settings::cells().mCacheExpiryDelay);''',
    '''mRendering.getResourceSystem()->setExpiryDelay(Settings::RamCache::cacheExpiryDelay());'''
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::changeToInteriorCell(
        std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        CellStore& cell = mWorld.getWorldModel().getInterior(cellName);''',
    '''    void Scene::changeToInteriorCell(
        std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_interior", cellName);
        CellStore& cell = mWorld.getWorldModel().getInterior(cellName);'''
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''    void Scene::changeToExteriorCell(
        const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {

        if (changeEvent)''',
    '''    void Scene::changeToExteriorCell(
        const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        Debug::V3Diagnostics::writeEvent("change_to_exterior");

        if (changeEvent)'''
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        Log(Debug::Info) << "Unloading cell " << cell->getCell()->getDescription();''',
    '''        Debug::V3Diagnostics::writeEvent("unload_cell", cell->getCell()->getDescription());
        Log(Debug::Info) << "Unloading cell " << cell->getCell()->getDescription();'''
)

# ---------------------------------------------------------------------------
# LuaSync + asynchronous Lua subphase profiler.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwlua/luamanagerimp.hpp",
    '''            DelayedAction(LuaUtil::LuaState* state, std::function<void()> fn, std::string_view name);
            void apply() const;''',
    '''            DelayedAction(LuaUtil::LuaState* state, std::function<void()> fn, std::string_view name);
            void apply() const;
            std::string_view name() const { return mName; }'''
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>'''
)

old_update = r'''    void LuaManager::update()
    {
        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        MWWorld::Ptr newPlayerPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (!(getId(mPlayer) == getId(newPlayerPtr)))
            throw std::logic_error("Player RefNum was changed unexpectedly");
        if (!mPlayer.isInCell() || !newPlayerPtr.isInCell() || mPlayer.getCell() != newPlayerPtr.getCell())
        {
            mPlayer = newPlayerPtr; // player was moved to another cell, update ptr in registry
            MWBase::Environment::get().getWorldModel()->registerPtr(mPlayer);
        }

        mObjectLists.update();

        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mQueuedAutoStartedScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->addAutoStartedScripts();
        }
        mQueuedAutoStartedScripts.clear();

        std::erase_if(mActiveLocalScripts, [](const LuaUtil::ScriptsContainerWeakPtr& ptr) {
            LocalScripts* l = asLocal(ptr);
            return l == nullptr || l->getPtrOrEmpty().isEmpty() || l->getPtrOrEmpty().mRef->isDeleted();
        });

        mGlobalScripts.statsNextFrame();
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
            asLocal(ptr)->statsNextFrame();

        mLuaEvents.finalizeEventBatch();

        MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();
        if (!timeManager.isPaused())
        {
            mMenuScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            mGlobalScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                asLocal(ptr)->processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
        }

        // Run event handlers for events that were sent before `finalizeEventBatch`.
        mLuaEvents.callEventHandlers();

        mLua.protectedCall([&](LuaUtil::LuaView& lua) {
            // Run queued callbacks
            for (CallbackWithData& c : mQueuedCallbacks)
                c.mCallback.tryCall(c.mArg);
            mQueuedCallbacks.clear();

            // Run engine handlers
            mEngineEvents.callEngineHandlers();
            bool isPaused = timeManager.isPaused();

            float frameDuration = MWBase::Environment::get().getFrameDuration();
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                asLocal(ptr)->update(isPaused ? 0 : frameDuration);
            mGlobalScripts.update(isPaused ? 0 : frameDuration);

            mScriptTracker.unloadInactiveScripts(lua);
        });
    }'''

new_update = r'''    void LuaManager::update()
    {
        static Debug::V3Diagnostics::CsvWriter writer("OPENMW_V3_LUA_UPDATE_FILE",
            "frame,epoch_ms,total_ms,object_lists_ms,autostart_ms,prune_ms,stats_ms,event_finalize_ms,timers_ms,"
            "event_handlers_ms,callbacks_ms,engine_events_ms,local_update_ms,global_update_ms,tracker_ms,"
            "active_locals,autostarts,callbacks");
        const bool profile = writer.enabled();
        const auto totalStart = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        auto startPhase = [&]() {
            return profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        };
        auto finishPhase = [&](Debug::V3Diagnostics::Clock::time_point start) {
            return profile ? Debug::V3Diagnostics::elapsedMs(start) : 0.0;
        };

        double objectListsMs = 0.0;
        double autostartMs = 0.0;
        double pruneMs = 0.0;
        double statsMs = 0.0;
        double eventFinalizeMs = 0.0;
        double timersMs = 0.0;
        double eventHandlersMs = 0.0;
        double callbacksMs = 0.0;
        double engineEventsMs = 0.0;
        double localUpdateMs = 0.0;
        double globalUpdateMs = 0.0;
        double trackerMs = 0.0;

        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        MWWorld::Ptr newPlayerPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (!(getId(mPlayer) == getId(newPlayerPtr)))
            throw std::logic_error("Player RefNum was changed unexpectedly");
        if (!mPlayer.isInCell() || !newPlayerPtr.isInCell() || mPlayer.getCell() != newPlayerPtr.getCell())
        {
            mPlayer = newPlayerPtr; // player was moved to another cell, update ptr in registry
            MWBase::Environment::get().getWorldModel()->registerPtr(mPlayer);
        }

        auto phaseStart = startPhase();
        mObjectLists.update();
        objectListsMs = finishPhase(phaseStart);

        const std::size_t autostartCount = mQueuedAutoStartedScripts.size();
        phaseStart = startPhase();
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mQueuedAutoStartedScripts)
        {
            if (LocalScripts* scripts = asLocal(ptr))
                scripts->addAutoStartedScripts();
        }
        mQueuedAutoStartedScripts.clear();
        autostartMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        std::erase_if(mActiveLocalScripts, [](const LuaUtil::ScriptsContainerWeakPtr& ptr) {
            LocalScripts* l = asLocal(ptr);
            return l == nullptr || l->getPtrOrEmpty().isEmpty() || l->getPtrOrEmpty().mRef->isDeleted();
        });
        pruneMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        mGlobalScripts.statsNextFrame();
        for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
            asLocal(ptr)->statsNextFrame();
        statsMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        mLuaEvents.finalizeEventBatch();
        eventFinalizeMs = finishPhase(phaseStart);

        MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();
        phaseStart = startPhase();
        if (!timeManager.isPaused())
        {
            mMenuScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            mGlobalScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                asLocal(ptr)->processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
        }
        timersMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        // Run event handlers for events that were sent before `finalizeEventBatch`.
        mLuaEvents.callEventHandlers();
        eventHandlersMs = finishPhase(phaseStart);

        const std::size_t callbackCount = mQueuedCallbacks.size();
        mLua.protectedCall([&](LuaUtil::LuaView& lua) {
            phaseStart = startPhase();
            // Run queued callbacks
            for (CallbackWithData& c : mQueuedCallbacks)
                c.mCallback.tryCall(c.mArg);
            mQueuedCallbacks.clear();
            callbacksMs = finishPhase(phaseStart);

            phaseStart = startPhase();
            // Run engine handlers
            mEngineEvents.callEngineHandlers();
            engineEventsMs = finishPhase(phaseStart);
            bool isPaused = timeManager.isPaused();

            float frameDuration = MWBase::Environment::get().getFrameDuration();
            phaseStart = startPhase();
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                asLocal(ptr)->update(isPaused ? 0 : frameDuration);
            localUpdateMs = finishPhase(phaseStart);

            phaseStart = startPhase();
            mGlobalScripts.update(isPaused ? 0 : frameDuration);
            globalUpdateMs = finishPhase(phaseStart);

            phaseStart = startPhase();
            mScriptTracker.unloadInactiveScripts(lua);
            trackerMs = finishPhase(phaseStart);
        });

        if (profile)
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << std::fixed << std::setprecision(3) << Debug::V3Diagnostics::elapsedMs(totalStart) << ','
                << objectListsMs << ',' << autostartMs << ',' << pruneMs << ',' << statsMs << ','
                << eventFinalizeMs << ',' << timersMs << ',' << eventHandlersMs << ',' << callbacksMs << ','
                << engineEventsMs << ',' << localUpdateMs << ',' << globalUpdateMs << ',' << trackerMs << ','
                << mActiveLocalScripts.size() << ',' << autostartCount << ',' << callbackCount;
            writer.writeLine(row.str());
        }
    }'''

replace_once("apps/openmw/mwlua/luamanagerimp.cpp", old_update, new_update)

old_sync = r'''    void LuaManager::synchronizedUpdateUnsafe()
    {
        if (mNewGameStarted)
        {
            mNewGameStarted = false;
            // Run onNewGame handler in synchronizedUpdate (at the beginning of the frame), so it
            // can teleport the player to the starting location before the first frame is rendered.
            mGlobalScripts.newGameStarted();
        }
        BoolScopeGuard updateGuard(mRunningSynchronizedUpdates);

        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        PlayerScripts* playerScripts
            = mPlayer.isEmpty() ? nullptr : dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        // We apply input events in `synchronizedUpdate` rather than in `update` in order to reduce input latency.
        {
            BoolScopeGuard processingGuard(mProcessingInputEvents);

            for (const auto& event : mMenuInputEvents)
                mMenuScripts.processInputEvent(event);
            mMenuInputEvents.clear();
            if (playerScripts && !windowManager->containsMode(MWGui::GM_MainMenu))
            {
                for (const auto& event : mInputEvents)
                    playerScripts->processInputEvent(event);
            }
            mInputEvents.clear();
            mLuaEvents.callMenuEventHandlers();
            float frameDuration = MWBase::Environment::get().getWorld()->getTimeManager()->isPaused()
                ? 0.f
                : MWBase::Environment::get().getFrameDuration();
            mInputActions.update(frameDuration);
            mMenuScripts.onFrame(frameDuration);
            if (playerScripts)
                playerScripts->onFrame(frameDuration);
        }

        for (const auto& [message, mode] : mUIMessages)
            windowManager->messageBox(message, mode);
        mUIMessages.clear();
        for (auto& [msg, color] : mInGameConsoleMessages)
            windowManager->printToConsole(msg, "#" + color.toHex());
        mInGameConsoleMessages.clear();

        applyDelayedActions();

        if (mReloadAllScriptsRequested)
        {
            // Reloading right after `applyDelayedActions` to guarantee that no delayed actions are currently queued.
            reloadAllScriptsImpl();
            mReloadAllScriptsRequested = false;
        }

        if (mDelayedUiModeChangedArg)
        {
            if (playerScripts)
                playerScripts->uiModeChanged(*mDelayedUiModeChangedArg, true);
            mDelayedUiModeChangedArg = std::nullopt;
        }
    }

    void LuaManager::applyDelayedActions()
    {
        BoolScopeGuard applyingGuard(mApplyingDelayedActions);
        for (DelayedAction& action : mActionQueue)
            action.apply();
        mActionQueue.clear();

        if (mTeleportPlayerAction)
            mTeleportPlayerAction->apply();
        mTeleportPlayerAction.reset();
    }'''

new_sync = r'''    void LuaManager::synchronizedUpdateUnsafe()
    {
        static Debug::V3Diagnostics::CsvWriter writer("OPENMW_V3_LUASYNC_FILE",
            "frame,epoch_ms,total_ms,new_game_ms,input_player_ms,ui_messages_ms,delayed_actions_ms,reload_ms,"
            "ui_mode_ms,action_count,had_teleport,other_ms");
        const bool profile = writer.enabled();
        const auto totalStart = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        auto startPhase = [&]() {
            return profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        };
        auto finishPhase = [&](Debug::V3Diagnostics::Clock::time_point start) {
            return profile ? Debug::V3Diagnostics::elapsedMs(start) : 0.0;
        };

        double newGameMs = 0.0;
        double inputMs = 0.0;
        double uiMessagesMs = 0.0;
        double delayedActionsMs = 0.0;
        double reloadMs = 0.0;
        double uiModeMs = 0.0;

        auto phaseStart = startPhase();
        if (mNewGameStarted)
        {
            mNewGameStarted = false;
            // Run onNewGame handler in synchronizedUpdate (at the beginning of the frame), so it
            // can teleport the player to the starting location before the first frame is rendered.
            mGlobalScripts.newGameStarted();
        }
        newGameMs = finishPhase(phaseStart);
        BoolScopeGuard updateGuard(mRunningSynchronizedUpdates);

        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        PlayerScripts* playerScripts
            = mPlayer.isEmpty() ? nullptr : dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        phaseStart = startPhase();
        // We apply input events in `synchronizedUpdate` rather than in `update` in order to reduce input latency.
        {
            BoolScopeGuard processingGuard(mProcessingInputEvents);

            for (const auto& event : mMenuInputEvents)
                mMenuScripts.processInputEvent(event);
            mMenuInputEvents.clear();
            if (playerScripts && !windowManager->containsMode(MWGui::GM_MainMenu))
            {
                for (const auto& event : mInputEvents)
                    playerScripts->processInputEvent(event);
            }
            mInputEvents.clear();
            mLuaEvents.callMenuEventHandlers();
            float frameDuration = MWBase::Environment::get().getWorld()->getTimeManager()->isPaused()
                ? 0.f
                : MWBase::Environment::get().getFrameDuration();
            mInputActions.update(frameDuration);
            mMenuScripts.onFrame(frameDuration);
            if (playerScripts)
                playerScripts->onFrame(frameDuration);
        }
        inputMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        for (const auto& [message, mode] : mUIMessages)
            windowManager->messageBox(message, mode);
        mUIMessages.clear();
        for (auto& [msg, color] : mInGameConsoleMessages)
            windowManager->printToConsole(msg, "#" + color.toHex());
        mInGameConsoleMessages.clear();
        uiMessagesMs = finishPhase(phaseStart);

        const std::size_t actionCount = mActionQueue.size();
        const bool hadTeleport = mTeleportPlayerAction.has_value();
        phaseStart = startPhase();
        applyDelayedActions();
        delayedActionsMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        if (mReloadAllScriptsRequested)
        {
            // Reloading right after `applyDelayedActions` to guarantee that no delayed actions are currently queued.
            reloadAllScriptsImpl();
            mReloadAllScriptsRequested = false;
        }
        reloadMs = finishPhase(phaseStart);

        phaseStart = startPhase();
        if (mDelayedUiModeChangedArg)
        {
            if (playerScripts)
                playerScripts->uiModeChanged(*mDelayedUiModeChangedArg, true);
            mDelayedUiModeChangedArg = std::nullopt;
        }
        uiModeMs = finishPhase(phaseStart);

        if (profile)
        {
            const double totalMs = Debug::V3Diagnostics::elapsedMs(totalStart);
            const double accounted
                = newGameMs + inputMs + uiMessagesMs + delayedActionsMs + reloadMs + uiModeMs;
            const double otherMs = totalMs > accounted ? totalMs - accounted : 0.0;
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << std::fixed << std::setprecision(3) << totalMs << ',' << newGameMs << ',' << inputMs << ','
                << uiMessagesMs << ',' << delayedActionsMs << ',' << reloadMs << ',' << uiModeMs << ','
                << actionCount << ',' << (hadTeleport ? 1 : 0) << ',' << otherMs;
            writer.writeLine(row.str());
        }
    }

    void LuaManager::applyDelayedActions()
    {
        static Debug::V3Diagnostics::CsvWriter writer("OPENMW_V3_LUA_ACTION_FILE",
            "frame,epoch_ms,total_ms,queue_ms,teleport_ms,action_count,slowest_action_ms,slowest_action_name");
        const bool profile = writer.enabled();
        const auto totalStart = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};

        BoolScopeGuard applyingGuard(mApplyingDelayedActions);
        const std::size_t actionCount = mActionQueue.size();
        double queueMs = 0.0;
        double teleportMs = 0.0;
        double slowestMs = 0.0;
        std::string slowestName;

        auto queueStart = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        for (DelayedAction& action : mActionQueue)
        {
            if (profile)
            {
                const auto actionStart = Debug::V3Diagnostics::Clock::now();
                action.apply();
                const double actionMs = Debug::V3Diagnostics::elapsedMs(actionStart);
                if (actionMs > slowestMs)
                {
                    slowestMs = actionMs;
                    slowestName = std::string(action.name());
                }
            }
            else
                action.apply();
        }
        if (profile)
            queueMs = Debug::V3Diagnostics::elapsedMs(queueStart);
        mActionQueue.clear();

        if (mTeleportPlayerAction)
        {
            const auto teleportStart
                = profile ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            mTeleportPlayerAction->apply();
            if (profile)
                teleportMs = Debug::V3Diagnostics::elapsedMs(teleportStart);
        }
        mTeleportPlayerAction.reset();

        if (profile)
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << std::fixed << std::setprecision(3) << Debug::V3Diagnostics::elapsedMs(totalStart) << ','
                << queueMs << ',' << teleportMs << ',' << actionCount << ',' << slowestMs << ','
                << Debug::V3Diagnostics::csvQuote(slowestName);
            writer.writeLine(row.str());
        }
    }'''

replace_once("apps/openmw/mwlua/luamanagerimp.cpp", old_sync, new_sync)

# ---------------------------------------------------------------------------
# MSOC detailed CPU timing. Existing aggregate telemetry remains unchanged.
# ---------------------------------------------------------------------------
replace_once(
    "components/sceneutil/occlusionculling.hpp",
    '''#include <components/occlusionculling/telemetry.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/occlusionculling/telemetry.hpp>'''
)
replace_once(
    "components/sceneutil/occlusionculling.hpp",
    '''        unsigned int getNumOccluded() const { return mNumOccluded; }''',
    '''        void setTelemetryFrameNumber(unsigned int frameNumber) { mExternalFrameNumber = frameNumber; }
        bool detailedTelemetryEnabled() const { return mDetailedTelemetryEnabled; }
        void addTerrainBuildTime(double milliseconds)
        {
            if (mDetailedTelemetryEnabled)
                mTerrainBuildMs += milliseconds;
        }

        unsigned int getNumOccluded() const { return mNumOccluded; }'''
)
replace_once(
    "components/sceneutil/occlusionculling.hpp",
    '''            mFrameActive = false;
        }

        unsigned int getNumOccluded() const''',
    '''            if (mDetailedTelemetryEnabled)
                writeDetailedTelemetryRow();

            mFrameActive = false;
        }

        unsigned int getNumOccluded() const'''
)
replace_once(
    "components/sceneutil/occlusionculling.hpp",
    '''        void writeTelemetryRow(std::uint64_t traverseNs)
        {''',
    '''        void writeDetailedTelemetryRow()
        {
            static Debug::V3Diagnostics::CsvWriter writer("OPENMW_V3_MSOC_DETAIL_FILE",
                "frame,epoch_ms,clear_ms,terrain_build_ms,terrain_raster_ms,building_raster_ms,"
                "aabb_total_ms,testrect_ms,test_calls,building_occluders,building_tris,aabbs_tested,aabbs_occluded");
            if (!writer.enabled())
                return;

            std::ostringstream row;
            row << mExternalFrameNumber << ',' << Debug::V3Diagnostics::epochMs() << ',' << std::fixed
                << std::setprecision(3) << mClearMs << ',' << mTerrainBuildMs << ',' << mTerrainRasterMs << ','
                << mBuildingRasterMs << ',' << mAabbTotalMs << ',' << mTestRectMs << ',' << mDetailedTestCalls << ','
                << mNumBuildingOccluders << ',' << mNumBuildingTris << ',' << mNumTested << ',' << mNumOccluded;
            writer.writeLine(row.str());
        }

        void writeTelemetryRow(std::uint64_t traverseNs)
        {'''
)
replace_once(
    "components/sceneutil/occlusionculling.hpp",
    '''        std::uint64_t mTelemetryFrameIndex = 0;
    };''',
    '''        std::uint64_t mTelemetryFrameIndex = 0;

        bool mDetailedTelemetryEnabled = [] {
            const char* path = std::getenv("OPENMW_V3_MSOC_DETAIL_FILE");
            return path && Debug::V3Diagnostics::pathEnabled(path);
        }();
        unsigned int mExternalFrameNumber = 0;
        double mClearMs = 0.0;
        double mTerrainBuildMs = 0.0;
        double mTerrainRasterMs = 0.0;
        mutable double mBuildingRasterMs = 0.0;
        mutable double mAabbTotalMs = 0.0;
        mutable double mTestRectMs = 0.0;
        mutable std::uint64_t mDetailedTestCalls = 0;
        bool mRasterizingTerrain = false;
    };'''
)

replace_once(
    "components/sceneutil/occlusionculling.cpp",
    '''    void OcclusionCuller::beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix)
    {
        mFrameActive = false;
        if (!mMOC)
            return;

        mMOC->ClearBuffer();
        if (mMOCTerrainOnly)
            mMOCTerrainOnly->ClearBuffer();''',
    '''    void OcclusionCuller::beginFrame(const osg::Matrixd& viewMatrix, const osg::Matrixd& projectionMatrix)
    {
        mFrameActive = false;
        mClearMs = 0.0;
        mTerrainBuildMs = 0.0;
        mTerrainRasterMs = 0.0;
        mBuildingRasterMs = 0.0;
        mAabbTotalMs = 0.0;
        mTestRectMs = 0.0;
        mDetailedTestCalls = 0;
        if (!mMOC)
            return;

        const auto clearStart
            = mDetailedTelemetryEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        mMOC->ClearBuffer();
        if (mMOCTerrainOnly)
            mMOCTerrainOnly->ClearBuffer();
        if (mDetailedTelemetryEnabled)
            mClearMs = Debug::V3Diagnostics::elapsedMs(clearStart);'''
)
replace_once(
    "components/sceneutil/occlusionculling.cpp",
    '''    void OcclusionCuller::rasterizeTerrainOccluder(
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)
    {
        // Rasterize terrain into both buffers so buildings can be tested against
        // terrain-only depth (via testVisibleAABBTerrainOnly).
        rasterizeOccluder(worldPositions, indices);''',
    '''    void OcclusionCuller::rasterizeTerrainOccluder(
        const std::vector<osg::Vec3f>& worldPositions, const std::vector<unsigned int>& indices)
    {
        Debug::V3Diagnostics::ScopedAccumulator timer(mDetailedTelemetryEnabled, mTerrainRasterMs);
        // Rasterize terrain into both buffers so buildings can be tested against
        // terrain-only depth (via testVisibleAABBTerrainOnly).
        mRasterizingTerrain = true;
        rasterizeOccluder(worldPositions, indices);
        mRasterizingTerrain = false;'''
)
replace_once(
    "components/sceneutil/occlusionculling.cpp",
    '''        mMOC->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // terrain can be seen from below at edges
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);''',
    '''        const auto rasterStart
            = (mDetailedTelemetryEnabled && !mRasterizingTerrain) ? Debug::V3Diagnostics::Clock::now()
                                                                 : Debug::V3Diagnostics::Clock::time_point{};
        mMOC->RenderTriangles(reinterpret_cast<const float*>(worldPositions.data()), indices.data(), numTris, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // terrain can be seen from below at edges
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        if (mDetailedTelemetryEnabled && !mRasterizingTerrain)
            mBuildingRasterMs += Debug::V3Diagnostics::elapsedMs(rasterStart);'''
)
replace_once(
    "components/sceneutil/occlusionculling.cpp",
    '''        mMOC->RenderTriangles(reinterpret_cast<const float*>(verts), indices, 12, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // both sides, nearest depth wins
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);

        ++mNumBuildingOccluders;''',
    '''        const auto rasterStart
            = mDetailedTelemetryEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        mMOC->RenderTriangles(reinterpret_cast<const float*>(verts), indices, 12, mVPFloat,
            MaskedOcclusionCulling::BACKFACE_NONE, // both sides, nearest depth wins
            MaskedOcclusionCulling::CLIP_PLANE_ALL, vtxLayout);
        if (mDetailedTelemetryEnabled)
            mBuildingRasterMs += Debug::V3Diagnostics::elapsedMs(rasterStart);

        ++mNumBuildingOccluders;'''
)
replace_once(
    "components/sceneutil/occlusionculling.cpp",
    '''    bool OcclusionCuller::testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const
    {
        const osg::Vec3f corners[8] = {''',
    '''    bool OcclusionCuller::testVisibleAABBImpl(MaskedOcclusionCulling* moc, const osg::BoundingBox& worldBB) const
    {
        Debug::V3Diagnostics::ScopedAccumulator totalTimer(mDetailedTelemetryEnabled, mAabbTotalMs);
        if (mDetailedTelemetryEnabled)
            ++mDetailedTestCalls;

        const osg::Vec3f corners[8] = {'''
)
replace_once(
    "components/sceneutil/occlusionculling.cpp",
    '''        auto result = moc->TestRect(ndcMinX, ndcMinY, ndcMaxX, ndcMaxY, wMin);
        return result != MaskedOcclusionCulling::OCCLUDED;''',
    '''        const auto testStart
            = mDetailedTelemetryEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        auto result = moc->TestRect(ndcMinX, ndcMinY, ndcMaxX, ndcMaxY, wMin);
        if (mDetailedTelemetryEnabled)
            mTestRectMs += Debug::V3Diagnostics::elapsedMs(testStart);
        return result != MaskedOcclusionCulling::OCCLUDED;'''
)

replace_once(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        // Begin occlusion frame with camera matrices
        mCuller->beginFrame(cam->getViewMatrix(), cam->getProjectionMatrix());''',
    '''        // Begin occlusion frame with camera matrices
        mCuller->setTelemetryFrameNumber(frameNumber);
        mCuller->beginFrame(cam->getViewMatrix(), cam->getProjectionMatrix());'''
)
replace_once(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''            mPositions.clear();
            mIndices.clear();
            mTerrainOccluder->build(cv->getEyePoint(), mRadiusCells, mPositions, mIndices);

            if (!mPositions.empty())
                mCuller->rasterizeTerrainOccluder(mPositions, mIndices);''',
    '''            mPositions.clear();
            mIndices.clear();
            const auto terrainBuildStart = mCuller->detailedTelemetryEnabled()
                ? Debug::V3Diagnostics::Clock::now()
                : Debug::V3Diagnostics::Clock::time_point{};
            mTerrainOccluder->build(cv->getEyePoint(), mRadiusCells, mPositions, mIndices);
            if (mCuller->detailedTelemetryEnabled())
                mCuller->addTerrainBuildTime(Debug::V3Diagnostics::elapsedMs(terrainBuildStart));

            if (!mPositions.empty())
                mCuller->rasterizeTerrainOccluder(mPositions, mIndices);'''
)
replace_once(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>'''
)

# ---------------------------------------------------------------------------
# Shadow CPU timing: receiver traversal and each cascade caster traversal.
# ---------------------------------------------------------------------------
replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''#include <sstream>
#include <vector>''',
    '''#include <array>
#include <sstream>
#include <vector>

#include <components/debug/v3diagnostics.hpp>'''
)
replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    OSG_INFO<<std::endl<<std::endl<<"MWShadowTechnique::cull(osg::CullVisitor&"<<&cv<<")"<<std::endl;

    if (!_shadowCastingStateSet)''',
    '''    OSG_INFO<<std::endl<<std::endl<<"MWShadowTechnique::cull(osg::CullVisitor&"<<&cv<<")"<<std::endl;

    static Debug::V3Diagnostics::CsvWriter v3ShadowWriter("OPENMW_V3_SHADOW_FILE",
        "frame,epoch_ms,total_ms,receiver_ms,caster_total_ms,cascade0_ms,cascade1_ms,cascade2_ms,cascade3_ms,"
        "cascade4_ms,cascade5_ms,cascade6_ms,cascade7_ms,num_cascades");
    const bool v3ShadowProfile = v3ShadowWriter.enabled();
    const auto v3ShadowTotalStart = v3ShadowProfile ? Debug::V3Diagnostics::Clock::now()
                                                    : Debug::V3Diagnostics::Clock::time_point{};
    double v3ReceiverMs = 0.0;
    double v3CasterTotalMs = 0.0;
    std::array<double, 8> v3CascadeMs{};
    unsigned int v3CascadeCount = 0;

    if (!_shadowCastingStateSet)'''
)
replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    cullShadowReceivingScene(&cv);

    cv.popStateSet();''',
    '''    const auto v3ReceiverStart = v3ShadowProfile ? Debug::V3Diagnostics::Clock::now()
                                                       : Debug::V3Diagnostics::Clock::time_point{};
    cullShadowReceivingScene(&cv);
    if (v3ShadowProfile)
        v3ReceiverMs = Debug::V3Diagnostics::elapsedMs(v3ReceiverStart);

    cv.popStateSet();'''
)
replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''            cullShadowCastingScene(&cv, camera.get());

            cv.popStateSet();''',
    '''            const auto v3CasterStart = v3ShadowProfile ? Debug::V3Diagnostics::Clock::now()
                                                             : Debug::V3Diagnostics::Clock::time_point{};
            cullShadowCastingScene(&cv, camera.get());
            if (v3ShadowProfile)
            {
                const double cascadeMs = Debug::V3Diagnostics::elapsedMs(v3CasterStart);
                v3CasterTotalMs += cascadeMs;
                if (v3CascadeCount < v3CascadeMs.size())
                    v3CascadeMs[v3CascadeCount] = cascadeMs;
                ++v3CascadeCount;
            }

            cv.popStateSet();'''
)
replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    // OSG_NOTICE<<"End of shadow setup Projection matrix "<<*cv.getProjectionMatrix()<<std::endl;
}

bool MWShadowTechnique::selectActiveLights''',
    '''    if (v3ShadowProfile)
    {
        std::ostringstream row;
        row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
            << std::fixed << std::setprecision(3) << Debug::V3Diagnostics::elapsedMs(v3ShadowTotalStart) << ','
            << v3ReceiverMs << ',' << v3CasterTotalMs;
        for (double value : v3CascadeMs)
            row << ',' << value;
        row << ',' << v3CascadeCount;
        v3ShadowWriter.writeLine(row.str());
    }

    // OSG_NOTICE<<"End of shadow setup Projection matrix "<<*cv.getProjectionMatrix()<<std::endl;
}

bool MWShadowTechnique::selectActiveLights'''
)

print("V3 diagnostic harness source patch completed successfully.")
