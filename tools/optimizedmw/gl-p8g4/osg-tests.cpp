#include <components/sceneutil/groundcoverbatch.hpp>
#include <components/sceneutil/opaqueshadowbatch.hpp>
#include <components/resource/benchmarkcapture.hpp>
#include <osg/Geode>
#include <components/sceneutil/material.hpp>
#include <osg/Depth>
#include <osg/Viewport>
#include <osgUtil/SceneView>
#include <set>
#include <iostream>
#include <stdexcept>
namespace G=SceneUtil::GroundcoverBatch;
namespace P=SceneUtil::GroundcoverPolicy;
namespace L=SceneUtil::GroundcoverLod2;
namespace S=SceneUtil::OpaqueShadowBatch;
static unsigned checks=0;
static void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
struct Visitor:osg::NodeVisitor {
    Visitor():osg::NodeVisitor(TRAVERSE_ALL_CHILDREN){}
    std::vector<osg::Geometry*> geometry;
    std::set<osg::StateSet*> policyStates;
    void apply(osg::Node& node)override{
        auto* s=node.getStateSet();if(s&&s->getUniform("p8g3LodParams"))policyStates.insert(s);traverse(node);
    }
    void apply(osg::Geometry& g)override{geometry.push_back(&g);apply(static_cast<osg::Node&>(g));}
};
static osg::ref_ptr<osg::Geometry> mesh(float x=10.f){
    osg::ref_ptr<osg::Geometry> g=new osg::Geometry;
    auto* v=new osg::Vec3Array;v->push_back({x,10.f,0.f});v->push_back({x+2.f,10.f,4.f});v->push_back({x,12.f,4.f});g->setVertexArray(v);
    auto* indices=new osg::DrawElementsUInt(GL_TRIANGLES);indices->push_back(0);indices->push_back(1);indices->push_back(2);g->addPrimitiveSet(indices);return g;
}
static osg::ref_ptr<osgUtil::SceneView> scene(osg::Node* node){
    auto value=osg::ref_ptr<osgUtil::SceneView>(new osgUtil::SceneView);value->setDefaults();
    value->setViewport(new osg::Viewport(0,0,512,512));value->setProjectionMatrixAsPerspective(60.,1.,1.,30000.);
    value->setViewMatrixAsLookAt({0.,-1000.,80.},{0.,0.,80.},{0.,0.,1.});value->setSceneData(node);return value;
}
int main()try{
#ifdef _WIN32
    _putenv_s("OPENMW_P8G4_STATS","1");
#else
    setenv("OPENMW_P8G4_STATS","1",1);
#endif
    auto source=mesh();osg::ref_ptr<osg::Geode> root=new osg::Geode;root->addDrawable(source);
    std::vector<P::Instance> values(256);
    for(unsigned i=0;i<values.size();++i){values[i].identity=i;values[i].rank=P::rank(i);values[i].scale=1;values[i].position={float(i%16)*10.f,0,float(i/16)*10.f};}
    P::sortRanks(values);
    P::Options options;options.fastCull=true;options.lod2=true;options.nearDistance=3000;options.farDistance=10000;
    const auto states=std::make_shared<G::Lod2States>(options);auto counters=std::make_shared<G::Counters>();
    osg::ref_ptr<osg::Group> population=new osg::Group;
    for(unsigned i=0;i<100;++i)population->addChild(G::buildTile(*root,values,true,options,20000,{},counters,states));
    Visitor visitor;population->accept(visitor);
    require(visitor.geometry.size()==400,"compile visitors must see exactly four immutable tiers per plant");
    require(visitor.policyStates.size()==1,"same-radius geometries must reuse ONE policy state");
    for(auto* geometry:visitor.geometry){
        require(geometry->getVertexArray()==source->getVertexArray(),"source vertices duplicated");
        require(geometry->getStateSet()==source->getStateSet(),"per-geometry state explosion reintroduced");
        require(geometry->getVertexAttribArray(8)==nullptr && geometry->getVertexAttribArray(9)==nullptr,"UV attribute alias");
    }
    auto tile=G::buildTile(*root,values,true,options,20000,{},counters,states);auto view=scene(tile);
    tile->getOrCreateStateSet()->addUniform(new osg::Uniform("windSpeed",5.f));
    auto cull=[&](float distance){
        auto before=counters->submittedInstances.load();view->setViewMatrixAsLookAt({0.,-distance,80.},{0.,0.,80.},{0.,0.,1.});view->cull();
        require(G::CullInputScope::current==nullptr,"scoped cull cache leaked out of traversal");return counters->submittedInstances.load()-before;
    };
    require(cull(1000)==256,"LOD2 reduced near plants");
    require(cull(15000)==L::tierCount(values,3),"LOD2 failed to remove submitted instances");
    require(cull(1000)==256,"camera cut did not refine immediately");
    auto alternate=scene(tile);cull(15000);auto before=counters->submittedInstances.load();alternate->cull();
    require(counters->submittedInstances.load()-before==256,"main-camera result contaminated alternate view");
    auto stockOptions=options;stockOptions.fastCull=false;
    auto fast=G::buildTile(*root,values,false,options,20000,{},counters);
    auto stock=G::buildTile(*root,values,false,stockOptions,20000,{},counters);
    auto a=scene(fast),b=scene(stock);
    for(unsigned i=0;i<80;++i){
        const float angle=float(i)*.2f;osg::Matrix camera=osg::Matrix::lookAt(osg::Vec3d(std::sin(angle)*10000.,-std::cos(angle)*10000.,80.),osg::Vec3d(0.,0.,80.),osg::Vec3d(0.,0.,1.));
        a->setViewMatrix(camera);b->setViewMatrix(camera);
        before=counters->submittedInstances.load();a->cull();const auto na=counters->submittedInstances.load()-before;
        before=counters->submittedInstances.load();b->cull();require(counters->submittedInstances.load()-before==na,"fast cull changed visibility");
    }
    auto hidden=G::buildTile(*root,values,false,options,20000,[](auto&,const auto&,auto){return false;},counters);
    hidden->getOrCreateStateSet()->addUniform(new osg::Uniform("windSpeed",std::numeric_limits<float>::quiet_NaN()));
    auto invalid=scene(hidden);before=counters->submittedInstances.load();invalid->cull();
    require(counters->submittedInstances.load()-before==256,"invalid wind did not fail open");
    require(G::lod2ScaleUpper(osg::Matrix::identity())<1.001f,"rigid transform still uses sqrt(3) bound");
    for(unsigned i=0;i<200;++i){
        osg::Matrix m=osg::Matrix::rotate(i*.03,osg::Vec3d(1,2,3))*osg::Matrix::scale(1.+i*.01,2.,.5);m(0,1)+=.3;
        float upper=G::lod2ScaleUpper(m);
        for(unsigned j=0;j<40;++j){osg::Vec3d v(std::sin(j*.31),std::cos(j*.19),std::sin(j*.17));v.normalize();
            require((v*m).length()<=upper,"general/sheared transform bound unsafe");}
    }
    osg::ref_ptr<osg::Group> normal=new osg::Group;normal->addChild(mesh());normal->addChild(mesh(20));
    // Use the production material uniform layout, not a state-free synthetic-only case.
    osg::ref_ptr<SceneUtil::Material> material=new SceneUtil::Material;
    for(unsigned i=0;i<normal->getNumChildren();++i){
        auto* state=normal->getChild(i)->getOrCreateStateSet();material->setStateSet(state);
        state->setAttribute(material);
        state->addUniform(new osg::Uniform("useDiffuseMapForShadowAlpha",false));
    }
    auto shadow=S::build(*normal);require(shadow.mShadowRoot.valid()&&shadow.mEligibleDrawables==2,"plain opaque geometry failed batching");
    Visitor proxies;shadow.mShadowRoot->accept(proxies);require(proxies.geometry.size()==1,"two opaque triangles not batched");
    require(proxies.geometry[0]->getPrimitiveSet(0)->getNumIndices()==6,"shadow indices lost");
    require(normal->getNumChildren()==2,"visible source graph mutated");
    osg::ref_ptr<SceneUtil::ShadowProxyGroup> wrapper=new SceneUtil::ShadowProxyGroup(normal,shadow.mShadowRoot);
    Visitor ordinary;wrapper->accept(ordinary);require(ordinary.geometry.size()==2,"normal rendering selected shadow proxy");
    {SceneUtil::ShadowTraversalScope pass(1);Visitor caster;wrapper->accept(caster);require(caster.geometry.size()==1,"shadow pass did not select proxy");}
    require(!SceneUtil::ShadowTraversalScope::active(),"shadow scope leaked");
    auto shadowView=scene(wrapper);shadowView->setCullingMode(osg::CullSettings::VIEW_FRUSTUM_CULLING);
    {SceneUtil::ShadowTraversalScope pass(1);before=SceneUtil::ShadowBatchCounters::proxyVisits[1].load();shadowView->cull();require(SceneUtil::ShadowBatchCounters::proxyVisits[1].load()>before,"near cascade proxy coverage not observed");}
    shadowView->setCullingMode(osg::CullSettings::VIEW_FRUSTUM_CULLING|osg::CullSettings::SMALL_FEATURE_CULLING);
    shadowView->setSmallFeatureCullingPixelSize(5.f);
    {SceneUtil::ShadowTraversalScope pass(2);before=SceneUtil::ShadowBatchCounters::normalFallback[2].load();shadowView->cull();require(SceneUtil::ShadowBatchCounters::normalFallback[2].load()>before,"far-caster small-feature semantics not preserved");}
    for(unsigned type=0;type<7;++type){
        osg::ref_ptr<osg::Group> unsupported=new osg::Group;auto g=mesh();
        if(type==0)g->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
        if(type==1)g->getOrCreateStateSet()->setAttribute(new osg::AlphaFunc(osg::AlphaFunc::GREATER,.5f));
        if(type==2)g->getOrCreateStateSet()->setAttribute(new osg::Depth);
        if(type==3)g->setDataVariance(osg::Object::DYNAMIC);
        if(type==4)static_cast<osg::DrawElementsUInt*>(g->getPrimitiveSet(0))->push_back(999);
        if(type==5)g->getOrCreateStateSet()->addUniform(new osg::Uniform("customDeformation",1.f));
        if(type==6)g->setUserValue("shaderPrefix",std::string("custom"));
        unsupported->addChild(g);unsupported->addChild(mesh());
        require(!S::build(*unsupported).mShadowRoot,"unsupported content lost exact fallback");
    }
    normal->getChild(1)->setNodeMask(2);require(!S::build(*normal).mShadowRoot,"different caster masks merged");
    std::cout<<checks<<" P8G4 native OSG checks passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
