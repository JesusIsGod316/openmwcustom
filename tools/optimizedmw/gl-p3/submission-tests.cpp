#include <cstdlib>
#include <iostream>
#include <string>

#include <osg/Geometry>
#include <osg/Group>
#include <osg/StateSet>
#include <osgDB/SharedStateManager>

#include <components/sceneutil/optimizer.hpp>

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

    osg::ref_ptr<osg::Geometry> makeMixedTriangles()
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        for (int i = 0; i < 9; ++i)
            vertices->push_back(osg::Vec3f(static_cast<float>(i), 0.f, 0.f));
        geometry->setVertexArray(vertices);

        osg::ref_ptr<osg::DrawElementsUByte> a = new osg::DrawElementsUByte(osg::PrimitiveSet::TRIANGLES);
        a->push_back(0); a->push_back(1); a->push_back(2);
        osg::ref_ptr<osg::DrawElementsUShort> b = new osg::DrawElementsUShort(osg::PrimitiveSet::TRIANGLES);
        b->push_back(3); b->push_back(4); b->push_back(5);
        osg::ref_ptr<osg::DrawElementsUInt> c = new osg::DrawElementsUInt(osg::PrimitiveSet::TRIANGLES);
        c->push_back(6); c->push_back(7); c->push_back(8);
        geometry->addPrimitiveSet(a);
        geometry->addPrimitiveSet(b);
        geometry->addPrimitiveSet(c);
        return geometry;
    }

    osg::ref_ptr<osg::Geometry> optimize(bool compact, bool displayList)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Geometry> geometry = makeMixedTriangles();
        root->addChild(geometry);

        SceneUtil::Optimizer optimizer;
        optimizer.setMergeCompatibleIndexTypes(compact);
        optimizer.setPreferDisplayListsForMergedGeometry(displayList);
        optimizer.optimize(root, SceneUtil::Optimizer::MERGE_GEOMETRY);

        if (compact)
            require(optimizer.getCompatibleIndexMergeCount() == 2, "expected two mixed-width merges");
        else
            require(optimizer.getCompatibleIndexMergeCount() == 0, "control must not report P3 merges");
        if (displayList)
            require(optimizer.getDisplayListPromotionCount() == 1, "expected one display-list promotion");
        else
            require(optimizer.getDisplayListPromotionCount() == 0, "control must not report display-list promotion");
        return geometry;
    }

    osg::ref_ptr<osg::Geometry> makeTriangle(float offset)
    {
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back(osg::Vec3f(offset, 0.f, 0.f));
        vertices->push_back(osg::Vec3f(offset + 1.f, 0.f, 0.f));
        vertices->push_back(osg::Vec3f(offset, 1.f, 0.f));
        geometry->setVertexArray(vertices);
        osg::ref_ptr<osg::DrawElementsUShort> elements
            = new osg::DrawElementsUShort(osg::PrimitiveSet::TRIANGLES);
        elements->push_back(0); elements->push_back(1); elements->push_back(2);
        geometry->addPrimitiveSet(elements);
        geometry->setStateSet(new osg::StateSet);
        return geometry;
    }

    osg::ref_ptr<osg::Group> makeEquivalentStatePair()
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->addChild(makeTriangle(0.f));
        root->addChild(makeTriangle(2.f));
        return root;
    }
}

int main()
{
    osg::ref_ptr<osg::Geometry> control = optimize(false, false);
    require(control->getNumPrimitiveSets() == 3, "P3-off must preserve three mixed-width primitive sets");

    osg::ref_ptr<osg::Geometry> candidate = optimize(true, false);
    require(candidate->getNumPrimitiveSets() == 1, "P3-on must compact adjacent compatible indexed draws");
    require(candidate->getUseVertexBufferObjects(), "historical merged path must retain VBOs");
    require(!candidate->getUseDisplayList(), "historical merged path must keep display lists off");
    osg::PrimitiveSet* primitive = candidate->getPrimitiveSet(0);
    require(primitive->getType() == osg::PrimitiveSet::DrawElementsUIntPrimitiveType,
        "widest required index type must be retained");
    require(primitive->getMode() == osg::PrimitiveSet::TRIANGLES, "primitive mode changed");
    require(primitive->getNumIndices() == 9, "index count changed");
    for (unsigned int i = 0; i < 9; ++i)
        require(primitive->index(i) == i, "index order changed");

    osg::ref_ptr<osg::Geometry> commandCached = optimize(true, true);
    require(commandCached->getUseDisplayList(), "distant command-cache experiment did not enable display list");
    require(!commandCached->getUseVertexBufferObjects(), "display-list experiment must not retain VBO submission");

    osg::ref_ptr<osg::Group> separate = makeEquivalentStatePair();
    SceneUtil::Optimizer separateOptimizer;
    separateOptimizer.optimize(separate, SceneUtil::Optimizer::MERGE_GEOMETRY);
    require(separate->getNumChildren() == 2,
        "distinct StateSet identities unexpectedly merged before canonicalization");

    osg::ref_ptr<osg::Group> canonical = makeEquivalentStatePair();
    osgDB::SharedStateManager shared;
    shared.share(canonical);
    SceneUtil::Optimizer canonicalOptimizer;
    canonicalOptimizer.optimize(canonical, SceneUtil::Optimizer::MERGE_GEOMETRY);
    require(canonical->getNumChildren() == 1,
        "semantic state canonicalization did not unlock geometry merge");

    std::cout << "OptimizedMW GL-P3 submission compaction tests passed\n";
    return 0;
}
