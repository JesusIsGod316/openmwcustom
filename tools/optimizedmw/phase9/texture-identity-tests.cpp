// Native architecture gate. No engine texture factory uses this prototype.
// Proves persistent material identity, clones and actual VFS/OSG reconstruction;
// does not authorize a blanket CPU-image unref switch or game promotion.
#include <components/resource/sharedstatecache.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/manager.hpp>

#include <osg/DisplaySettings>
#include <osg/GraphicsContext>
#include <osg/Group>
#include <osg/State>
#include <osg/Texture2D>
#include <osgDB/Options>
#include <osgDB/Registry>
#include <osgViewer/Viewer>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace
{
    unsigned checks = 0;
    void require(bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }
    using Color = std::array<unsigned char, 4>;
    constexpr Color green{0, 255, 0, 255}, red{255, 0, 0, 255};
    using Bytes = std::vector<unsigned char>;

    osg::ref_ptr<osg::Image> pixelsImage(Color color)
    {
        osg::ref_ptr<osg::Image> result = new osg::Image;
        result->allocateImage(32, 32, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        result->setInternalTextureFormat(GL_RGBA8);
        result->setDataVariance(osg::Object::STATIC);
        for (unsigned i = 0; i < 32 * 32; ++i)
            std::copy(color.begin(), color.end(), result->data() + 4 * i);
        return result;
    }
    std::uint64_t fingerprint(const osg::Image& image)
    {
        // Fixture corruption detector, NOT a collision-free production content
        // revision. Real factories still need immutable winning-provider and
        // verified source-content revision authority, including in-place edits.
        std::uint64_t value = 14695981039346656037ull;
        for (std::size_t i = 0; i < image.getTotalSizeInBytesIncludingMipmaps(); ++i)
            value = (value ^ image.data()[i]) * 1099511628211ull;
        return value;
    }
    struct Layout
    {
        int width, height, depth, internal, format, type, packing, rowLength, origin;
        std::size_t bytes;
        osg::Image::MipmapDataType mipmaps;
        friend auto operator<=>(const Layout&, const Layout&) = default;
        static Layout of(const osg::Image& image)
        {
            return {image.s(), image.t(), image.r(), image.getInternalTextureFormat(),
                static_cast<int>(image.getPixelFormat()), static_cast<int>(image.getDataType()),
                static_cast<int>(image.getPacking()), image.getRowLength(), static_cast<int>(image.getOrigin()),
                image.getTotalSizeInBytesIncludingMipmaps(), image.getMipmapLevels()};
        }
    };
    struct Identity
    {
        // Values never depend on a nullable Image, GL name, pointer address,
        // currently resident context, or the clone's allocation lifetime.
        std::uint64_t provider, sourceRevision, decodeRevision, payloadFingerprint;
        std::string path;
        Layout layout;
        friend auto operator<=>(const Identity&, const Identity&) = default;
    };
    struct Recipe
    {
        const Identity identity;
        const std::function<osg::ref_ptr<osg::Image>()> reader;
        mutable std::atomic<unsigned> opens{0};
        Recipe(Identity key, std::function<osg::ref_ptr<osg::Image>()> load)
            : identity(std::move(key)), reader(std::move(load)) {}
        bool valid(const osg::Image* value) const
        {
            return value && value->data() && value->getDataVariance() == osg::Object::STATIC
                && !value->requiresUpdateCall() && Layout::of(*value) == identity.layout
                && fingerprint(*value) == identity.payloadFingerprint;
        }
        osg::ref_ptr<osg::Image> reopen() const
        {
            ++opens;
            auto value = reader();
            if (!valid(value.get())) return {};
            return value;
        }
    };
    class PersistentTexture final : public osg::Texture2D
    {
    public:
        PersistentTexture() = default; // Unconfigured cloneType has no authority.
        PersistentTexture(osg::Image* value, std::shared_ptr<const Recipe> recipe)
            : osg::Texture2D(value), mRecipe(std::move(recipe))
        {
            if (!mRecipe || !mRecipe->valid(value)) throw std::runtime_error("Invalid immutable source");
            setDataVariance(osg::Object::STATIC);
            setResizeNonPowerOfTwoHint(false);
            setFilter(MIN_FILTER, LINEAR); setFilter(MAG_FILTER, LINEAR);
            setUseHardwareMipMapGeneration(false);
            // Freeze format before sharing; OSG's upload-computed format and
            // object-buffer comparison must not reorder the shared-state set.
            setInternalFormat(mRecipe->identity.layout.internal);
            setSourceFormat(mRecipe->identity.layout.format);
            setSourceType(mRecipe->identity.layout.type);
            setClientStorageHint(false);
            setUnRefImageDataAfterApply(true);
        }
        // Clone only on the producer's quiescent graph, like ordinary OSG.
        // No clone inherits allocation/capture/completion proof from its source.
        PersistentTexture(const PersistentTexture& other, const osg::CopyOp& copy)
            : osg::Texture2D(other, copy), mRecipe(other.mRecipe) {}
        META_StateAttribute(OptimizedMWNative, PersistentTexture, TEXTURE)
        int compare(const osg::StateAttribute& other) const override
        {
            COMPARE_StateAttribute_Types(PersistentTexture, other)
            if (static_cast<bool>(mRecipe) != static_cast<bool>(rhs.mRecipe)) return mRecipe ? 1 : -1;
            if (mRecipe)
            {
                if (mRecipe->identity < rhs.mRecipe->identity) return -1;
                if (rhs.mRecipe->identity < mRecipe->identity) return 1;
            }
            // This inherited helper compares sampling semantics, not image or
            // texture object buffers. Parameters remain owner-thread/static.
            if (const auto value = compareTexture(rhs)) return value;
            COMPARE_StateAttribute_Parameter(_borderWidth)
            COMPARE_StateAttribute_Parameter(_borderColor)
            return 0;
        }
        osg::ref_ptr<osg::Image> cpuLease() const
        {
            std::lock_guard lock(mUploadMutex);
            if (!mRecipe) return {};
            if (getImage()) return mRecipe->valid(getImage()) ? osg::ref_ptr<osg::Image>(const_cast<osg::Image*>(getImage())) : nullptr;
            return mRecipe->reopen();
        }
        void apply(osg::State& state) const override
        {
            std::lock_guard lock(mUploadMutex);
            if (!mRecipe) throw std::runtime_error("Unconfigured source authority");
            if (getImage() && !mRecipe->valid(getImage())) throw std::runtime_error("Mutable image is ineligible");
            if (!getImage() && !getTextureObject(state.getContextID()))
            {
                const auto value = mRecipe->reopen();
                // Native refusal only. Production must use its ordinary
                // reader/fallback before this boundary; never blank allocation.
                if (!value) throw std::runtime_error("Closed or changed immutable source");
                const_cast<PersistentTexture*>(this)->setImage(value.get());
            }
            osg::Texture2D::apply(state);
        }
    private:
        const std::shared_ptr<const Recipe> mRecipe;
        mutable std::mutex mUploadMutex;
    };
    std::shared_ptr<const Recipe> procedural(std::string path, Color color, std::uint64_t revision = 1)
    {
        auto image = pixelsImage(color);
        return std::make_shared<Recipe>(Identity{1, revision, 1, fingerprint(*image), std::move(path), Layout::of(*image)},
            [color]{ return pixelsImage(color); });
    }
    void gpuPixels(const osg::Texture2D& texture, osg::State& state, Color expected)
    {
        const auto* object = texture.getTextureObject(state.getContextID());
        require(object && object->id(), "Actual texture allocation exists");
        glBindTexture(GL_TEXTURE_2D, object->id());
        Bytes read(32 * 32 * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
        require(glGetError() == GL_NO_ERROR, "Actual GL readback succeeds");
        bool equal = true;
        for (unsigned i = 0; i < 32 * 32; ++i)
            equal &= std::equal(expected.begin(), expected.end(), read.begin() + 4 * i);
        require(equal, "All actual GPU pixels match the intended source color");
    }
    osg::ref_ptr<osg::Group> graph(osg::Texture* texture)
    {
        osg::ref_ptr<osg::Group> result = new osg::Group;
        result->getOrCreateStateSet()->setDataVariance(osg::Object::STATIC);
        result->getStateSet()->setTextureAttributeAndModes(0, texture);
        return result;
    }
    void put(Bytes& bytes, unsigned offset, unsigned value)
    { for (unsigned i=0; i<4; ++i) bytes[offset+i] = static_cast<unsigned char>(value >> (8*i)); }
    Bytes compressedDds(Color color)
    {
        Bytes bytes(128 + 64 * 8, 0);
        put(bytes,0,0x20534444); put(bytes,4,124); put(bytes,8,0x81007);
        put(bytes,12,32); put(bytes,16,32); put(bytes,20,512);
        put(bytes,76,32); put(bytes,80,4); put(bytes,84,0x31545844); put(bytes,108,0x1000);
        const unsigned packed = color == green ? 0x07e0 : 0xf800;
        for (unsigned i=0; i<64; ++i)
        { bytes[128+i*8]=static_cast<unsigned char>(packed); bytes[129+i*8]=static_cast<unsigned char>(packed>>8); }
        return bytes;
    }
    struct File final : VFS::File
    {
        Bytes bytes;
        unsigned opens = 0;
        Files::IStreamPtr open() override
        { ++opens; return std::make_unique<std::istringstream>(std::string(bytes.begin(), bytes.end()), std::ios::binary); }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "identity"; }
    };
    struct Archive final : VFS::Archive
    {
        File file;
        void listResources(VFS::FileMap& out) override
        { out.emplace(VFS::Path::Normalized("textures/identity.dds"), &file); }
        bool contains(VFS::Path::NormalizedView path) const override
        { return path.value() == "textures/identity.dds"; }
        std::string getDescription() const override { return "immutable-source-native"; }
    };
    struct Provider
    {
        VFS::Manager vfs;
        File* file;
        Provider()
        {
            auto archive = std::make_unique<Archive>();
            file = &archive->file; file->bytes = compressedDds(green);
            vfs.addArchive(std::move(archive)); vfs.buildIndex();
        }
        osg::ref_ptr<osg::Image> read()
        {
            auto stream = vfs.get(VFS::Path::NormalizedView("textures/identity.dds"));
            auto* reader = osgDB::Registry::instance()->getReaderWriterForExtension("dds");
            if (!reader) throw std::runtime_error("Installed DDS reader unavailable");
            osg::ref_ptr<osgDB::Options> options = new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields");
            auto result = reader->readImage(*stream, options);
            if (!result.success() || !result.getImage()) throw std::runtime_error("Ordinary DDS reader failed");
            osg::ref_ptr<osg::Image> image = result.getImage();
            image->setFileName("textures/identity.dds"); image->setDataVariance(osg::Object::STATIC);
            return image;
        }
    };
    osg::ref_ptr<osg::GraphicsContext> makeContext()
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width=32; traits->height=32; traits->doubleBuffer=false;
        auto result = osg::GraphicsContext::createGraphicsContext(traits);
        require(result && result->realize() && result->makeCurrent(), "Actual private GL context is current");
        result->getState()->setMaxTexturePoolSize(0);
        return result;
    }
    template<class Operation> void refused(Operation&& operation, const char* message)
    {
        bool threw=false;
        try { operation(); } catch(const std::runtime_error&) { threw=true; }
        require(threw,message);
    }
}

int main() try
{
    osgViewer::Viewer anchor;
    osg::DisplaySettings::instance()->setMaxNumberOfGraphicsContexts(1);
    auto context = makeContext(); auto* state = context->getState();

    // Reproduce why stock unref is not a persistent source identity: after
    // both allocations disappear, distinct colors compare as the same state.
    osg::ref_ptr<osg::Texture2D> rawGreen = new osg::Texture2D(pixelsImage(green));
    osg::ref_ptr<osg::Texture2D> rawRed = new osg::Texture2D(pixelsImage(red));
    for (auto* texture : {rawGreen.get(), rawRed.get()})
    { texture->setUnRefImageDataAfterApply(true); texture->setResizeNonPowerOfTwoHint(false); texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR); }
    require(rawGreen->compare(*rawRed) != 0, "Ordinary attached sources distinguish different colors");
    rawGreen->apply(*state); rawRed->apply(*state);
    require(!rawGreen->getImage() && !rawRed->getImage(), "Stock actual apply retires both attached images");
    gpuPixels(*rawGreen,*state,green); gpuPixels(*rawRed,*state,red);
    rawGreen->releaseGLObjects(state); rawRed->releaseGLObjects(state);
    require(rawGreen->compare(*rawRed) == 0, "Reproduced stock source-identity loss after image and allocation retirement");

    auto greenRecipe=procedural("textures/green",green), redRecipe=procedural("textures/red",red);
    auto aliasRecipe=procedural("textures/green",green);
    osg::ref_ptr<PersistentTexture> a=new PersistentTexture(pixelsImage(green),greenRecipe);
    osg::ref_ptr<PersistentTexture> b=new PersistentTexture(pixelsImage(red),redRecipe);
    osg::ref_ptr<PersistentTexture> alias=new PersistentTexture(pixelsImage(green),aliasRecipe);
    const auto order=a->compare(*b);
    require(order!=0 && order==-b->compare(*a), "Persistent different-source order is symmetric");
    require(a->compare(*alias)==0, "Equal immutable sources compare equal before upload");
    a->apply(*state); b->apply(*state);
    require(!a->getImage() && !b->getImage(), "Candidate actual uploads retire unpinned CPU backing");
    require(a->compare(*alias)==0 && a->compare(*b)==order, "Material order remains stable across upload and backing retirement");

    osg::ref_ptr<Resource::SharedStateManager> shares = new Resource::SharedStateManager;
    auto ga=graph(a), gb=graph(b), gc=graph(alias);
    shares->share(ga); shares->share(gb); shares->share(gc);
    require(ga->getStateSet()==gc->getStateSet(), "Actual production shared-state manager merges equal immutable materials");
    require(ga->getStateSet()!=gb->getStateSet(), "Actual production shared-state manager preserves distinct colors");
    a->releaseGLObjects(state); b->releaseGLObjects(state);
    require(a->compare(*b)==order && a->compare(*alias)==0, "Material order survives complete allocation retirement");
    auto newAlias=graph(new PersistentTexture(pixelsImage(green),aliasRecipe)); shares->share(newAlias);
    require(newAlias->getStateSet()==ga->getStateSet(), "Shared ordered set still finds its entry after allocation retirement");
    a->apply(*state); b->apply(*state); gpuPixels(*a,*state,green); gpuPixels(*b,*state,red);

    for (const auto copy : {osg::CopyOp::SHALLOW_COPY,osg::CopyOp::DEEP_COPY_ALL})
    {
        osg::ref_ptr<osg::Object> copied=a->clone(osg::CopyOp(copy));
        auto* clone=dynamic_cast<PersistentTexture*>(copied.get());
        require(clone && clone!=a.get() && clone->compare(*a)==0, "Actual shallow/deep clones preserve immutable source identity");
        require(!clone->getTextureObject(state->getContextID()), "Clone inherits no native allocation proof");
        clone->compileGLObjects(*state); gpuPixels(*clone,*state,green);
        require(!clone->getImage(), "Retired-image clone reconstructs and retires its temporary CPU backing");
        clone->releaseGLObjects(state);
    }
    auto cpu=a->cpuLease();
    require(cpu && cpu->data()[1]==255 && cpu->getTotalSizeInBytesIncludingMipmaps()==4096,
        "Explicit CPU consumer reconstructs an independent exact payload lease");
    a->releaseGLObjects(state); a->apply(*state);
    require(cpu->data()[1]==255 && !a->getImage(), "External CPU reader survives upload retirement");
    cpu=nullptr;
    auto changedSampler=procedural("textures/green",green);
    osg::ref_ptr<PersistentTexture> sampled=new PersistentTexture(pixelsImage(green),changedSampler);
    sampled->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
    require(a->compare(*sampled)!=0, "Sampling semantics remain part of material identity");
    osg::ref_ptr<PersistentTexture> revised=new PersistentTexture(pixelsImage(green),procedural("textures/green",green,2));
    require(a->compare(*revised)!=0, "Same path with a new source revision is a distinct immutable material");
    auto dynamic=pixelsImage(green); dynamic->setDataVariance(osg::Object::DYNAMIC);
    refused([&]{ osg::ref_ptr<PersistentTexture> invalid=new PersistentTexture(dynamic,greenRecipe); },"Dynamic CPU source is declined before retirement");

    // Exact ordinary VFS stream + installed DDS reader, preserving compression.
    auto provider=std::make_shared<Provider>(); auto dds=provider->read();
    require(dds->isCompressed() && dds->getTotalSizeInBytesIncludingMipmaps()==512, "Actual DDS reader preserves compressed CPU storage");
    const auto revision=provider->vfs.getIndexGeneration();
    const std::weak_ptr<Provider> weak=provider;
    auto vfsRecipe=std::make_shared<Recipe>(Identity{2,revision,1,fingerprint(*dds),"textures/identity.dds",Layout::of(*dds)},
        [weak,revision]()->osg::ref_ptr<osg::Image>{
            auto owner=weak.lock();
            if (!owner || owner->vfs.getIndexGeneration()!=revision) return {};
            return owner->read();
        });
    osg::ref_ptr<PersistentTexture> compressed=new PersistentTexture(dds,vfsRecipe);
    dds=nullptr;
    compressed->apply(*state); gpuPixels(*compressed,*state,green);
    require(!compressed->getImage() && provider->file->opens==1,"Compressed first upload does not reopen its VFS source");
    compressed->releaseGLObjects(state); compressed->apply(*state); gpuPixels(*compressed,*state,green);
    require(provider->file->opens==2,"Compressed reconstruction uses the actual VFS provider exactly once");
    compressed->releaseGLObjects(state);
    provider->file->bytes=compressedDds(red);
    refused([&]{compressed->apply(*state);},"In-place changed pixels are declined even without an index generation change");
    require(!compressed->getTextureObject(state->getContextID()),"Failed reconstruction cannot publish a blank or wrong allocation");
    provider->file->bytes=compressedDds(green);
    provider->vfs.buildIndex();
    refused([&]{compressed->apply(*state);},"VFS index revision changes invalidate old reconstruction authority");
    provider.reset();
    refused([&]{compressed->apply(*state);},"Closed provider cannot be revived by a texture or clone");
    require(!compressed->cpuLease(),"CPU consumers also refuse a closed source");

    // Real context registration and OSG pool policy, not synthetic handles.
    osg::DisplaySettings::instance()->setMaxNumberOfGraphicsContexts(2);
    auto second=makeContext(); auto* secondState=second->getState();
    require(state->getContextID()!=secondState->getContextID(),"Two actual contexts have distinct storage identities");
    osg::ref_ptr<PersistentTexture> multi=new PersistentTexture(pixelsImage(red),redRecipe);
    multi->resizeGLObjectBuffers(2);
    require(context->makeCurrent(),"First real context reacquired");
    multi->apply(*state);
    require(multi->getImage()!=nullptr,"OSG preserves backing while another registered context still needs it");
    gpuPixels(*multi,*state,red);
    require(second->makeCurrent(),"Second real context reacquired");
    multi->apply(*secondState);
    require(!multi->getImage(),"All actual context uploads permit OSG backing retirement");
    gpuPixels(*multi,*secondState,red);
    multi->releaseGLObjects(secondState); multi->apply(*secondState);
    gpuPixels(*multi,*secondState,red);
    require(!multi->getImage(),"Lost second-context allocation reconstructs without invalidating the other allocation");
    require(context->makeCurrent(),"First context restored after reconstruction");
    gpuPixels(*multi,*state,red);

    state->setMaxTexturePoolSize(1024*1024); secondState->setMaxTexturePoolSize(1024*1024);
    osg::ref_ptr<PersistentTexture> pooled=new PersistentTexture(pixelsImage(green),greenRecipe);
    pooled->resizeGLObjectBuffers(2); pooled->apply(*state);
    require(second->makeCurrent(),"Pool test second context current"); pooled->apply(*secondState);
    require(pooled->getImage()!=nullptr,"Nonzero production-style OSG pool retains backing under the existing safety rule");
    gpuPixels(*pooled,*secondState,green);
    // Restore fixture policy only; never bypass the production pool contract.
    state->setMaxTexturePoolSize(0); secondState->setMaxTexturePoolSize(0);
    multi->releaseGLObjects(secondState); pooled->releaseGLObjects(secondState);
    second->releaseContext(); second->close(true);
    require(context->makeCurrent(),"Remaining actual context reacquired for final retirement");
    for (auto* texture : {static_cast<osg::Texture2D*>(a.get()),static_cast<osg::Texture2D*>(b.get()),
            static_cast<osg::Texture2D*>(multi.get()),static_cast<osg::Texture2D*>(pooled.get())})
        texture->releaseGLObjects(state);
    require(a->compare(*alias)==0 && a->compare(*b)==order,"Final close cannot change persistent source ordering");
    context->releaseContext(); context->close(true);
    std::cout << "PASS immutable texture identity: " << checks << " assertions; actual shared state, distinct colors, clones, VFS DDS, CPU readers, two contexts and pool policy\n"
              << "LIMITATIONS: native prototype only; controlled 32x32 fixtures, noncryptographic corruption fingerprint, no game factory/ordinary missing-source fallback, arbitrary plugins or CPU-consumer integration, concurrent clone/context-release race qualification, driver RAM/FPS or visual acceptance. Nonzero pools intentionally retain CPU backing.\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL immutable texture identity: " << error.what() << '\n';
    return 1;
}
