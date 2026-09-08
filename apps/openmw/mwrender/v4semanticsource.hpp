#ifndef OPENMW_MWRENDER_V4SEMANTICSOURCE_H
#define OPENMW_MWRENDER_V4SEMANTICSOURCE_H

#include "v4engineframesource.hpp"

#include <components/rendercore/activecellproducer.hpp>
#include <components/rendercore/framerenderstate.hpp>

#include <optional>
#include <string>

namespace MWWorld
{
    class Cell;
    class CellStore;
    class Ptr;
}

namespace MWRender
{
    class Camera;
    class RenderingManager;
    struct FogState;

    // Source-side conversions for the V4 semantic renderer. These functions
    // deliberately consume game state, never OSG scene nodes: content-file
    // identity, active-cell ownership, placement, and the gameplay camera stay
    // authoritative regardless of the selected rendering backend.
    [[nodiscard]] std::optional<std::string> makeV4CellIdentity(const MWWorld::CellStore& cell);

    [[nodiscard]] std::optional<std::string> makeV4ReferenceIdentity(const MWWorld::Ptr& ptr);

    [[nodiscard]] std::optional<RenderCore::ActiveCellSource> makeV4ActiveCellSource(
        const MWWorld::CellStore& cell);

    [[nodiscard]] std::optional<RenderCore::StaticInstanceSource> makeV4StaticInstanceSource(
        const MWWorld::Ptr& ptr, RenderCore::ModelHandle model, RenderCore::AxisAlignedBounds localBounds);

    // Preserves authored ESM3/ESM4 light behaviour at the backend-neutral
    // boundary, including negative colors, fallback attenuation, off-default
    // state, and temporal flicker/pulse classification.
    [[nodiscard]] std::optional<RenderCore::CellLightSource> makeV4CellLightSource(const MWWorld::Ptr& ptr);

    // CP3C authoritative interior environment adapter. Quasi/exteriors are
    // intentionally rejected until the weather/sky producer can supply their
    // live blended state; fabricating those values would break shader mods.
    [[nodiscard]] std::optional<RenderCore::FrameEnvironmentState> makeV4InteriorEnvironmentState(
        const MWWorld::Cell& cell, const FogState& fog, bool underwater, float nightEyeFactor);

    [[nodiscard]] std::optional<RenderCore::CameraState> makeV4MainCameraState(const Camera& camera,
        RenderCore::Extent2D extent, double verticalFieldOfViewDegrees, double nearPlane, double farPlane);

    // Transitional production adapter: capture the established gameplay
    // controller/environment after world update, then cross the engine/backend
    // boundary only as immutable neutral data.
    [[nodiscard]] std::optional<V4MainFrameSource> makeV4MainFrameSource(const RenderingManager& rendering,
        const MWWorld::Cell& cell, bool underwater, RenderCore::Extent2D extent, double simulationTime,
        double frameDelta, bool invalidateHistory = false);
}

#endif
