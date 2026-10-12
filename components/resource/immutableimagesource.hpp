#ifndef OPENMW_COMPONENTS_RESOURCE_IMMUTABLEIMAGESOURCE_H
#define OPENMW_COMPONENTS_RESOURCE_IMMUTABLEIMAGESOURCE_H

#include "derivedassetcache.hpp"
#include "imagepayload.hpp"
#include "speculativebudget.hpp"
#include <components/vfs/manager.hpp>

#include <osgDB/Options>
#include <osgDB/Registry>
#include <osgDB/FileNameUtils>
#include <atomic>
#include <compare>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Resource::ImageSource
{
    class SourceBuffer : public std::streambuf
    {
    public:
        explicit SourceBuffer(std::span<const std::uint8_t> bytes)
        {
            if (bytes.size()>static_cast<std::size_t>(std::numeric_limits<off_type>::max()))
                throw std::length_error("Source stream extent overflow");
            char* first=bytes.empty()?&mEmpty:const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
            setg(first,first,first+bytes.size());
        }
    protected:
        pos_type seekoff(off_type off,std::ios_base::seekdir direction,std::ios_base::openmode mode) override
        {
            if (!(mode&std::ios_base::in) || (mode&std::ios_base::out)) return pos_type(off_type(-1));
            const off_type size=egptr()-eback();
            const off_type base=direction==std::ios_base::beg?0:direction==std::ios_base::cur?gptr()-eback()
                :direction==std::ios_base::end?size:-1;
            if (base<0 || off< -base || off>size-base) return pos_type(off_type(-1));
            setg(eback(),eback()+base+off,egptr());
            return pos_type(base+off);
        }
        pos_type seekpos(pos_type position,std::ios_base::openmode mode) override
        { return seekoff(static_cast<off_type>(position),std::ios_base::beg,mode); }
    private:
        char mEmpty=0;
    };
    class SourceStream final : private SourceBuffer,public std::istream
    {
    public:
        explicit SourceStream(std::span<const std::uint8_t> bytes)
            : SourceBuffer(bytes),std::istream(static_cast<SourceBuffer*>(this)) {}
    };
    // Foundation for a future immutable texture factory. No engine factory
    // retires CPU backing through this service yet. Index rebuilds still obey
    // VFS::Manager's quiescent-reader rule; this is not a concurrent VFS index.
    struct Config
    {
        std::uint64_t maximumSourceBytes = 32 * SpeculativeBudget::MiB;
        std::uint64_t maximumDecodedBytes = 128 * SpeculativeBudget::MiB;
        std::uint64_t maximumOpenPeakBytes = 64 * SpeculativeBudget::MiB;
        // This allowance is admission, not proof of every plugin allocation.
        std::uint64_t decoderScratchAllowance = 8 * SpeculativeBudget::MiB;
    };
    struct Layout
    {
        int width, height, depth, internal, format, type, packing, rowLength, origin;
        float pixelAspectRatio;
        std::uint64_t bytes;
        osg::Image::MipmapDataType mipmaps;
        friend auto operator<=>(const Layout&, const Layout&) = default;
        static std::optional<Layout> of(const osg::Image& image)
        {
            const auto extent = imagePayloadExtent(image);
            if (!extent || image.getAllocationMode() == osg::Image::NO_DELETE
                || image.requiresUpdateCall() || image.getDataVariance() != osg::Object::STATIC) return {};
            return Layout{image.s(),image.t(),image.r(),image.getInternalTextureFormat(),
                static_cast<int>(image.getPixelFormat()),static_cast<int>(image.getDataType()),
                static_cast<int>(image.getPacking()),image.getRowLength(),static_cast<int>(image.getOrigin()),
                image.getPixelAspectRatio(),extent.bytes,image.getMipmapLevels()};
        }
    };
    struct Revision
    {
        std::uint64_t provider, indexGeneration;
        std::string path, sourceSha256, decodedSha256, extension, readerClass, options;
        Layout layout;
        // Raw-reader fixtures and actual ImageManager publications have
        // different origin/alpha contracts. These are immutable source values,
        // never inferred from whatever GL allocation currently survives.
        bool imageManagerPostprocess = false, disableFlip = false, killAlpha = false;
        friend auto operator<=>(const Revision&, const Revision&) = default;
    };
    class Authority final
    {
    public:
        explicit Authority(std::shared_ptr<const VFS::Manager> vfs) : mOwnedVfs(std::move(vfs)),mVfs(mOwnedVfs.get())
        { issue(); }
        // ImageManager borrows the engine VFS just like its ordinary reader.
        // It must close this authority before its owner can destroy/rebuild
        // that VFS. Recipes retain the closed authority, not a fake shared_ptr
        // that pretends to extend the provider's lifetime.
        explicit Authority(const VFS::Manager* vfs) : mVfs(vfs) { issue(); }
        void close() { std::lock_guard lock(mMutex);mVfs=nullptr;mOwnedVfs.reset(); }
        std::uint64_t identity() const noexcept { return mIdentity; }
        bool current(std::uint64_t generation) const
        { std::lock_guard lock(mMutex); return mVfs && mVfs->getIndexGeneration()==generation; }
    private:
        void issue()
        {
            if (!mVfs) throw std::invalid_argument("Missing immutable source provider");
            static std::atomic_uint64_t issued{0};
            auto value=issued.load();
            do { if (value==std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("Image source provider identity exhausted"); }
            while (!issued.compare_exchange_weak(value,value+1));
            mIdentity=value+1;
        }
    public:
        struct Snapshot
        {
            SpeculativeBudget::Stage reservation; // Buffer dies before admission.
            DerivedAssets::Bytes bytes;
            std::uint64_t generation=0;
        };
        std::optional<Snapshot> snapshot(VFS::Path::NormalizedView path, SpeculativeBudget* budget,
            const OpenGlPressureSample& sample, const Config& config, std::uint64_t expectedGeneration=0) const
        {
            if (!budget || !config.maximumSourceBytes || !config.maximumDecodedBytes) return {};
            std::lock_guard lock(mMutex); // close cannot destroy a provider behind an open reader.
            if (!mVfs || !mVfs->getIndexGeneration()
                || (expectedGeneration && mVfs->getIndexGeneration()!=expectedGeneration)) return {};
            const auto estimate=mVfs->getOpenEstimate(path);
            if (!estimate || !estimate->sourceBytes || estimate->sourceBytes>config.maximumSourceBytes
                || estimate->openPeakBytes>config.maximumOpenPeakBytes
                || estimate->sourceBytes>std::numeric_limits<std::size_t>::max()
                || estimate->sourceBytes>static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) return {};
            std::uint64_t peak=estimate->openPeakBytes;
            const auto add=[&](std::uint64_t bytes) {
                if (bytes>std::numeric_limits<std::uint64_t>::max()-peak) return false;
                peak+=bytes; return true;
            };
            // Admit the bounded source first. Reserve decode work separately
            // from its checked header extent, not a maximum-size image for
            // every small asset (which would make the default child cap inert).
            if (!add(estimate->sourceBytes)) return {};
            auto reservation=budget->reserve(sample,peak,SpeculativePriority::Background,true);
            if (reservation.future()!=peak) return {};
            auto input=mVfs->get(path);
            if (!input) return {};
            Snapshot result{std::move(reservation),DerivedAssets::Bytes(static_cast<std::size_t>(estimate->sourceBytes)),
                mVfs->getIndexGeneration()};
            input->read(reinterpret_cast<char*>(result.bytes.data()),static_cast<std::streamsize>(result.bytes.size()));
            if (!*input || input->peek()!=std::char_traits<char>::eof() || input->bad()) return {};
            return result;
        }
    private:
        mutable std::mutex mMutex;
        std::shared_ptr<const VFS::Manager> mOwnedVfs;
        const VFS::Manager* mVfs = nullptr;
        std::uint64_t mIdentity=0;
    };
    inline std::string payloadDigest(const osg::Image& image,const Layout& layout)
    {
        return DerivedAssets::digest(std::span(image.data(),static_cast<std::size_t>(layout.bytes)));
    }
    class Recipe final
    {
    public:
        struct Decoded
        {
            SpeculativeBudget::Stage reservation;
            osg::ref_ptr<osg::Image> image; // Owned image dies before its reservation.
            // A future factory must transfer both owner and charge, not extract
            // an uncharged long-lived Image and drop this lease.
            explicit operator bool() const noexcept { return image.valid(); }
        };
        static std::shared_ptr<const Recipe> capture(std::shared_ptr<Authority> authority,
            VFS::Path::NormalizedView path,const osg::Image& expected,SpeculativeBudget* budget,
            const OpenGlPressureSample& sample,Config config={},std::string options={}) noexcept
        {
            try
            {
                if (!authority || path.value().size()>1024 || options.size()>1024) return {};
                auto layout=Layout::of(expected);
                if (!layout || layout->bytes>config.maximumDecodedBytes) return {};
                const auto extension=osgDB::getLowerCaseFileExtension(std::string(path.value()));
                if (extension!="dds" && extension!="tga") return {}; // Unknown plugin stays ordinary.
                auto source=authority->snapshot(path,budget,sample,config);
                if (!source) return {};
                const auto bound=preflight(source->bytes,extension,config);
                if (!bound || layout->bytes>bound) return {};
                auto decodedReservation=reserveDecode(budget,sample,bound,config);
                if (!decodedReservation.future()) return {};
                auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension(extension);
                if (!reader) return {};
                Revision revision{authority->identity(),source->generation,std::string(path.value()),
                    DerivedAssets::digest(source->bytes),payloadDigest(expected,*layout),extension,reader->className(),
                    std::move(options),std::move(*layout)};
                auto recipe=std::shared_ptr<Recipe>(new Recipe(std::move(authority),std::move(revision),config));
                auto decoded=recipe->decode(source->bytes,*reader);
                if (!decoded || !recipe->matches(*decoded) || !recipe->mAuthority->current(source->generation)) return {};
                return recipe;
            }
            catch (...) { return {}; } // Optional refusal leaves ordinary owner/source intact.
        }
        const Revision& revision() const noexcept { return mRevision; }
        // The actual required decoder already owns this winning source
        // snapshot and postprocessed image. Seal them without another VFS open
        // or decode. No CPU-backing retirement permission is issued here.
        static std::shared_ptr<const Recipe> adoptPublication(std::shared_ptr<Authority> authority,
            VFS::Path::NormalizedView path,const Authority::Snapshot& source,const osg::Image& image,
            osgDB::ReaderWriter& reader,Config config,std::string options,bool disableFlip,bool killAlpha) noexcept
        {
            try
            {
                if (!authority || path.value().size()>1024 || options.size()>1024
                    || !source.reservation.future() || !authority->current(source.generation)) return {};
                const auto layout=Layout::of(image);
                const auto extension=osgDB::getLowerCaseFileExtension(std::string(path.value()));
                const auto normalized=extension=="targa"?std::string("tga"):extension;
                const auto bound=preflight(source.bytes,normalized,config);
                if (!layout || !bound || layout->bytes>bound || layout->bytes>config.maximumDecodedBytes) return {};
                Revision revision{authority->identity(),source.generation,std::string(path.value()),
                    DerivedAssets::digest(source.bytes),payloadDigest(image,*layout),normalized,reader.className(),
                    std::move(options),*layout,true,disableFlip,killAlpha};
                return std::shared_ptr<Recipe>(new Recipe(std::move(authority),std::move(revision),config));
            }
            catch (...) { return {}; }
        }
        static std::uint64_t decodedBound(const Authority::Snapshot& source,std::string_view extension,const Config& config)
        { return preflight(source.bytes,extension=="targa"?std::string_view("tga"):extension,config); }
        static SpeculativeBudget::Stage admitDecode(SpeculativeBudget* budget,const OpenGlPressureSample& sample,
            std::uint64_t bound,const Config& config)
        { return bound?reserveDecode(budget,sample,bound,config):SpeculativeBudget::Stage{}; }
        bool matches(const osg::Image& image) const
        {
            const auto layout=Layout::of(image);
            return layout && *layout==mRevision.layout && payloadDigest(image,*layout)==mRevision.decodedSha256;
        }
        Decoded reconstruct(SpeculativeBudget* budget,const OpenGlPressureSample& sample) const noexcept
        {
            try
            {
                auto source=mAuthority->snapshot(VFS::Path::Normalized(mRevision.path),budget,sample,mConfig,
                    mRevision.indexGeneration);
                if (!source || DerivedAssets::digest(source->bytes)!=mRevision.sourceSha256) return {};
                const auto bound=preflight(source->bytes,mRevision.extension,mConfig);
                if (!bound || mRevision.layout.bytes>bound) return {};
                auto decodedReservation=reserveDecode(budget,sample,bound,mConfig);
                if (!decodedReservation.future()) return {};
                auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension(mRevision.extension);
                if (!reader || std::string_view(reader->className())!=mRevision.readerClass) return {};
                auto image=decode(source->bytes,*reader);
                if (!image || !matches(*image) || !mAuthority->current(mRevision.indexGeneration)) return {};
                // Source bytes and their separate reservation die locally.
                // Only the output/decode grant survives with the image lease.
                return {std::move(decodedReservation),std::move(image)};
            }
            catch (...) { return {}; }
        }
    private:
        Recipe(std::shared_ptr<Authority> authority,Revision revision,Config config)
            : mAuthority(std::move(authority)),mRevision(std::move(revision)),mConfig(config) {}
        static std::uint32_t le32(const DerivedAssets::Bytes& bytes,std::size_t offset)
        {
            std::uint32_t result=0;
            for (unsigned i=0;i<4;++i) result|=std::uint32_t(bytes[offset+i])<<(8*i);
            return result;
        }
        static SpeculativeBudget::Stage reserveDecode(SpeculativeBudget* budget,const OpenGlPressureSample& sample,
            std::uint64_t bound,const Config& config)
        {
            if (!budget || bound>(std::numeric_limits<std::uint64_t>::max()-config.decoderScratchAllowance)/2) return {};
            return budget->reserve(sample,bound*2+config.decoderScratchAllowance,SpeculativePriority::Background,true);
        }
        static std::uint64_t preflight(const DerivedAssets::Bytes& bytes,std::string_view extension,const Config& config)
        {
            // Conservative predecoder dimension envelope. Volume/array/cubemap,
            // DX10, custom plugins and unsupported layouts keep original backing.
            // This constrains source/decoded extents, not all opaque plugin scratch.
            std::uint64_t width=0,height=0,blockBytes=0;
            unsigned levels=1;
            if (extension=="dds")
            {
                if (bytes.size()<128 || std::memcmp(bytes.data(),"DDS ",4) || le32(bytes,4)!=124
                    || le32(bytes,76)!=32 || le32(bytes,24)>1 || le32(bytes,28)>32 || le32(bytes,112)!=0
                    || le32(bytes,84)==0x30315844) return 0; // DX10.
                height=le32(bytes,12); width=le32(bytes,16);
                unsigned maximumLevels=1;
                for (auto extent=std::max(width,height);extent>1;extent>>=1) ++maximumLevels;
                if (le32(bytes,28)>maximumLevels) return 0;
                levels=std::max(1u,le32(bytes,28));
                if (le32(bytes,80)&4) // Known installed DDS FOURCC layouts only.
                {
                    const auto format=le32(bytes,84);
                    if (format!=0x31545844 && format!=0x33545844 && format!=0x35545844
                        && format!=0x31495441 && format!=0x32495441) return 0;
                    blockBytes=format==0x31545844 || format==0x31495441?8:16;
                }
                else if (const auto bits=le32(bytes,88);bits!=8 && bits!=16 && bits!=24 && bits!=32)
                    return 0;
            }
            else if (extension=="tga")
            {
                if (bytes.size()<18 || bytes[1]!=0 || (bytes[2]!=2 && bytes[2]!=3 && bytes[2]!=10 && bytes[2]!=11)
                    || (bytes[16]!=8 && bytes[16]!=16 && bytes[16]!=24 && bytes[16]!=32)) return 0;
                width=std::uint32_t(bytes[12])|(std::uint32_t(bytes[13])<<8);
                height=std::uint32_t(bytes[14])|(std::uint32_t(bytes[15])<<8);
            }
            if (!width || !height || width>8192 || height>8192) return 0;
            std::uint64_t bound=0;
            for (unsigned level=0;level<levels;++level)
            {
                // Installed block DDS stays compressed in the CPU image.
                // Raw DDS/TGA allows conservative RGBA conversion/padded rows.
                const auto bytesAtLevel=blockBytes?((width+3)/4)*((height+3)/4)*blockBytes:width*height*4;
                if (bytesAtLevel>config.maximumDecodedBytes || bound>config.maximumDecodedBytes-bytesAtLevel) return 0;
                bound+=bytesAtLevel; width=std::max(std::uint64_t{1},width/2);height=std::max(std::uint64_t{1},height/2);
            }
            return bound;
        }
        osg::ref_ptr<osg::Image> decode(const DerivedAssets::Bytes& bytes,osgDB::ReaderWriter& reader) const
        {
            SourceStream stream(bytes);
            osg::ref_ptr<osgDB::Options> options=new osgDB::Options(mRevision.options);
            auto result=reader.readImage(stream,options);
            if (!result.success()) return {};
            osg::ref_ptr<osg::Image> image=result.takeImage();
            if (!image) return {};
            if (mRevision.imageManagerPostprocess)
            {
                // Match the actual successful ImageManager publication. A
                // software-decompressed source is deliberately not admitted
                // by adoptPublication's original checked block bound.
                if (mRevision.killAlpha)
                {
                    osg::ref_ptr<osg::Image> rgb=new osg::Image;
                    rgb->setOrigin(image->getOrigin());
                    rgb->allocateImage(image->s(),image->t(),image->r(),GL_RGB,GL_UNSIGNED_BYTE);
                    for (int x=0;x<image->s();++x)for (int y=0;y<image->t();++y)for (int z=0;z<image->r();++z)
                        rgb->setColor(image->getColor(x,y,z),x,y,z);
                    image=std::move(rgb);
                }
                if (mRevision.extension=="dds") image->setOrigin(osg::Image::TOP_LEFT);
                if (image->getOrigin()==osg::Image::BOTTOM_LEFT && !mRevision.disableFlip)
                { image->flipVertical();image->setOrigin(osg::Image::TOP_LEFT); }
                image->setFileName(mRevision.path);
                // Actual decoder variance is part of this engagement gate. Do
                // not blanket-label arbitrary file images STATIC to pass it.
            }
            return image;
        }
        const std::shared_ptr<Authority> mAuthority;
        const Revision mRevision;
        const Config mConfig;
    };
}
#endif
