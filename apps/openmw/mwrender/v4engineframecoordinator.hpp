#ifndef OPENMW_MWRENDER_V4ENGINEFRAMECOORDINATOR_H
#define OPENMW_MWRENDER_V4ENGINEFRAMECOORDINATOR_H

#include <components/rendercore/renderer.hpp>

#include "v4engineframesource.hpp"

#include <optional>
#include <string>

namespace MWWorld
{
    class Cell;
}

namespace MWRender
{
    class RenderingManager;
    class V4EngineRenderBridge;

    // Transitional application-loop adapter. It snapshots the authoritative
    // gameplay camera/environment only after the world/rendering update, then
    // hands immutable neutral state to the Vulkan bridge. Source or backend
    // failures are sticky; temporary surface unavailability remains retryable.
    class V4EngineFrameCoordinator final
    {
    public:
        explicit V4EngineFrameCoordinator(V4EngineRenderBridge& bridge)
            : mBridge(bridge)
        {
        }

        // Capture every live OpenMW/OSG value on the main thread before the Lua
        // worker is released. presentPrepared() subsequently consumes only this
        // backend-neutral snapshot and Vulkan-owned state.
        RenderCore::RenderFrameResult prepare(const RenderingManager& rendering, const MWWorld::Cell& cell,
            double simulationTime, double frameDelta, bool invalidateHistory = false);
        RenderCore::RenderFrameResult presentPrepared();

        RenderCore::RenderFrameResult render(const RenderingManager& rendering, const MWWorld::Cell& cell,
            double simulationTime, double frameDelta, bool invalidateHistory = false);

        [[nodiscard]] bool healthy() const noexcept { return mHealthy; }
        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        RenderCore::RenderFrameResult fail(std::string diagnostic);

        V4EngineRenderBridge& mBridge;
        std::optional<V4MainFrameSource> mPreparedSource;
        std::string mLastDiagnostic;
        bool mHealthy = true;
    };
}

#endif
