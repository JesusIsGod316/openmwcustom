// Actual ImageManager/SHA256/GL integration gate. This texture is a native
// prototype, not an engine factory or permission to retire arbitrary images.
#include <components/resource/imagemanager.hpp>
#include <components/resource/immutableimagesource.hpp>
#include <components/resource/sharedstatecache.hpp>
#include <components/vfs/archive.hpp>
#include <osg/DisplaySettings>
#include <osg/ContextData>
#include <osg/GraphicsContext>
#include <osg/Group>
#include <osg/State>
#include <osg/Texture2D>
#include <osgViewer/Viewer>
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

namespace
{
    using namespace Resource;
    using Recipe=ImageSource::Recipe;
    using Bytes=DerivedAssets::Bytes;
    using Color=std::array<unsigned char,4>;
    constexpr Color green{0,255,0,255},red{255,0,0,255};
    std::atomic_uint checks=0;
    void require(bool result,const char* message)
    { ++checks;if (!result) throw std::runtime_error(message); }
    OpenGlPressureSample sample(void* owner)
    {
        auto& budget=*static_cast<SpeculativeBudget*>(owner);
        OpenGlPressureSample s;s.generation=1;s.growthWatermarkValid=true;s.accountedGrowth=budget.watermark()->load();
        s.memory.physicalValid=s.memory.commitValid=true;s.memory.physicalTotal=32ull*1024*SpeculativeBudget::MiB;
        s.memory.physicalAvailable=16ull*1024*SpeculativeBudget::MiB;s.memory.commitAvailable=24ull*1024*SpeculativeBudget::MiB;
        s.decision.state=OpenGlPressure::Normal;s.decision.admissionsPerSample=4;
        s.decision.limits=OpenGlPressurePolicy{}.limits(s.memory.physicalTotal);return s;
    }
    struct File final : VFS::File
    {
        Bytes bytes;std::atomic_uint opens=0;bool missing=false;
        Files::IStreamPtr open() override
        { ++opens;if (missing) throw std::runtime_error("native missing source");return std::make_unique<ImageSource::SourceStream>(bytes); }
        std::optional<VFS::SourceOpenEstimate> getOpenEstimate() const override
        { return VFS::SourceOpenEstimate{bytes.size(),1024}; }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "texture-source-contract"; }
    };
    struct Archive final : VFS::Archive
    {
        File file;VFS::Path::Normalized path;
        explicit Archive(std::string name):path(name) {}
        void listResources(VFS::FileMap& out) override { out.insert_or_assign(path,&file); }
        bool contains(VFS::Path::NormalizedView name) const override { return name.value()==path.value(); }
        std::string getDescription() const override { return "texture-source-contract"; }
    };
    Bytes tga(Color color)
    {
        Bytes bytes(18+32*32*4,0);bytes[2]=2;bytes[12]=32;bytes[14]=32;bytes[16]=32;bytes[17]=8;
        for (unsigned i=0;i<32*32;++i)
        { auto* p=bytes.data()+18+i*4;p[0]=color[2];p[1]=color[1];p[2]=color[0];p[3]=color[3]; }
        return bytes;
    }
    void put(Bytes& bytes,unsigned offset,unsigned value)
    { for (unsigned i=0;i<4;++i)bytes[offset+i]=static_cast<unsigned char>(value>>(8*i)); }
    Bytes dds(Color color)
    {
        Bytes bytes(128+512,0);put(bytes,0,0x20534444);put(bytes,4,124);put(bytes,8,0x81007);
        put(bytes,12,32);put(bytes,16,32);put(bytes,20,512);put(bytes,76,32);put(bytes,80,4);
        put(bytes,84,0x31545844);put(bytes,108,0x1000);const unsigned packed=color==green?0x07e0:0xf800;
        for (unsigned i=0;i<64;++i) {bytes[128+i*8]=static_cast<unsigned char>(packed);bytes[129+i*8]=static_cast<unsigned char>(packed>>8);}
        return bytes;
    }
    File* add(VFS::Manager& vfs,const char* path,Bytes bytes)
    {
        auto archive=std::make_unique<Archive>(path);auto* file=&archive->file;file->bytes=std::move(bytes);
        vfs.addArchive(std::move(archive));return file;
    }
    class PersistentTexture final : public osg::Texture2D
    {
        struct Snapshot
        {
            osg::ref_ptr<osg::Texture2D> sampler;
            osg::ref_ptr<osg::Image> image;
            std::shared_ptr<const Recipe> recipe;
            std::shared_ptr<SpeculativeBudget> budget;
            bool pooledRetirement=false;
        };
    public:
        PersistentTexture()=default;
        PersistentTexture(osg::Image* image,std::shared_ptr<SpeculativeBudget> budget,bool pooledRetirement=false)
            : osg::Texture2D(image),mRecipe(image?ImageManager::imageSourceRecipe(*image):nullptr),mBudget(std::move(budget)),mPooledRetirement(pooledRetirement)
        {
            if (!mRecipe || !mBudget || !mRecipe->matches(*image)) throw std::runtime_error("No actual immutable source publication");
            setDataVariance(osg::Object::STATIC);setResizeNonPowerOfTwoHint(false);
            setFilter(MIN_FILTER,LINEAR);setFilter(MAG_FILTER,LINEAR);setUseHardwareMipMapGeneration(false);
            setInternalFormat(mRecipe->revision().layout.internal);setSourceFormat(mRecipe->revision().layout.format);
            setSourceType(mRecipe->revision().layout.type);setClientStorageHint(false);setUnRefImageDataAfterApply(true);
        }
        PersistentTexture(const PersistentTexture& other,const osg::CopyOp& copy)
            : PersistentTexture(other.snapshot(copy)) {}
        META_StateAttribute(OptimizedMWNative,PersistentTexture,TEXTURE)
        int compare(const osg::StateAttribute& other) const override
        {
            COMPARE_StateAttribute_Types(PersistentTexture,other)
            if (bool(mRecipe)!=bool(rhs.mRecipe)) return mRecipe?1:-1;
            if (mRecipe)
            {
                if (mRecipe->revision()<rhs.mRecipe->revision()) return -1;
                if (rhs.mRecipe->revision()<mRecipe->revision()) return 1;
            }
            if (mPooledRetirement!=rhs.mPooledRetirement) return mPooledRetirement?1:-1;
            if (const auto value=compareTexture(rhs)) return value;
            COMPARE_StateAttribute_Parameter(_borderWidth)
            COMPARE_StateAttribute_Parameter(_borderColor)
            return 0;
        }
        osg::ref_ptr<osg::Image> cpuLease() const
        {
            std::lock_guard lock(mMutex);
            if (!mRecipe) return {};
            if (getImage()) return mRecipe->matches(*getImage())?osg::ref_ptr<osg::Image>(const_cast<osg::Image*>(getImage())):nullptr;
            return mRecipe->reconstructOwned(mBudget.get(),sample(mBudget.get()));
        }
        bool tryApply(osg::State& state) const
        {
            std::lock_guard lock(mMutex);
            if (!mRecipe || (getImage() && !mRecipe->matches(*getImage()))) return false;
            if (!getImage() && !getTextureObject(state.getContextID()))
            {
                auto image=mRecipe->reconstructOwned(mBudget.get(),sample(mBudget.get()));
                if (!image) return false; // Caller rebuilds an ordinary, newly keyed material; no live re-key.
                const_cast<PersistentTexture*>(this)->setImage(image);
            }
            osg::Texture2D::apply(state);
            const auto* object=getTextureObject(state.getContextID());
            // Independently selected native experiment. A pool can steal an
            // active TextureObject and zero this texture's slot. Our next
            // owner apply reconstructs the original SHA256 source before any
            // new upload. This never changes the production OSG safety rule.
            if (mPooledRetirement && state.getMaxTexturePoolSize()!=0 && getImage()
                && areAllTextureObjectsLoaded() && object && object->getTexture()==this && object->isAllocated())
                const_cast<PersistentTexture*>(this)->_image=nullptr;
            return object!=nullptr;
        }
        void apply(osg::State& state) const override
        { if (!tryApply(state)) throw std::runtime_error("Native immutable material requires ordinary replacement"); }
        void releaseGLObjects(osg::State* state=nullptr) const override
        { std::lock_guard lock(mMutex);osg::Texture2D::releaseGLObjects(state); }
        void resizeGLObjectBuffers(unsigned count) override
        { std::lock_guard lock(mMutex);osg::Texture2D::resizeGLObjectBuffers(count); }
        const std::shared_ptr<const Recipe>& recipe() const { return mRecipe; }
    private:
        explicit PersistentTexture(Snapshot value)
            : osg::Texture2D(*value.sampler,osg::CopyOp::SHALLOW_COPY),mRecipe(std::move(value.recipe)),mBudget(std::move(value.budget)),mPooledRetirement(value.pooledRetirement)
        { setImage(value.image); }
        Snapshot snapshot(const osg::CopyOp& copy) const
        {
            std::lock_guard lock(mMutex);
            Snapshot result{new osg::Texture2D(*this,osg::CopyOp::SHALLOW_COPY),{},mRecipe,mBudget,mPooledRetirement};
            if (getImage())
            {
                if (copy.getCopyFlags()&osg::CopyOp::DEEP_COPY_IMAGES)
                {
                    result.image=mRecipe->cloneOwned(*getImage(),mBudget.get(),sample(mBudget.get()));
                    if (!result.image) throw std::runtime_error("Native deep clone refused before output allocation");
                }
                else result.image=const_cast<osg::Image*>(getImage());
            }
            return result;
        }
        const std::shared_ptr<const Recipe> mRecipe;
        const std::shared_ptr<SpeculativeBudget> mBudget;
        const bool mPooledRetirement=false;
        mutable std::mutex mMutex;
    };
    osg::ref_ptr<osg::GraphicsContext> context()
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
        traits->readDISPLAY();traits->setUndefinedScreenDetailsToDefaultScreen();traits->width=32;traits->height=32;traits->doubleBuffer=false;
        auto value=osg::GraphicsContext::createGraphicsContext(traits);
        require(value && value->realize() && value->makeCurrent(),"Actual hidden GL context current");
        value->getState()->setMaxTexturePoolSize(0);return value;
    }
    void pixels(const osg::Texture2D& texture,osg::State& state,Color color)
    {
        const auto* object=texture.getTextureObject(state.getContextID());require(object && object->id(),"Actual GL allocation exists");
        glBindTexture(GL_TEXTURE_2D,object->id());Bytes bytes(32*32*4);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
        require(glGetError()==GL_NO_ERROR,"Actual GL readback succeeds");
        for (unsigned i=0;i<32*32;++i)if (!std::equal(color.begin(),color.end(),bytes.begin()+4*i))
            throw std::runtime_error("Actual GPU color differs from its immutable source");
        require(true,"All actual GL pixels match their distinct winning source");
    }
    osg::ref_ptr<osg::Group> graph(osg::Texture* texture)
    {
        osg::ref_ptr<osg::Group> result=new osg::Group;auto* state=result->getOrCreateStateSet();
        state->setDataVariance(osg::Object::STATIC);state->setTextureAttributeAndModes(0,texture);return result;
    }
}
int main() try
{
    osgViewer::Viewer anchor;osg::DisplaySettings::instance()->setMaxNumberOfGraphicsContexts(1);
    auto first=context();auto* state=first->getState();
    VFS::Manager vfs;auto* greenFile=add(vfs,"textures/green.tga",tga(green));add(vfs,"textures/red.tga",tga(red));
    auto* blockFile=add(vfs,"textures/blocks.dds",dds(green));vfs.buildIndex();
    auto budget=std::make_shared<SpeculativeBudget>(SpeculativeBudget::Config{512*SpeculativeBudget::MiB,4,1,256*SpeculativeBudget::MiB,64*SpeculativeBudget::MiB,1});
    auto manager=std::make_unique<ImageManager>(&vfs,120);manager->setSpeculativeBudget(budget.get());
    manager->configureImageSources(true,sample,budget.get());
    auto gi=manager->getImage(VFS::Path::Normalized("textures/green.tga"));
    auto ri=manager->getImage(VFS::Path::Normalized("textures/red.tga"));
    auto bi=manager->getImage(VFS::Path::Normalized("textures/blocks.dds"));
    auto recipe=ImageManager::imageSourceRecipe(*gi);
    require(recipe && recipe->revision().sourceSha256==DerivedAssets::digest(greenFile->bytes),"Material identity comes from the actual required winning ImageManager source");
    require(gi->getDataVariance()==osg::Object::STATIC && ri->getDataVariance()==osg::Object::STATIC,"Actual file decoder variance is eligible without relabeling");
    const auto sourceBytes=8192+512;
    require(budget->stats().knownOwnerBytes==sourceBytes,"Actual three image publications are charged once by backing");
    {
        auto owned=recipe->reconstructOwned(budget.get(),sample(budget.get()));
        require(owned && ImageSource::ownedBackingBytes(*owned)==4096 && !budget->stats().futureBytes,"Reconstructed output owns its charge after source/scratch grants end");
        auto cpu=owned;owned=nullptr;
        require(budget->stats().knownOwnerBytes==sourceBytes+4096 && cpu->getColor(0,0)==gi->getColor(0,0),"An extracted actual CPU Image ref remains charged and readable");
        auto copy=recipe->cloneOwned(*cpu,budget.get(),sample(budget.get()));
        require(copy && copy->data()!=cpu->data() && recipe->matches(*copy) && ImageSource::ownedBackingBytes(*copy)==4096,"Explicit deep clone has a separate exact backing allocation and charge");
        cpu=nullptr;require(budget->stats().knownOwnerBytes==sourceBytes+4096,"Dropping original CPU lease cannot repay a live distinct clone");
        copy=nullptr;require(budget->stats().knownOwnerBytes==sourceBytes,"Final CPU/clone consumers retire both distinct charges");
        auto critical=sample(budget.get());critical.decision.state=OpenGlPressure::Critical;
        require(!recipe->cloneOwned(*gi,budget.get(),critical),"Critical pressure declines optional deep clone before allocation");
    }
    osg::ref_ptr<PersistentTexture> a=new PersistentTexture(gi,budget),b=new PersistentTexture(ri,budget),alias=new PersistentTexture(gi,budget);
    // Deep clone while the compressed source is attached. Stock OSG's single-level size is 2048; the checked payload is512.
    osg::ref_ptr<PersistentTexture> block=new PersistentTexture(bi,budget);
    require(bi->getTotalSizeInBytesIncludingMipmaps()==2048 && imagePayloadExtent(*bi).bytes==512,"Installed stock compressed-size mismatch is reproduced");
    osg::ref_ptr<osg::Object> blockCopy=block->clone(osg::CopyOp::DEEP_COPY_ALL);
    auto* blockClone=dynamic_cast<PersistentTexture*>(blockCopy.get());
    require(blockClone && blockClone->getImage() && blockClone->getImage()->data()!=bi->data()
        && ImageSource::ownedBackingBytes(*blockClone->getImage())==512 && blockClone->recipe()->matches(*blockClone->getImage()),"Attached compressed deep clone copies only the checked block span and owns a separate charge");
    blockClone->apply(*state);pixels(*blockClone,*state,green);blockClone->releaseGLObjects(state);blockCopy=nullptr;
    const auto order=a->compare(*b);require(order!=0 && order==-b->compare(*a) && a->compare(*alias)==0,"Actual source revisions distinguish colors and equal sources symmetrically");
    auto ga=graph(a),gb=graph(b),gc=graph(alias);osg::ref_ptr<SharedStateManager> shared=new SharedStateManager;
    shared->share(ga);shared->share(gb);shared->share(gc);
    require(ga->getStateSet()==gc->getStateSet() && ga->getStateSet()!=gb->getStateSet(),"Actual SharedStateManager merges equal source materials and preserves distinct colors");
    gi=nullptr;ri=nullptr;bi=nullptr;manager->clearCache();alias=nullptr;
    a->apply(*state);b->apply(*state);block->apply(*state);pixels(*a,*state,green);pixels(*b,*state,red);pixels(*block,*state,green);
    require(!a->getImage() && !b->getImage() && !block->getImage() && !budget->stats().knownOwnerBytes,"Actual zero-pool uploads retire unpinned unique CPU charges");
    require(a->compare(*b)==order && a->compare(*dynamic_cast<PersistentTexture*>(gc->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE)))==0,"Shared material order survives actual CPU/native residency changes");
    a->releaseGLObjects(state);a->apply(*state);pixels(*a,*state,green);
    require(!a->getImage() && !budget->stats().knownOwnerBytes,"Reconstructed texture upload does not leave an uncharged output or leaked scratch");
    for (const auto flags:{osg::CopyOp::SHALLOW_COPY,osg::CopyOp::DEEP_COPY_ALL})
    {
        osg::ref_ptr<osg::Object> object=a->clone(osg::CopyOp(flags));auto* clone=dynamic_cast<PersistentTexture*>(object.get());
        require(clone && clone!=a && clone->compare(*a)==0 && !clone->getTextureObject(state->getContextID()),"Retired-source clone preserves immutable order and no native allocation authority");
        clone->compileGLObjects(*state);pixels(*clone,*state,green);clone->releaseGLObjects(state);
    }
    auto cpu=a->cpuLease();require(cpu && cpu->getColor(0,0)==osg::Vec4(0,1,0,1) && ImageSource::ownedBackingBytes(*cpu)==4096,"Actual CPU getColor consumer gets a charged reconstructed lease");
    a->releaseGLObjects(state);a->apply(*state);require(cpu->data()[1]==255 && budget->stats().knownOwnerBytes==4096,"CPU reader survives another native upload and retirement");
    cpu=nullptr;
    // The real nonzero pool steals an active object irrespective of an extra
    // ref_ptr pin. Its GL name, pointer, extent and format can all stay the
    // same while the color/source changes: also a DLSS storage-identity gate.
    auto* pool=osg::get<osg::TextureObjectManager>(state->getContextID());
    pool->flushAllDeletedGLObjects();state->setMaxTexturePoolSize(8192);pool->newFrame(nullptr);
    osg::ref_ptr<PersistentTexture> poolSource=new PersistentTexture(manager->getImage(VFS::Path::Normalized("textures/green.tga")),budget,true);
    manager->clearCache();poolSource->apply(*state);pixels(*poolSource,*state,green);
    require(!poolSource->getImage() && !budget->stats().knownOwnerBytes,"Independent immutable-source candidate retires its CPU backing with an actual nonzero pool");
    osg::ref_ptr<osg::Texture::TextureObject> pinnedObject=poolSource->getTextureObject(state->getContextID());
    const auto pinnedName=pinnedObject->id();std::vector<osg::ref_ptr<PersistentTexture>> competitors;bool stolen=false;
    for(unsigned i=0;i<12 && !stolen;++i)
    {
        pool->newFrame(nullptr);
        osg::ref_ptr<PersistentTexture> competitor=new PersistentTexture(manager->getImage(VFS::Path::Normalized("textures/red.tga")),budget,true);
        manager->clearCache();competitor->apply(*state);pixels(*competitor,*state,red);competitors.push_back(competitor);
        stolen=!poolSource->getTextureObject(state->getContextID()) && pinnedObject->getTexture()!=poolSource.get();
        if(stolen)require(competitor->getTextureObject(state->getContextID())==pinnedObject && pinnedObject->id()==pinnedName,
            "Actual pool reuses the same pinned TextureObject and GL name for a different colored source");
    }
    require(stolen,"Finite real-pool workload actually evicts the active immutable-source allocation");
    const auto opensBefore=greenFile->opens.load();pool->newFrame(nullptr);poolSource->apply(*state);pixels(*poolSource,*state,green);
    require(greenFile->opens==opensBefore+1 && !poolSource->getImage() && !budget->stats().knownOwnerBytes,
        "Actual stolen pooled allocation reconstructs exact original pixels once and retires its complete temporary charge");
    for(auto& competitor:competitors)competitor->releaseGLObjects(state);
    poolSource->releaseGLObjects(state);competitors.clear();poolSource=nullptr;pinnedObject=nullptr;state->setMaxTexturePoolSize(0);
    osg::DisplaySettings::instance()->setMaxNumberOfGraphicsContexts(2);auto second=context();auto* state2=second->getState();
    require(state->getContextID()!=state2->getContextID(),"Two real contexts have separate native storage");
    osg::ref_ptr<PersistentTexture> multi=new PersistentTexture(manager->getImage(VFS::Path::Normalized("textures/red.tga")),budget);
    manager->clearCache();multi->resizeGLObjectBuffers(2);require(first->makeCurrent(),"First context reacquired");multi->apply(*state);
    require(multi->getImage()!=nullptr,"A registered not-yet-uploaded context protects original backing");
    require(second->makeCurrent(),"Second context reacquired");multi->apply(*state2);pixels(*multi,*state2,red);
    require(!multi->getImage() && !budget->stats().knownOwnerBytes,"All real contexts loaded permits ordinary zero-pool retirement");
    state->setMaxTexturePoolSize(1024*1024);state2->setMaxTexturePoolSize(1024*1024);
    multi->releaseGLObjects(state2);multi->apply(*state2);
    require(multi->getImage() && ImageSource::ownedBackingBytes(*multi->getImage())==4096 && budget->stats().knownOwnerBytes==4096,"Nonzero OSG pools intentionally preserve a charged reconstructed CPU image");
    auto pooledCpu=multi->cpuLease();state->setMaxTexturePoolSize(0);state2->setMaxTexturePoolSize(0);
    multi->releaseGLObjects(state2);multi->apply(*state2);
    require(!multi->getImage() && pooledCpu->data()[0]==255 && budget->stats().knownOwnerBytes==4096,"Pool policy change never frees a pinned CPU consumer");pooledCpu=nullptr;
    // Competing actual clone/CPU acquisition and native apply/release. Every
    // GL caller owns its current context; sampler parameters stay quiescent.
    require(second->releaseContext(),"Second context detached before its worker acquires it");
    std::atomic_bool okay=true;std::exception_ptr workerFailure;std::mutex failureMutex;
    const auto guarded=[&](auto operation){try{operation();}catch(...){okay=false;std::lock_guard lock(failureMutex);if(!workerFailure)workerFailure=std::current_exception();}};
    std::thread glWorker([&]{guarded([&]{require(second->makeCurrent(),"Worker owns actual second context");
        for(unsigned i=0;i<24;++i){multi->releaseGLObjects(state2);multi->apply(*state2);pixels(*multi,*state2,red);}
        require(second->releaseContext(),"Worker releases its own context");});});
    std::thread reader([&]{guarded([&]{for(unsigned i=0;i<32;++i){auto lease=multi->cpuLease();require(lease && lease->getColor(0,0)==osg::Vec4(1,0,0,1),"Concurrent real CPU consumer gets stable red pixels");
        osg::ref_ptr<osg::Object> object=multi->clone(osg::CopyOp(i%2?osg::CopyOp::DEEP_COPY_ALL:osg::CopyOp::SHALLOW_COPY));
        auto* clone=dynamic_cast<PersistentTexture*>(object.get());require(clone && clone->compare(*multi)==0,"Concurrent shallow/deep texture clone preserves source semantics");}});});
    guarded([&]{require(first->makeCurrent(),"Main owns first context during competing work");
        for(unsigned i=0;i<24;++i){multi->releaseGLObjects(state);multi->apply(*state);pixels(*multi,*state,red);}});
    glWorker.join();reader.join();if(workerFailure)std::rethrow_exception(workerFailure);
    require(okay && !budget->stats().futureBytes,"Concurrent native apply/release/clone/CPU leases leave no future grant debt");
    multi->resizeGLObjectBuffers(3);multi->resizeGLObjectBuffers(2); // Serialize metadata changes; no fake context acquisition proof.
    require(second->makeCurrent(),"Second context restored after worker");multi->releaseGLObjects(state2);multi->apply(*state2);pixels(*multi,*state2,red);
    multi->releaseGLObjects(state2);second->releaseContext();second->close(true);
    auto replacementContext=context();auto* state3=replacementContext->getState();multi->resizeGLObjectBuffers(state3->getContextID()+1);
    multi->apply(*state3);pixels(*multi,*state3,red);multi->releaseGLObjects(state3);replacementContext->releaseContext();replacementContext->close(true);
    require(first->makeCurrent(),"Original context restored after actual context destruction/recreation");
    // Source change cannot mutate the ordered shared material in place. The
    // native caller constructs a new ordinary material on its separate graph.
    a->releaseGLObjects(state);greenFile->bytes=tga(red);
    require(!a->tryApply(*state) && !a->getTextureObject(state->getContextID()) && a->compare(*b)==order,"Changed source refuses stale publication and preserves old material ordering");
    manager->clearCache();auto ordinaryImage=manager->getImage(VFS::Path::Normalized("textures/green.tga"));
    osg::ref_ptr<osg::Texture2D> ordinary=new osg::Texture2D(ordinaryImage);ordinary->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
    ordinary->setResizeNonPowerOfTwoHint(false);ordinary->apply(*state);pixels(*ordinary,*state,red);ordinary->releaseGLObjects(state);
    require(a->compare(*b)==order,"Actual ordinary replacement never re-keys the live shared immutable entry");ordinary=nullptr;ordinaryImage=nullptr;
    greenFile->missing=true;manager->clearCache();
    auto warning=manager->getImage(VFS::Path::Normalized("textures/green.tga"));
    require(warning==manager->getWarningImage() && !a->cpuLease(),"Missing source uses actual ordinary ImageManager warning; immutable CPU consumer refuses stale pixels");
    osg::ref_ptr<osg::Texture2D> warningTexture=new osg::Texture2D(warning);warningTexture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);warningTexture->apply(*state);
    require(warningTexture->getTextureObject(state->getContextID())!=nullptr,"Actual ordinary missing-source fallback has a native allocation");warningTexture->releaseGLObjects(state);warningTexture=nullptr;warning=nullptr;
    manager.reset();block->releaseGLObjects(state);
    require(!block->cpuLease() && !block->tryApply(*state) && blockFile->opens>0,"Closed manager cannot revive the borrowed winning VFS through a surviving texture");
    for (auto* texture:{a.get(),b.get(),block.get(),multi.get()})texture->releaseGLObjects(state);
    auto detached=shared->detachAll();detached={};ga=nullptr;gb=nullptr;gc=nullptr;shared=nullptr;a=nullptr;b=nullptr;block=nullptr;multi=nullptr;
    require(!budget->stats().knownOwnerBytes && !budget->stats().experimentRetainedBytes && !budget->stats().futureBytes,"Final real CPU/texture/shared-state teardown retires every tracked output allocation and grant");
    first->releaseContext();first->close(true);
    std::cout<<"PASS actual texture source contract: "<<checks.load()<<" assertions; ImageManager SHA256, distinct colors, shared state, charged CPU/clones, two contexts, nonzero pools, concurrent native work and ordinary source fallback\n"
        <<"LIMITATIONS: native texture prototype, controlled32x32 required DDS/TGA; no engine factory/retirement, arbitrary CPU consumer/plugin audit, asynchronous preparation, whole-game RAM/FPS or visual acceptance. Ordinary pool rule retains backing; separate native candidate reconstructs after actual pool stealing. Ordinary replacement is explicit and never a live shared-state re-key. GL names/pointers alone are not storage authority.\n";
    return 0;
}
catch(const std::exception& e){std::cerr<<"FAIL actual texture source contract after "<<checks.load()<<" assertions: "<<e.what()<<'\n';return 1;}
