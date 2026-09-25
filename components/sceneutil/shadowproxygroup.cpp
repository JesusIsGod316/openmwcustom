#include "shadowproxygroup.hpp"

#include <osg/NodeVisitor>

namespace SceneUtil
{
    ShadowProxyGroup::ShadowProxyGroup(osg::Node* normal, osg::Node* shadow)
    {
        if (normal)
            addChild(normal);
        if (shadow)
            addChild(shadow);
    }

    ShadowProxyGroup::ShadowProxyGroup(const ShadowProxyGroup& copy, const osg::CopyOp& copyop)
        : osg::Group(copy, copyop)
    {
    }

    void ShadowProxyGroup::traverse(osg::NodeVisitor& nv)
    {
        osg::Node* node = ShadowTraversalScope::active() ? shadowNode() : normalNode();
        if (node && nv.validNodeMask(*node))
            node->accept(nv);
    }
}
