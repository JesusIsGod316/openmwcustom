#ifndef COMPONENTS_LUA_SCRIPTTRACKER_H
#define COMPONENTS_LUA_SCRIPTTRACKER_H

#include <chrono>
#include <memory>
#include <queue>
#include <utility>

#include "scriptscontainer.hpp"

namespace LuaUtil
{
    class ScriptTracker
    {
        using Clock = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;
        using TrackedScriptContainer = std::pair<ScriptsContainerWeakPtr, TimePoint>;
        std::queue<TrackedScriptContainer> mLoadedScripts;

    public:
        void unloadInactiveScripts(LuaView& lua);

        void onLoad(ScriptsContainer& container);

        std::size_t size() const { return mLoadedScripts.size(); }
    };
}

#endif // COMPONENTS_LUA_SCRIPTTRACKER_H
