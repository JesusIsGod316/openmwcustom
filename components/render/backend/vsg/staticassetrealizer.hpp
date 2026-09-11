#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETREALIZER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETREALIZER_H

#include "staticassetplan.hpp"

#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/utils/SharedObjects.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace RenderVsg
{
    using StaticTextureResolver = std::function<vsg::ref_ptr<vsg::Data>(
        const RenderCore::TextureRecord&, const RenderCore::TextureRealizationKey&)>;
    using MeshPayloadResolver = std::function<const RenderCore::MeshPayload*(
        RenderCore::MeshHandle, RenderCore::ModelNodeIndex)>;

    // Backend-private shader-family routing. RenderCore stays source-format and
    // renderer agnostic; CP3B3 can reproduce legacy fixed-function/NIF material
    // semantics without forcing future CP4+/modern content through the same
    // shader family. ModernPbr is intentionally reserved for an explicitly
    // selected future producer path rather than inferred from NIF provenance.
    enum class StaticMaterialShaderFamily : std::uint8_t
    {
        LegacyCompatibility,
        ModernPbr,
    };

    struct StaticRealizationStats
    {
        std::uint32_t drawCount = 0;
        std::uint32_t sortedDrawCount = 0;
        std::uint32_t pipelineKeys = 0;
        std::uint32_t materialKeys = 0;
        std::uint32_t textureViewKeys = 0;
        std::uint32_t samplerKeys = 0;
        std::uint32_t textureLoads = 0;
        std::uint32_t textureCacheHits = 0;
        std::uint32_t unsupportedTextureBindings = 0;
        std::uint32_t billboardDraws = 0;
        std::uint32_t runtimeContextEffects = 0;
        std::uint32_t legacyCompatibilityDraws = 0;
        std::uint32_t modernPbrDraws = 0;
    };

    struct StaticRealizationResult
    {
        vsg::ref_ptr<vsg::Group> root;
        StaticRealizationStats stats;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(root); }
    };

    // CP3B3 backend-private VSG realization. RenderCore remains VSG/Vulkan free;
    // the realizer consumes only published neutral records plus the deterministic
    // StaticAssetPlan and materializes VSG arrays, descriptors, pipeline state and
    // draw commands. Texture bytes are supplied through a resolver so VFS/image
    // decoding ownership stays outside RenderCore and can be shared with CP4 paging.
    //
    // CP4+ boundary: shader-family choice remains backend-private and may later be
    // supplied by terrain/modern-material producers without changing neutral
    // texture/material handles, sampler identities, residency accounting, or
    // publication lifetime rules.
    class StaticAssetRealizer
    {
    public:
        explicit StaticAssetRealizer(vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {});

        [[nodiscard]] StaticRealizationResult realize(const RenderCore::RenderWorld& world,
            const StaticAssetPlan& plan, const StaticTextureResolver& textureResolver,
            const MeshPayloadResolver& meshPayloadResolver = {},
            std::span<const RenderCore::PopulationInstanceRecord> placements = {},
            glm::dvec3 placementOrigin = {}, float opacityMultiplier = 1.0f) const;

    private:
        vsg::ref_ptr<vsg::SharedObjects> mSharedObjects;
    };
}

#endif
