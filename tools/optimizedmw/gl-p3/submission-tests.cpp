#include <cstdlib>
#include <iostream>
#include <string>

#include <osg/Geometry>
#include <osg/Group>

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

    osg::ref_ptr<osg::Geometry> optimize(bool compact)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        osg::ref_ptr<osg::Geometry> geometry = makeMixedTriangles();
        root->addChild(geometry);

        SceneUtil::Optimizer optimizer;
        optimizer.setMergeCompatibleIndexTypes(compact);
        optimizer.optimize(root, SceneUtil::Optimizer::MERGE_GEOMETRY);

        if (compact)
            require(optimizer.getCompatibleIndexMergeCount() == 2, "expected two mixed-width merges");
        else
            require(optimizer.getCompatibleIndexMergeCount() == 0, "control must not report P3 merges");
        return geometry;
    }
}

int main()
{
    osg::ref_ptr<osg::Geometry> control = optimize(false);
    require(control->getNumPrimitiveSets() == 3, "P3-off must preserve three mixed-width primitive sets");

    osg::ref_ptr<osg::Geometry> candidate = optimize(true);
    require(candidate->getNumPrimitiveSets() == 1, "P3-on must compact adjacent compatible indexed draws");
    osg::PrimitiveSet* primitive = candidate->getPrimitiveSet(0);
    require(primitive->getType() == osg::PrimitiveSet::DrawElementsUIntPrimitiveType,
        "widest required index type must be retained");
    require(primitive->getMode() == osg::PrimitiveSet::TRIANGLES, "primitive mode changed");
    require(primitive->getNumIndices() == 9, "index count changed");
    for (unsigned int i = 0; i < 9; ++i)
        require(primitive->index(i) == i, "index order changed");

    std::cout << "OptimizedMW GL-P3 submission compaction tests passed\n";
    return 0;
}
