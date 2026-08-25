#include "scripttracker.hpp"

#include <algorithm>
#include <chrono>

namespace LuaUtil
{
    namespace
    {
        // Keep recently used local Lua containers hot long enough for short
        // interior/exterior transitions. Real-time expiry avoids making the
        // cache lifetime depend on frame rate.
        constexpr auto sMinLoadedTime = std::chrono::seconds(30);
        constexpr auto sMaxLoadedTime = std::chrono::seconds(120);
        constexpr auto sUsageTimeGrowth = std::chrono::seconds(10);
        constexpr std::size_t sMinToProcess = 1;
        constexpr std::size_t sToProcessDiv = 20; // 5%
    }

    void ScriptTracker::onLoad(ScriptsContainer& container)
    {
        mLoadedScripts.emplace(container.getWeakPointer(), Clock::now() + sMinLoadedTime);
    }

    void ScriptTracker::unloadInactiveScripts(LuaView& lua)
    {
        const auto now = Clock::now();
        std::size_t toProcess = std::max(mLoadedScripts.size() / sToProcessDiv, sMinToProcess);
        while (toProcess && !mLoadedScripts.empty())
        {
            --toProcess;
            auto [ptr, ttl] = std::move(mLoadedScripts.front());
            mLoadedScripts.pop();
            ScriptsContainer* container = *ptr;
            // Object no longer exists, cease tracking
            if (!container)
                continue;
            // Keep active local scripts hot. Once they become inactive they
            // retain at least the minimum real-time grace period.
            if (container->isActive())
                ttl = std::max(ttl, now + sMinLoadedTime);
            else
            {
                bool activeSinceLastPop = container->mRequiredLoading;
                if (activeSinceLastPop)
                {
                    container->mRequiredLoading = false;
                    ttl = std::min(ttl + sUsageTimeGrowth, now + sMaxLoadedTime);
                }
                else if (ttl < now)
                {
                    container->ensureUnloaded(lua);
                    continue;
                }
            }
            mLoadedScripts.emplace(std::move(ptr), ttl);
        }
    }
}
