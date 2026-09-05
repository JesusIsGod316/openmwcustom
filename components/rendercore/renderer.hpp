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

    struct RenderBackendCapabilities
    {
        bool legacyOpenGL = true;
        bool vsgVulkan = false;
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
            if (capabilities.vsgVulkan)
                return { RenderBackendKind::VsgVulkan, true, false };
            if (capabilities.legacyOpenGL)
                return { RenderBackendKind::LegacyOpenGL, true, false };
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
