#include "engineevents.hpp"

#include <cstdlib>
#include <set>
#include <type_traits>

#include <components/debug/debuglog.hpp>
#include <components/debug/v33luatrace.hpp>
#include <components/debug/v320luafastpath.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/worldmodel.hpp"

#include "globalscripts.hpp"
#include "localscripts.hpp"
#include "object.hpp"

namespace MWLua
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
        bool v317EngineFastPathEnabled()
        {
            // V3.20 promotes the mature V3.17 path to a native setting while preserving
            // the old environment switch and adding a fail-closed causal override.
            static const bool enabled = [] {
                const bool configured = Settings::lua().mV320EngineLuaFastPaths;
                if (const char* value = std::getenv("OPENMW_V320_ENGINE_LUA_FASTPATHS"))
                    return *value != 0 ? std::atoi(value) != 0 : configured;
                return std::getenv("OPENMW_V317_LUA_OPT") != nullptr || configured;
            }();
            return enabled;
        }
    }

    class EngineEvents::Visitor
    {
    public:
        explicit Visitor(GlobalScripts& globalScripts)
            : mGlobalScripts(globalScripts)
        {
        }

        void operator()(const OnActive& event) const
        {
            MWWorld::Ptr ptr = getPtr(event.mObject);
            if (ptr.isEmpty())
                return;

            if (static_cast<bool>(Settings::cells().mV37ActiveEventFastPath))
            {
                if (ptr.getCellRef().getRefId() == "player")
                {
                    if (mGlobalScripts.hasPlayerAddedHandlers())
                    {
                        const GObject object(ptr);
                        mGlobalScripts.playerAdded(object);
                    }
                }
                else
                {
                    const bool objectHandlers = mGlobalScripts.hasObjectActiveHandlers();
                    const bool actorHandlers = mGlobalScripts.hasActorActiveHandlers();
                    const bool itemHandlers = mGlobalScripts.hasItemActiveHandlers();
                    if (objectHandlers || actorHandlers || itemHandlers)
                    {
                        const GObject object(ptr);
                        if (objectHandlers)
                            mGlobalScripts.objectActive(object);
                        if (actorHandlers || itemHandlers)
                        {
                            const MWWorld::Class& objClass = ptr.getClass();
                            if (actorHandlers && objClass.isActor())
                                mGlobalScripts.actorActive(object);
                            if (itemHandlers && objClass.isItem(ptr))
                                mGlobalScripts.itemActive(object);
                        }
                    }
                }
            }
            else if (ptr.getCellRef().getRefId() == "player")
                mGlobalScripts.playerAdded(GObject(ptr));
            else
            {
                mGlobalScripts.objectActive(GObject(ptr));
                const MWWorld::Class& objClass = ptr.getClass();
                if (objClass.isActor())
                    mGlobalScripts.actorActive(GObject(ptr));
                if (objClass.isItem(ptr))
                    mGlobalScripts.itemActive(GObject(ptr));
            }

            // Do not skip or defer local activation: this can materialize a previously
            // unloaded container and executing onActive at the original semantic point
            // is required for compatibility.
            if (auto* scripts = getLocalScripts(ptr))
                scripts->setActive(true);
        }

        void operator()(const OnInactive& event) const
        {
            if (auto* scripts = getLocalScripts(event.mObject))
                scripts->setActive(false);
        }

        void operator()(const OnTeleported& event) const
        {
            if (auto* scripts = getLocalScripts(event.mObject))
                scripts->onTeleported();
        }

        void operator()(const OnActivate& event) const
        {
            if (v317EngineFastPathEnabled())
            {
                MWWorld::Ptr obj = getPtr(event.mObject);
                if (obj.isEmpty())
                    return;
                LocalScripts* scripts = getLocalScripts(obj);
                const bool globalHandlers = mGlobalScripts.hasOnActivateHandlers();
                const bool localHandlers = scripts && scripts->hasOnActivatedHandlers();
                Debug::V320LuaFastPath::recordEventCheck(globalHandlers || localHandlers);
                if (!globalHandlers && !localHandlers)
                {
                    if (Settings::lua().mLuaDebug)
                        (void)getPtr(event.mActor);
                    return;
                }
                MWWorld::Ptr actor = getPtr(event.mActor);
                if (actor.isEmpty())
                    return;
                if (globalHandlers)
                {
                    Debug::V320LuaFastPath::recordDispatch();
                    mGlobalScripts.onActivate(GObject(obj), GObject(actor));
                }
                if (localHandlers)
                {
                    Debug::V320LuaFastPath::recordDispatch();
                    scripts->onActivated(LObject(actor));
                }
                return;
            }

            MWWorld::Ptr obj = getPtr(event.mObject);
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty() || obj.isEmpty())
                return;
            mGlobalScripts.onActivate(GObject(obj), GObject(actor));
            if (auto* scripts = getLocalScripts(obj))
                scripts->onActivated(LObject(actor));
        }

        void operator()(const OnUseItem& event) const
        {
            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || mGlobalScripts.hasOnUseItemHandlers();
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (!v320HasHandlers)
            {
                if (Settings::lua().mLuaDebug)
                {
                    (void)getPtr(event.mObject);
                    (void)getPtr(event.mActor);
                }
                return;
            }
            MWWorld::Ptr obj = getPtr(event.mObject);
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty() || obj.isEmpty())
                return;
            Debug::V320LuaFastPath::recordDispatch();
            mGlobalScripts.onUseItem(GObject(obj), GObject(actor), event.mForce);
        }

        void operator()(const OnConsume& event) const
        {
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty())
                return;
            LocalScripts* scripts = getLocalScripts(actor);
            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || (scripts && scripts->hasOnConsumeHandlers());
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (!v320HasHandlers)
            {
                if (Settings::lua().mLuaDebug)
                    (void)getPtr(event.mConsumable);
                return;
            }
            MWWorld::Ptr consumable = getPtr(event.mConsumable);
            if (consumable.isEmpty())
                return;
            if (scripts)
            {
                Debug::V320LuaFastPath::recordDispatch();
                scripts->onConsume(LObject(consumable));
            }
        }

        void operator()(const OnDropped& event) const
        {
            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || mGlobalScripts.hasOnDroppedHandlers();
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (!v320HasHandlers)
            {
                if (Settings::lua().mLuaDebug)
                {
                    (void)getPtr(event.mObject);
                    (void)getPtr(event.mActor);
                }
                return;
            }
            MWWorld::Ptr obj = getPtr(event.mObject);
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (obj.isEmpty() || actor.isEmpty())
                return;
            Debug::V320LuaFastPath::recordDispatch();
            mGlobalScripts.onDropped(GObject(obj), GObject(actor), event.mPosition, event.mRotation);
        }

        void operator()(const OnPlaced& event) const
        {
            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || mGlobalScripts.hasOnPlacedHandlers();
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (!v320HasHandlers)
            {
                if (Settings::lua().mLuaDebug)
                {
                    (void)getPtr(event.mObject);
                    (void)getPtr(event.mActor);
                }
                return;
            }
            MWWorld::Ptr obj = getPtr(event.mObject);
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (obj.isEmpty() || actor.isEmpty())
                return;
            Debug::V320LuaFastPath::recordDispatch();
            mGlobalScripts.onPlaced(GObject(obj), GObject(actor), event.mPosition, event.mRotation);
        }

        void operator()(const OnNewExterior& event) const
        {
            const bool v320FastPath = v317EngineFastPathEnabled();
            const bool v320HasHandlers = !v320FastPath || mGlobalScripts.hasOnNewExteriorHandlers();
            if (v320FastPath)
                Debug::V320LuaFastPath::recordEventCheck(v320HasHandlers);
            if (v320HasHandlers)
            {
                Debug::V320LuaFastPath::recordDispatch();
                mGlobalScripts.onNewExterior(GCell{ &event.mCell });
            }
        }

        void operator()(const OnAnimationTextKey& event) const
        {
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty())
                return;
            if (auto* scripts = getLocalScripts(actor))
                scripts->onAnimationTextKey(event.mGroupname, event.mKey);
        }

        void operator()(const OnAnimationEnded& event) const
        {
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty())
                return;
            if (auto* scripts = getLocalScripts(actor))
                scripts->onAnimationEnded(
                    event.mGroupname, event.mStartKey, event.mStopKey, event.mTime, event.mCompletion);
        }

        void operator()(const OnSkillUse& event) const
        {
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty())
                return;
            if (auto* scripts = getLocalScripts(actor))
                scripts->onSkillUse(event.mSkill, event.useType, event.scale);
        }

        void operator()(const OnSkillLevelUp& event) const
        {
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty())
                return;
            if (auto* scripts = getLocalScripts(actor))
                scripts->onSkillLevelUp(event.mSkill, event.mSource);
        }

        void operator()(const OnJailTimeServed& event) const
        {
            MWWorld::Ptr actor = getPtr(event.mActor);
            if (actor.isEmpty())
                return;
            if (auto* scripts = getLocalScripts(actor))
                scripts->onJailTimeServed(event.mDays);
        }

    private:
        MWWorld::Ptr getPtr(ESM::RefNum id) const
        {
            MWWorld::Ptr res = mWorldModel->getPtr(id);
            if (res.isEmpty() && Settings::lua().mLuaDebug)
                Log(Debug::Verbose) << "Can not find object" << id.toString() << " when calling engine hanglers";
            return res;
        }

        LocalScripts* getLocalScripts(const MWWorld::Ptr& ptr) const
        {
            if (ptr.isEmpty())
                return nullptr;
            else
                return ptr.getRefData().getLuaScripts();
        }

        LocalScripts* getLocalScripts(ESM::RefNum id) const { return getLocalScripts(getPtr(id)); }

        GlobalScripts& mGlobalScripts;
        MWWorld::WorldModel* mWorldModel = MWBase::Environment::get().getWorldModel();
    };

    void EngineEvents::callEngineHandlers()
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
    }

}
