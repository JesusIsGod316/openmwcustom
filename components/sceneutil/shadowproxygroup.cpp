#include "shadowproxygroup.hpp"

#include <osg/NodeVisitor>
#include <osg/AlphaFunc>
#include <osgUtil/CullVisitor>
#include <osgUtil/StateGraph>
#include "shadowbatchcounters.hpp"

namespace SceneUtil
{
    ShadowProxyGroup::ShadowProxyGroup(osg::Node* normal, osg::Node* shadow)
    {
        if (normal)
        {
            setNodeMask(normal->getNodeMask());
            addChild(normal);
        }
        ShadowBatchCounters::add(ShadowBatchCounters::wrapped);
        if (shadow)
            addChild(shadow);
    }

    ShadowProxyGroup::ShadowProxyGroup(const ShadowProxyGroup& copy, const osg::CopyOp& copyop)
        : osg::Group(copy, copyop)
    {
    }

    void ShadowProxyGroup::traverse(osg::NodeVisitor& nv)
    {
        bool useProxy = ShadowTraversalScope::active();
        if (useProxy)
        {
            if (auto* cv = nv.asCullVisitor())
            {
                // A merged bound must not bypass the existing far-caster 5-pixel
                // policy. Keep the exact original graph whenever that policy is active.
                if ((cv->getCullingMode() & osg::CullSettings::SMALL_FEATURE_CULLING)
                    && cv->getSmallFeatureCullingPixelSize() > 0.f) useProxy = false;
                // Inherited cutouts/blending or protected shader state also require
                // original textures and geometry. Conservative over-fallback is safe.
                for (auto* graph = cv->getCurrentStateGraph(); useProxy && graph; graph = graph->_parent)
                    if (const auto* state = graph->getStateSet())
                    {
                        if ((state->getMode(GL_BLEND) & osg::StateAttribute::ON) != 0) useProxy = false;
                        if (const auto* alpha = dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)))
                            if (alpha->getFunction() != GL_ALWAYS) useProxy = false;
                        for (const auto& entry : state->getAttributeList())
                            if (entry.first.first == osg::StateAttribute::PROGRAM
                                && (entry.second.second & osg::StateAttribute::PROTECTED)) useProxy = false;
                    }
                if (!useProxy)
                    ShadowBatchCounters::add(ShadowBatchCounters::normalFallback[std::min(ShadowTraversalScope::cascade(), 7u)]);
            }
        }
        osg::Node* node = useProxy ? shadowNode() : normalNode();
        if (node && nv.validNodeMask(*node))
            node->accept(nv);
    }
}
