#include <components/sceneutil/drawphasetrace.hpp>
#include <osg/Geode>
#include <osg/Material>
#include <osg/MatrixTransform>
#include <osg/Program>
#include <osg/Shader>
#include <osgUtil/SceneView>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace SceneUtil::DrawPhaseTrace;
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct SlowAttribute : osg::StateAttribute
{
    SlowAttribute()=default;
    SlowAttribute(const SlowAttribute& o,const osg::CopyOp& c):osg::StateAttribute(o,c){}
    META_StateAttribute(Test,SlowAttribute,FOG)
    int compare(const osg::StateAttribute& other) const override {return this==&other ? 0 : (this<&other?-1:1);}
    void apply(osg::State&) const override {std::this_thread::sleep_for(std::chrono::milliseconds(2));}
};
struct SlowDraw : osg::Drawable::DrawCallback
{
    void drawImplementation(osg::RenderInfo& info,const osg::Drawable* d)const override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));d->drawImplementation(info);
    }
};
struct Pixels : osg::Camera::DrawCallback
{
    mutable std::array<unsigned char,32*32*4> data{};
    mutable std::atomic<unsigned> completed{0};
    void operator()(osg::RenderInfo& info) const override
    {
        glReadPixels(0,0,32,32,GL_RGBA,GL_UNSIGNED_BYTE,data.data());
        completed.store(info.getState()->getFrameStamp()->getFrameNumber()+1,std::memory_order_release);
    }
    void wait(unsigned frame) const
    {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(completed.load(std::memory_order_acquire)<frame+1)
        {
            require(std::chrono::steady_clock::now()<deadline,"draw thread failed to finish fixture frame");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};
int main(int argc, char** argv) try
{
    require(Capture::instance().enabled(),"test must explicitly enable bounded trace");
    const bool threaded=argc>1 && std::string(argv[1])=="--threaded";
    osgViewer::Viewer viewer;
    viewer.setThreadingModel(threaded ? osgViewer::Viewer::DrawThreadPerContext : osgViewer::Viewer::SingleThreaded);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY();traits->setUndefinedScreenDetailsToDefaultScreen();traits->width=32;traits->height=32;
    traits->doubleBuffer=false;traits->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(),"no context");
    auto* camera=viewer.getCamera();camera->setGraphicsContext(context);camera->setViewport(0,0,32,32);
    camera->setProjectionMatrix(osg::Matrix::identity());camera->setViewMatrix(osg::Matrix::identity());
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    camera->setCullingMode(osg::CullSettings::NO_CULLING);
    camera->setClearColor(osg::Vec4(0,0,0,1));camera->setClearMask(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    viewer.setLightingMode(osgViewer::View::NO_LIGHT);
    osg::ref_ptr<Pixels> pixels=new Pixels;camera->setFinalDrawCallback(pixels);
    osg::ref_ptr<osg::Group> root=new osg::Group;
    root->getOrCreateStateSet()->setMode(GL_LIGHTING,osg::StateAttribute::OFF);
    for(int i=0;i<4;++i)
    {
        osg::ref_ptr<osg::Geometry> g=new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> v=new osg::Vec3Array;
        const float x=-.9f+i*.45f;
        v->push_back(osg::Vec3(x,-.8f,0));v->push_back(osg::Vec3(x+.4f,-.8f,0));v->push_back(osg::Vec3(x+.2f,.8f,0));
        g->setVertexArray(v);g->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        osg::ref_ptr<osg::Vec4Array> color=new osg::Vec4Array;
        color->push_back(osg::Vec4(i%2?0.f:1.f,i%2?1.f:0.f,0,1));
        g->setColorArray(color,osg::Array::BIND_OVERALL);
        g->setUseDisplayList(false);g->setUseVertexBufferObjects(true);g->setDataVariance(osg::Object::DYNAMIC);
        g->setName("phase9-trace-fixture-"+std::to_string(i));
        if(i==0)g->getOrCreateStateSet()->setAttribute(new SlowAttribute);
        if(i==1)g->setDrawCallback(new SlowDraw);
        osg::ref_ptr<osg::Geode> geode=new osg::Geode;geode->addDrawable(g);
        if(i>=2)
        {
            osg::ref_ptr<osg::Group> branch=new osg::Group;
            branch->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::OFF);
            branch->addChild(geode);root->addChild(branch);
        }
        else root->addChild(geode);
    }
    viewer.setSceneData(root);
    viewer.realize();require(viewer.isRealized(),"context could not realize");
    if(!threaded) context->getState()->setDynamicObjectCount(4);
    viewer.frame(); pixels->wait(viewer.getFrameStamp()->getFrameNumber());
    viewer.stopThreading();
    const auto reference=pixels->data;
    const auto referenceDynamic=context->getState()->getDynamicObjectCount();
    require(std::count(reference.begin(),reference.end(),255)>1024,"reference geometry did not render");
    require(install(viewer)==2,"real viewer double-buffered cull installation failed");
    auto* renderer=dynamic_cast<osgViewer::Renderer*>(camera->getRenderer());
    for(unsigned i=0;i<2;++i)
    {
        auto* cv=renderer->getSceneView(i)->getCullVisitor();
        require(dynamic_cast<SceneUtil::DrawPhaseTrace::CullVisitor*>(cv),"cull visitor was not replaced");
        osg::ref_ptr<osgUtil::CullVisitor> clone=cv->clone();
        require(dynamic_cast<SceneUtil::DrawPhaseTrace::CullVisitor*>(clone.get()),"nested view lost tracing subclass");
    }
    if(threaded) viewer.startThreading();
    for(int frame=0;frame<4;++frame)
    {
        if(!threaded) context->getState()->setDynamicObjectCount(4);
        viewer.frame(); pixels->wait(viewer.getFrameStamp()->getFrameNumber());
        require(pixels->data==reference,"traced renderer changed exact RGBA output");
        require(context->getState()->getDynamicObjectCount()==referenceDynamic,"dynamic completion differs from stock renderer");
    }
    viewer.stopThreading();
    const auto totals=Capture::instance().totals(context->getState()->getContextID());
    require(totals.calls>=16,"pooled traced RenderLeaves were not actually used");
    bool slowState=false,slowDraw=false;
    for(std::size_t i=0;i<Capture::instance().count();++i)
    {
        const auto row=Capture::instance().row(i);
        slowState |= row.stateMs>1.;slowDraw |= row.drawMs>2.;
    }
    require(slowState&&slowDraw,"state application and drawable delay were not separated");
    require(context->getState()->getDynamicObjectCount()==referenceDynamic,"dynamic draw completion semantics changed");
    viewer.setDone(true);viewer.stopThreading();viewer.setSceneData(nullptr);context->close(true);
    std::cout<<"PASS "<<(threaded ? "DrawThreadPerContext" : "SingleThreaded")<<": exact control/traced pixels, real double-buffer cull pools, nested visitor cloning, state-vs-draw delay attribution, and dynamic completion count\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
