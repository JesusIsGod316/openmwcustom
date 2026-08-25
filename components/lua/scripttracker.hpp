#ifndef COMPONENTS_LUA_SCRIPTTRACKER_H
#define COMPONENTS_LUA_SCRIPTTRACKER_H

#include <memory>
#include <queue>

#include "scriptscontainer.hpp"

namespace LuaUtil
{
    class ScriptTracker
    {
        std::queue<ScriptsContainerWeakPtr> mLoadedScripts;

    public:
        void unloadInactiveScripts(LuaView& lua);

        void onLoad(ScriptsContainer& container);

        std::size_t size() const { return mLoadedScripts.size(); }
    };
}

#endif // COMPONENTS_LUA_SCRIPTTRACKER_H
