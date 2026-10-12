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
                extent.bytes,image.getMipmapLevels()};
        }
    };
    struct Revision
    {
        std::uint64_t provider, indexGeneration;
        std::string path, sourceSha256, decodedSha256, extension, readerClass, options;
        Layout layout;
        friend auto operator<=>(const Revision&, const Revision&) = default;
    };
    class Authority final
    {
    public:
        explicit Authority(std::shared_ptr<const VFS::Manager> vfs) : mVfs(std::move(vfs))
        {
            if (!mVfs) throw std::invalid_argument("Missing immutable source provider");
            static std::atomic_uint64_t issued{0};
            auto value=issued.load();
            do { if (value==std::numeric_limits<std::uint64_t>::max())
                throw std::overflow_error("Image source provider identity exhausted"); }
            while (!issued.compare_exchange_weak(value,value+1));
            mIdentity=value+1;
        }
        void close() { std::lock_guard lock(mMutex); mVfs.reset(); }
        std::uint64_t identity() const noexcept { return mIdentity; }
        bool current(std::uint64_t generation) const
        { std::lock_guard lock(mMutex); return mVfs && mVfs->getIndexGeneration()==generation; }
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
            if (!add(estimate->sourceBytes) || !add(config.maximumDecodedBytes)
                || !add(config.maximumDecodedBytes) || !add(config.decoderScratchAllowance)) return {};
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
        std::shared_ptr<const VFS::Manager> mVfs;
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
                if (!source || !preflight(source->bytes,extension,config)) return {};
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
                if (!source || DerivedAssets::digest(source->bytes)!=mRevision.sourceSha256
                    || !preflight(source->bytes,mRevision.extension,mConfig)) return {};
                auto* reader=osgDB::Registry::instance()->getReaderWriterForExtension(mRevision.extension);
                if (!reader || std::string_view(reader->className())!=mRevision.readerClass) return {};
                auto image=decode(source->bytes,*reader);
                if (!image || !matches(*image) || !mAuthority->current(mRevision.indexGeneration)) return {};
                return {std::move(source->reservation),std::move(image)};
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
        static bool preflight(const DerivedAssets::Bytes& bytes,std::string_view extension,const Config& config)
        {
            // Conservative predecoder dimension envelope. Volume/array/cubemap,
            // DX10, custom plugins and unsupported layouts keep original backing.
            // This constrains source/decoded extents, not all opaque plugin scratch.
            std::uint64_t width=0,height=0,factor=0;
            if (extension=="dds")
            {
                if (bytes.size()<128 || std::memcmp(bytes.data(),"DDS ",4) || le32(bytes,4)!=124
                    || le32(bytes,76)!=32 || le32(bytes,24)>1 || le32(bytes,28)>32 || le32(bytes,112)!=0
                    || le32(bytes,84)==0x30315844) return false; // DX10.
                height=le32(bytes,12); width=le32(bytes,16);
                unsigned maximumLevels=1;
                for (auto extent=std::max(width,height);extent>1;extent>>=1) ++maximumLevels;
                if (le32(bytes,28)>maximumLevels) return false;
                if (le32(bytes,80)&4) // Known installed DDS FOURCC layouts only.
                {
                    const auto format=le32(bytes,84);
                    if (format!=0x31545844 && format!=0x33545844 && format!=0x35545844
                        && format!=0x31495441 && format!=0x32495441) return false;
                }
                else if (const auto bits=le32(bytes,88);bits!=8 && bits!=16 && bits!=24 && bits!=32)
                    return false;
                factor=32; // All mip levels plus conservative supported raw conversion.
            }
            else if (extension=="tga")
            {
                if (bytes.size()<18 || bytes[1]!=0 || (bytes[2]!=2 && bytes[2]!=3 && bytes[2]!=10 && bytes[2]!=11)
                    || (bytes[16]!=8 && bytes[16]!=16 && bytes[16]!=24 && bytes[16]!=32)) return false;
                width=std::uint32_t(bytes[12])|(std::uint32_t(bytes[13])<<8);
                height=std::uint32_t(bytes[14])|(std::uint32_t(bytes[15])<<8);
                factor=4;
            }
            return width && height && width<=8192 && height<=8192
                && width<=config.maximumDecodedBytes/factor/height;
        }
        osg::ref_ptr<osg::Image> decode(const DerivedAssets::Bytes& bytes,osgDB::ReaderWriter& reader) const
        {
            SourceStream stream(bytes);
            osg::ref_ptr<osgDB::Options> options=new osgDB::Options(mRevision.options);
            auto result=reader.readImage(stream,options);
            osg::ref_ptr<osg::Image> image=result.takeImage();
            if (image && !image->requiresUpdateCall()) image->setDataVariance(osg::Object::STATIC);
            return image;
        }
        const std::shared_ptr<Authority> mAuthority;
        const Revision mRevision;
        const Config mConfig;
    };
}
#endif
