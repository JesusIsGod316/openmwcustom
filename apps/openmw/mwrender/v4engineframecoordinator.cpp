#include "v4engineframecoordinator.hpp"

#include "renderingmanager.hpp"
#include "v4enginerenderbridge.hpp"
#include "v4semanticsource.hpp"

#include <optional>
#include <utility>

namespace MWRender
{
    RenderCore::RenderFrameResult V4EngineFrameCoordinator::render(const RenderingManager& rendering,
        const MWWorld::Cell& cell, double simulationTime, double frameDelta, bool invalidateHistory)
    {
        if (!mHealthy)
            return RenderCore::RenderFrameResult::Failed;

        const std::optional<RenderCore::Extent2D> extent = mBridge.outputExtent();
        if (!extent)
        {
            mLastDiagnostic = "V4 output surface is hidden, minimized, or temporarily has no pixel extent";
            return RenderCore::RenderFrameResult::Skipped;
        }

        const std::optional<V4MainFrameSource> source = makeV4MainFrameSource(rendering, cell,
            rendering.isUnderwater(), *extent, simulationTime, frameDelta, invalidateHistory);
        if (!source)
            return fail("authoritative gameplay state could not produce a compatible V4 main frame");

        const RenderCore::RenderFrameResult result = mBridge.renderMainFrame(*source);
        mLastDiagnostic = mBridge.lastDiagnostic();
        if (result == RenderCore::RenderFrameResult::Failed)
            return fail(mLastDiagnostic.empty() ? "V4 render bridge rejected the main frame" : mLastDiagnostic);
        return result;
    }

    RenderCore::RenderFrameResult V4EngineFrameCoordinator::fail(std::string diagnostic)
    {
        if (mHealthy)
        {
            mHealthy = false;
            mLastDiagnostic = std::move(diagnostic);
        }
        return RenderCore::RenderFrameResult::Failed;
    }
}
