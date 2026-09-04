#ifndef COMPONENTS_LUA_SCRIPTTRACKER_H
#define COMPONENTS_LUA_SCRIPTTRACKER_H

#include <cstddef>
#include <queue>
#include <utility>

#include "scriptscontainer.hpp"

namespace LuaUtil
{
    class ScriptTracker
    {
        using Frame = unsigned int;
        using TrackedScriptContainer = std::pair<ScriptsContainerWeakPtr, Frame>;

        std::queue<TrackedScriptContainer> mLoadedScripts;
        Frame mFrame = 0;
        bool mKeepResident = false;

    public:
        // V3's resident-cache optimization is a policy of the runtime local-script tracker,
        // not a replacement for ScriptTracker's normal unload semantics. Keeping the policy
        // explicit preserves unload/save/load behavior for other users (including component tests).
        void setKeepResident(bool keepResident) { mKeepResident = keepResident; }

        void unloadInactiveScripts(LuaView& lua);

        void onLoad(ScriptsContainer& container);

        std::size_t size() const { return mLoadedScripts.size(); }
    };
}

#endif // COMPONENTS_LUA_SCRIPTTRACKER_H
