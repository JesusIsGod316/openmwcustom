#ifndef OPENMW_MWRENDER_V4ENGINEFRAMECOORDINATOR_H
#define OPENMW_MWRENDER_V4ENGINEFRAMECOORDINATOR_H

#include <components/rendercore/renderer.hpp>

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

        RenderCore::RenderFrameResult render(const RenderingManager& rendering, const MWWorld::Cell& cell,
            double simulationTime, double frameDelta, bool invalidateHistory = false);

        [[nodiscard]] bool healthy() const noexcept { return mHealthy; }
        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        RenderCore::RenderFrameResult fail(std::string diagnostic);

        V4EngineRenderBridge& mBridge;
        std::string mLastDiagnostic;
        bool mHealthy = true;
    };
}

#endif
