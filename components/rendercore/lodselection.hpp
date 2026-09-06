#ifndef OPENMW_COMPONENTS_RENDERCORE_LODSELECTION_H
#define OPENMW_COMPONENTS_RENDERCORE_LODSELECTION_H

#include "records.hpp"

#include <optional>

namespace RenderCore
{
    // Legacy source semantics select the LAST direct child whose authored range
    // contains the current eye distance. Some scene-graph implementations can
    // select every matching child and therefore require overlap-rewriting
    // workarounds. The neutral contract records authored ranges unchanged and
    // defines selection directly so backend realization inherits source behavior,
    // not an implementation artifact.
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
