#ifndef OPENMW_SCENEUTIL_DEFORMATIONINTERSECTIONVISITOR_H
#define OPENMW_SCENEUTIL_DEFORMATIONINTERSECTIONVISITOR_H

#include <osgUtil/IntersectionVisitor>

namespace SceneUtil
{
    // Vulkan has no OSG cull traversal to populate RigGeometry/MorphGeometry's
    // double buffers. Only marked, synchronous gameplay queries may evaluate
    // them on demand. Ordinary OpenGL intersection/cull threading is unchanged.
    class DeformationIntersectionVisitor : public osgUtil::IntersectionVisitor
    {
    public:
        bool evaluateDeformation = false;
    };
}

#endif
