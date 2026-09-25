#include <cstdlib>
#include <iostream>
#include <string>

#include <osg/Group>
#include <osg/NodeVisitor>

#include <components/sceneutil/shadowproxygroup.hpp>

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    class NameVisitor final : public osg::NodeVisitor
    {
    public:
        NameVisitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}

        void apply(osg::Node& node) override
        {
            if (node.getName() == "normal-leaf") ++normal;
            if (node.getName() == "shadow-leaf") ++shadow;
            traverse(node);
        }

        int normal = 0;
        int shadow = 0;
    };
}

int main()
{
    osg::ref_ptr<osg::Group> normal = new osg::Group;
    osg::ref_ptr<osg::Node> normalLeaf = new osg::Node;
    normalLeaf->setName("normal-leaf");
    normal->addChild(normalLeaf);

    osg::ref_ptr<osg::Group> shadow = new osg::Group;
    osg::ref_ptr<osg::Node> shadowLeaf = new osg::Node;
    shadowLeaf->setName("shadow-leaf");
    shadow->addChild(shadowLeaf);

    osg::ref_ptr<SceneUtil::ShadowProxyGroup> proxy
        = new SceneUtil::ShadowProxyGroup(normal, shadow);

    NameVisitor ordinary;
    proxy->accept(ordinary);
    require(ordinary.normal == 1, "normal traversal missed original subtree");
    require(ordinary.shadow == 0, "normal traversal leaked shadow proxy subtree");

    NameVisitor shadowVisitor;
    {
        SceneUtil::ShadowTraversalScope scope;
        proxy->accept(shadowVisitor);
    }
    require(shadowVisitor.normal == 0, "shadow traversal leaked original subtree");
    require(shadowVisitor.shadow == 1, "shadow traversal missed proxy subtree");

    std::cout << "OptimizedMW GL-P3 shadow proxy traversal tests passed\n";
    return 0;
}
