#include "scripttracker.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace LuaUtil
{
    namespace
    {
        // Resident-cache mode for the V3 performance build: local Lua containers
        // stay loaded for the lifetime of the game session. Inactive containers
        // are not updated; keeping their Lua state resident only avoids the
        // serialize/unload/reload churn when revisiting cells.
        //
        // We still amortize cleanup of dead container handles so the tracking
        // queue does not grow without bound across loads/new games.
        constexpr std::size_t sCleanupDiv = 300;
    }

    void ScriptTracker::onLoad(ScriptsContainer& container)
    {
        mLoadedScripts.emplace(container.getWeakPointer());
    }

    void ScriptTracker::unloadInactiveScripts(LuaView&)
    {
        if (mLoadedScripts.empty())
            return;

        const std::size_t toProcess = std::max<std::size_t>(mLoadedScripts.size() / sCleanupDiv, 1);
        for (std::size_t i = 0; i < toProcess; ++i)
        {
            ScriptsContainerWeakPtr ptr = std::move(mLoadedScripts.front());
            mLoadedScripts.pop();

            // The container was destroyed (for example after loading another
            // save/new game). Drop the stale tracking handle.
            if (*ptr == nullptr)
                continue;

            // Keep live containers resident. We intentionally do not call
            // ensureUnloaded() in this build.
            mLoadedScripts.emplace(std::move(ptr));
        }
    }
}
