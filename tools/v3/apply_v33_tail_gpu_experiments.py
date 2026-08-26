import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.3 tail/GPU match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"V3.3 tail/GPU patched {rel}")


replace_once(
    "components/settings/categories/lua.hpp",
    '''        SettingValue<bool> mLuaProfiler{ mIndex, "Lua", "lua profiler" };
        SettingValue<std::uint64_t> mSmallAllocMaxSize{ mIndex, "Lua", "small alloc max size" };''',
    '''        SettingValue<bool> mLuaProfiler{ mIndex, "Lua", "lua profiler" };
        SettingValue<bool> mV33IdleTimerFastPath{ mIndex, "Lua", "v3.3 idle timer fast path" };
        SettingValue<std::uint64_t> mSmallAllocMaxSize{ mIndex, "Lua", "small alloc max size" };''',
)

replace_once(
    "components/settings/categories/shadows.hpp",
    '''        SettingValue<float> mV33FarCascadeMaxTexelDrift{ mIndex, "Shadows", "v3.3 far cascade max texel drift",
            makeClampSanitizerFloat(0, 8) };
        SettingValue<float> mMinimumLispsmNearFarRatio{ mIndex, "Shadows", "minimum lispsm near far ratio",''',
    '''        SettingValue<float> mV33FarCascadeMaxTexelDrift{ mIndex, "Shadows", "v3.3 far cascade max texel drift",
            makeClampSanitizerFloat(0, 8) };
        SettingValue<int> mV33FarCascadeResolutionDivisor{ mIndex, "Shadows",
            "v3.3 far cascade resolution divisor", makeClampSanitizerInt(1, 2) };
        SettingValue<float> mMinimumLispsmNearFarRatio{ mIndex, "Shadows", "minimum lispsm near far ratio",''',
)

replace_once(
    "files/settings-default.cfg",
    '''# Enable Lua profiler
lua profiler = false

# No ownership tracking for allocations below or equal this size.''',
    '''# Enable Lua profiler
lua profiler = false

# V3.3 semantics-preserving timer overhead experiment. When true, already-loaded script containers whose
# timer heaps contain no due timer skip the otherwise empty protected Lua call. Due and restored timers are unchanged.
v3.3 idle timer fast path = false

# No ownership tracking for allocations below or equal this size.''',
)

replace_once(
    "files/settings-default.cfg",
    '''v3.3 far cascade update interval = 1
v3.3 far cascade max texel drift = 0.75

# Controls the minimum near/far ratio''',
    '''v3.3 far cascade update interval = 1
v3.3 far cascade max texel drift = 0.75

# V3.3 far-cascade-only GPU experiment. 1 preserves full resolution. 2 keeps near/middle cascades at full
# resolution while rendering the far cascade at half width and height. All configured caster types still render.
v3.3 far cascade resolution divisor = 1

# Controls the minimum near/far ratio''',
)

replace_once(
    "components/lua/scriptscontainer.hpp",
    '''#include <components/debug/debuglog.hpp>
#include <components/esm/luascripts.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v33luatrace.hpp>
#include <components/esm/luascripts.hpp>''',
)

replace_once(
    "components/lua/scriptscontainer.hpp",
    '''        void processTimers(double simulationTime, double gameTime);''',
    '''        void processTimers(double simulationTime, double gameTime, bool idleTimerFastPath = false);''',
)

replace_once(
    "components/lua/scriptscontainer.hpp",
    '''                try
                {
                    LuaUtil::call({ this, handler.mScriptId }, handler.mFn, args...);
                }''',
    '''                try
                {
                    const std::string_view v33ScriptPath = Debug::V33LuaTrace::enabled()
                        ? std::string_view(scriptPath(handler.mScriptId).value())
                        : std::string_view{};
                    Debug::V33LuaTrace::CallbackScope v33Trace("engine_handler", handlers.mName, mNamePrefix,
                        handler.mScriptId, v33ScriptPath, handlers.mName);
                    LuaUtil::call({ this, handler.mScriptId }, handler.mFn, args...);
                }''',
)

replace_once(
    "components/lua/scriptscontainer.hpp",
    '''        void callTimer(const Timer& t);
        void updateTimerQueue(std::vector<Timer>& timerQueue, double time);''',
    '''        void callTimer(const Timer& t);
        unsigned updateTimerQueue(std::vector<Timer>& timerQueue, double time, TimerType type);''',
)

replace_once(
    "components/lua/scriptscontainer.cpp",
    '''#include "scripttracker.hpp"

#include <components/esm/luascripts.hpp>''',
    '''#include "scripttracker.hpp"

#include <iomanip>
#include <sstream>

#include <components/esm/luascripts.hpp>''',
)

replace_once(
    "components/lua/scriptscontainer.cpp",
    '''    void ScriptsContainer::receiveEvent(std::string_view eventName, std::string_view eventData)
    {
        LoadedData& data = ensureLoaded();''',
    '''    void ScriptsContainer::receiveEvent(std::string_view eventName, std::string_view eventData)
    {
        Debug::V33LuaTrace::CallbackScope v33DispatchTrace(
            "lua_event_dispatch", eventName, mNamePrefix, -1, "", eventName);
        LoadedData& data = ensureLoaded();''',
)

replace_once(
    "components/lua/scriptscontainer.cpp",
    '''                try
                {
                    sol::object res = LuaUtil::call({ this, h.mScriptId }, h.mFn, object);''',
    '''                try
                {
                    const std::string_view v33ScriptPath = Debug::V33LuaTrace::enabled()
                        ? std::string_view(scriptPath(h.mScriptId).value())
                        : std::string_view{};
                    Debug::V33LuaTrace::CallbackScope v33HandlerTrace(
                        "lua_event_handler", eventName, mNamePrefix, h.mScriptId, v33ScriptPath, eventName);
                    sol::object res = LuaUtil::call({ this, h.mScriptId }, h.mFn, object);''',
)

replace_once(
    "components/lua/scriptscontainer.cpp",
    '''    void ScriptsContainer::updateTimerQueue(std::vector<Timer>& timerQueue, double time)
    {
        while (!timerQueue.empty() && timerQueue.front().mTime <= time)
        {
            callTimer(timerQueue.front());
            std::pop_heap(timerQueue.begin(), timerQueue.end());
            timerQueue.pop_back();
        }
    }

    void ScriptsContainer::processTimers(double simulationTime, double gameTime)
    {
        mLua.protectedCall([&](LuaView& view) {
            LoadedData& data = ensureLoaded();
            updateTimerQueue(data.mSimulationTimersQueue, simulationTime);
            updateTimerQueue(data.mGameTimersQueue, gameTime);
        });
    }''',
    '''    unsigned ScriptsContainer::updateTimerQueue(std::vector<Timer>& timerQueue, double time, TimerType type)
    {
        unsigned fired = 0;
        while (!timerQueue.empty() && timerQueue.front().mTime <= time)
        {
            const Timer& timer = timerQueue.front();
            std::string callbackName;
            std::string detail;
            std::string_view scriptPathValue;
            if (Debug::V33LuaTrace::enabled())
            {
                callbackName = timer.mSerializable
                    ? std::get<std::string>(timer.mCallback)
                    : std::string("<unsavable:") + std::to_string(std::get<int64_t>(timer.mCallback)) + ">";
                scriptPathValue = scriptPath(timer.mScriptId).value();
                std::ostringstream stream;
                stream << "scheduled=" << std::fixed << std::setprecision(6) << timer.mTime
                       << ";lateness=" << std::max(0.0, time - timer.mTime);
                detail = stream.str();
            }
            Debug::V33LuaTrace::CallbackScope v33TimerTrace("timer",
                type == TimerType::SIMULATION_TIME ? "simulation" : "game", mNamePrefix, timer.mScriptId,
                scriptPathValue, callbackName, detail);
            callTimer(timer);
            std::pop_heap(timerQueue.begin(), timerQueue.end());
            timerQueue.pop_back();
            ++fired;
        }
        return fired;
    }

    void ScriptsContainer::processTimers(double simulationTime, double gameTime, bool idleTimerFastPath)
    {
        if (LoadedData* data = std::get_if<LoadedData>(&mData))
        {
            const bool simulationDue
                = !data->mSimulationTimersQueue.empty() && data->mSimulationTimersQueue.front().mTime <= simulationTime;
            const bool gameDue = !data->mGameTimersQueue.empty() && data->mGameTimersQueue.front().mTime <= gameTime;
            if (idleTimerFastPath && !simulationDue && !gameDue)
            {
                Debug::V33LuaTrace::recordTimerContainer(false, true, 0, 0);
                return;
            }
        }

        unsigned simulationFired = 0;
        unsigned gameFired = 0;
        bool due = false;
        mLua.protectedCall([&](LuaView& view) {
            LoadedData& data = ensureLoaded();
            due = (!data.mSimulationTimersQueue.empty() && data.mSimulationTimersQueue.front().mTime <= simulationTime)
                || (!data.mGameTimersQueue.empty() && data.mGameTimersQueue.front().mTime <= gameTime);
            simulationFired
                = updateTimerQueue(data.mSimulationTimersQueue, simulationTime, TimerType::SIMULATION_TIME);
            gameFired = updateTimerQueue(data.mGameTimersQueue, gameTime, TimerType::GAME_TIME);
        });
        Debug::V33LuaTrace::recordTimerContainer(due, false, simulationFired, gameFired);
    }''',
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v33luatrace.hpp>''',
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        MWWorld::Ptr newPlayerPtr''',
    '''        if (mPlayer.isEmpty())
            return; // The game is not started yet.

        Debug::V33LuaTrace::FrameScope v33LuaTraceFrame;

        MWWorld::Ptr newPlayerPtr''',
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();
        phaseStart = startPhase();
        if (!timeManager.isPaused())
        {
            mMenuScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            mGlobalScripts.processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                asLocal(ptr)->processTimers(timeManager.getSimulationTime(), timeManager.getGameTime());
        }
        timersMs = finishPhase(phaseStart);''',
    '''        MWWorld::DateTimeManager& timeManager = *MWBase::Environment::get().getWorld()->getTimeManager();
        const double simulationTime = timeManager.getSimulationTime();
        const double gameTime = timeManager.getGameTime();
        const bool v33IdleTimerFastPath = Settings::lua().mV33IdleTimerFastPath;
        phaseStart = startPhase();
        {
            Debug::V33LuaTrace::PhaseScope v33Phase(Debug::V33LuaTrace::Phase::Timers);
            if (!timeManager.isPaused())
            {
                mMenuScripts.processTimers(simulationTime, gameTime, v33IdleTimerFastPath);
                mGlobalScripts.processTimers(simulationTime, gameTime, v33IdleTimerFastPath);
                for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                    asLocal(ptr)->processTimers(simulationTime, gameTime, v33IdleTimerFastPath);
            }
        }
        timersMs = finishPhase(phaseStart);''',
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        phaseStart = startPhase();
        // Run event handlers for events that were sent before `finalizeEventBatch`.
        mLuaEvents.callEventHandlers();
        eventHandlersMs = finishPhase(phaseStart);''',
    '''        phaseStart = startPhase();
        {
            Debug::V33LuaTrace::PhaseScope v33Phase(Debug::V33LuaTrace::Phase::LuaEvents);
            // Run event handlers for events that were sent before `finalizeEventBatch`.
            mLuaEvents.callEventHandlers();
        }
        eventHandlersMs = finishPhase(phaseStart);''',
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''            phaseStart = startPhase();
            // Run queued callbacks
            for (CallbackWithData& c : mQueuedCallbacks)
                c.mCallback.tryCall(c.mArg);
            mQueuedCallbacks.clear();
            callbacksMs = finishPhase(phaseStart);''',
    '''            phaseStart = startPhase();
            {
                Debug::V33LuaTrace::PhaseScope v33Phase(Debug::V33LuaTrace::Phase::QueuedCallbacks);
                // Run queued callbacks
                for (CallbackWithData& c : mQueuedCallbacks)
                    c.mCallback.tryCall(c.mArg);
                mQueuedCallbacks.clear();
            }
            callbacksMs = finishPhase(phaseStart);''',
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''            phaseStart = startPhase();
            // Run engine handlers
            mEngineEvents.callEngineHandlers();
            engineEventsMs = finishPhase(phaseStart);''',
    '''            phaseStart = startPhase();
            {
                Debug::V33LuaTrace::PhaseScope v33Phase(Debug::V33LuaTrace::Phase::EngineEvents);
                // Run engine handlers
                mEngineEvents.callEngineHandlers();
            }
            engineEventsMs = finishPhase(phaseStart);''',
)

replace_once(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''            phaseStart = startPhase();
            for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                asLocal(ptr)->update(isPaused ? 0 : frameDuration);
            localUpdateMs = finishPhase(phaseStart);

            phaseStart = startPhase();
            mGlobalScripts.update(isPaused ? 0 : frameDuration);
            globalUpdateMs = finishPhase(phaseStart);''',
    '''            phaseStart = startPhase();
            {
                Debug::V33LuaTrace::PhaseScope v33Phase(Debug::V33LuaTrace::Phase::LocalUpdate);
                for (const LuaUtil::ScriptsContainerWeakPtr& ptr : mActiveLocalScripts)
                    asLocal(ptr)->update(isPaused ? 0 : frameDuration);
            }
            localUpdateMs = finishPhase(phaseStart);

            phaseStart = startPhase();
            {
                Debug::V33LuaTrace::PhaseScope v33Phase(Debug::V33LuaTrace::Phase::GlobalUpdate);
                mGlobalScripts.update(isPaused ? 0 : frameDuration);
            }
            globalUpdateMs = finishPhase(phaseStart);''',
)

replace_once(
    "apps/openmw/mwlua/engineevents.cpp",
    '''#include "engineevents.hpp"

#include <components/debug/debuglog.hpp>''',
    '''#include "engineevents.hpp"

#include <set>
#include <type_traits>

#include <components/debug/debuglog.hpp>
#include <components/debug/v33luatrace.hpp>''',
)

replace_once(
    "apps/openmw/mwlua/engineevents.cpp",
    '''namespace MWLua
{

    class EngineEvents::Visitor''',
    '''namespace MWLua
{
    namespace
    {
        template <class T>
        constexpr std::string_view v33EngineEventName()
        {
            if constexpr (std::is_same_v<T, EngineEvents::OnActive>)
                return "OnActive";
            else if constexpr (std::is_same_v<T, EngineEvents::OnInactive>)
                return "OnInactive";
            else if constexpr (std::is_same_v<T, EngineEvents::OnConsume>)
                return "OnConsume";
            else if constexpr (std::is_same_v<T, EngineEvents::OnActivate>)
                return "OnActivate";
            else if constexpr (std::is_same_v<T, EngineEvents::OnUseItem>)
                return "OnUseItem";
            else if constexpr (std::is_same_v<T, EngineEvents::OnNewExterior>)
                return "OnNewExterior";
            else if constexpr (std::is_same_v<T, EngineEvents::OnTeleported>)
                return "OnTeleported";
            else if constexpr (std::is_same_v<T, EngineEvents::OnAnimationTextKey>)
                return "OnAnimationTextKey";
            else if constexpr (std::is_same_v<T, EngineEvents::OnAnimationEnded>)
                return "OnAnimationEnded";
            else if constexpr (std::is_same_v<T, EngineEvents::OnSkillUse>)
                return "OnSkillUse";
            else if constexpr (std::is_same_v<T, EngineEvents::OnSkillLevelUp>)
                return "OnSkillLevelUp";
            else if constexpr (std::is_same_v<T, EngineEvents::OnJailTimeServed>)
                return "OnJailTimeServed";
            else if constexpr (std::is_same_v<T, EngineEvents::OnDropped>)
                return "OnDropped";
            else if constexpr (std::is_same_v<T, EngineEvents::OnPlaced>)
                return "OnPlaced";
            else
                return "Unknown";
        }
    }

    class EngineEvents::Visitor''',
)

replace_once(
    "apps/openmw/mwlua/engineevents.cpp",
    '''    void EngineEvents::callEngineHandlers()
    {
        Visitor vis(mGlobalScripts);
        for (const Event& event : mQueue)
            std::visit(vis, event);
        mQueue.clear();
    }''',
    '''    void EngineEvents::callEngineHandlers()
    {
        if (Debug::V33LuaTrace::enabled())
        {
            std::set<ESM::RefNum> activeObjects;
            std::set<ESM::RefNum> inactiveObjects;
            unsigned duplicateSameTypeObjects = 0;
            for (const Event& event : mQueue)
            {
                if (const OnActive* active = std::get_if<OnActive>(&event))
                    duplicateSameTypeObjects += !activeObjects.insert(active->mObject).second;
                else if (const OnInactive* inactive = std::get_if<OnInactive>(&event))
                    duplicateSameTypeObjects += !inactiveObjects.insert(inactive->mObject).second;
            }
            unsigned activeInactiveSameFrameObjects = 0;
            for (const ESM::RefNum& object : activeObjects)
                activeInactiveSameFrameObjects += inactiveObjects.count(object) != 0;
            std::set<ESM::RefNum> uniqueObjects = activeObjects;
            uniqueObjects.insert(inactiveObjects.begin(), inactiveObjects.end());
            Debug::V33LuaTrace::recordEngineEventBatch(static_cast<unsigned>(mQueue.size()),
                static_cast<unsigned>(uniqueObjects.size()), duplicateSameTypeObjects, activeInactiveSameFrameObjects);
        }

        Visitor vis(mGlobalScripts);
        for (const Event& event : mQueue)
        {
            std::visit(
                [&](const auto& typedEvent) {
                    using EventType = std::decay_t<decltype(typedEvent)>;
                    Debug::V33LuaTrace::EngineEventScope v33Trace(v33EngineEventName<EventType>());
                    vis(typedEvent);
                },
                event);
        }
        mQueue.clear();
    }''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''            ShadowData(ViewDependentData* vdd);''',
    '''            ShadowData(ViewDependentData* vdd, unsigned int shadowMapIndex = 0, unsigned int shadowMapCount = 1);''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        void setV33FarCascadeReuse(unsigned int interval, double maxTexelDrift, bool dynamicActorCasters);

        osg::ref_ptr<osg::StateSet> getOrCreateShadowsBinStateSet();''',
    '''        void setV33FarCascadeReuse(unsigned int interval, double maxTexelDrift, bool dynamicActorCasters);
        void setV33FarCascadeResolutionDivisor(unsigned int divisor);
        unsigned int getV33FarCascadeResolutionDivisor() const { return _v33FarCascadeResolutionDivisor; }

        osg::ref_ptr<osg::StateSet> getOrCreateShadowsBinStateSet();''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        unsigned int                            _v33FarCascadeUpdateInterval = 1;
        double                                  _v33FarCascadeMaxTexelDrift = 0.75;''',
    '''        unsigned int                            _v33FarCascadeUpdateInterval = 1;
        double                                  _v33FarCascadeMaxTexelDrift = 0.75;
        unsigned int                            _v33FarCascadeResolutionDivisor = 1;''',
)

replace_once(
    "components/sceneutil/shadow.cpp",
    '''        mShadowTechnique->setV33FarCascadeReuse(static_cast<unsigned>(settings.mV33FarCascadeUpdateInterval),
            settings.mV33FarCascadeMaxTexelDrift, settings.mActorShadows || settings.mPlayerShadows);

        if (settings.mEnableDebugHud)''',
    '''        mShadowTechnique->setV33FarCascadeReuse(static_cast<unsigned>(settings.mV33FarCascadeUpdateInterval),
            settings.mV33FarCascadeMaxTexelDrift, settings.mActorShadows || settings.mPlayerShadows);
        mShadowTechnique->setV33FarCascadeResolutionDivisor(
            static_cast<unsigned>(settings.mV33FarCascadeResolutionDivisor));

        if (settings.mEnableDebugHud)''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''#include <array>
#include <sstream>
#include <vector>''',
    '''#include <algorithm>
#include <array>
#include <sstream>
#include <vector>''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''MWShadowTechnique::ShadowData::ShadowData(MWShadowTechnique::ViewDependentData* vdd):
    _viewDependentData(vdd),''',
    '''MWShadowTechnique::ShadowData::ShadowData(
    MWShadowTechnique::ViewDependentData* vdd, unsigned int shadowMapIndex, unsigned int shadowMapCount):
    _viewDependentData(vdd),''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    osg::Vec2s textureSize = debug ? osg::Vec2s(512,512) : settings->getTextureSize();
    _texture->setTextureSize(textureSize.x(), textureSize.y());''',
    '''    osg::Vec2s textureSize = debug ? osg::Vec2s(512,512) : settings->getTextureSize();
    const unsigned int v33ResolutionDivisor
        = vdd->getViewDependentShadowMap()->getV33FarCascadeResolutionDivisor();
    if (!debug && v33ResolutionDivisor > 1 && shadowMapCount > 1 && shadowMapIndex + 1 == shadowMapCount)
    {
        textureSize.set(static_cast<short>(std::max(1, static_cast<int>(textureSize.x())
                                                        / static_cast<int>(v33ResolutionDivisor))),
            static_cast<short>(std::max(1, static_cast<int>(textureSize.y())
                                            / static_cast<int>(v33ResolutionDivisor))));
    }
    _texture->setTextureSize(textureSize.x(), textureSize.y());''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''void SceneUtil::MWShadowTechnique::setV33FarCascadeReuse(
    unsigned int interval, double maxTexelDrift, bool dynamicActorCasters)
{
    _v33FarCascadeUpdateInterval = dynamicActorCasters ? 1u : std::max(1u, interval);
    _v33FarCascadeMaxTexelDrift = std::max(0.0, maxTexelDrift);
}

void SceneUtil::MWShadowTechnique::enableFrontFaceCulling()''',
    '''void SceneUtil::MWShadowTechnique::setV33FarCascadeReuse(
    unsigned int interval, double maxTexelDrift, bool dynamicActorCasters)
{
    _v33FarCascadeUpdateInterval = dynamicActorCasters ? 1u : std::max(1u, interval);
    _v33FarCascadeMaxTexelDrift = std::max(0.0, maxTexelDrift);
}

void SceneUtil::MWShadowTechnique::setV33FarCascadeResolutionDivisor(unsigned int divisor)
{
    _v33FarCascadeResolutionDivisor = std::clamp(divisor, 1u, 2u);
}

void SceneUtil::MWShadowTechnique::enableFrontFaceCulling()''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''                sd = new ShadowData(vdd);''',
    '''                sd = new ShadowData(vdd, sm_i, numShadowMapsPerLight);''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''        "cascade4_ms,cascade5_ms,cascade6_ms,cascade7_ms,num_cascades,updated_cascades,reused_cascades,"
        "max_reuse_texel_drift");''',
    '''        "cascade4_ms,cascade5_ms,cascade6_ms,cascade7_ms,num_cascades,updated_cascades,reused_cascades,"
        "max_reuse_texel_drift,far_width,far_height,far_resolution_divisor");''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    unsigned int v33ReusedCascades = 0;
    double v33MaxReuseTexelDrift = 0.0;''',
    '''    unsigned int v33ReusedCascades = 0;
    double v33MaxReuseTexelDrift = 0.0;
    unsigned int v33FarWidth = 0;
    unsigned int v33FarHeight = 0;''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''            osg::ref_ptr<osg::Camera> camera = sd->_camera;

            camera->setProjectionMatrix(projectionMatrix);''',
    '''            osg::ref_ptr<osg::Camera> camera = sd->_camera;
            if (sm_i + 1 == numShadowMapsPerLight && camera->getViewport())
            {
                v33FarWidth = static_cast<unsigned int>(camera->getViewport()->width());
                v33FarHeight = static_cast<unsigned int>(camera->getViewport()->height());
            }

            camera->setProjectionMatrix(projectionMatrix);''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''                const double texelScale = static_cast<double>(settings->getTextureSize().x()) * 0.5;''',
    '''                const double texelScale = (camera->getViewport()
                        ? static_cast<double>(camera->getViewport()->width())
                        : static_cast<double>(settings->getTextureSize().x()))
                    * 0.5;''',
)

replace_once(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''        row << ',' << v3CascadeCount << ',' << v33UpdatedCascades << ',' << v33ReusedCascades << ','
            << v33MaxReuseTexelDrift;
        v3ShadowWriter.writeLine(row.str());''',
    '''        row << ',' << v3CascadeCount << ',' << v33UpdatedCascades << ',' << v33ReusedCascades << ','
            << v33MaxReuseTexelDrift << ',' << v33FarWidth << ',' << v33FarHeight << ','
            << _v33FarCascadeResolutionDivisor;
        v3ShadowWriter.writeLine(row.str());''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$FarShadowInterval = '1'
$FarShadowMaxTexelDrift = '0.75'
$RendererProfiling''',
    '''$FarShadowInterval = '1'
$FarShadowMaxTexelDrift = '0.75'
$LuaIdleTimerFastPath = 'false'
$FarShadowResolutionDivisor = '1'
$RendererProfiling''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 11 = V3.3 combined legacy experiments (same limitations as 9 and 10)'
do { $choice = Read-Host 'Enter 1 through 11' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11'))''',
    '''Write-Host ' 11 = V3.3 combined legacy experiments (same limitations as 9 and 10)'
Write-Host ' 12 = V3.3 idle-timer fast path (Lua/traversal optimization only)'
Write-Host ' 13 = V3.3 half-resolution far cascade (GPU optimization only)'
Write-Host ' 14 = V3.3 idle-timer + far-cascade GPU optimizations'
do { $choice = Read-Host 'Enter 1 through 14' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14'))''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '11' { $Experiment = 'v33-framepacing-gpu'; $PreloadBudget = '2'; $FarShadowInterval = '2' }
}''',
    '''    '11' { $Experiment = 'v33-framepacing-gpu'; $PreloadBudget = '2'; $FarShadowInterval = '2' }
    '12' { $Experiment = 'v33-idle-timer-fast-path'; $LuaIdleTimerFastPath = 'true' }
    '13' { $Experiment = 'v33-far-cascade-half-res'; $FarShadowResolutionDivisor = '2' }
    '14' { $Experiment = 'v33-tail-gpu-combined'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '2' }
}''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v33_far_shadow_update_interval=$FarShadowInterval",
    "v33_far_shadow_max_texel_drift=$FarShadowMaxTexelDrift"''',
    '''    "v33_far_shadow_update_interval=$FarShadowInterval",
    "v33_far_shadow_max_texel_drift=$FarShadowMaxTexelDrift",
    "v33_idle_timer_fast_path=$LuaIdleTimerFastPath",
    "v33_far_shadow_resolution_divisor=$FarShadowResolutionDivisor"''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_V33_FRAME_SUMMARY_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
    '''    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_V33_FRAME_SUMMARY_FILE','OPENMW_V33_LUA_CALLBACK_FILE',
    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    """    $env:OPENMW_V3_LUA_UPDATE_FILE = Join-Path $ProfileDir 'v3-lua-update.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'""",
    """    $env:OPENMW_V3_LUA_UPDATE_FILE = Join-Path $ProfileDir 'v3-lua-update.csv'
    $env:OPENMW_V33_LUA_CALLBACK_FILE = Join-Path $ProfileDir 'v33-lua-callbacks.csv'
    $env:OPENMW_V3_TRANSITION_FILE = Join-Path $ProfileDir 'v3-transition.csv'
    $env:OPENMW_V3_NAV_FILE = Join-Path $ProfileDir 'v3-nav.csv'
    $env:OPENMW_V3_TRACE_FILE = Join-Path $ProfileDir 'v3-trace.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'""",
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade max texel drift' $FarShadowMaxTexelDrift
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
    '''    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade max texel drift' $FarShadowMaxTexelDrift
    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade resolution divisor' $FarShadowResolutionDivisor
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
)

print("V3.3 tail-attribution, idle-timer, far-cascade resolution, and City world-stream patch completed successfully.")
