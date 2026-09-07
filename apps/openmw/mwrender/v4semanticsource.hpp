#ifndef OPENMW_MWRENDER_V4SEMANTICSOURCE_H
#define OPENMW_MWRENDER_V4SEMANTICSOURCE_H

#include <components/rendercore/activecellproducer.hpp>
#include <components/rendercore/framerenderstate.hpp>

#include <optional>

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace MWRender
{
    class Camera;

    // Source-side conversions for the V4 semantic renderer. These functions
    // deliberately consume game state, never OSG scene nodes: content-file
    // identity, active-cell ownership, placement, and the gameplay camera stay
    // authoritative regardless of the selected rendering backend.
    [[nodiscard]] std::optional<RenderCore::ActiveCellSource> makeV4ActiveCellSource(
        const MWWorld::CellStore& cell);

    [[nodiscard]] std::optional<RenderCore::StaticInstanceSource> makeV4StaticInstanceSource(
        const MWWorld::Ptr& ptr, RenderCore::ModelHandle model, RenderCore::AxisAlignedBounds localBounds);

    [[nodiscard]] std::optional<RenderCore::CameraState> makeV4MainCameraState(const Camera& camera,
        RenderCore::Extent2D extent, double verticalFieldOfViewDegrees, double nearPlane, double farPlane);
}

#endif
