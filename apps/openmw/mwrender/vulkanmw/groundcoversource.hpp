#ifndef OPENMW_MWRENDER_VULKANMW_GROUNDCOVERSOURCE_H
#define OPENMW_MWRENDER_VULKANMW_GROUNDCOVERSOURCE_H

#include <components/render/native/nifassetservice.hpp>
#include <components/rendercore/staticpopulationproducer.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace MWRender
{
    class Groundcover;
}

namespace RenderCore
{
    class RenderWorld;
}

namespace MWRender::VulkanMW
{
    struct GroundcoverPopulationSource
    {
        RenderCore::StaticPopulationCellSource cell;
        std::vector<RenderCore::StaticPopulationInstanceSource> instances;
        std::string diagnostic;

        [[nodiscard]] bool valid() const noexcept { return diagnostic.empty(); }
    };

    [[nodiscard]] GroundcoverPopulationSource makeGroundcoverPopulationSource(const Groundcover& groundcover,
        RenderNative::NifAssetService& assets, const RenderCore::RenderWorld& world, std::string_view worldspaceIdentity,
        std::int32_t gridX, std::int32_t gridY, float renderingDistance);
}

#endif
