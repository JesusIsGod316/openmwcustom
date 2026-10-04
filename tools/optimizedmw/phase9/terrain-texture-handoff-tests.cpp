#include <components/resource/assetgpubudget.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/openmwcompileoperation.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/texturestreaming.hpp>
#include <components/resource/uploadpool.hpp>
#include <components/sceneutil/textureownerboundary.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/chunkmanager.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/storage.hpp>
#include <components/terrain/terraindrawable.hpp>
#include <components/terrain/texturemanager.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/vfs/manager.hpp>
#include <osg/Geode>
#include <osg/Group>
#include <osg/Program>
#include <osg/Shader>
#include <osgViewer/Viewer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace N=Resource;
namespace T=Resource::TextureStreaming;
namespace
{
    using Bytes=std::vector<std::uint8_t>;
    void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
    template<class Predicate> void until(Predicate predicate,const char* message)
    {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!predicate())
        { require(std::chrono::steady_clock::now()<end,message);std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    void put(Bytes& bytes,unsigned at,unsigned value)
    { for(unsigned i=0;i<4;++i) bytes[at+i]=static_cast<std::uint8_t>(value>>(i*8)); }
    Bytes dds()
    {
        Bytes bytes(128,0);put(bytes,0,0x20534444);put(bytes,4,124);put(bytes,8,0x21007);
        put(bytes,12,512);put(bytes,16,512);put(bytes,76,32);put(bytes,80,4);put(bytes,84,0x31545844);
        put(bytes,108,0x401008);unsigned level=0;
        for(unsigned side=512;;side=(std::max)(1u,side/2))
        {
            const auto begin=bytes.size();const auto blocks=((side+3)/4)*((side+3)/4);bytes.resize(begin+blocks*8,0);
            for(unsigned block=0;block<blocks;++block)
            { bytes[begin+block*8]=level ? 0xe0 : 0x00;bytes[begin+block*8+1]=level ? 0x07 : 0xf8; }
            ++level;if(side==1) break;
        }
        put(bytes,28,level);put(bytes,20,static_cast<unsigned>(bytes.size()-128));return bytes;
    }
    class Source final : public VFS::File
    {
    public:
        Bytes bytes=dds();std::atomic_bool hold{false},entered{false};
        Files::IStreamPtr open() override
        {
            // Delay only the real native preparation worker. The ordinary CPU
            // image is seeded before this gate, as it is in image-cache reuse.
            if(N::SpeculativeScope::active())
            { entered=true;until([this]{return !hold.load();},"delayed native source was not released"); }
            return std::make_unique<std::istringstream>(std::string(bytes.begin(),bytes.end()),std::ios::binary);
        }
        std::optional<VFS::SourceOpenEstimate> getOpenEstimate() const override
        { return VFS::SourceOpenEstimate{bytes.size(),bytes.size()*2+1024*1024}; }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "handoff"; }
    };
    class Archive final : public VFS::Archive
    {
    public:
        Source source;
        void listResources(VFS::FileMap& out) override
        { out[VFS::Path::Normalized("textures/handoff.dds")]=&source; }
        bool contains(VFS::Path::NormalizedView name) const override { return name.value()=="textures/handoff.dds"; }
        std::string getDescription() const override { return "bounded delayed production texture fixture"; }
    };
    class Storage final : public Terrain::Storage
    {
    public:
        Source* source=nullptr;bool requireEarly=false;unsigned prepared=0,filled=0;
        void getBounds(float& a,float& b,float& c,float& d,ESM::RefId) override { a=c=0;b=d=1; }
        bool getMinMaxHeights(float,const osg::Vec2f&,ESM::RefId,float& a,float& b) override { a=b=0;return true; }
        void fillVertexBuffers(int,float,const osg::Vec2f&,ESM::RefId,osg::Vec3Array& positions,
            osg::Vec3Array& normals,osg::Vec4ubArray& colors) override
        {
            if(requireEarly)
            {
                require(prepared>0,"MainDraw preparation started after serial vertex filling");
                until([this]{return source->entered.load();},"real preparation worker never opened delayed source");
            }
            ++filled;
            for(unsigned y=0;y<3;++y) for(unsigned x=0;x<3;++x)
            { positions.push_back({float(x)-1,float(y)-1,0});normals.push_back({0,0,1});colors.push_back({255,255,255,255}); }
        }
        void getBlendmaps(float,const osg::Vec2f&,ImageVector&,std::vector<Terrain::LayerInfo>& layers,ESM::RefId) override
        { ++prepared;layers.push_back({VFS::Path::Normalized("textures/handoff.dds"),{},false,false}); }
        float getHeightAt(const osg::Vec3f&,ESM::RefId) override { return 0; }
        float getCellWorldSize(ESM::RefId) override { return 2; }
        int getCellVertices(ESM::RefId) override { return 3; }
        int getTextureTileCount(float,ESM::RefId) override { return 1; }
    };
    std::mutex sampleMutex;
    N::OpenGlPressureSample sample;
    N::OpenGlPressureSample readSample(void*) { std::lock_guard lock(sampleMutex);return sample; }
    void healthy(N::SpeculativeBudget& ledger)
    {
        { std::lock_guard lock(sampleMutex);
          ++sample.generation;sample.decision.state=N::OpenGlPressure::Normal;
          sample.growthWatermarkValid=true;sample.accountedGrowth=ledger.watermark()->load();
          sample.decision.admissionsPerSample=4;sample.memory.physicalValid=sample.memory.commitValid=true;
          sample.memory.physicalTotal=32ull*1024*1024*1024;sample.memory.physicalAvailable=16ull*1024*1024*1024;
          sample.memory.commitAvailable=16ull*1024*1024*1024;
          sample.decision.limits.physicalReserve=4ull*1024*1024*1024;sample.decision.limits.commitReserve=1024ull*1024*1024; }
        std::lock_guard lock(Debug::V3GpuMemory::sAdmissionMutex);
        Debug::V3GpuMemory::sAdmissionSample={2ull*1024*1024*1024,N::AssetGpuBudget::milliseconds(),
            Debug::V3GpuMemory::PressureState::Comfortable,true};
    }
    osg::ref_ptr<osg::Program> sampledColorProgram()
    {
        // The production terrain pass/texture/geometry remain real. A narrow
        // fixture shader samples only color unit0, isolating asset handoff from
        // engine-wide lighting/shadow configuration and normal reconstruction.
        osg::ref_ptr<osg::Program> program=new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvarying vec2 uv;void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;uv=gl_MultiTexCoord0.xy;}"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nuniform sampler2D diffuseMap;varying vec2 uv;void main(){gl_FragColor=texture2D(diffuseMap,uv);}"));
        return program;
    }
    struct Receipt final : osg::Drawable::DrawCallback
    {
        osg::Texture2D* texture=nullptr;bool candidate=false,negative=false,streaming=false;
        const osg::Drawable* originalDrawable=nullptr;
        std::vector<std::pair<const osg::BufferData*,const osg::BufferObject*>> sourceBuffers;
        mutable GLuint name=0;mutable std::atomic_uint draws{0};mutable std::uint64_t epoch=0,bytes=0,allocations=0;
        void drawImplementation(osg::RenderInfo& info,const osg::Drawable* drawable) const override
        {
            require(drawable==originalDrawable,"pass adapter duplicated or bypassed the original draw callback");
            for(const auto& [data,buffer]:sourceBuffers)
                require(data->getBufferObject()==buffer,"pass adapter mutated source VBO/EBO ownership during cull");
            auto& state=*info.getState();const auto frame=state.getFrameStamp()->getFrameNumber();
            const auto currentBytes=T::counters().uploadedBytes.load()+N::nativeTextureStats().fullUploadBytes.load();
            const auto currentAllocations=T::counters().allocations.load()+N::nativeTextureStats().fullAllocations.load();
            GLint current=0,width=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&current);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
            require(current!=0,"actual terrain draw had no texture backing");
            if(candidate && !negative)
            {
                require(!texture->getTextureObject(state.getContextID()),"delayed-ready cached producer ordinary-allocated before first draw");
                require(width==(streaming ? 256 : 512),"first native backing quality did not match selected lane");
            }
            else require(texture->getTextureObject(state.getContextID()) && width==512,"control/cold fallback lost exact full source quality");
            if(draws && epoch==frame)
                require(name==static_cast<GLuint>(current) && bytes==currentBytes && allocations==currentAllocations,
                    "main/refraction subviews changed frozen backing or upload debt");
            epoch=frame;name=current;bytes=currentBytes;allocations=currentAllocations;++draws;
            drawable->drawImplementation(info);
        }
    };
    void compile(osg::Texture* texture,osg::GraphicsContext* context)
    {
        osg::ref_ptr<N::OpenMWIncrementalCompileOperation> ico=new N::OpenMWIncrementalCompileOperation({});
        osg::ref_ptr<osg::Group> graph=new osg::Group;graph->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);
        osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> set=new osgUtil::IncrementalCompileOperation::CompileSet(graph);
        osgUtil::IncrementalCompileOperation::ContextSet contexts;contexts.insert(context);set->buildCompileMap(contexts);
        ico->add(set,false);ico->compileAllForNextFrame();(*ico)(context);
        require(set->compiled(),"production optional ICO did not finish its actual CompileTextureOp");
    }
}
int main(int argc,char** argv) try
{
    unsigned mask=argc>1 ? static_cast<unsigned>(std::stoul(argv[1])) : 4;
    bool negative=false,threaded=false,compositeSource=false,cancelled=false;
    for(int i=2;i<argc;++i)
        if(std::string_view(argv[i])=="--cold-visible") negative=true;
        else if(std::string_view(argv[i])=="--threaded") threaded=true;
        else if(std::string_view(argv[i])=="--composite") compositeSource=true;
        else if(std::string_view(argv[i])=="--cancel") { cancelled=true;negative=true; }
        else throw std::runtime_error("unknown terrain handoff fixture argument");
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK",std::to_string(mask).c_str());
#else
    setenv("OPENMW_P9_ASSET_MASK",std::to_string(mask).c_str(),1);
#endif
    Settings::SettingsFileParser parser;
    parser.loadSettingsFile(std::filesystem::path(P9_SOURCE)/"files/settings-default.cfg",Settings::Manager::mDefaultSettings);
    Settings::StaticValues::initDefaults();Settings::Manager::mUserSettings=Settings::Manager::mDefaultSettings;Settings::StaticValues::init();
    Settings::cells().mOptimizedMWParallelTerrainCpuPrep.set(false);
    N::SpeculativeBudget::Config config;config.transientLimit=N::AssetPolicy::CpuTransientLimit;
    N::SpeculativeBudget ledger(config);N::UploadPool::ledger=&ledger;N::UploadPool::sample=&readSample;healthy(ledger);
    VFS::Manager vfs;auto archive=std::make_unique<Archive>();auto* source=&archive->source;vfs.addArchive(std::move(archive));vfs.buildIndex();
    N::ImageManager images(&vfs,100);auto original=images.getImage(VFS::Path::Normalized("textures/handoff.dds"));
    require(original && original->s()==512,"ordinary DDS image fixture did not load real winning source");
    source->hold=mask!=0;
    struct Release { Source* source;~Release(){source->hold=false;} } release{source};
    N::SceneManager scenes(&vfs,&images,nullptr,nullptr,100);scenes.setShaderPath(std::filesystem::path(P9_SOURCE)/"files/shaders");
    osg::ref_ptr<SceneUtil::LightManager> lighting=new SceneUtil::LightManager;
    auto defines=Shader::getDefaultDefines();
    for(const auto& [name,value]:SceneUtil::ShadowManager::getShadowsDisabledDefines()) defines[name]=value;
    for(const auto& [name,value]:lighting->getLightDefines()) defines[name]=value;
    scenes.getShaderManager().setGlobalDefines(defines);
    Terrain::TextureManager textures(&scenes,100,true);Storage storage;storage.source=source;storage.requireEarly=mask!=0 && !compositeSource;
    osg::ref_ptr<Terrain::CompositeMapRenderer> composite=new Terrain::CompositeMapRenderer;
    Terrain::ChunkManager chunks(&storage,&scenes,&textures,composite,ESM::RefId{},100);chunks.setCompositeMapLevel(compositeSource ? .5f : 16.f);chunks.setNodeMask(~0u);
    auto chunk=chunks.getChunk(1,{0,0},0,0,true,{0,0,2},false);
    auto* terrain=dynamic_cast<Terrain::TerrainDrawable*>(chunk.get());
    require(terrain && terrain->getNodeMask()!=0 && terrain->getPasses().size()==1 && storage.prepared==1 && storage.filled==1,
        "real ChunkManager did not run one preparePassData/realizePasses terrain consumer");
    terrain->setLightListCallback(nullptr); // This gate does not qualify dynamic-light cull.
    osg::Drawable* sampledDrawable=terrain;
    osg::StateSet* pass=terrain->getPasses()[0].get();
    if(compositeSource)
    {
        const auto* map=terrain->getCompositeMap();
        require(map && map->mDrawables.size()==1,"real composite-source ChunkManager did not create its exact producer quad");
        sampledDrawable=map->mDrawables[0];pass=sampledDrawable->getStateSet();
    }
    const auto purpose=compositeSource ? N::TexturePurpose::CompositeSource : N::TexturePurpose::MainDraw;
    auto* texture=dynamic_cast<osg::Texture2D*>(pass->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
    require(texture && texture->getImage()==original,"pending cached producer changed its live semantic image");
    require(textures.getTexture(VFS::Path::Normalized("textures/handoff.dds"),N::TextureRole::Diffuse,purpose).get()==texture,"terrain TextureManager did not share the exact cached producer");
    auto* native=dynamic_cast<T::Texture*>(texture);require((mask!=0)==bool(native),"candidate/control producer type incorrect");
    pass->setAttributeAndModes(sampledColorProgram());pass->setMode(GL_LIGHTING,osg::StateAttribute::OFF);
    osg::ref_ptr<Receipt> receipt=new Receipt;receipt->texture=texture;receipt->candidate=mask!=0;receipt->negative=negative;receipt->streaming=(mask&8)!=0 && !compositeSource;
    receipt->originalDrawable=sampledDrawable;
    if(const auto* geometry=sampledDrawable->asGeometry())
    {
        const auto remember=[&](const osg::BufferData* data)
        { if(data) receipt->sourceBuffers.emplace_back(data,data->getBufferObject()); };
        remember(geometry->getVertexArray());remember(geometry->getNormalArray());remember(geometry->getColorArray());
        for(const auto& array:geometry->getTexCoordArrayList()) remember(array);
        for(const auto& primitive:geometry->getPrimitiveSetList())
            remember(dynamic_cast<const osg::DrawElements*>(primitive.get()));
    }
    sampledDrawable->setDrawCallback(receipt);
    osgViewer::Viewer viewer;viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);viewer.setLightingMode(osgViewer::View::NO_LIGHT);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY();traits->setUndefinedScreenDetailsToDefaultScreen();traits->width=32;traits->height=32;traits->doubleBuffer=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);require(context.valid(),"production fixture GL context unavailable");
    auto* camera=viewer.getCamera();camera->setGraphicsContext(context);camera->setViewport(0,0,32,32);
    camera->setProjectionMatrix(osg::Matrix::identity());camera->setViewMatrix(osg::Matrix::identity());camera->setCullingMode(osg::CullSettings::NO_CULLING);
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    osg::ref_ptr<osg::Node> drawNode=chunk;
    if(compositeSource)
    {
        osg::ref_ptr<osg::Geode> sourceQuad=new osg::Geode;sourceQuad->addDrawable(sampledDrawable);drawNode=sourceQuad;
    }
    osg::ref_ptr<osg::Group> root=new osg::Group;root->addChild(drawNode);
    osg::ref_ptr<osg::Camera> refraction=new osg::Camera;refraction->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    refraction->setProjectionMatrix(osg::Matrix::identity());refraction->setViewMatrix(osg::Matrix::identity());refraction->setViewport(0,0,32,32);
    refraction->setRenderOrder(osg::Camera::PRE_RENDER);refraction->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    osg::ref_ptr<osg::Texture2D> target=new osg::Texture2D;target->setTextureSize(32,32);target->setInternalFormat(GL_RGBA);
    refraction->attach(osg::Camera::COLOR_BUFFER0,target);refraction->addChild(drawNode);root->addChild(refraction);
    viewer.setSceneData(root);require(SceneUtil::TextureOwnerBoundary::installBeforeRealize(viewer)==(mask ? 2u : 0u),"owner installer lane mismatch");viewer.realize();
    require(context->makeCurrent(),"production fixture context was not current for ICO");
    if(cancelled) textures.clearCache();else compile(texture,context);
    if(mask && !cancelled) require(!texture->getTextureObject(context->getState()->getContextID()),"optional ICO ordinary-allocated pending cached terrain producer");
    if(!negative) source->hold=false;
    if(mask && !negative)
    {
        until([&]{return N::nativeTextureStats().ready.load()>0;},"delayed native worker did not publish completion");
        require(textures.getTexture(VFS::Path::Normalized("textures/handoff.dds"),N::TextureRole::Diffuse,purpose).get()==texture && texture->getImage()==original,
            "ready cache lookup replaced live producer/image rather than owner handoff");
    }
    if(threaded)
    {
        context->releaseContext();viewer.setThreadingModel(osgViewer::Viewer::DrawThreadPerContext);viewer.startThreading();
        require(viewer.areThreadsRunning(),"real production DrawThreadPerContext did not start");
    }
    for(unsigned i=0;i<3;++i) { healthy(ledger);viewer.frame(); }
    until([&]{return receipt->draws.load()>=6;},"production draw thread did not submit both frozen subviews");
    viewer.stopThreading();
    require(receipt->draws==6,"real terrain main/refraction fixture did not submit six draws");
    if(mask && !negative)
    {
        require(N::nativeTextureStats().nativeImages>0 && N::nativeTextureStats().handoffs>0,"late-ready producer did not construct owner native payload view");
        if((mask&8) && !compositeSource) require(T::counters().coarsePublished==1 && T::counters().coarseDraws>=6,"T2 delayed producer never consumed coarse backing");
        else require(N::nativeTextureStats().fullPublished==1
            && (compositeSource ? N::nativeTextureStats().consumedCompositeDraws.load() : N::nativeTextureStats().consumedMainDraws.load())>=6,
            "T1 delayed producer never consumed its exact full-quality purpose backing");
    }
    else
    {
        source->hold=false;const auto allocations=T::counters().allocations.load()+N::nativeTextureStats().fullAllocations.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));healthy(ledger);viewer.frame();
        require(T::counters().allocations.load()+N::nativeTextureStats().fullAllocations.load()==allocations,
            "late worker readiness replaced an authoritative ordinary allocation");
        require(N::nativeTextureStats().consumedMainDraws==0 && T::counters().coarseDraws==0,
            "ordinary control/cold fallback manufactured candidate consumption");
    }
    viewer.stopThreading();require(context->makeCurrent(),"owner teardown context unavailable");const auto nativeName=receipt->name;
    T::releaseContext(*context->getState());
    if(mask && !negative) require(!glIsTexture(nativeName),"actual candidate GPU name remained in OSG recycling after teardown");
    require(N::nativeTextureStats().fullLiveBytes==0 && T::counters().liveBytes==0 && N::AssetGpuBudget::liveBytes==0,"owner teardown retained GPU backing debt");
    viewer.setDone(true);viewer.setSceneData(nullptr);context->releaseContext();context->close(true);
    std::cout<<"PASS: real ChunkManager preparePassData->realizePasses/TextureManager cached delayed ticket, owner-native handoff, real ICO and frozen main/refraction terrain draws; mask="<<mask<<" cold_visible="<<negative<<" threaded="<<threaded<<" composite_source="<<compositeSource<<" cancelled="<<cancelled<<'\n';
    return 0;
}
catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
