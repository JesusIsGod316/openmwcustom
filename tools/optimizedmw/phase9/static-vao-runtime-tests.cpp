#include <components/sceneutil/staticvaogeometry.hpp>
#include <osg/Camera>
#include <osg/Program>
#include <osg/Shader>
#include <osg/ClipControl>
#include <osg/Depth>
#include <osgViewer/Viewer>
#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#include <DbgHelp.h>
#endif

namespace V=SceneUtil::StaticVao;
namespace
{
    unsigned checks=0;
    void require(bool condition,const char* text);
    void progress(const char* text) { std::cerr << "STATIC_VAO_STAGE " << text << std::endl; }
#ifdef _WIN32
    LONG WINAPI exceptionReport(EXCEPTION_POINTERS* exception)
    {
        const HANDLE process=GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        const bool symbols=SymInitialize(process,nullptr,TRUE)!=FALSE;
        CONTEXT context=*exception->ContextRecord;
        STACKFRAME64 frame{};frame.AddrPC.Offset=context.Rip;frame.AddrStack.Offset=context.Rsp;
        frame.AddrFrame.Offset=context.Rbp;frame.AddrPC.Mode=frame.AddrStack.Mode=frame.AddrFrame.Mode=AddrModeFlat;
        std::cerr << "STATIC_VAO_EXCEPTION code=" << std::hex << exception->ExceptionRecord->ExceptionCode
            << " address=" << exception->ExceptionRecord->ExceptionAddress << std::dec << std::endl;
        for(unsigned i=0;i<24 && frame.AddrPC.Offset;++i)
        {
            alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+1024]{};
            auto* symbol=reinterpret_cast<SYMBOL_INFO*>(storage);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);
            symbol->MaxNameLen=1023;DWORD64 displacement=0;
            std::cerr << "stack " << i << " " << std::hex << frame.AddrPC.Offset << std::dec;
            if(symbols && SymFromAddr(process,frame.AddrPC.Offset,&displacement,symbol))
                std::cerr << " " << symbol->Name << " +" << displacement;
            IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD lineOffset=0;
            if(symbols && SymGetLineFromAddr64(process,frame.AddrPC.Offset,&lineOffset,&line))
                std::cerr << " " << line.FileName << ":" << line.LineNumber;
            std::cerr << std::endl;
            if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,GetCurrentThread(),&frame,&context,nullptr,
                SymFunctionTableAccess64,SymGetModuleBase64,nullptr)) break;
        }
        if(symbols) SymCleanup(process);
        return EXCEPTION_EXECUTE_HANDLER;
    }
#endif
    void require(bool condition,const char* text) {++checks;if(!condition) throw std::runtime_error(text);}
    struct Pixels final : osg::Camera::DrawCallback
    {
        mutable std::array<unsigned char,128*128*4> data{};
        mutable GLenum error=GL_NO_ERROR;
        void operator()(osg::RenderInfo&) const override
        {error=glGetError();glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,data.data());if(error==GL_NO_ERROR) error=glGetError();}
    };
    struct UnknownDraw final : osg::Drawable::DrawCallback
    {
        void drawImplementation(osg::RenderInfo& info,const osg::Drawable* drawable) const override
        {drawable->drawImplementation(info);}
    };
    osg::ref_ptr<osg::Geometry> mesh(osg::Array::Binding colorBinding)
    {
        osg::ref_ptr<osg::Geometry> value=new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array;
        for(const osg::Vec3& vertex : {osg::Vec3(-.85f,-.8f,0),osg::Vec3(.85f,-.8f,0),osg::Vec3(.85f,.8f,0),osg::Vec3(-.85f,.8f,0)})
            vertices->push_back(vertex);
        osg::ref_ptr<osg::Vec3Array> normals=new osg::Vec3Array;
        normals->push_back(osg::Vec3(0,0,1));
        osg::ref_ptr<osg::Vec2Array> uv=new osg::Vec2Array;
        for(const osg::Vec2& coordinate : {osg::Vec2(0,0),osg::Vec2(1,0),osg::Vec2(1,1),osg::Vec2(0,1)})
            uv->push_back(coordinate);
        osg::ref_ptr<osg::Vec4Array> colors=new osg::Vec4Array;
        if(colorBinding==osg::Array::BIND_PER_VERTEX)
            for(const osg::Vec4& color : {osg::Vec4(.9f,.2f,.3f,1),osg::Vec4(.2f,.9f,.3f,1),osg::Vec4(.3f,.2f,.9f,1),osg::Vec4(.9f,.8f,.3f,1)})
                colors->push_back(color);
        else if(colorBinding==osg::Array::BIND_PER_PRIMITIVE_SET)
            for(const osg::Vec4& color : {osg::Vec4(.9f,.2f,.3f,1),osg::Vec4(.2f,.9f,.3f,1)})
                colors->push_back(color);
        else colors->push_back({.9f,.2f,.3f,1});
        value->setVertexArray(vertices);value->setNormalArray(normals,osg::Array::BIND_OVERALL);
        value->setColorArray(colors,colorBinding);value->setTexCoordArray(0,uv,osg::Array::BIND_PER_VERTEX);
        osg::ref_ptr<osg::DrawElementsUInt> first=new osg::DrawElementsUInt(GL_TRIANGLES),second=new osg::DrawElementsUInt(GL_TRIANGLES);
        for(unsigned index : {0u,1u,2u}) first->push_back(index);
        for(unsigned index : {0u,2u,3u}) second->push_back(index);
        value->addPrimitiveSet(first);value->addPrimitiveSet(second);
        value->setUseDisplayList(false);value->setUseVertexBufferObjects(true);value->setDataVariance(osg::Object::STATIC);
        auto* states=value->getOrCreateStateSet();states->setMode(GL_LIGHTING,osg::StateAttribute::OFF);
        states->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
        osg::ref_ptr<osg::Program> program=new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvarying vec4 value; void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;value=gl_Color;value.g+=gl_MultiTexCoord0.x*.05;value.b+=gl_Normal.z*.05;}\n"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nvarying vec4 value;void main(){gl_FragColor=value;}\n"));
        states->setAttributeAndModes(program,osg::StateAttribute::ON);return value;
    }
}
int main() try
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(exceptionReport);
#endif
    progress("intact production Geometry::drawImplementation; no diagnostic split or pointer probes");
    progress("initial mesh");
    auto ordinary=mesh(osg::Array::BIND_PER_VERTEX);
    require(!ordinary->getUseVertexArrayObject(),"ordinary default control already enables VAO");
    progress("initial candidate factory");
    auto candidate=V::make(*ordinary);require(candidate && candidate->getUseVertexArrayObject(),"static recipe refused");
    progress("callback and dynamic rejection");
    auto callback=mesh(osg::Array::BIND_PER_VERTEX);callback->setDrawCallback(new UnknownDraw);
    require(!V::make(*callback),"unknown draw callback admitted");
    callback=mesh(osg::Array::BIND_PER_VERTEX);callback->setDataVariance(osg::Object::DYNAMIC);
    require(!V::make(*callback),"dynamic geometry admitted");
    callback=mesh(osg::Array::BIND_PER_VERTEX);callback->getVertexArray()->setDataVariance(osg::Object::DYNAMIC);
    require(!V::make(*callback),"dynamic array admitted");
    callback=mesh(osg::Array::BIND_PER_VERTEX);
    progress("foreign storage rejection");
    osg::ref_ptr<osg::Vec3Array> foreign=new osg::Vec3Array;
    foreign->push_back(osg::Vec3(1,2,3));
    foreign->setBufferObject(callback->getVertexArray()->getBufferObject());
    require(!V::make(*callback),"foreign shared buffer storage admitted");
    callback=nullptr;foreign=nullptr;
    progress("metadata cap and clone rejection");
    const auto live=V::Counters::liveBytes.load();V::Counters::liveBytes=V::MaximumBytes;
    require(!V::make(*ordinary),"metadata cap did not preserve control");V::Counters::liveBytes=live;
    osg::ref_ptr<osg::Geometry> cloned=dynamic_cast<osg::Geometry*>(candidate->clone(osg::CopyOp::SHALLOW_COPY));
    require(cloned && !cloned->getUseVertexArrayObject(),"cloned candidate inherited another producer certificate");
    cloned=nullptr;candidate=nullptr;ordinary=nullptr;
    progress("create pbuffer");

    {
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->width=128;traits->height=128;traits->pbuffer=true;traits->doubleBuffer=false;traits->windowDecoration=false;
    traits->readDISPLAY();traits->setUndefinedScreenDetailsToDefaultScreen();
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(),"offscreen graphics context unavailable");
    progress("create viewer");
    osgViewer::Viewer viewer;viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    osg::Camera* camera=viewer.getCamera();camera->setGraphicsContext(context);camera->setViewport(0,0,128,128);
    camera->setProjectionMatrix(osg::Matrix::identity());camera->setViewMatrix(osg::Matrix::identity());
    camera->setClearColor({0,0,0,1});camera->setDrawBuffer(GL_FRONT);camera->setReadBuffer(GL_FRONT);
    osg::ref_ptr<Pixels> pixels=new Pixels;camera->setFinalDrawCallback(pixels);
    osg::ref_ptr<osg::Group> root=new osg::Group;viewer.setSceneData(root);viewer.realize();
    progress("viewer realized");
    const auto image=[&](osg::Node* node,unsigned frames=3) {
        root->removeChildren(0,root->getNumChildren());root->addChild(node);
        for(unsigned i=0;i<frames;++i) {progress("before frame");viewer.frame();progress("after frame");}
        require(pixels->error==GL_NO_ERROR,"native layout produced GL error");return pixels->data;
    };
    const auto compare=[&](const char* label) {
        progress(label);progress("comparison ordinary");const auto control=image(ordinary);
        progress("comparison candidate");const auto derived=image(candidate);
        require(control==derived,label);
    };
    const auto resetOrdinaryState=[&] {
        require(context->makeCurrent(),"shared release ordinary reset could not acquire context");
        auto* state=context->getState();state->setCurrentToGlobalVertexArrayState();
        state->bindVertexArrayObject(state->getCurrentVertexArrayState());
        state->disableAllVertexArrays();state->getCurrentVertexArrayState()->resetBufferObjectPointers();
        auto* extensions=state->get<osg::GLExtensions>();
        extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);extensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB,0);
        context->releaseContext();progress("explicit ordinary global VAS reset after deliberate shared buffer release");
    };
    const auto aliasNamespace=[&](bool enabled) {
        require(context->makeCurrent(),"alias transition ordinary cleanup could not acquire context");
        auto* state=context->getState();state->setCurrentToGlobalVertexArrayState();
        auto* ordinaryArrays=state->getCurrentVertexArrayState();
        state->bindVertexArrayObject(ordinaryArrays);state->disableAllVertexArrays();
        // Pinned native State replaces global dispatcher owning refs during
        // alias changes. Its active lists store raw dispatcher pointers; clear
        // those lists while the old owners still exist, before replacing them.
        ordinaryArrays->_activeDispatchers.clear();ordinaryArrays->_previous_activeDispatchers.clear();
        ordinaryArrays->resetBufferObjectPointers();
        auto* extensions=state->get<osg::GLExtensions>();
        extensions->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);extensions->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_ARB,0);
        progress("ordinary global dispatcher list retirement before alias namespace transition");
        state->setUseVertexAttributeAliasing(enabled);context->releaseContext();
    };
    for(auto binding:{osg::Array::BIND_OVERALL,osg::Array::BIND_PER_PRIMITIVE_SET,osg::Array::BIND_PER_VERTEX})
    {
        progress("binding pixels: ordinary");
        ordinary=mesh(binding);candidate=V::make(*ordinary);require(candidate.valid(),"ordinary binding refused");
        const auto control=image(ordinary);const auto before=V::Counters::layoutReuses.load();
        progress("binding pixels: candidate");
        const auto derived=image(candidate);
        require(control==derived,"VAO changed ordinary overall/per-primitive/per-vertex pixels");
        require(V::Counters::layoutReuses.load()>before,"actual initialized native VAO was not reused");
        require(derived[64*128*4+64*4]!=0 || derived[64*128*4+64*4+1]!=0,"pixel proof only compared empty frames");
        progress("ordinary following retained candidate");
        require(image(ordinary)==control,"VAO corrupted following ordinary VBO draw");
        progress("candidate rebind following ordinary");
        require(image(candidate)==control,"ordinary draw corrupted retained VAO rebind");
        auto* changed=dynamic_cast<osg::Vec3Array*>(candidate->getVertexArray());
        (*changed)[0].x()+=.2f;changed->dirty(); // Array::dirty alone must invalidate retained layout.
        const auto fallbackBefore=V::Counters::producerFallbacks.load();
        progress("dirty source ordinary");
        const auto changedControl=image(ordinary);
        require(context->makeCurrent(),"dirty input binding proof could not acquire context");
        GLint rawArrayBuffer=0;glGetIntegerv(GL_ARRAY_BUFFER_BINDING_ARB,&rawArrayBuffer);
        auto* nativeArrays=candidate->getVertexArrayStateList()[context->getState()->getContextID()].get();
        require(nativeArrays && nativeArrays->getCurrentVertexBufferObject(),"candidate VAS lacks cached VBO for binding proof");
        const GLuint cachedArrayBuffer=nativeArrays->getCurrentVertexBufferObject()->getGLObjectID();
        std::cerr<<"STATIC_VAO_RAW_BINDING actual="<<rawArrayBuffer<<" candidate_cached="<<cachedArrayBuffer<<std::endl;
        // Reproduce a valid intervening ordinary/client draw leaving global
        // ARRAY_BUFFER=0. The retained candidate VAS must not treat its cached
        // buffer pointer as the current context binding during full setup.
        context->getState()->get<osg::GLExtensions>()->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);
        context->getState()->getCurrentVertexArrayState()->setCurrentVertexBufferObject(nullptr);
        require(cachedArrayBuffer!=0,"binding proof did not retain a real candidate VBO");
        context->releaseContext();
        progress("dirty source candidate fallback");
        const auto changedCandidate=image(candidate);
        require(changedControl==changedCandidate,"dirty shared source input changed fallback pixels");
        require(V::Counters::producerFallbacks.load()>fallbackBefore,"dirty producer did not take fallback");
        ordinary=nullptr;candidate=nullptr;
    }
    ordinary=mesh(osg::Array::BIND_PER_VERTEX);candidate=V::make(*ordinary);
    progress("reverse depth");
    camera->setClearDepth(0);camera->getOrCreateStateSet()->setAttributeAndModes(new osg::Depth(osg::Depth::GEQUAL,0,1,true));
    compare("reverse depth changed VAO/control pixels");
    camera->setClearDepth(1);camera->getOrCreateStateSet()->removeAttribute(osg::StateAttribute::DEPTH);
    // A deliberate release of one shallow clone also releases its shared
    // source buffers. Prove candidate reconstruction first with no intervening
    // ordinary draws, then explicitly reset the ordinary global VAS whose
    // unchanged-array dispatch cache otherwise still refers to released data.
    progress("pre-release ordinary baseline");const auto beforeRelease=image(ordinary);
    context->makeCurrent();candidate->releaseGLObjects(context->getState());context->releaseContext();
    progress("released context resources");
    const auto setups=V::Counters::layoutSetups.load();
    progress("released candidate alone");const auto afterRelease=image(candidate);
    require(beforeRelease==afterRelease,"candidate GL resource reconstruction changed pixels");
    require(V::Counters::layoutSetups.load()>setups,"released VAO was treated as initialized");
    resetOrdinaryState();compare("shared release with explicit ordinary reset changed pixels");
    // Releasing ordinary shared buffers leaves the candidate VAO itself alive.
    // A new clean GLBufferObject must still invalidate its captured storage.
    context->makeCurrent();ordinary->releaseGLObjects(context->getState());context->releaseContext();
    resetOrdinaryState();const auto storageSetups=V::Counters::layoutSetups.load();
    compare("clean replacement GL storage did not rebuild retained candidate layout");
    require(V::Counters::layoutSetups.load()>storageSetups,"changed clean GL storage reused old VAO layout");
    // Array resize/replacement and EBO replacement keep live ordinary output.
    osg::ref_ptr<osg::Vec3Array> replacement=new osg::Vec3Array(*static_cast<osg::Vec3Array*>(ordinary->getVertexArray()));
    (*replacement)[1].x()=.3f;ordinary->setVertexArray(replacement);candidate->setVertexArray(replacement);
    progress("replacement vertex storage");
    compare("replacement vertex storage used stale VAO offset");
    osg::ref_ptr<osg::DrawElementsUInt> element=new osg::DrawElementsUInt(GL_TRIANGLES);
    for(unsigned index : {0u,1u,3u}) element->push_back(index);
    ordinary->setPrimitiveSet(0,element);candidate->setPrimitiveSet(0,element);
    progress("replacement index storage");
    compare("replacement index storage used stale EBO");
    // Use a freshly admitted immutable producer, so these namespace tests
    // exercise candidate reconstruction followed by actual retained reuse.
    ordinary=mesh(osg::Array::BIND_PER_VERTEX);candidate=V::make(*ordinary);
    compare("fresh alias control/candidate baseline pixels");
    const auto aliasSetups=V::Counters::layoutSetups.load();
    const auto aliasReuse=V::Counters::layoutReuses.load();
    aliasNamespace(true);
    compare("attribute alias namespace changed control/candidate pixels");
    require(V::Counters::layoutSetups.load()>aliasSetups,"candidate alias change did not reconstruct native namespace");
    require(V::Counters::layoutReuses.load()>aliasReuse,"candidate alias layout did not become reusable");
    aliasNamespace(false);
    compare("attribute alias namespace restoration changed control/candidate pixels");
    // Client index transition after an initialized indexed VAO must not leave
    // its retained EBO binding treating the host index address as an offset.
    progress("VBO and indexed VAO baseline before client input mutation");
    ordinary=mesh(osg::Array::BIND_PER_VERTEX);candidate=V::make(*ordinary);
    compare("indexed VAO baseline before client fallback changed pixels");
    for(unsigned i=0;i<ordinary->getNumPrimitiveSets();++i)
        ordinary->getPrimitiveSet(i)->setBufferObject(nullptr);
    resetOrdinaryState();
    const auto clientFallbacks=V::Counters::producerFallbacks.load();
    compare("VBO vertices with client indices fallback changed pixels");
    require(V::Counters::producerFallbacks.load()>clientFallbacks,"client indices did not decline retained layout");
    // Then remove VBO storage from all shared input arrays too. Native global
    // VAS/client rendering remains valid, whereas a nonzero ARB VAO forbids it.
    ordinary->getVertexArray()->setBufferObject(nullptr);ordinary->getNormalArray()->setBufferObject(nullptr);
    ordinary->getColorArray()->setBufferObject(nullptr);ordinary->getTexCoordArray(0)->setBufferObject(nullptr);
    resetOrdinaryState();
    compare("client vertex and index inputs fallback changed pixels");
    root->removeChildren(0,root->getNumChildren());ordinary=nullptr;candidate=nullptr;viewer.setSceneData(nullptr);
    } // Renderer/CullVisitor retained leaves release their final native leases.
    require(V::Counters::liveBytes.load()==0,"candidate owner leaked bounded metadata charge");
    std::cout<<"static VAO checks="<<checks<<" draws="<<V::Counters::draws<<" actual_initialized_layout_reuses="
        <<V::Counters::layoutReuses<<" setups="<<V::Counters::layoutSetups<<" producer_fallbacks="<<V::Counters::producerFallbacks<<'\n';
    return 0;
}
catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
