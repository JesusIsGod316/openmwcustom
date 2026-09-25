#ifndef OPENMW_COMPONENTS_SCENEUTIL_SHADOWPROXYGROUP_H
#define OPENMW_COMPONENTS_SCENEUTIL_SHADOWPROXYGROUP_H

#include <osg/Group>

namespace SceneUtil
{
    class ShadowTraversalScope final
    {
    public:
        ShadowTraversalScope() noexcept { ++sDepth; }
        ~ShadowTraversalScope() { --sDepth; }
        ShadowTraversalScope(const ShadowTraversalScope&) = delete;
        ShadowTraversalScope& operator=(const ShadowTraversalScope&) = delete;
        static bool active() noexcept { return sDepth != 0; }

    private:
        inline static thread_local unsigned sDepth = 0;
    };

    class ShadowProxyGroup final : public osg::Group
    {
    public:
        ShadowProxyGroup() = default;
        ShadowProxyGroup(osg::Node* normal, osg::Node* shadow);
        ShadowProxyGroup(const ShadowProxyGroup& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);

        META_Node(SceneUtil, ShadowProxyGroup)

        void traverse(osg::NodeVisitor& nv) override;

        osg::Node* normalNode() { return getNumChildren() > 0 ? getChild(0) : nullptr; }
        osg::Node* shadowNode() { return getNumChildren() > 1 ? getChild(1) : nullptr; }
    };
}

#endif
