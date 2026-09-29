#include <components/sceneutil/cullviewcache.hpp>
#include <components/sceneutil/drawphasetrace.hpp>
#include <components/sceneutil/sharedgeometryprep.hpp>
#include <osg/Geode>
#include <osg/GraphicsContext>
#include <osg/Texture2D>
#include <osgViewer/Viewer>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    osg::ref_ptr<osg::GraphicsContext> context()
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> t = new osg::GraphicsContext::Traits;
        t->readDISPLAY(); t->setUndefinedScreenDetailsToDefaultScreen();
        t->width = 32; t->height = 32; t->doubleBuffer = false; t->windowDecoration = false;
        return osg::GraphicsContext::createGraphicsContext(t);
    }
    osg::ref_ptr<osg::Geometry> triangle()
    {
        osg::ref_ptr<osg::Geometry> g = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> p = new osg::Vec3Array;
        p->push_back(osg::Vec3(-.8f,-.8f,0)); p->push_back(osg::Vec3(.8f,-.8f,0)); p->push_back(osg::Vec3(0,.8f,0));
        g->setVertexArray(p); g->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        g->setUseDisplayList(false); g->setUseVertexBufferObjects(true);
        return g;
    }
    void cullCache()
    {
        using namespace SceneUtil::CullViewCache;
        require(!Scope::current, "scope leaked before first traversal");
        osg::ref_ptr<osg::Camera> main = new osg::Camera;
        osg::ref_ptr<osg::Camera> shadow = new osg::Camera;
        main->setViewMatrix(osg::Matrixd::lookAt(osg::Vec3d(2,3,4), osg::Vec3d(0,0,0), osg::Vec3d(0,0,1)));
        for (unsigned i = 0; i < 1000; ++i)
        {
            const auto view = osg::Matrixd::rotate(i*.001, osg::Vec3d(0,0,1)) * main->getViewMatrix();
            main->setViewMatrix(view); shadow->setViewMatrix(view);
            osg::Matrixd expected; require(expected.invert(view), "fixture matrix singular");
            Scope outer(*main);
            require(Scope::find(*main) && *Scope::find(*main) == expected, "cached inverse changed exact matrix values");
            require(!Scope::find(*shadow), "main camera result leaked to shadow view");
            {
                Scope nested(*shadow);
                require(Scope::find(*shadow) && !Scope::find(*main), "nested view identity not isolated");
            }
            require(Scope::find(*main), "nested view did not restore scope");
            main->setViewMatrix(osg::Matrixd::translate(.01,0,0) * view);
            require(!Scope::find(*main), "camera mutation reused stale inverse");
            main->setViewMatrix(view);
        }
        require(!Scope::current, "main scope leaked outside traversal");
        main->setViewMatrix(osg::Matrixd::scale(0,0,0));
        { Scope singular(*main); require(!Scope::find(*main), "singular inverse must fall back"); }
        std::atomic_bool bad{false};
        auto worker = [&](double x) {
            osg::ref_ptr<osg::Camera> c = new osg::Camera;
            c->setViewMatrix(osg::Matrixd::translate(x,0,0));
            for (int i=0;i<1000;++i) {
                Scope scope(*c);
                if (!Scope::find(*c) || (*Scope::find(*c))(3,0) != -x) bad.store(true);
                std::this_thread::yield();
            }
        };
        std::thread a(worker, 2), b(worker, 5); a.join(); b.join();
        require(!bad.load() && !Scope::current, "parallel camera calculation escaped its owner");
        std::cout << "PASS: 1000 exact inverse comparisons, changed/singular/nested/secondary views and concurrent scopes; no visibility cache\n";
    }
    void sharedPrep()
    {
        osgViewer::Viewer anchor;
        auto gc = context(); require(gc && gc->realize() && gc->makeCurrent(), "no real GL context");
        auto* state = gc->getState(); osg::RenderInfo info(state, nullptr);
        auto source = triangle();
        osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
        uv->push_back(osg::Vec2(0,0)); uv->push_back(osg::Vec2(1,0)); uv->push_back(osg::Vec2(.5,1));
        osg::ref_ptr<osg::VertexBufferObject> shared = new osg::VertexBufferObject;
        shared->setUsage(GL_STATIC_DRAW_ARB); uv->setVertexBufferObject(shared); source->setTexCoordArray(0, uv);
        source->removePrimitiveSet(0, source->getNumPrimitiveSets());
        osg::ref_ptr<osg::DrawElementsUShort> indices = new osg::DrawElementsUShort(GL_TRIANGLES);
        indices->push_back(0); indices->push_back(1); indices->push_back(2);
        osg::ref_ptr<osg::ElementBufferObject> ebo = new osg::ElementBufferObject;
        ebo->setUsage(GL_STATIC_DRAW_ARB); indices->setElementBufferObject(ebo); source->addPrimitiveSet(indices);
        std::array<osg::ref_ptr<osg::Geometry>,2> privateGeometry;
        std::array<osg::ref_ptr<osg::VertexBufferObject>,2> privateBuffers;
        for(unsigned i=0;i<2;++i)
        {
            privateGeometry[i] = new osg::Geometry(*source, osg::CopyOp::SHALLOW_COPY);
            osg::ref_ptr<osg::Array> positions = static_cast<osg::Array*>(source->getVertexArray()->clone(osg::CopyOp::DEEP_COPY_ALL));
            privateBuffers[i] = new osg::VertexBufferObject; privateBuffers[i]->setUsage(GL_DYNAMIC_DRAW_ARB);
            positions->setVertexBufferObject(privateBuffers[i]); privateGeometry[i]->setVertexArray(positions);
        }
        const unsigned revision = uv->getModifiedCount();
        auto first = SceneUtil::SharedGeometryPrep::compile(info, *source, true);
        require(first.submitted == 2 && first.bytes == 32, "shared UV and index buffers were not prepared");
        require(uv->getModifiedCount() == revision, "preparation mutated source CPU revision");
        for(auto& buffer : privateBuffers) require(!buffer->getGLBufferObject(state->getContextID()), "preparation touched private pose allocation");
        auto second = SceneUtil::SharedGeometryPrep::compile(info, *source, false);
        require(second.submitted == 0 && second.clean == 2, "clean static storage redundantly uploaded");
        (*uv)[0] = osg::Vec2(.25f, .5f); uv->dirty();
        auto updated = SceneUtil::SharedGeometryPrep::compile(info, *source, true);
        require(updated.submitted == 1 && updated.clean == 1, "dirty source revision not respected");
        auto* ext = state->get<osg::GLExtensions>();
        ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, shared->getGLBufferObject(state->getContextID())->getGLObjectID());
        std::array<float,6> actual{}; ext->glGetBufferSubData(GL_ARRAY_BUFFER_ARB,0,sizeof(actual),actual.data());
        require(actual[0] == .25f && actual[1] == .5f, "prepared static bytes differ from source");
        ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, 0);
        shared->setUsage(GL_DYNAMIC_DRAW_ARB);
        require(SceneUtil::SharedGeometryPrep::compile(info,*source,true).skipped == 1, "dynamic buffer admitted");
        shared->setUsage(GL_STATIC_DRAW_ARB); uv->setDataVariance(osg::Object::DYNAMIC);
        require(SceneUtil::SharedGeometryPrep::compile(info,*source,true).skipped == 1, "mutable source array admitted");
        uv->setDataVariance(osg::Object::STATIC);
        osg::ref_ptr<osg::Vec2Array> huge = new osg::Vec2Array(300000);
        osg::ref_ptr<osg::VertexBufferObject> oversized = new osg::VertexBufferObject;
        huge->setVertexBufferObject(oversized); oversized->setUsage(GL_STATIC_DRAW_ARB); source->setTexCoordArray(1,huge);
        require(SceneUtil::SharedGeometryPrep::compile(info,*source,true).skipped == 1, "oversized preparation not bounded");
        require(!oversized->getGLBufferObject(state->getContextID()), "oversized preparation allocated before admission");
        SceneUtil::SharedGeometryPrep::prepare(info,source,true);
        require(SceneUtil::SharedGeometryPrep::Counters::instance().calls == 1, "production opt-in hook not engaged");
        require(glGetError() == GL_NO_ERROR, "GL error during shared preparation");
        source->releaseGLObjects(state); state->unbindVertexBufferObject(); state->unbindElementBufferObject();
        gc->releaseContext(); gc->close(true);
        std::cout << "PASS: real shared UV/index preparation, dirty/clean revisions, untouched private double buffers, mutable/oversized fallback and opt-in engagement\n";
    }
    using SubData = void (GL_APIENTRY *)(GLenum,GLintptr,GLsizeiptr,const GLvoid*);
    SubData underlying = nullptr;
    void GL_APIENTRY slowSubData(GLenum target,GLintptr offset,GLsizeiptr bytes,const GLvoid* data)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        underlying(target,offset,bytes,data);
    }
    void glHooks()
    {
        using namespace SceneUtil::GLCallTrace;
        osgViewer::Viewer anchor;
        auto gc=context(); require(gc && gc->realize() && gc->makeCurrent(), "no GL context");
        auto* state=gc->getState(); auto* ext=state->get<osg::GLExtensions>();
        osg::ref_ptr<osg::FrameStamp> stamp=new osg::FrameStamp; stamp->setFrameNumber(1);state->setFrameStamp(stamp);
        auto originalData=ext->glBufferData; underlying=ext->glBufferSubData;ext->glBufferSubData=&slowSubData;
        require(install(*state)>=25 && install(*state)>=25,"extension hooks did not attach idempotently");
        GLuint buffer=0;ext->glGenBuffers(1,&buffer);ext->glBindBuffer(GL_ARRAY_BUFFER_ARB,buffer);
        std::array<float,4> wanted{1,2,3,4};ext->glBufferData(GL_ARRAY_BUFFER_ARB,sizeof(wanted),wanted.data(),GL_DYNAMIC_DRAW_ARB);
        void* mapped=ext->glMapBuffer(GL_ARRAY_BUFFER_ARB,GL_READ_ONLY_ARB);
        require(mapped && std::memcmp(mapped,wanted.data(),sizeof(wanted))==0,"hook altered mapping return/data");
        require(ext->glUnmapBuffer(GL_ARRAY_BUFFER_ARB)==GL_TRUE,"hook altered boolean return");
        stamp->setFrameNumber(2);
        { LeafScope scope(123);scope.phase(Phase::State);float replacement=8;ext->glBufferSubData(GL_ARRAY_BUFFER_ARB,0,sizeof(replacement),&replacement); }
        require(location.drawable==0 && location.phase==Phase::OutsideLeaf,"GL leaf scope escaped");
        std::array<float,4> actual{};ext->glGetBufferSubData(GL_ARRAY_BUFFER_ARB,0,sizeof(actual),actual.data());
        require(actual[0]==8 && actual[1]==2,"hook altered GL upload arguments");
        require(Capture::instance().calls(state->getContextID(),Api::glBufferData)==1
            && Capture::instance().calls(state->getContextID(),Api::glBufferSubData)==1,"GL calls not counted");
        restore(*state);require(ext->glBufferData==originalData && ext->glBufferSubData==&slowSubData,"original dispatch pointers not restored");
        ext->glBufferSubData=underlying;ext->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);ext->glDeleteBuffers(1,&buffer);
        require(glGetError()==GL_NO_ERROR,"GL hook caused API error");gc->releaseContext();gc->close(true);
        std::cout<<"PASS: real GL ABI/argument/return parity, mapping, stable frame/phase identity, injected delay and idempotent install/restore\n";
    }
    struct Completion : osg::Camera::DrawCallback
    {
        mutable std::atomic<unsigned> frames{0};
        void operator()(osg::RenderInfo&) const override { frames.fetch_add(1,std::memory_order_release); }
    };
    void startup()
    {
        using namespace SceneUtil::DrawPhaseTrace;
        osgViewer::Viewer viewer;viewer.setThreadingModel(osgViewer::Viewer::DrawThreadPerContext);
        auto gc=context();require(gc.valid(),"no window");
        auto* camera=viewer.getCamera();camera->setGraphicsContext(gc);camera->setViewport(0,0,32,32);
        camera->setViewMatrix(osg::Matrixd::identity());camera->setProjectionMatrix(osg::Matrixd::identity());
        camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);camera->setName("MainCamera");
        osg::ref_ptr<Completion> done=new Completion;camera->setFinalDrawCallback(done);
        viewer.setSceneData(new osg::Group);
        require(installRequired(viewer,false)==2,"pre-realize real viewer installation failed");
        viewer.setRealizeOperation(new SceneUtil::GLCallTrace::InstallOperation);
        viewer.realize();require(viewer.areThreadsRunning(),"fixture did not start real draw thread");
        // Mirror game startup: PostProcessor replaces the root AFTER realization.
        osg::ref_ptr<osg::Geode> geode=new osg::Geode;auto g=triangle();g->setDataVariance(osg::Object::DYNAMIC);
        geode->addDrawable(g);viewer.setSceneData(geode);
        bool rejected=false;try{installRequired(viewer,false);}catch(const std::exception&){rejected=true;}
        require(rejected,"late installation still silently accepted");
        for(unsigned i=1;i<=5;++i)
        {
            viewer.frame();const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(done->frames.load(std::memory_order_acquire)<i)
            {require(std::chrono::steady_clock::now()<until,"startup fixture draw blocked");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        }
        viewer.stopThreading();
        require(Capture::instance().totals(gc->getState()->getContextID()).calls>=5,"pre-realize pools were replaced or unused after scene-root setup");
        require(SceneUtil::GLCallTrace::Capture::instance().installed(gc->getState()->getContextID()),"GL context hook missing");
        SceneUtil::GLCallTrace::restore(*gc->getState());viewer.setDone(true);viewer.setSceneData(nullptr);gc->close(true);
        std::cout<<"PASS: actual DrawThreadPerContext pre-realize installation, late-attempt rejection, replacement scene root, used leaves and GL-context hook\n";
    }
}
int main(int argc,char** argv) try
{
    require(argc==2,"select fixture");std::string which=argv[1];
    if(which=="cull")cullCache();else if(which=="prep")sharedPrep();else if(which=="gl")glHooks();else if(which=="startup")startup();else throw std::runtime_error("unknown fixture");
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
