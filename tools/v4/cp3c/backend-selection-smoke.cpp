#include <components/rendercore/renderer.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
        {
            std::cerr << "CP3C backend selection smoke failure: " << message << '\n';
            std::exit(1);
        }
    }
}

int main()
{
    using namespace RenderCore;

    require(parseRenderBackendPreference("auto") == RenderBackendPreference::Auto, "auto parse");
    require(parseRenderBackendPreference("opengl") == RenderBackendPreference::LegacyOpenGL, "OpenGL parse");
    require(parseRenderBackendPreference("vulkan") == RenderBackendPreference::VsgVulkan, "Vulkan parse");
    require(!parseRenderBackendPreference("Vulkan"), "configuration values must remain canonical");
    require((RequiredAutomaticVsgCompatibility
                & compatibilityFacet(RenderCompatibilityFacet::ConfigurationAndContentDiscovery))
            != 0,
        "configuration/content discovery must be part of automatic parity");
    require(renderCompatibilityFacetName(RenderCompatibilityFacet::ShaderModSurface) == "shader mod surface",
        "shader compatibility diagnostic name");
    require(renderCompatibilityFacetName(RenderCompatibilityFacet::ConfigurationAndContentDiscovery)
            == "configuration and content discovery",
        "configuration/content diagnostic name");

    RenderBackendCapabilities productionNow{
        .legacyOpenGL = true,
        .vsgVulkan = false,
        .vsgVulkanCompatibilityFacets = 0,
    };
    const RenderBackendSelection automatic = selectRenderBackend({}, productionNow);
    require(automatic.valid && automatic.backend == RenderBackendKind::LegacyOpenGL,
        "Auto must retain the compatibility renderer");
    require(automatic.reason == RenderBackendSelection::Reason::AutomaticCompatibilityControl,
        "Auto control reason");

    const RenderBackendSelection fallback
        = selectRenderBackend({ RenderBackendPreference::VsgVulkan, true }, productionNow);
    require(fallback.valid && fallback.fellBack && fallback.backend == RenderBackendKind::LegacyOpenGL,
        "explicit Vulkan fallback");
    require(!selectRenderBackend({ RenderBackendPreference::VsgVulkan, false }, productionNow).valid,
        "strict explicit Vulkan must fail when unavailable");

    RenderBackendCapabilities incompleteVulkanOnly{
        .legacyOpenGL = false,
        .vsgVulkan = true,
        .vsgVulkanCompatibilityFacets
        = RequiredAutomaticVsgCompatibility
            & ~compatibilityFacet(RenderCompatibilityFacet::ConfigurationAndContentDiscovery),
    };
    require(!selectRenderBackend({}, incompleteVulkanOnly).valid,
        "Auto must fail closed for an incomplete Vulkan-only build");
    require(selectRenderBackend({ RenderBackendPreference::VsgVulkan, false }, incompleteVulkanOnly).valid,
        "explicit Vulkan remains available for checkpoint testing");
    require(!selectRenderBackend({ RenderBackendPreference::LegacyOpenGL, true }, incompleteVulkanOnly).valid,
        "OpenGL fallback must not silently select incomplete Vulkan");

    incompleteVulkanOnly.vsgVulkanCompatibilityFacets = RequiredAutomaticVsgCompatibility;
    const RenderBackendSelection qualified = selectRenderBackend({}, incompleteVulkanOnly);
    require(qualified.valid && qualified.backend == RenderBackendKind::VsgVulkan,
        "Auto may select Vulkan only after full qualification");
    require(qualified.missingVsgCompatibilityFacets == 0, "qualified Vulkan missing-facet mask");

    std::cout << "CP3C backend selection smoke: PASS\n";
}
