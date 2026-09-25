#ifndef OPENMW_MWRENDER_VULKANMW_STATICWORLDSOURCE_H
#define OPENMW_MWRENDER_VULKANMW_STATICWORLDSOURCE_H

#include <components/render/native/staticworldservice.hpp>

#include <optional>
#include <string>

namespace MWWorld
{
    class CellStore;
    class Ptr;
}

namespace MWRender::VulkanMW
{
    [[nodiscard]] std::optional<std::string> makeCellIdentity(const MWWorld::CellStore& cell);
    [[nodiscard]] std::optional<std::string> makeReferenceIdentity(const MWWorld::Ptr& ptr);

    [[nodiscard]] std::optional<RenderNative::StaticWorldCellSource> makeStaticWorldCellSource(
        const MWWorld::CellStore& cell);

    [[nodiscard]] std::optional<RenderCore::StaticInstanceSource> makeStaticInstanceSource(
        const MWWorld::Ptr& ptr, RenderCore::ModelHandle model, RenderCore::AxisAlignedBounds localBounds);
}

#endif
