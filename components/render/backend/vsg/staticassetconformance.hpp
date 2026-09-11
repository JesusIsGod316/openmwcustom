#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETCONFORMANCE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETCONFORMANCE_H

#include "staticassetrealizer.hpp"

#include <vsg/nodes/Bin.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace RenderVsg
{
    // Backend-private bins used by the standalone static conformance View. The
    // neutral planner owns ordering semantics; these numbers are deliberately
    // absent from RenderCore and source-format records.
    inline constexpr std::int32_t StaticTraversalBinNumber = 9;
    inline constexpr std::int32_t StaticBackToFrontBinNumber = 10;

    struct StaticBillboardViewFrame
    {
        glm::dvec3 eye{ 0.0 };
        glm::dvec3 look{ 0.0, 0.0, -1.0 };
        glm::dvec3 up{ 0.0, 1.0, 0.0 };
    };

    // Pure value-level port of V3.25 NifOsg::AutoTransform. authoredLocal is
    // the billboard node's own neutral local matrix; eye/look/up are expressed
    // in the billboard parent's coordinate frame. This keeps the compatibility
    // math testable without exposing VSG or OSG through RenderCore.
    [[nodiscard]] glm::dmat4 evaluateLegacyBillboardLocal(const glm::dmat4& authoredLocal,
        RenderCore::ModelBillboardMode mode, const StaticBillboardViewFrame& frame) noexcept;

    // The View that records the result of realizeStaticAssetConformant() must
    // install these bins. NO_SORT preserves authored traversal insertion order;
    // DESCENDING consumes DepthSorted camera distance as far-to-near.
    [[nodiscard]] std::vector<vsg::ref_ptr<vsg::Bin>> createStaticConformanceBins();

    // Authoritative CP3B3 static realization entry. StaticAssetRealizer builds
    // backend objects and descriptors; this seam then applies the effective
    // loader-global V3.25 sort policy and reinstates the exact billboard
    // transform boundary that cannot be represented by one flattened matrix.
    // Nested billboard boundaries fail closed rather than being silently
    // flattened. The returned graph is ready for a View configured with
    // createStaticConformanceBins().
    [[nodiscard]] StaticRealizationResult realizeStaticAssetConformant(const RenderCore::RenderWorld& world,
        RenderCore::ModelHandle model, const StaticAssetPlan& plan, const StaticTextureResolver& textureResolver,
        vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {},
        const MeshPayloadResolver& meshPayloadResolver = {},
        std::span<const RenderCore::PopulationInstanceRecord> placements = {},
        glm::dvec3 placementOrigin = {}, float opacityMultiplier = 1.0f);
}

#endif
