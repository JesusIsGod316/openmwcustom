#include "v4engineframecoordinator.hpp"

#include "renderingmanager.hpp"
#include "v4enginerenderbridge.hpp"
#include "v4semanticsource.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include <components/rendercore/namedvisualsemantics.hpp>
#include <components/settings/values.hpp>

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

        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (!world)
            return fail("authoritative world state is unavailable for named visual switch capture");
        RenderCore::NightDaySwitchState nightDayState;
        switch (world->getNightDayMode())
        {
            case 0:
                nightDayState = RenderCore::NightDaySwitchState::Default;
                break;
            case 1:
                nightDayState = RenderCore::NightDaySwitchState::ExteriorNight;
                break;
            case 2:
                nightDayState = RenderCore::NightDaySwitchState::InteriorDay;
                break;
            default:
                return fail("authoritative weather state produced an invalid NightDaySwitch mode");
        }
        if (!mBridge.configureNamedSwitchState(nightDayState, Settings::game().mDayNightSwitches))
            return fail("V4 renderer rejected the authoritative NightDaySwitch state");

        if (!mBridge.synchronizeExteriorTerrain(rendering, cell))
            return fail(mBridge.lastDiagnostic().empty()
                    ? "authoritative terrain state could not produce a compatible V4 frame"
                    : mBridge.lastDiagnostic());

        std::optional<V4MainFrameSource> source = makeV4MainFrameSource(
            rendering, cell, rendering.isUnderwater(), *extent, simulationTime, frameDelta, invalidateHistory);
        if (!source)
            return fail("authoritative gameplay state could not produce a compatible V4 main frame");
        if (!mBridge.captureDynamicFrameState(rendering, *source))
            return fail(mBridge.lastDiagnostic().empty()
                    ? "authoritative actor state could not produce a compatible V4 frame"
                    : mBridge.lastDiagnostic());

        const RenderCore::RenderFrameResult result = mBridge.renderMainFrameWithNativeLocalMap(*source);
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
