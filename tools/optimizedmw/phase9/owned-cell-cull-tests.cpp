#include <components/sceneutil/ownedcellcull.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <osg/Geometry>
#include <osg/Geode>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace OC = SceneUtil::OwnedCellCull;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct ObservedGroup final : osg::Group
{
    bool clean() const { return _boundingSphereComputed; }
};
osg::ref_ptr<osg::Node> child(float radius)
{
    osg::ref_ptr<osg::Group> result = new osg::Group;
    result->setInitialBound(osg::BoundingSphere(osg::Vec3(0,0,0), radius));
    return result;
}
std::vector<unsigned> indices(osg::Group& root, bool large)
{
    OC::Read read(root); std::vector<unsigned> result;
    for (unsigned cursor=0, position=0; read.next(cursor,position,root.getNumChildren(),large);)
    {
        const auto i = cursor++;
        const auto& bound = read.bound(i,*root.getChild(i));
        if ((bound.valid() && bound.radius()>=400) == large) result.push_back(i);
    }
    return result;
}
int main() try
{
    OC::enabled = true;
    osg::ref_ptr<OC::Group> root = new OC::Group;
    root->addChild(child(20)); root->addChild(child(500)); root->addChild(child(350));
    root->addChild(child(700)); root->addChild(child(-1)); root->getBound();
    require(indices(*root,true)==std::vector<unsigned>({1,3}),"large order changed");
    require(indices(*root,false)==std::vector<unsigned>({0,2,4}),"small/invalid order changed");
    require(OC::Counters::boundReadsAvoided>0 && OC::Counters::classificationVisitsAvoided>0,"fixture did not reuse actual bounds/classification");
    auto snapshot = root->snapshot(); require(bool(snapshot),"missing owned snapshot");
    root->getChild(0)->setInitialBound(osg::BoundingSphere({},600));
    require(!root->current(snapshot),"child dirtying retained old root authorization");
    require(indices(*root,true)==std::vector<unsigned>({0,1,3}),"dirty root did not use current live bounds");
    root->getBound(); require(!root->current(snapshot),"old publication survived recomputation");
    require(snapshot->children[0].bound.radius()==20,"new publication mutated held immutable data");
    snapshot.reset();
    const auto retained=OC::Counters::liveBytes.load();
    root->dirtyBound(); root->getBound(); require(OC::Counters::liveBytes==retained,"sole-owner republish duplicated metadata");
    {
        OC::Read read(*root); unsigned cursor=0,position=0;
        require(read.next(cursor,position,5,true) && cursor==0,"mutation cursor setup");
        ++cursor;
        root->getChild(2)->setInitialBound(osg::BoundingSphere({},650));
        std::vector<unsigned> rest;
        while(read.next(cursor,position,5,true))
        {
            const auto i=cursor++; const auto& bound=read.bound(i,*root->getChild(i));
            if(bound.valid() && bound.radius()>=400) rest.push_back(i);
        }
        require(rest==std::vector<unsigned>({1,2,3}),"callback mutation skipped newly large sibling or changed order");
    }
    root->getBound(); snapshot=root->snapshot();
    root->setChild(1,child(10)); require(!root->current(snapshot),"same-count child replacement retained snapshot");
    root->getBound(); require(indices(*root,false)==std::vector<unsigned>({1,4}),"replacement not published");
    snapshot.reset(); root->removeChildren(0,1); root->getBound();
    require(indices(*root,true)==std::vector<unsigned>({1,2}),"topology index invalidation failed");
    struct Callback final : osg::Node::ComputeBoundingSphereCallback {};
    root->setComputeBoundingSphereCallback(new Callback); root->dirtyBound(); root->getBound();
    require(!root->snapshot(),"callback bypass was authorized");
    root->setComputeBoundingSphereCallback(nullptr); root->dirtyBound(); root->getBound();
    osg::ref_ptr<osg::Node> copy=dynamic_cast<osg::Node*>(root->clone(osg::CopyOp::SHALLOW_COPY));
    auto* copied=dynamic_cast<OC::Group*>(copy.get()); require(copied && !copied->snapshot(),"clone inherited another owner's snapshot");
    copied->getBound(); require(copied->snapshot() && copied->snapshot()!=root->snapshot(),"clone failed independent publication");
    copy=nullptr; root=nullptr;
    require(OC::Counters::liveBytes==0,"final owner did not release metadata charge");

    osg::ref_ptr<OC::Group> large=new OC::Group;
    for(unsigned i=0;i<=OC::MaximumChildren;++i) large->addChild(child(1));
    large->getBound(); require(!large->snapshot(),"unbounded child population accepted"); large=nullptr;
    OC::Counters::liveBytes=OC::MaximumBytes;
    root=new OC::Group; root->addChild(child(10)); root->getBound();
    require(!root->snapshot(),"metadata cap did not preserve live fallback"); root=nullptr; OC::Counters::liveBytes=0;

    osg::ref_ptr<SceneUtil::PositionAttitudeTransform> transform = new SceneUtil::PositionAttitudeTransform;
    osg::ref_ptr<ObservedGroup> parent = new ObservedGroup; transform->addChild(child(1)); parent->addChild(transform);
    parent->getBound(); SceneUtil::UnchangedTransform::enabled=false;
    transform->setPosition(transform->getPosition()); require(!parent->clean(),"disabled control changed dirtying");
    parent->getBound(); SceneUtil::UnchangedTransform::enabled=true;
    transform->setPosition(transform->getPosition()); transform->setScale(transform->getScale()); transform->setAttitude(transform->getAttitude());
    require(parent->clean(),"identical finite transform dirtied bounds");
    transform->setPosition({1,0,0}); require(!parent->clean(),"real transform mutation lost dirtying");
    parent->getBound(); transform->setPosition({1,-0.f,0});
    require(!parent->clean() && std::signbit(transform->getPosition().y()),"signed zero assignment changed");
    parent->getBound(); transform->getChild(0)->setInitialBound(osg::BoundingSphere({},2));
    require(!parent->clean(),"independent child dirtiness was lost");
    parent->getBound(); transform->setPosition({std::numeric_limits<float>::infinity(),0,0});
    require(!parent->clean(),"nonfinite fallback lost dirtying");
    parent->getBound(); transform->setPosition({std::numeric_limits<float>::quiet_NaN(),0,0});
    require(!parent->clean() && std::isnan(transform->getPosition().x()),"NaN assignment/fallback lost dirtying");
    std::cout << "OWNED_CELL_CULL_PASS order=1 mutation_fallback=1 immutable_generation=1 topology=1 callback=1 clone=1 bounded_release=1 real_bounds_reuse=1 transform_exact=1 signed_zero=1 child_dirtying=1\n";
    return 0;
}
catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
