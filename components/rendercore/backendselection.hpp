#ifndef OPENMW_COMPONENTS_RENDERCORE_BACKENDSELECTION_H
#define OPENMW_COMPONENTS_RENDERCORE_BACKENDSELECTION_H

#include <cstdint>
#include <optional>
#include <string_view>

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
        ConfigurationAndContentDiscovery = 1ull << 10,
    };

    [[nodiscard]] constexpr std::uint64_t compatibilityFacet(RenderCompatibilityFacet facet) noexcept
    {
        return static_cast<std::uint64_t>(facet);
    }

    [[nodiscard]] constexpr std::string_view renderCompatibilityFacetName(
        RenderCompatibilityFacet facet) noexcept
    {
        switch (facet)
        {
            case RenderCompatibilityFacet::StaticWorld: return "static world";
            case RenderCompatibilityFacet::DynamicActors: return "dynamic actors";
            case RenderCompatibilityFacet::TerrainAndGroundcover: return "terrain and groundcover";
            case RenderCompatibilityFacet::EnvironmentLightingWaterSky: return "environment, lighting, water and sky";
            case RenderCompatibilityFacet::UiVideoAndComposition: return "UI, video and composition";
            case RenderCompatibilityFacet::PostProcessingApi: return "post-processing API";
            case RenderCompatibilityFacet::ShaderModSurface: return "shader mod surface";
            case RenderCompatibilityFacet::StereoMultiview: return "stereo and multiview";
            case RenderCompatibilityFacet::CaptureMapAndPreviewViews: return "capture, map and preview views";
            case RenderCompatibilityFacet::GameplaySaveLuaIntegration: return "gameplay, save and Lua integration";
            case RenderCompatibilityFacet::ConfigurationAndContentDiscovery:
                return "configuration and content discovery";
        }
        return "unknown";
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
        | compatibilityFacet(RenderCompatibilityFacet::GameplaySaveLuaIntegration)
        | compatibilityFacet(RenderCompatibilityFacet::ConfigurationAndContentDiscovery);

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
        enum class Reason : std::uint8_t
        {
            None,
            RequestedBackend,
            AutomaticCompatibilityControl,
            AutomaticQualifiedVulkan,
            RequestedBackendUnavailableFallback,
        } reason = Reason::None;
        // Always reports the unqualified automatic-parity surface, including for
        // explicit Vulkan checkpoint requests. Startup diagnostics can therefore
        // distinguish "available for testing" from "qualified as the default".
        std::uint64_t missingVsgCompatibilityFacets = RequiredAutomaticVsgCompatibility;
    };

    [[nodiscard]] constexpr std::optional<RenderBackendPreference> parseRenderBackendPreference(
        std::string_view value) noexcept
    {
        if (value == "auto")
            return RenderBackendPreference::Auto;
        if (value == "opengl")
            return RenderBackendPreference::LegacyOpenGL;
        if (value == "vulkan")
            return RenderBackendPreference::VsgVulkan;
        return std::nullopt;
    }

    [[nodiscard]] constexpr std::string_view renderBackendKindName(RenderBackendKind kind) noexcept
    {
        switch (kind)
        {
            case RenderBackendKind::LegacyOpenGL: return "OpenGL";
            case RenderBackendKind::VsgVulkan: return "VSG/Vulkan";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr std::string_view renderBackendPreferenceName(RenderBackendPreference preference) noexcept
    {
        switch (preference)
        {
            case RenderBackendPreference::Auto: return "auto";
            case RenderBackendPreference::LegacyOpenGL: return "opengl";
            case RenderBackendPreference::VsgVulkan: return "vulkan";
        }
        return "unknown";
    }

    [[nodiscard]] constexpr RenderBackendSelection selectRenderBackend(
        RenderBackendRequest request, RenderBackendCapabilities capabilities) noexcept
    {
        const std::uint64_t missingVsgCompatibilityFacets
            = RequiredAutomaticVsgCompatibility & ~capabilities.vsgVulkanCompatibilityFacets;
        if (request.preference == RenderBackendPreference::Auto)
        {
            // Compatibility-first default: prefer the modern backend only after it
            // has passed the applicable OpenMW mod/shader/content parity gate.
            if (capabilities.vsgVulkan
                && (capabilities.vsgVulkanCompatibilityFacets & RequiredAutomaticVsgCompatibility)
                    == RequiredAutomaticVsgCompatibility)
                return { RenderBackendKind::VsgVulkan, true, false,
                    RenderBackendSelection::Reason::AutomaticQualifiedVulkan, missingVsgCompatibilityFacets };
            if (capabilities.legacyOpenGL)
                return { RenderBackendKind::LegacyOpenGL, true, false,
                    RenderBackendSelection::Reason::AutomaticCompatibilityControl, missingVsgCompatibilityFacets };
            // Auto is compatibility gated, not a synonym for "whatever exists".
            // An incomplete Vulkan-only build must be requested explicitly.
            return { RenderBackendKind::LegacyOpenGL, false, false, RenderBackendSelection::Reason::None,
                missingVsgCompatibilityFacets };
        }

        if (request.preference == RenderBackendPreference::VsgVulkan)
        {
            if (capabilities.vsgVulkan)
                return { RenderBackendKind::VsgVulkan, true, false,
                    RenderBackendSelection::Reason::RequestedBackend, missingVsgCompatibilityFacets };
            if (request.allowFallback && capabilities.legacyOpenGL)
                return { RenderBackendKind::LegacyOpenGL, true, true,
                    RenderBackendSelection::Reason::RequestedBackendUnavailableFallback,
                    missingVsgCompatibilityFacets };
            return { RenderBackendKind::LegacyOpenGL, false, false, RenderBackendSelection::Reason::None,
                missingVsgCompatibilityFacets };
        }

        if (capabilities.legacyOpenGL)
            return { RenderBackendKind::LegacyOpenGL, true, false,
                RenderBackendSelection::Reason::RequestedBackend, missingVsgCompatibilityFacets };
        // Falling back from an explicitly requested compatibility backend to
        // Vulkan is only safe after the same full-parity gate used by Auto.
        if (request.allowFallback && capabilities.vsgVulkan && missingVsgCompatibilityFacets == 0)
            return { RenderBackendKind::VsgVulkan, true, true,
                RenderBackendSelection::Reason::RequestedBackendUnavailableFallback, missingVsgCompatibilityFacets };
        return { RenderBackendKind::LegacyOpenGL, false, false, RenderBackendSelection::Reason::None,
            missingVsgCompatibilityFacets };
    }
}

#endif
