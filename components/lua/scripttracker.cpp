#include "scripttracker.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace LuaUtil
{
    namespace
    {
        // Normal ScriptTracker eviction policy. Component semantics (save/unload/reload,
        // including content-file remapping) depend on inactive containers eventually
        // reaching ensureUnloaded().
        constexpr unsigned sMinLoadedFrames = 50;
        constexpr unsigned sMaxLoadedFrames = 600;
        constexpr unsigned sUsageFrameGrowth = 10;
        constexpr std::size_t sMinToProcess = 1;
        constexpr std::size_t sToProcessDiv = 20; // 5%

        // V3 resident-cache mode still amortizes cleanup of dead container handles so
        // the tracking queue does not grow without bound across loads/new games.
        constexpr std::size_t sCleanupDiv = 300;
    }

    void ScriptTracker::onLoad(ScriptsContainer& container)
    {
        mLoadedScripts.emplace(container.getWeakPointer(), sMinLoadedFrames + mFrame);
    }

    void ScriptTracker::unloadInactiveScripts(LuaView& lua)
    {
        if (mKeepResident)
        {
            if (mLoadedScripts.empty())
                return;

            const std::size_t toProcess = std::max<std::size_t>(mLoadedScripts.size() / sCleanupDiv, 1);
            for (std::size_t i = 0; i < toProcess; ++i)
            {
                auto [ptr, ttl] = std::move(mLoadedScripts.front());
                mLoadedScripts.pop();

                // The container was destroyed (for example after loading another
                // save/new game). Drop the stale tracking handle.
                if (*ptr == nullptr)
                    continue;

                // V3 performance policy: keep live local-script containers resident.
                // The TTL is retained only so switching policy does not lose tracker state.
                mLoadedScripts.emplace(std::move(ptr), ttl);
            }
            return;
        }

        // This code is technically incorrect if mFrame overflows, but even at very high
        // frame rates that takes long enough to be irrelevant for a game session.
        std::size_t toProcess = std::max(mLoadedScripts.size() / sToProcessDiv, sMinToProcess);
        while (toProcess && !mLoadedScripts.empty())
        {
            --toProcess;
            auto [ptr, ttl] = std::move(mLoadedScripts.front());
            mLoadedScripts.pop();
            ScriptsContainer* container = *ptr;

            // Object no longer exists, cease tracking.
            if (!container)
                continue;

            // Ignore activity of local scripts in the active grid.
            if (container->isActive())
                ttl = std::max(ttl, mFrame + sMinLoadedFrames);
            else
            {
                const bool activeSinceLastPop = container->mRequiredLoading;
                if (activeSinceLastPop)
                {
                    container->mRequiredLoading = false;
                    ttl = std::min(ttl + sUsageFrameGrowth, mFrame + sMaxLoadedFrames);
                }
                else if (ttl < mFrame)
                {
                    container->ensureUnloaded(lua);
                    continue;
                }
            }
            mLoadedScripts.emplace(std::move(ptr), ttl);
        }
        ++mFrame;
    }
}
