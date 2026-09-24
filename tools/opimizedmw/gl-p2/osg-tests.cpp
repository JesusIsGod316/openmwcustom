#include <components/sceneutil/pagingwork.hpp>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/TriangleIndexFunctor>
#include <osgUtil/MeshOptimizers>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
    unsigned checks = 0;
    void check(bool ok, const char* name) { if (!ok) throw std::runtime_error(name); ++checks; }
    struct Collect
    {
        std::vector<unsigned>* out = nullptr;
        void operator()(unsigned a,unsigned b,unsigned c){out->insert(out->end(),{a,b,c});}
    };
    std::vector<unsigned> indices(osg::Geometry& geom)
    {
        std::vector<unsigned> result; osg::TriangleIndexFunctor<Collect> f; f.out=&result;
        geom.accept(f);return result;
    }
    osg::ref_ptr<osg::Geometry> mesh(const std::vector<unsigned>& data,unsigned mode,unsigned count)
    {
        osg::ref_ptr<osg::Geometry> geom=new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array(count);
        for(unsigned i=0;i<count;++i) (*vertices)[i]=osg::Vec3(float(i%13),float(i/13),float(i%7));
        geom->setVertexArray(vertices);
        geom->addPrimitiveSet(new osg::DrawElementsUInt(mode,data.begin(),data.end()));
        geom->setUseVertexBufferObjects(true);geom->setUseDisplayList(false);
        geom->setDataVariance(osg::Object::STATIC);
        return geom;
    }
    void compare(const std::vector<unsigned>& data,unsigned mode,unsigned count)
    {
        auto original=mesh(data,mode,count), candidate=mesh(data,mode,count);
        auto* array=candidate->getVertexArray();
        osgUtil::VertexCacheVisitor inherited;
        original->accept(inherited);inherited.optimizeVertices();
        std::atomic<bool> abort{false};SceneUtil::PagingWorkScope scope(&abort);
        SceneUtil::optimizePagingVertices(*candidate);
        check(indices(*original)==indices(*candidate),"exact OSG triangle-order equivalence");
        check(candidate->getVertexArray()==array,"vertex backing unchanged");
        check(original->getPrimitiveSet(0)->getType()==candidate->getPrimitiveSet(0)->getType(),"index width unchanged");
    }
}
int main()
{
    try
    {
        std::mt19937 rng(1923);
        for(unsigned seed=0;seed<80;++seed)
        {
            const unsigned count=17+seed*3;std::vector<unsigned> data;
            for(unsigned i=0;i<90+seed*12;++i) data.push_back(rng()%count);
            data.resize(data.size()/3*3);compare(data,GL_TRIANGLES,count);
        }
        std::vector<unsigned> islands;
        for(unsigned i=0;i<1500;++i)islands.insert(islands.end(),{3*i,3*i+1,3*i+2});
        compare(islands,GL_TRIANGLES,4500);
        std::vector<unsigned> fan;
        for(unsigned i=1;i<1500;++i)fan.insert(fan.end(),{0,i,i+1});
        compare(fan,GL_TRIANGLES,1501);
        compare({0,1,2,3,4,5,6,7,8,9},GL_TRIANGLE_STRIP,30);
        compare({0,1,2,3,4,5,6,7,8,9},GL_TRIANGLE_FAN,30);
        compare({0,1,2,3,4,5,6,7},GL_QUADS,30);
        compare({0,1,2,3,4,5,6,7},GL_QUAD_STRIP,30);
        compare({0,1,2,3,4,5,6,7},GL_POLYGON,30);
        compare({1,1,1,2,2,3},GL_TRIANGLES,30);
        compare({65535,65536,65537,65536,65538,65537},GL_TRIANGLES,65539);
        compare({0,1,2},GL_TRIANGLES,3);
        auto cancelled=mesh(islands,GL_TRIANGLES,4500);
        const auto old=indices(*cancelled);auto* primitive=cancelled->getPrimitiveSet(0);
        std::atomic<bool> abort{true};SceneUtil::PagingWorkScope scope(&abort);bool threw=false;
        try{SceneUtil::optimizePagingVertices(*cancelled);}catch(const SceneUtil::PagingWorkCancelled&){threw=true;}
        check(threw && indices(*cancelled)==old && cancelled->getPrimitiveSet(0)==primitive,"cancel cannot publish partial geometry");
        abort=false;
        auto lines=mesh({0,1,2,3},GL_LINES,20);auto* linePrimitive=lines->getPrimitiveSet(0);
        SceneUtil::optimizePagingVertices(*lines);
        check(lines->getPrimitiveSet(0)==linePrimitive,"unsupported geometry untouched");
        std::cout<<checks<<" real-OSG equivalence and publication checks passed\n";
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
