#ifndef OPENMW_COMPONENTS_RENDERCORE_RENDERER_H
#define OPENMW_COMPONENTS_RENDERCORE_RENDERER_H

#include "framerenderstate.hpp"
#include "renderworld.hpp"

#include <cstdint>

namespace RenderCore
{
    enum class RenderBackendKind : std::uint8_t
    {
        LegacyOpenGL,
        VsgVulkan,
    };

    enum class RenderBackendPreference : std::uint8_t
    {
        Auto,
        LegacyOpenGL,
        VsgVulkan,
    };

    enum class RenderCompatibilityFacet : std::uint64_t
    {
        StaticWorld = 1ull << 0,
        DynamicActors = 1ull << 1,
        TerrainAndGroundcover = 1ull << 2,
        EnvironmentLightingWaterSky = 1ull << 3,
        UiVideoAndComposition = 1ull << 4,
        PostProcessingApi = 1ull << 5,
        ShaderModSurface = 1ull << 6,
        StereoMultiview = 1ull << 7,
        CaptureMapAndPreviewViews = 1ull << 8,
        GameplaySaveLuaIntegration = 1ull << 9,
    };

    [[nodiscard]] constexpr std::uint64_t compatibilityFacet(RenderCompatibilityFacet facet) noexcept
    {
        return static_cast<std::uint64_t>(facet);
    }

    inline constexpr std::uint64_t RequiredAutomaticVsgCompatibility
        = compatibilityFacet(RenderCompatibilityFacet::StaticWorld)
        | compatibilityFacet(RenderCompatibilityFacet::DynamicActors)
        | compatibilityFacet(RenderCompatibilityFacet::TerrainAndGroundcover)
        | compatibilityFacet(RenderCompatibilityFacet::EnvironmentLightingWaterSky)
        | compatibilityFacet(RenderCompatibilityFacet::UiVideoAndComposition)
        | compatibilityFacet(RenderCompatibilityFacet::PostProcessingApi)
        | compatibilityFacet(RenderCompatibilityFacet::ShaderModSurface)
        | compatibilityFacet(RenderCompatibilityFacet::StereoMultiview)
        | compatibilityFacet(RenderCompatibilityFacet::CaptureMapAndPreviewViews)
        | compatibilityFacet(RenderCompatibilityFacet::GameplaySaveLuaIntegration);

    struct RenderBackendCapabilities
    {
        bool legacyOpenGL = true;
        bool vsgVulkan = false;
        // Qualification is granular so passing the first static-world slice can
        // never accidentally make Vulkan the default while shader/postfx, UI,
        // actors, terrain, or gameplay integration remain incomplete. Explicit
        // VSG/Vulkan requests remain available for checkpoint testing.
        std::uint64_t vsgVulkanCompatibilityFacets = 0;
    };

    struct RenderBackendRequest
    {
        RenderBackendPreference preference = RenderBackendPreference::Auto;
        bool allowFallback = true;
    };

    struct RenderBackendSelection
    {
        RenderBackendKind backend = RenderBackendKind::LegacyOpenGL;
        bool valid = false;
        bool fellBack = false;
    };

    [[nodiscard]] constexpr RenderBackendSelection selectRenderBackend(
        RenderBackendRequest request, RenderBackendCapabilities capabilities) noexcept
    {
        if (request.preference == RenderBackendPreference::Auto)
        {
            // Compatibility-first default: prefer the modern backend only after it
            // has passed the applicable OpenMW mod/shader/content parity gate.
            if (capabilities.vsgVulkan
                && (capabilities.vsgVulkanCompatibilityFacets & RequiredAutomaticVsgCompatibility)
                    == RequiredAutomaticVsgCompatibility)
                return { RenderBackendKind::VsgVulkan, true, false };
            if (capabilities.legacyOpenGL)
                return { RenderBackendKind::LegacyOpenGL, true, false };
            if (capabilities.vsgVulkan)
                return { RenderBackendKind::VsgVulkan, true, false };
            return {};
        }

        if (request.preference == RenderBackendPreference::VsgVulkan)
        {
            if (capabilities.vsgVulkan)
                return { RenderBackendKind::VsgVulkan, true, false };
            if (request.allowFallback && capabilities.legacyOpenGL)
                return { RenderBackendKind::LegacyOpenGL, true, true };
            return {};
        }

        if (capabilities.legacyOpenGL)
            return { RenderBackendKind::LegacyOpenGL, true, false };
        if (request.allowFallback && capabilities.vsgVulkan)
            return { RenderBackendKind::VsgVulkan, true, true };
        return {};
    }

    // Renderer entry coherence gate. Backends must not consume frame-local state
    // against a different logical world revision, or dereference dynamic handles
    // that were retired/replaced after the frame snapshot was assembled.
    [[nodiscard]] inline bool frameCompatibleWithWorld(
        const RenderWorld& world, const FrameRenderState& frame) noexcept
    {
        if (!frame.valid() || frame.worldEpoch() != world.epoch()
            || frame.renderWorldRevision() != world.revision())
            return false;

        for (const DynamicTransformState& transform : frame.dynamicTransforms())
        {
            if (!world.get(transform.instance))
                return false;
        }

        for (const DynamicMaterialState& state : frame.dynamicMaterials())
        {
            const MaterialRecord* material = world.get(state.material);
            if (!material)
                return false;
            for (const DynamicTextureTransformState& transform : state.textureTransforms)
            {
                if (transform.bindingIndex >= material->textures.size())
                    return false;
            }
        }
        return true;
    }

    enum class RenderFrameResult : std::uint8_t
    {
        Presented,
        Skipped,
        Failed,
    };

    // Deliberately narrow semantic backend seam. The game publishes logical world
    // and immutable frame state above this boundary; backend objects never cross it.
    class SemanticRenderer
    {
    public:
        virtual ~SemanticRenderer() = default;
        [[nodiscard]] virtual RenderBackendKind backendKind() const noexcept = 0;
        virtual RenderFrameResult renderFrame(const RenderWorld& world, const FrameRenderState& frame) = 0;
        virtual void waitIdle() = 0;
    };
}

#endif
