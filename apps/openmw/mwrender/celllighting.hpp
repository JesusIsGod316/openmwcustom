#ifndef OPENMW_MWRENDER_CELLLIGHTING_H
#define OPENMW_MWRENDER_CELLLIGHTING_H

#include <osg/Vec4f>

namespace MWWorld
{
    class Cell;
}

namespace MWRender
{
    struct CellLightingState
    {
        osg::Vec4f ambient;
        osg::Vec4f directional;
        osg::Vec4f directionalPosition;
    };

    // Shared source-side lighting semantics for both render backends. This is
    // deliberately derived from cell/settings state rather than an OSG graph.
    [[nodiscard]] CellLightingState resolveCellLighting(const MWWorld::Cell& cell);

    [[nodiscard]] osg::Vec4f applyNightEyeToAmbient(osg::Vec4f ambient, float nightEyeFactor) noexcept;
}

#endif
