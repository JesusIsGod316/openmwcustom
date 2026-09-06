#ifndef OPENMW_COMPONENTS_RENDERCORE_LODSELECTION_H
#define OPENMW_COMPONENTS_RENDERCORE_LODSELECTION_H

#include "records.hpp"

#include <optional>

namespace RenderCore
{
    // OpenMW/NIF semantics select the LAST direct child whose authored range
    // contains the current eye distance. This is intentionally different from
    // osg::LOD, which can select every matching child and therefore required the
    // legacy RemoveLodOverlapVisitor workaround. The neutral contract records
    // authored ranges unchanged and defines selection directly, so CP3B Vulkan
    // realization does not inherit an OSG implementation artifact.
    [[nodiscard]] inline std::optional<ModelNodeIndex> selectModelLodChild(
        const ModelLodSemantic& lod, float eyeDistance) noexcept
    {
        if (!semantic_detail::finite(eyeDistance))
            return std::nullopt;

        std::optional<ModelNodeIndex> selected;
        for (const ModelLodRange& range : lod.ranges)
        {
            if (eyeDistance >= range.minimumDistance && eyeDistance <= range.maximumDistance)
                selected = range.child;
        }
        return selected;
    }
}

#endif
