#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETREALIZER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICASSETREALIZER_H

#include "staticassetplan.hpp"

#include <vsg/core/Data.h>
#include <vsg/core/ref_ptr.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vsg
{
    class Group;
    class SharedObjects;
}

namespace RenderVsg
{
    using StaticTextureResolver = std::function<vsg::ref_ptr<vsg::Data>(
        const RenderCore::TextureRecord&, const RenderCore::TextureRealizationKey&)>;

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
    class StaticAssetRealizer
    {
    public:
        explicit StaticAssetRealizer(vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {});

        [[nodiscard]] StaticRealizationResult realize(const RenderCore::RenderWorld& world,
            const StaticAssetPlan& plan, const StaticTextureResolver& textureResolver) const;

    private:
        vsg::ref_ptr<vsg::SharedObjects> mSharedObjects;
    };
}

#endif
