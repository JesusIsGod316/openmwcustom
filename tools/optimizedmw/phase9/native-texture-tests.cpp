#include <components/resource/nativetexture.hpp>
#include <components/resource/texturestreaming.hpp>
#include <components/resource/assetgpubudget.hpp>
#include <components/resource/derivedassetcache.hpp>
#include <components/resource/uploadpool.hpp>
#include <components/resource/openmwcompileoperation.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <components/terrain/terraindrawable.hpp>
#include <components/terrain/nativetextureproducer.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/textureownerboundary.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <osg/FrameStamp>
#include <osg/GraphicsContext>
#include <osg/Geode>
#include <osg/Group>
#include <osg/RenderInfo>
#include <osg/State>
#include <osgDB/Registry>
#include <osgDB/Options>
#include <osgViewer/Viewer>
#include <osgViewer/Renderer>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace N=Resource;
namespace T=Resource::TextureStreaming;
// This fixture submits the real terrain geometry draw implementation, without
// a dynamic-light cull callback. Reaching that unrelated dependency is a hard
// fixture failure; it is never a success-producing light behavior substitute.
bool SceneUtil::LightListCallback::pushLightState(osg::Node*,osgUtil::CullVisitor*)
{ throw std::runtime_error("native texture fixture unexpectedly invoked dynamic-light cull"); }
namespace
{
    using Bytes=std::vector<std::uint8_t>;
    void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
    void put(Bytes& bytes,unsigned p,unsigned value)
    { for(unsigned i=0;i<4;++i) bytes[p+i]=static_cast<std::uint8_t>(value>>(i*8)); }
    Bytes dds(unsigned width,unsigned height,unsigned fourcc=0x31545844)
    {
        Bytes result(128,0); put(result,0,0x20534444); put(result,4,124);
        put(result,8,0x21007); put(result,12,height); put(result,16,width);
        put(result,76,32); put(result,80,4); put(result,84,fourcc); put(result,108,0x401008);
        unsigned level=0;
        constexpr unsigned colors[]={0xf800,0x07e0,0x001f,0xffe0,0xf81f,0x07ff,0xffff};
        for(;;)
        {
            const auto block=fourcc==0x31545844 ? 8u : 16u;
            const unsigned count=((width+3)/4)*((height+3)/4),offset=static_cast<unsigned>(result.size());
            result.resize(offset+count*block,0);
            for(unsigned b=0;b<count;++b)
            {
                auto* target=result.data()+offset+b*block;
                if(block==16)
                {
                    if(fourcc==0x33545844) std::fill(target,target+8,0x88);
                    else { target[0]=137; target[1]=0; }
                    target+=8;
                }
                const auto color=colors[level%std::size(colors)];
                target[0]=static_cast<std::uint8_t>(color); target[1]=static_cast<std::uint8_t>(color>>8);
            }
            ++level;
            if(width==1 && height==1) break;
            width=(std::max)(1u,width/2);height=(std::max)(1u,height/2);
        }
        put(result,28,level); put(result,20,static_cast<unsigned>(result.size()-128)); return result;
    }
    std::shared_ptr<N::NativeMipPayload> prepare(const Bytes& bytes,N::TextureRole role=N::TextureRole::Diffuse,
        N::TexturePurpose purpose=N::TexturePurpose::MainDraw)
    {
        N::NativeTextureSkip skip=N::NativeTextureSkip::None;
        auto result=N::prepareNativeTexture(bytes,"textures/native-fixture.dds",role,purpose,skip);
        require(result && skip==N::NativeTextureSkip::None,"complete fixture DDS preparation failed"); return result;
    }
    osg::ref_ptr<osg::Image> ordinaryDds(const Bytes& bytes)
    {
        auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension("dds");
        require(reader,"installed DDS reader unavailable: backend is not qualified");
        std::istringstream input(std::string(bytes.begin(),bytes.end()),std::ios::binary);
        osg::ref_ptr<osgDB::Options> options=new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields");
        auto result=reader->readImage(input,options);
        require(result.success() && result.getImage(),"ordinary DDS fixture decode failed"); return result.getImage();
    }
    osg::ref_ptr<osg::Image> ordinaryTga(const Bytes& bytes)
    {
        auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension("tga");
        require(reader,"installed TGA reader unavailable: conversion backend is not qualified");
        std::istringstream input(std::string(bytes.begin(),bytes.end()),std::ios::binary);
        osg::ref_ptr<osgDB::Options> options=new osgDB::Options("ignoreTga2Fields");
        auto result=reader->readImage(input,options);
        require(result.success() && result.getImage(),"ordinary TGA fixture decode failed");
        osg::ref_ptr<osg::Image> image=result.getImage();
        if(image->getOrigin()==osg::Image::BOTTOM_LEFT) image->flipVertical();
        image->setOrigin(osg::Image::TOP_LEFT);return image;
    }
    class MemoryFile final : public VFS::File
    {
    public:
        explicit MemoryFile(Bytes b):bytes(std::move(b)) {}
        Files::IStreamPtr open() override
        {
            ++opened;enteredOpen=true;
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
            while(holdOpen)
            {
                if(std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("fixture source gate was not released");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return std::make_unique<std::istringstream>(std::string(bytes.begin(),bytes.end()),std::ios::binary);
        }
        std::optional<VFS::SourceOpenEstimate> getOpenEstimate() const override
        { return bounded ? std::optional(VFS::SourceOpenEstimate{bytes.size(),bytes.size()*2+1024*1024}) : std::nullopt; }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "native-fixture"; }
        Bytes bytes;
        bool bounded=true;
        std::atomic_uint64_t opened{0};
        std::atomic_bool holdOpen{false},enteredOpen{false};
    };
    class MemoryArchive final : public VFS::Archive
    {
    public:
        explicit MemoryArchive(Bytes bytes):file(std::move(bytes)) {}
        void listResources(VFS::FileMap& out) override
        {
            out[VFS::Path::Normalized("textures/native-fixture.dds")]=&file;
            out[VFS::Path::Normalized("textures/second.dds")]=&second;
            unknown.bounded=false;out[VFS::Path::Normalized("textures/unknown-compressed.dds")]=&unknown;
        }
        bool contains(VFS::Path::NormalizedView path) const override
        { return path.value()=="textures/native-fixture.dds" || path.value()=="textures/second.dds" || path.value()=="textures/unknown-compressed.dds"; }
        std::string getDescription() const override { return "native texture fixture"; }
        MemoryFile file;
        MemoryFile second{dds(32,32)},unknown{dds(64,64)};
    };
    N::OpenGlPressureSample sample;
    N::OpenGlPressureSample readSample(void*) { return sample; }
    void healthyHost(N::SpeculativeBudget& ledger)
    {
        ++sample.generation; sample.decision.state=N::OpenGlPressure::Normal;
        sample.growthWatermarkValid=true; sample.accountedGrowth=ledger.watermark()->load();
        sample.decision.admissionsPerSample=4;
        sample.memory.physicalValid=true;sample.memory.physicalTotal=32ull*1024*1024*1024;
        sample.memory.physicalAvailable=16ull*1024*1024*1024;
        sample.memory.commitValid=true;sample.memory.commitAvailable=16ull*1024*1024*1024;
        sample.decision.limits.physicalReserve=4ull*1024*1024*1024;
        sample.decision.limits.commitReserve=1024ull*1024*1024;
    }
    void healthyGpu()
    {
        std::lock_guard lock(Debug::V3GpuMemory::sAdmissionMutex);
        Debug::V3GpuMemory::sAdmissionSample={2ull*1024*1024*1024,N::AssetGpuBudget::milliseconds(),
            Debug::V3GpuMemory::PressureState::Comfortable,true};
    }
    std::shared_ptr<N::NativeTextureTicket> readyTicket(std::shared_ptr<const N::NativeMipPayload> payload)
    {
        auto epoch=std::make_shared<N::NativeTextureEpoch>();
        auto ticket=std::make_shared<N::NativeTextureTicket>(epoch,1,1,payload->path,payload->role,payload->purpose);
        ticket->publish(std::move(payload)); return ticket;
    }
    GLuint binding()
    { GLint result=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&result);return static_cast<GLuint>(result); }
    void compileFromProductionIco(osg::Texture* texture,osg::GraphicsContext* context,bool forceDownload=false)
    {
        osg::ref_ptr<N::OpenMWIncrementalCompileOperation> ico=
            new N::OpenMWIncrementalCompileOperation(N::OpenMWCompileSchedulerConfig{});
        // Engine/SceneManager never assign force-download geometry. Preserve
        // that exact constructor configuration and execute the actual OSG op,
        // rather than bypassing its possible post-compile forced dummy draw.
        if(forceDownload) ico->assignForceTextureDownloadGeometry();
        else require(!ico->getForceTextureDownloadGeometry(),"production ICO force-download configuration changed: requalify pending streaming deferral");
        osg::ref_ptr<osg::Group> graph=new osg::Group;
        graph->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);
        osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> set=
            new osgUtil::IncrementalCompileOperation::CompileSet(graph);
        osgUtil::IncrementalCompileOperation::ContextSet contexts;contexts.insert(context);
        set->buildCompileMap(contexts);require(set->_numberCompileListsToCompile==1,"production ICO fixture did not enqueue its real texture op");
        ico->add(set,false);ico->compileAllForNextFrame();(*ico)(context);
        require(set->compiled(),"production ICO did not consume its actual native texture CompileSet");
    }
    void verifyPixels(const N::NativeMipPayload& payload,unsigned first)
    {
        // glGetTexImage is a fixture-only synchronization/readback, never part
        // of the streaming production controller.
        glPixelStorei(GL_PACK_ALIGNMENT,1);
        for(unsigned level=first;level<payload.mips.size();++level)
        {
            const auto& mip=payload.mips[level]; GLint width=0,height=0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D,level-first,GL_TEXTURE_WIDTH,&width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,level-first,GL_TEXTURE_HEIGHT,&height);
            require(width==static_cast<int>(mip.width) && height==static_cast<int>(mip.height),"published chain has wrong dimensions");
            Bytes actual(std::size_t(width)*height*4);
            glGetTexImage(GL_TEXTURE_2D,level-first,GL_RGBA,GL_UNSIGNED_BYTE,actual.data());
            const auto offset=mip.offset+(payload.format==GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                || payload.format==GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? 8 : 0);
            const auto value=payload.bytes[offset]|(unsigned(payload.bytes[offset+1])<<8);
            const unsigned r=(value>>11)&31,g=(value>>5)&63,b=value&31;
            const unsigned red=(r<<3)|(r>>2),green=(g<<2)|(g>>4),blue=(b<<3)|(b>>2);
            const unsigned alpha=payload.format==GL_COMPRESSED_RGBA_S3TC_DXT3_EXT ? 136
                : payload.format==GL_COMPRESSED_RGBA_S3TC_DXT5_EXT ? 137 : 255;
            for(std::size_t p=0;p<actual.size();p+=4)
                if(actual[p]!=red || actual[p+1]!=green || actual[p+2]!=blue || actual[p+3]!=alpha)
                {
                    std::ostringstream message;
                    message<<"native compressed source color/data-alpha changed: format="<<payload.format
                        <<" source_mip="<<level<<" upload_mip="<<level-first<<" pixel="<<p/4
                        <<" expected="<<red<<','<<green<<','<<blue<<','<<alpha
                        <<" observed="<<unsigned(actual[p])<<','<<unsigned(actual[p+1])<<','
                        <<unsigned(actual[p+2])<<','<<unsigned(actual[p+3])<<" gl_error="<<glGetError();
                    throw std::runtime_error(message.str());
                }
        }
        require(glGetError()==GL_NO_ERROR,"pixel qualification produced GL errors");
    }
    osg::ref_ptr<Terrain::TerrainDrawable> triangle()
    {
        osg::ref_ptr<Terrain::TerrainDrawable> result=new Terrain::TerrainDrawable;
        osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array;
        vertices->push_back({-1,-1,0});vertices->push_back({1,-1,0});vertices->push_back({0,1,0});
        result->setVertexArray(vertices);result->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        result->setUseDisplayList(false);return result;
    }
    struct RendererDrawReceipt final : osg::Drawable::DrawCallback
    {
        T::Texture* texture=nullptr;
        const N::NativeMipPayload* payload=nullptr;
        std::uint64_t expectedBoundary=0,expectedAllocation=0,expectedBytes=0;
        mutable unsigned draws=0,mainDraws=0,refractionDraws=0;
        mutable GLuint firstName=0;
        void drawImplementation(osg::RenderInfo& render,const osg::Drawable* drawable) const override
        {
            const auto* camera=render.getCurrentCamera();
            require(camera,"real Renderer draw lost current camera identity");
            require(T::counters().boundaries==expectedBoundary,
                "real Renderer/scene-view draw preceded or repeated owner boundary");
            require(T::counters().allocations==expectedAllocation && T::counters().uploadedBytes==expectedBytes,
                "texture allocation/upload changed between main and refraction draws");
            require(!texture->getTextureObject(render.getState()->getContextID()),
                "cold worker-prepared producer fell back to ordinary full allocation before real Renderer draw");
            const auto name=binding();require(name,"real Renderer applied an empty native backing");
            if(firstName) require(firstName==name,"main/refraction Renderer draws saw different frozen backing");
            else firstName=name;
            GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
            require(width==static_cast<int>(payload->mips[payload->coarseLevel()].width),
                "real Renderer first use did not bind the complete coarse allocation");
            if(camera->getName()=="NativeMainCamera") ++mainDraws;
            else if(camera->getName()=="NativeRefractionCamera") ++refractionDraws;
            else throw std::runtime_error("unexpected camera submitted native queue fixture");
            ++draws;drawable->drawImplementation(render);
        }
    };
    template<class InstallOwnerHook>
    void verifyRendererOrder(N::SpeculativeBudget& ledger,InstallOwnerHook installOwnerHook)
    {
        // The same facade used by TextureManager.prepareTexture/getTexture is
        // selected while its real bounded VFS worker is pending. This catches
        // an ICO/realization path that would silently lock every cold chunk
        // into ordinary allocation before its ready owner boundary.
        VFS::Manager vfs;auto archive=std::make_unique<MemoryArchive>(dds(512,512));
        auto* source=archive.get();source->file.holdOpen=true;
        vfs.addArchive(std::move(archive));vfs.buildIndex();healthyHost(ledger);
        Terrain::NativeTextureProducer producer(&vfs);
        struct ReleaseGate { MemoryFile& source;~ReleaseGate() { source.holdOpen=false; } } release{source->file};
        producer.prepare(VFS::Path::Normalized("textures/native-fixture.dds"),N::TextureRole::Diffuse,N::TexturePurpose::MainDraw);
        auto selected=producer.select(VFS::Path::Normalized("textures/native-fixture.dds"),N::TextureRole::Diffuse,
            N::TexturePurpose::MainDraw,true);
        require(selected.ticket && !selected.payload && !selected.image,"pending production producer fixture was not pending");
        auto ordinary=ordinaryDds(source->file.bytes);
        osg::ref_ptr<osg::Texture2D> base=Terrain::NativeTextureProducer::realize(selected,ordinary,false);
        auto* texture=dynamic_cast<T::Texture*>(base.get());
        require(texture && texture->descriptor(),"pending terrain realization did not register the production streaming descriptor");
        {
            osg::ref_ptr<T::Texture> clone=new T::Texture(*texture);
            require(texture->compare(*clone)!=0,"OSG state sharing collapsed independent cloned producer/lifetime descriptors");
        }
        texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
        texture->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::REPEAT);
        osgViewer::Viewer viewer;viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
        osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
        traits->readDISPLAY();traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width=32;traits->height=32;traits->doubleBuffer=false;traits->windowDecoration=false;
        osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
        require(context.valid(),"real Renderer owner fixture could not create GL context");
        auto* camera=viewer.getCamera();camera->setName("NativeMainCamera");camera->setGraphicsContext(context);
        camera->setViewport(0,0,32,32);camera->setProjectionMatrix(osg::Matrix::identity());
        camera->setViewMatrix(osg::Matrix::identity());camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        camera->setCullingMode(osg::CullSettings::NO_CULLING);viewer.setLightingMode(osgViewer::View::NO_LIGHT);
        osg::ref_ptr<osg::Group> root=new osg::Group;
        auto drawable=triangle();osg::ref_ptr<RendererDrawReceipt> receipt=new RendererDrawReceipt;
        receipt->texture=texture;drawable->setDrawCallback(receipt);
        osg::ref_ptr<osg::Geode> geode=new osg::Geode;geode->addDrawable(drawable);
        osg::ref_ptr<osg::StateSet> pass=new osg::StateSet;
        pass->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);
        pass->setMode(GL_LIGHTING,osg::StateAttribute::OFF);drawable->setPasses({pass});root->addChild(geode);
        osg::ref_ptr<osg::Camera> refraction=new osg::Camera;
        refraction->setName("NativeRefractionCamera");refraction->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        refraction->setViewMatrix(osg::Matrix::identity());refraction->setProjectionMatrix(osg::Matrix::identity());
        refraction->setRenderOrder(osg::Camera::PRE_RENDER);refraction->setViewport(0,0,32,32);
        refraction->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        osg::ref_ptr<osg::Texture2D> target=new osg::Texture2D;target->setTextureSize(32,32);target->setInternalFormat(GL_RGBA);
        refraction->attach(osg::Camera::COLOR_BUFFER0,target);refraction->addChild(geode);root->addChild(refraction);
        viewer.setSceneData(root);installOwnerHook(viewer);viewer.realize();
        require(viewer.isRealized() && context->makeCurrent(),"real Renderer fixture failed to realize owner context");
        auto& state=*context->getState();compileFromProductionIco(texture,context);
        require(!texture->getTextureObject(state.getContextID()) && !selected.ticket->ordinaryAllocated,
            "real pending terrain descriptor ordinary-allocated during pre-boundary ICO");
        source->file.holdOpen=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!selected.ticket->payload())
        { require(std::chrono::steady_clock::now()<deadline,"pending terrain worker did not publish before real Renderer first use");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        const auto payload=selected.ticket->payload();receipt->payload=payload.get();
        for(unsigned frame=0;frame<3;++frame)
        {
            healthyHost(ledger);healthyGpu();T::setTestTime(400+frame);
            receipt->expectedBoundary=T::counters().boundaries.load()+1;
            receipt->expectedAllocation=T::counters().allocations.load()+(frame==0 ? 1 : 0);
            receipt->expectedBytes=T::counters().uploadedBytes.load()+(frame==0 ? payload->suffixBytes(payload->coarseLevel()) : 0);
            const auto draws=receipt->draws;viewer.frame();
            require(receipt->draws==draws+2,"real main/refraction Renderer fixture did not submit both terrain draws");
            require(T::counters().boundaries==receipt->expectedBoundary,"Renderer owner hook failed exactly one boundary per frame");
        }
        require(receipt->mainDraws==3 && receipt->refractionDraws==3 && !selected.ticket->ordinaryAllocated,
            "production pending worker source failed real Renderer cold engagement");
        viewer.stopThreading();require(context->makeCurrent(),"Renderer context could not become current for owner teardown");
        T::releaseContext(state);viewer.setDone(true);viewer.setSceneData(nullptr);
        context->releaseContext();context->close(true);
        require(T::counters().liveBytes==0,"Renderer owner teardown retained raw native allocation debt");
    }
}
int main(int argc,char** argv) try
{
    const bool control=argc>1 && std::string_view(argv[1])=="--control";
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK",control ? "0" : "28");
#else
    setenv("OPENMW_P9_ASSET_MASK",control ? "0" : "28",1);
#endif
    N::SpeculativeBudget::Config config;config.transientLimit=N::AssetPolicy::CpuTransientLimit;
    N::SpeculativeBudget ledger(config);N::UploadPool::ledger=&ledger;N::UploadPool::sample=&readSample;
    healthyHost(ledger);
    const auto bytes=dds(2048,2048);
    auto payload=prepare(bytes);
    const auto persistentBefore=N::nativeTextureStats().cacheHits.load();
    auto persistent=prepare(bytes);
    require(N::nativeTextureStats().cacheHits.load()>persistentBefore,"positive persistent native DDS cache hit absent");
    require(payload->bytes==persistent->bytes && payload->mips==persistent->mips,"immutable persisted DDS identity changed");
    auto ordinary=ordinaryDds(bytes);
    require(ordinary->getPixelFormat()==payload->format,"native BC1 alpha qualification differs from ordinary DDS reader");
    require(ordinary->getTotalSizeInBytesIncludingMipmaps()==payload->bytes.size()
        && std::memcmp(ordinary->data(),payload->bytes.data(),payload->bytes.size())==0,"native DDS blocks were altered");
    auto alphaBytes=dds(16,16);alphaBytes[128]=0;alphaBytes[129]=0;alphaBytes[130]=255;alphaBytes[131]=255;
    alphaBytes[132]=255;alphaBytes[133]=255;alphaBytes[134]=255;alphaBytes[135]=255;
    auto alphaPayload=prepare(alphaBytes,N::TextureRole::DiffuseSpecular);
    require(alphaPayload->format==ordinaryDds(alphaBytes)->getPixelFormat(),"BC1 data-alpha detection differs");
    auto malformed=bytes;put(malformed,28,1);
    N::NativeTextureSkip skip;
    require(!N::prepareNativeTexture(malformed,"textures/bad.dds",N::TextureRole::Diffuse,N::TexturePurpose::MainDraw,skip)
        && skip==N::NativeTextureSkip::IncompleteMips,"incomplete DDS chain was admitted");
    malformed=bytes;put(malformed,84,0x30315844);
    require(!N::prepareNativeTexture(malformed,"textures/bc7.dds",N::TextureRole::Diffuse,N::TexturePurpose::MainDraw,skip)
        && skip==N::NativeTextureSkip::Format,"unqualified DX10/BC7 entered pipeline");
    malformed=bytes;put(malformed,112,0x200);
    require(!N::prepareNativeTexture(malformed,"textures/cube.dds",N::TextureRole::Diffuse,N::TexturePurpose::MainDraw,skip),"cube entered 2D pipeline");
    Bytes tga(18+8*8*3,0);tga[2]=2;tga[12]=8;tga[14]=8;tga[16]=24;tga[17]=32;
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x)
    { const auto p=18+(y*8+x)*3;tga[p]=y<4 ? 0 : 255;tga[p+2]=y<4 ? 255 : 0; }
#ifdef OPENMW_HAVE_DIRECTXTEX
    auto converted=N::prepareNativeTexture(tga,"textures/opaque.tga",N::TextureRole::Diffuse,N::TexturePurpose::MainDraw,skip);
    require(converted && converted->converted && converted->mips.size()==4,"qualified CPU opaque TGA conversion did not engage");
    const auto hits=N::nativeTextureStats().cacheHits.load();
    require(N::prepareNativeTexture(tga,"textures/opaque.tga",N::TextureRole::Diffuse,N::TexturePurpose::MainDraw,skip)
        && N::nativeTextureStats().cacheHits.load()>hits,"CPU conversion artifact did not persist");
#endif
    require(!N::prepareNativeTexture(tga,"textures/opaque.tga",N::TextureRole::Diffuse,N::TexturePurpose::CompositeSource,skip)
        && skip==N::NativeTextureSkip::Purpose,"composite-source TGA entered lossy MainDraw conversion/cache path");
    {
        Terrain::NativeTextureProducer::Selection exact;exact.supported=true;
        exact.ticket=std::make_shared<N::NativeTextureTicket>(std::make_shared<N::NativeTextureEpoch>(),1,1,
            "textures/opaque.tga",N::TextureRole::Diffuse,N::TexturePurpose::CompositeSource);
        const auto source=ordinaryTga(tga);
        const auto texture=Terrain::NativeTextureProducer::realize(exact,source,true);
        require(texture->getImage()==source.get() && !dynamic_cast<T::Texture*>(texture.get()),
            "composite-source TGA realization replaced exact ordinary source colors or entered streaming");
    }
    require(!N::prepareNativeTexture(tga,"textures/normal.tga",N::TextureRole::NormalHeight,N::TexturePurpose::MainDraw,skip)
        && skip==N::NativeTextureSkip::Role,"normal/height alpha was converted as color");
    require(!N::prepareNativeTexture(tga,"textures/specular.tga",N::TextureRole::DiffuseSpecular,N::TexturePurpose::MainDraw,skip),"specular data alpha was converted");
    VFS::Manager vfs;vfs.addArchive(std::make_unique<MemoryArchive>(dds(8,8)));
    auto winning=std::make_unique<MemoryArchive>(dds(16,16));auto* provider=winning.get();
    vfs.addArchive(std::move(winning));vfs.buildIndex();
    {
        N::NativeTexturePreparation producer(&vfs);
        auto ticket=producer.request(VFS::Path::Normalized("textures/native-fixture.dds"),N::TextureRole::Diffuse,N::TexturePurpose::MainDraw);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!ticket->payload())
        { require(std::chrono::steady_clock::now()<deadline,"production VFS worker did not publish known ready native source"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(ticket->payload()->mips.front().width==16,"preparation did not snapshot winning VFS source");
        // A distinct role/purpose creates a separate retained immutable owner.
        // Its source-specific peak must admit under the real aggregate cap
        // while the first result still holds its host/native charge.
        healthyHost(ledger);
        auto second=producer.request(VFS::Path::Normalized("textures/second.dds"),
            N::TextureRole::DiffuseSpecular,N::TexturePurpose::CompositeSource);
        while(!second->payload())
        { require(std::chrono::steady_clock::now()<deadline,"second distinct retained producer payload was denied under 256 MiB aggregate ledger"); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(second->payload().get()!=ticket->payload().get()
            && second->payload()->recipeKey!=ticket->payload()->recipeKey
            && second->payload()->sourceDigest!=ticket->payload()->sourceDigest,"distinct source/role/purpose preparation owners were aliased");
        const auto refused=N::nativeTextureStats().skips[static_cast<unsigned>(N::NativeTextureSkip::SourceProvider)].load();
        auto unknown=producer.request(VFS::Path::Normalized("textures/unknown-compressed.dds"),
            N::TextureRole::Diffuse,N::TexturePurpose::MainDraw);
        while(N::nativeTextureStats().skips[static_cast<unsigned>(N::NativeTextureSkip::SourceProvider)].load()==refused)
        { require(std::chrono::steady_clock::now()<deadline,"unknown/compressed provider admission was not refused");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(!unknown->payload() && provider->unknown.opened==0,"unknown/compressed provider materialized before bounded admission");
        producer.clear();require(!ticket->current() && !ticket->payload(),"invalidated epoch retained usable preparation proof");
    }
    {
        const auto directory=N::DerivedAssets::cache().config().directory/
            ("native-provider-fixture-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory),"filesystem provider fixture directory could not be created");
        struct Cleanup
        {
            std::filesystem::path directory;
            ~Cleanup() { std::error_code ec;std::filesystem::remove(directory/"known.dds",ec);std::filesystem::remove(directory,ec); }
        } cleanup{directory};
        const auto fileBytes=dds(16,16);
        { std::ofstream output(directory/"known.dds",std::ios::binary);output.write(reinterpret_cast<const char*>(fileBytes.data()),fileBytes.size());require(bool(output),"filesystem fixture source write failed"); }
        VFS::Manager loose;loose.addArchive(std::make_unique<VFS::FileSystemArchive>(directory));loose.buildIndex();
        const auto estimate=loose.getOpenEstimate(VFS::Path::Normalized("known.dds"));
        require(estimate && estimate->sourceBytes==fileBytes.size() && estimate->openPeakBytes<=1024*1024,
            "known filesystem provider did not advertise bounded pre-open size/peak");
        healthyHost(ledger);N::NativeTexturePreparation producer(&loose);
        auto ticket=producer.request(VFS::Path::Normalized("known.dds"),N::TextureRole::Diffuse,N::TexturePurpose::MainDraw);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(!ticket->payload())
        { require(std::chrono::steady_clock::now()<deadline,"actual filesystem provider did not publish through bounded production worker");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        require(ticket->payload()->sourceDigest==N::DerivedAssets::digest(fileBytes),"metadata replaced actual filesystem byte digest identity");
    }
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY();traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=32;traits->height=32;traits->doubleBuffer=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(),"real OpenGL owner context unavailable");
    auto& state=*context->getState();state.setActiveTextureUnit(0);
    osg::ref_ptr<osg::FrameStamp> stamp=new osg::FrameStamp;state.setFrameStamp(stamp);
    osg::ref_ptr<T::Texture> texture=new T::Texture(N::nativeTextureImage(payload),readyTicket(payload));
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
    texture->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::REPEAT);
    auto draw=triangle();osg::RenderInfo render(&state,nullptr);
    const auto boundary=[&](unsigned frame,double time)
    {
        healthyHost(ledger);healthyGpu();stamp->setFrameNumber(frame);T::setTestTime(time);
        const auto uploaded=T::counters().uploadedBytes.load(),allocated=T::counters().allocations.load();
        T::frameBoundary(state);
        require(T::counters().uploadedBytes.load()-uploaded<=1024*1024,"owner boundary exceeded 1 MiB upload byte cap");
        require(T::counters().allocations.load()-allocated<=1,"owner boundary allocated multiple replacement textures");
    };
    compileFromProductionIco(texture,context);
    if(!control)
        require(!texture->getTextureObject(state.getContextID()) && T::counters().compileDeferred==1,
            "ICO precompile-before-first-boundary allocated ordinary full backing");
    boundary(1,100);
    state.applyTextureAttribute(0,texture);const auto coarse=binding();
    if(control)
    {
        require(T::counters().allocations==0 && texture->getTextureObject(state.getContextID()),"control altered original OSG allocation path");
        verifyPixels(*payload,0);T::releaseContext(state);context->releaseContext();context->close(true);
        std::cout<<"native texture CONTROL: ordinary full mip allocation preserved\n";return 0;
    }
    require(coarse && T::counters().coarsePublished==1 && !texture->getTextureObject(state.getContextID()),"cold-ready production descriptor did not publish coarse allocation before first draw");
    require(T::counters().firstCoarseDraws==0,"apply/compile was incorrectly counted as real first draw");
    verifyPixels(*payload,payload->coarseLevel());draw->drawImplementation(render);
    require(T::counters().firstCoarseDraws==1,"actual TerrainDrawable first coarse draw was not recorded");
    state.applyTextureAttribute(3,texture);
    require(binding()==coarse,"producer did not bind its initial coarse backing above unit zero");state.setActiveTextureUnit(0);
    boundary(2,109);state.applyTextureAttribute(0,texture);draw->drawImplementation(render);
    require(binding()==coarse,"quality hold allowed early promotion");
    boundary(3,110.5);state.applyTextureAttribute(0,texture);draw->drawImplementation(render);
    require(binding()==coarse,"stale draw demand triggered speculative fine promotion");
    boundary(4,111);state.applyTextureAttribute(0,texture);draw->drawImplementation(render);
    require(binding()==coarse,"incomplete fine allocation appeared in a draw");
    const auto allocations=T::counters().allocations.load(),bytesAfter=T::counters().uploadedBytes.load();
    T::frameBoundary(state);state.applyTextureAttribute(0,texture);
    require(T::counters().allocations==allocations && T::counters().uploadedBytes==bytesAfter && binding()==coarse,
        "second same-frame/refraction boundary changed frozen allocation epoch");
    boundary(5,112);state.applyTextureAttribute(0,texture);draw->drawImplementation(render);
    require(binding()==coarse,"partially uploaded fine chain was published");
    boundary(6,113);state.applyTextureAttribute(0,texture);const auto fine=binding();draw->drawImplementation(render);
    require(fine && fine!=coarse && T::counters().finePublished==1 && T::counters().firstFineDraws==1,
        "complete fine promotion and actual fine draw did not engage");
    state.applyTextureAttribute(3,texture);
    require(binding()==fine,"fine publication left a cached coarse raw name on a nonzero texture unit");state.setActiveTextureUnit(0);
    verifyPixels(*payload,0);
    require(T::counters().liveBytes>=payload->bytes.size()+payload->suffixBytes(payload->coarseLevel()),"old/new overlap was not charged");
    glFinish();boundary(7,114);state.applyTextureAttribute(0,texture);
    require(!glIsTexture(coarse) && T::counters().deletions>=1,"retired coarse object remained in OSG pool/recycling instead of targeted deletion");
    boundary(8,124);state.applyTextureAttribute(0,texture);const auto demoted=binding();
    require(demoted && demoted!=fine && T::counters().demotions==1,"cold fine texture did not physically demote to a separate small allocation");
    state.applyTextureAttribute(3,texture);
    require(binding()==demoted,"demotion left a cached full raw name on a nonzero texture unit");state.setActiveTextureUnit(0);
    verifyPixels(*payload,payload->coarseLevel());glFinish();boundary(9,125);
    require(!glIsTexture(fine) && T::counters().liveBytes==payload->suffixBytes(payload->coarseLevel()),
        "demotion did not delete full backing and release its exact allocation debt");
    auto compositePayload=prepare(dds(512,512),N::TextureRole::DiffuseSpecular,N::TexturePurpose::CompositeSource);
    osg::ref_ptr<T::Texture> composite=new T::Texture(N::nativeTextureImage(compositePayload),readyTicket(compositePayload));
    boundary(10,126);state.applyTextureAttribute(0,composite);
    require(!composite->descriptor() && composite->getTextureObject(state.getContextID()),"composite-source lane acquired streamed/coarse allocation");
    verifyPixels(*compositePayload,0);
    {
        auto a=prepare(dds(512,512));a->path="textures/prefetch-a.dds";
        auto b=prepare(dds(512,512));b->path="textures/prefetch-b.dds";
        osg::ref_ptr<T::Texture> prefetchedA=new T::Texture(N::nativeTextureImage(a),readyTicket(a));
        osg::ref_ptr<T::Texture> prefetchedB=new T::Texture(N::nativeTextureImage(b),readyTicket(b));
        prefetchedA->compileGLObjects(state);prefetchedB->compileGLObjects(state);
        require(!prefetchedA->getTextureObject(state.getContextID()) && !prefetchedB->getTextureObject(state.getContextID()),
            "multiple prefetched resources allocated hidden full objects in ICO");
        boundary(11,127);boundary(12,128);
        state.applyTextureAttribute(0,prefetchedA);verifyPixels(*a,a->coarseLevel());draw->drawImplementation(render);
        require(!prefetchedA->getTextureObject(state.getContextID()),"first prefetched descriptor failed production owner ingestion");
        state.applyTextureAttribute(0,prefetchedB);verifyPixels(*b,b->coarseLevel());draw->drawImplementation(render);
        require(!prefetchedB->getTextureObject(state.getContextID()),"second prefetched descriptor failed bounded next-boundary allocation");
        auto epoch=std::make_shared<N::NativeTextureEpoch>();
        auto notReady=std::make_shared<N::NativeTextureTicket>(epoch,1,1,"textures/not-ready.dds",
            N::TextureRole::Diffuse,N::TexturePurpose::MainDraw);
        osg::ref_ptr<T::Texture> visibleFallback=new T::Texture(N::nativeTextureImage(a),notReady);
        visibleFallback->compileGLObjects(state);boundary(13,129);state.applyTextureAttribute(0,visibleFallback);
        require(visibleFallback->getTextureObject(state.getContextID()) && notReady->ordinaryAllocated
            && T::counters().coldVisibleFallbacks>=1,"cold visible not-ready texture lost ordinary correctness fallback");
        osg::ref_ptr<T::Texture> forced=new T::Texture(N::nativeTextureImage(a),readyTicket(a));
        const auto compileFallbacks=T::counters().compileFallbacks.load(),visibleFallbacks=T::counters().coldVisibleFallbacks.load();
        compileFromProductionIco(forced,context,true);
        require(forced->getTextureObject(state.getContextID()) && T::counters().compileFallbacks==compileFallbacks+1
            && T::counters().coldVisibleFallbacks==visibleFallbacks,
            "force-download ICO effects were skipped or mislabeled as actual visible first-use fallback");
        verifyPixels(*a,0);
        osg::ref_ptr<T::Texture> unknown=new T::Texture(N::nativeTextureImage(a),readyTicket(a));
        unknown->compileGLObjects(state);stamp->setFrameNumber(14);T::setTestTime(130);
        { std::lock_guard lock(Debug::V3GpuMemory::sAdmissionMutex);Debug::V3GpuMemory::sAdmissionSample={}; }
        const auto before=T::counters().allocations.load();T::frameBoundary(state);state.applyTextureAttribute(0,unknown);
        require(T::counters().allocations==before && unknown->getTextureObject(state.getContextID()),
            "unknown GPU pressure admitted new optional native allocation");
        notReady->publish(a);boundary(15,131);
        require(T::counters().allocations==before,"ordinary cold fallback later created a hidden duplicate stream allocation");
    }
    {
        // A source larger than the full-allocation cap must take the original
        // path before publishing even its cheap coarse suffix.
        auto large=prepare(dds(8192,8192));large->path="textures/full-cap.dds";
        osg::ref_ptr<T::Texture> fullCap=new T::Texture(N::nativeTextureImage(large),readyTicket(large));
        fullCap->compileGLObjects(state);const auto before=T::counters().allocations.load();
        boundary(16,132);state.applyTextureAttribute(0,fullCap);
        require(T::counters().allocations==before && fullCap->getTextureObject(state.getContextID()),
            "oversized full source entered permanent coarse-only streaming");
        GLint width=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
        require(width==8192,"full cap refusal changed original full source quality");
    }
    {
        auto data=prepare(dds(512,512));data->path="textures/upload-fault.dds";
        osg::ref_ptr<T::Texture> fault=new T::Texture(N::nativeTextureImage(data),readyTicket(data));
        fault->compileGLObjects(state);const auto retired=T::counters().retirements.load();
        T::failNextUpload();boundary(17,133);state.applyTextureAttribute(0,fault);
        require(fault->getTextureObject(state.getContextID()) && T::counters().retirements>retired,
            "failed private construction did not preserve full-quality ordinary fallback and retirement debt");
        glFinish();const auto deleted=T::counters().deletions.load();
        auto recovery=prepare(dds(512,512));recovery->path="textures/after-fault.dds";
        osg::ref_ptr<T::Texture> next=new T::Texture(N::nativeTextureImage(recovery),readyTicket(recovery));
        next->compileGLObjects(state);boundary(18,134);state.applyTextureAttribute(0,next);
        require(!next->getTextureObject(state.getContextID()) && T::counters().deletions>deleted,
            "failed construction blocked future admission or skipped targeted backing deletion");
        verifyPixels(*recovery,recovery->coarseLevel());
    }
    {
        glFinish();auto data=prepare(dds(2048,2048));data->path="textures/ordinary-during-private.dds";
        auto ticket=readyTicket(data);
        osg::ref_ptr<T::Texture> superseded=new T::Texture(N::nativeTextureImage(data),ticket);
        superseded->compileGLObjects(state);boundary(20,140);state.applyTextureAttribute(0,superseded);
        verifyPixels(*data,data->coarseLevel());draw->drawImplementation(render);
        boundary(21,150.5);state.applyTextureAttribute(0,superseded);draw->drawImplementation(render);
        const auto live=T::counters().liveBytes.load();boundary(22,151);
        require(T::counters().liveBytes>=live+data->bytes.size(),"ordinary supersession fixture did not construct private fine backing");
        // A shared producer may already have taken ordinary fallback in another
        // context. The current context must retire its private candidate debt
        // and restore full quality rather than remaining coarse indefinitely.
        ticket->ordinaryAllocated=true;const auto retired=T::counters().retirements.load();
        boundary(23,152);state.applyTextureAttribute(0,superseded);
        require(superseded->getTextureObject(state.getContextID()) && T::counters().retirements>=retired+2,
            "ordinary fallback failed to retire both active coarse and private fine backing");
        glFinish();const auto deleted=T::counters().deletedBytes.load();
        auto recovery=prepare(dds(512,512));recovery->path="textures/after-ordinary.dds";
        osg::ref_ptr<T::Texture> next=new T::Texture(N::nativeTextureImage(recovery),readyTicket(recovery));
        next->compileGLObjects(state);boundary(24,153);state.applyTextureAttribute(0,next);
        require(!next->getTextureObject(state.getContextID())
            && T::counters().deletedBytes.load()>=deleted+data->bytes.size()+data->suffixBytes(data->coarseLevel()),
            "ordinary supersession skipped deletion debt or left global construction permanently blocked");
        verifyPixels(*recovery,recovery->coarseLevel());
    }
    for(const auto format:{0x33545844u,0x35545844u})
    {
        auto data=prepare(dds(16,16,format),N::TextureRole::DiffuseSpecular);
        osg::ref_ptr<osg::Texture2D> alpha=new osg::Texture2D(N::nativeTextureImage(data));
        alpha->setResizeNonPowerOfTwoHint(false);alpha->setUseHardwareMipMapGeneration(false);
        // These direct fixture attributes have no StateSet/RenderLeaf owner
        // after each loop iteration. Clear State's raw last-applied pointer:
        // allocator reuse must not make the next distinct alpha texture look
        // like the same already-applied OSG attribute.
        state.haveAppliedTextureAttribute(0,osg::StateAttribute::TEXTURE);
        state.applyTextureAttribute(0,alpha);verifyPixels(*data,0);
    }
#ifdef OPENMW_HAVE_DIRECTXTEX
    {
        auto source=ordinaryTga(tga);
        osg::ref_ptr<osg::Texture2D> color=new osg::Texture2D(N::nativeTextureImage(converted));
        color->setResizeNonPowerOfTwoHint(false);color->setUseHardwareMipMapGeneration(false);
        state.haveAppliedTextureAttribute(0,osg::StateAttribute::TEXTURE);
        state.applyTextureAttribute(0,color);
        GLint width=0,height=0;glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&height);
        require(width==8 && height==8,"converted artifact did not apply its exact source dimensions");
        Bytes pixels(8*8*4);
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x)
        {
            const auto original=source->getColor(x,y);const auto p=(y*8+x)*4;
            for(unsigned channel=0;channel<3;++channel)
                require(std::abs(int(pixels[p+channel])-int(original[channel]*255.0f+0.5f))<=8,
                    "CPU color conversion changed orientation/transfer or exceeded qualified BC1 tolerance");
            require(pixels[p+3]==255,"opaque conversion manufactured alpha data");
        }
        require(glGetError()==GL_NO_ERROR,"converted artifact upload failed");
    }
#endif
    {
        // Fill the actual four retirement slots while the test hook emulates
        // outstanding GPU fences. Binding correctness cannot depend on having
        // a spare debt slot, and fallback cannot discharge still-owned storage.
        glFinish();boundary(25,154);glFinish();boundary(26,155);
        const auto baseline=T::counters().liveBytes.load();
        auto data=prepare(dds(512,512));data->path="textures/saturated-retirement.dds";
        const auto coarseBytes=data->suffixBytes(data->coarseLevel());
        osg::ref_ptr<T::Texture> stale=new T::Texture(N::nativeTextureImage(data),readyTicket(data));
        boundary(27,156);state.applyTextureAttribute(0,stale);const auto staleName=binding();
        auto ordinaryTicket=readyTicket(data);
        osg::ref_ptr<T::Texture> ordinaryOwner=new T::Texture(N::nativeTextureImage(data),ordinaryTicket);
        boundary(28,157);state.applyTextureAttribute(0,ordinaryOwner);const auto ordinaryName=binding();
        require(staleName && ordinaryName && staleName!=ordinaryName && !stale->getTextureObject(state.getContextID())
            && !ordinaryOwner->getTextureObject(state.getContextID()),"saturated-debt fixture failed initial raw owners");
        T::holdTestRetirements(true);
        struct ReleaseRetirements { ~ReleaseRetirements() { T::holdTestRetirements(false); } } release;
        std::array<osg::ref_ptr<T::Texture>,4> fillers;
        std::array<GLuint,4> names{};const auto retired=T::counters().retirements.load();
        for(unsigned i=0;i<fillers.size();++i)
        {
            fillers[i]=new T::Texture(N::nativeTextureImage(data),readyTicket(data));
            boundary(29+i*2,158+i*2);state.applyTextureAttribute(0,fillers[i]);names[i]=binding();
            require(names[i] && !fillers[i]->getTextureObject(state.getContextID()),"retirement filler did not receive raw backing");
            fillers[i]->setImage(N::nativeTextureImage(data));
            boundary(30+i*2,159+i*2);state.applyTextureAttribute(0,fillers[i]);
            require(fillers[i]->getTextureObject(state.getContextID()) && glIsTexture(names[i]),
                "stale retirement filler lost ordinary fallback or prematurely released pending debt");
        }
        require(T::counters().retirements==retired+4 && T::counters().liveBytes==baseline+6*coarseBytes,
            "saturated fixture did not retain all four debt slots and two active backing charges");
        auto replacement=prepare(dds(32,32));stale->setImage(N::nativeTextureImage(replacement));
        ordinaryTicket->ordinaryAllocated=true;
        const auto before=T::counters().retirements.load(),live=T::counters().liveBytes.load();boundary(37,166);
        state.applyTextureAttribute(0,stale);
        require(stale->getTextureObject(state.getContextID()) && binding()!=staleName,"stale descriptor bound obsolete raw backing with saturated retirement debt");
        verifyPixels(*replacement,0);const auto coarseDraws=T::counters().coarseDraws.load();draw->drawImplementation(render);
        require(T::counters().coarseDraws==coarseDraws,"ordinary stale fallback forged a native draw receipt while old debt remained");
        state.applyTextureAttribute(0,ordinaryOwner);
        require(ordinaryOwner->getTextureObject(state.getContextID()) && binding()!=ordinaryName,
            "ordinary-authoritative descriptor rebound obsolete raw backing with saturated retirement debt");
        verifyPixels(*data,0);draw->drawImplementation(render);
        require(T::counters().coarseDraws==coarseDraws && T::counters().retirements==before
            && T::counters().liveBytes==live && glIsTexture(staleName) && glIsTexture(ordinaryName),
            "saturated fallback dropped unsafe storage debt or counted an obsolete native draw");
        T::holdTestRetirements(false);glFinish();boundary(38,167);glFinish();boundary(39,168);
        for(const auto name:names) require(!glIsTexture(name),"saturated pending slot did not eventually delete its exact GL object");
        require(!glIsTexture(staleName) && !glIsTexture(ordinaryName) && T::counters().liveBytes==baseline,
            "saturated stale/ordinary owners did not repay debt after safe targeted retirement");
    }
    T::releaseContext(state);
    require(T::counters().liveBytes==0 && N::AssetGpuBudget::liveBytes==0 && !glIsTexture(demoted),"context owner teardown retained candidate allocation debt");
    N::UploadPool::ledger=nullptr;N::UploadPool::sample=nullptr;
    context->releaseContext();context->close(true);
    texture=nullptr;composite=nullptr;
    N::UploadPool::ledger=&ledger;N::UploadPool::sample=&readSample;
    verifyRendererOrder(ledger,[](osgViewer::Viewer& viewer)
    { require(SceneUtil::TextureOwnerBoundary::installBeforeRealize(viewer)==2,"production root-stage owner hook installation failed"); });
    N::UploadPool::ledger=nullptr;N::UploadPool::sample=nullptr;
    std::cout<<"native texture production: source/role/cache/BC1-3 qualified; cold coarse->private fine->real demotion/deletion engaged; upload and old/new caps passed; real pending producer and Renderer main/refraction owner order passed\n";
    return 0;
}
catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
