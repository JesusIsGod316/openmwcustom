#ifndef OPENMW_COMPONENTS_SCENEUTIL_UPDATEONLYVISITOR_H
#define OPENMW_COMPONENTS_SCENEUTIL_UPDATEONLYVISITOR_H

#include <osgUtil/UpdateVisitor>

namespace SceneUtil
{
    // CPU scene authority for a renderer which does not perform OSG cull traversals.
    // Actor range/Inactive policy still applies; OSG visibility cannot gate animation.
    class UpdateOnlyVisitor final : public osgUtil::UpdateVisitor
    {
    };
}

#endif
