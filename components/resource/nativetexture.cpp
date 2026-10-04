#include "nativetexture.hpp"
#include "assetpolicy.hpp"
#include "derivedassetcache.hpp"
#include "uploadpool.hpp"
#include <osg/Texture>
#include <components/debug/hitchtrace.hpp>
#include <components/vfs/manager.hpp>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <limits>
#include <optional>
#ifdef OPENMW_HAVE_DIRECTXTEX
#include <DirectXTex.h>
#endif

namespace Resource
{
    namespace
    {
        constexpr std::uint64_t MiB = 1024ull * 1024;
        constexpr std::uint64_t SourceLimit = 64 * MiB, RetainedLimit = 64 * MiB;
        NativeTextureStats sStats;
        unsigned producerPriority(const NativeTextureTicket& ticket)
        {
            if(ticket.purpose==TexturePurpose::MainDraw
                && (ticket.role==TextureRole::Diffuse || ticket.role==TextureRole::DiffuseSpecular)) return 0;
            return ticket.purpose==TexturePurpose::CompositeSource ? 1 : 2;
        }
        struct Retained
        {
            std::uint64_t bytes;
            SpeculativeBudget::ChargePtr charge;
            ~Retained() { sStats.retainedBytes.fetch_sub(bytes); }
        };
        bool retain(NativeMipPayload& value)
        {
            const auto bytes = value.bytes.size();
            auto live = sStats.retainedBytes.load();
            do { if (bytes > RetainedLimit || live > RetainedLimit - bytes) return false; }
            while (!sStats.retainedBytes.compare_exchange_weak(live, live + bytes));
            try
            {
                auto charge = std::make_shared<Retained>();
                charge->bytes = bytes;
                value.retainedCharge = charge;
                if (SpeculativeScope::active() && UploadPool::ledger)
                    charge->charge = SpeculativeScope::track(UploadPool::ledger, {charge.get(), 0x54314d49u}, bytes, true);
                return true;
            }
            catch (...)
            {
                if (!value.retainedCharge) sStats.retainedBytes.fetch_sub(bytes);
                throw;
            }
        }
        std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t p)
        {
            if (p > bytes.size() || bytes.size() - p < 4) return 0;
            return std::uint32_t(bytes[p]) | (std::uint32_t(bytes[p+1]) << 8)
                | (std::uint32_t(bytes[p+2]) << 16) | (std::uint32_t(bytes[p+3]) << 24);
        }
        std::uint64_t bc1Bytes(unsigned width,unsigned height)
        {
            std::uint64_t bytes=0;
            for(;;)
            {
                bytes+=std::uint64_t((width+3)/4)*((height+3)/4)*8;
                if(width==1 && height==1) return bytes;
                width=(std::max)(1u,width/2);height=(std::max)(1u,height/2);
            }
        }
        std::uint64_t preparationPeak(std::span<const std::uint8_t> header,std::uint64_t sourceBytes)
        {
            // The source snapshot and native block copy coexist. A native DDS
            // cache entry is only a bounded metadata manifest, not another DDS.
            if(u32(header,0)==0x20534444u) return sourceBytes*2+64*1024+DerivedAssets::MetadataScratchBytes;
            if(header.size()>=18)
            {
                const unsigned width=header[12]|(unsigned(header[13])<<8),height=header[14]|(unsigned(header[15])<<8);
                if(width && height && width<=2048 && height<=2048)
                {
                    const std::uint64_t rgba=std::uint64_t(width)*height*4,native=bc1Bytes(width,height);
                    // CPU DirectXTex has decoded base + complete RGBA chain,
                    // codec scratch, compressed chain, DDS blob and copied
                    // immutable payload. The bounded cache-hit branch uses at
                    // most two native copies and never coexists with encoding.
                    return sourceBytes+rgba*4+native*3+256*1024+DerivedAssets::MetadataScratchBytes;
                }
            }
            return sourceBytes*2+64*1024+DerivedAssets::MetadataScratchBytes;
        }
        std::string recipe(TextureRole role, TexturePurpose purpose, bool convert)
        {
            return std::string("native-texture-v1|top-left|unorm|complete|role=")
                + std::to_string(static_cast<unsigned>(role)) + "|purpose="
                + std::to_string(static_cast<unsigned>(purpose))
                + (convert ? "|directxtex-4feb3e11a020f35b796fc769a74216a555d4f5ef|ignore-tga2|opaque-bc1-box-wrap-cpu"
                           : "|legacy-dds-block-reuse");
        }
        std::shared_ptr<NativeMipPayload> parseDds(std::span<const std::uint8_t> source,
            std::string_view path, TextureRole role, TexturePurpose purpose, NativeTextureSkip& skip)
        {
            if (source.size() < 128 || u32(source,0) != 0x20534444u || u32(source,4) != 124
                || (u32(source,8)&0x1007u)!=0x1007u || u32(source,76) != 32 || !(u32(source,80) & 4)
                || !(u32(source,108)&0x1000u))
            { skip = NativeTextureSkip::Format; return {}; }
            // Legacy 2D only. Arrays/DX10/BC7 never enter the conversion path.
            if (u32(source,24) > 1 || (u32(source,112) & (0xfe00u | 0x200000u)))
            { skip = NativeTextureSkip::Format; return {}; }
            const auto width = u32(source,16), height = u32(source,12);
            if (!width || !height || width > 16384 || height > 16384)
            { skip = NativeTextureSkip::Dimensions; return {}; }
            unsigned format = 0, block = 16;
            switch (u32(source,84))
            {
                case 0x31545844u: format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT; block = 8; break;
                case 0x33545844u: format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT; break;
                case 0x35545844u: format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT; break;
                default: skip = NativeTextureSkip::Format; return {};
            }
            unsigned levels = 1;
            for (auto s = (std::max)(width,height); s > 1; s >>= 1) ++levels;
            if ((levels>1 && !(u32(source,8)&0x20000u))
                || (u32(source,28) != levels && !(levels==1 && u32(source,28)==0)))
            { skip = NativeTextureSkip::IncompleteMips; return {}; }
            auto result = std::make_shared<NativeMipPayload>();
            result->format = format; result->role = role; result->purpose = purpose;
            result->path = path;
            std::uint64_t offset = 0;
            auto w = width, h = height;
            for (unsigned i=0; i<levels; ++i)
            {
                const std::uint64_t bytes = std::uint64_t((w+3)/4) * ((h+3)/4) * block;
                if (bytes > SourceLimit || offset > SourceLimit - bytes || 128 + offset + bytes > source.size())
                { skip = NativeTextureSkip::IncompleteMips; return {}; }
                result->mips.push_back({w,h,static_cast<std::uint32_t>(offset),static_cast<std::uint32_t>(bytes)});
                offset += bytes; w = (std::max)(1u,w/2); h = (std::max)(1u,h/2);
            }
            result->bytes.assign(source.begin()+128,source.begin()+128+static_cast<std::ptrdiff_t>(offset));
            if(format==GL_COMPRESSED_RGBA_S3TC_DXT1_EXT)
            {
                // Match the installed ordinary DDS reader's
                // dds_dxt1_detect_rgba option using the same OSG predicate.
                // Header alpha flags and padding alone are not color semantics.
                osg::ref_ptr<osg::Image> detector=new osg::Image;
                detector->setImage(width,height,1,format,format,GL_UNSIGNED_BYTE,
                    result->bytes.data(),osg::Image::NO_DELETE,1);
                osg::Image::MipmapDataType offsets;
                for(std::size_t i=1;i<result->mips.size();++i) offsets.push_back(result->mips[i].offset);
                detector->setMipmapLevels(offsets);
                if(!detector->isImageTranslucent()) result->format=GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
            }
            if (!retain(*result)) { skip = NativeTextureSkip::RetainedLimit; return {}; }
            skip = NativeTextureSkip::None;
            return result;
        }
        class NativeImage final : public osg::Image
        {
        public:
            NativeImage() = default;
            explicit NativeImage(std::shared_ptr<const NativeMipPayload> data) : mData(std::move(data))
            {
                if(!mData || mData->mips.empty()) return;
                const auto& first=mData->mips.front();
                setImage(first.width, first.height, 1, mData->format, mData->format, GL_UNSIGNED_BYTE,
                    const_cast<unsigned char*>(mData->bytes.data()), osg::Image::NO_DELETE, 1);
                osg::Image::MipmapDataType offsets;
                for (std::size_t i=1; i<mData->mips.size(); ++i) offsets.push_back(mData->mips[i].offset);
                setMipmapLevels(offsets); setFileName(mData->path); setOrigin(osg::Image::TOP_LEFT);
                setDataVariance(osg::Object::STATIC);
            }
            NativeImage(const NativeImage& other, const osg::CopyOp& = osg::CopyOp::SHALLOW_COPY)
                : NativeImage(other.mData) {}
            META_Object(Resource, NativeImage)
        private:
            const std::shared_ptr<const NativeMipPayload> mData;
        };
    }
    NativeTextureStats& nativeTextureStats() { return sStats; }
    const char* nativeTextureSkipName(NativeTextureSkip reason)
    {
        static constexpr const char* names[] = {"none","disabled","queue","host_admission","source_size",
            "source_read","stale","format","incomplete_mips","dimensions","role","backend","encode",
            "retained_limit","cache","source_provider","purpose","ordinary_allocation"};
        const auto index=static_cast<unsigned>(reason);
        return index<std::size(names) ? names[index] : "unknown";
    }
    std::string nativeTextureKey(std::string_view path, TextureRole role, TexturePurpose purpose)
    {
        return std::string(path) + "|role=" + std::to_string(static_cast<unsigned>(role))
            + "|purpose=" + std::to_string(static_cast<unsigned>(purpose));
    }
    std::size_t NativeMipPayload::suffixBytes(unsigned first) const noexcept
    { return first < mips.size() ? bytes.size() - mips[first].offset : 0; }
    unsigned NativeMipPayload::coarseLevel() const noexcept
    {
        unsigned level=0;
        while (level+1<mips.size() && (mips[level].width>256 || mips[level].height>256)) ++level;
        return level;
    }
    osg::ref_ptr<osg::Image> nativeTextureImage(std::shared_ptr<const NativeMipPayload> data)
    { return data && !data->mips.empty() ? new NativeImage(std::move(data)) : nullptr; }

    std::shared_ptr<NativeMipPayload> prepareNativeTexture(std::span<const std::uint8_t> source,
        std::string_view path, TextureRole role, TexturePurpose purpose, NativeTextureSkip& skip)
    {
        if (source.empty() || source.size()>SourceLimit) { skip=NativeTextureSkip::SourceSize; return {}; }
        const auto digest=DerivedAssets::digest(source);
        if (u32(source,0)==0x20534444u)
        {
            if(!path.ends_with(".dds")) { skip=NativeTextureSkip::Format;return {}; }
            auto result=parseDds(source,path,role,purpose,skip);
            if (!result) return {};
            result->sourceDigest=digest;
            result->recipeKey=DerivedAssets::key(digest,recipe(role,purpose,false));
            // Metadata only: suitable native source blocks are not duplicated on
            // disk. Revalidate source/table on each new source snapshot.
            const std::string manifest="native-dds-v1|"+digest+"|"+DerivedAssets::digest(result->bytes)
                +"|"+std::to_string(result->format)+"|"+std::to_string(result->mips.size());
            auto existing=DerivedAssets::cache().read(result->recipeKey,4096);
            if (existing && std::string(existing->begin(),existing->end())==manifest) ++sStats.cacheHits;
            else
            {
                ++sStats.cacheMisses;
                if (DerivedAssets::cache().write(result->recipeKey,
                    std::span(reinterpret_cast<const std::uint8_t*>(manifest.data()),manifest.size()))) ++sStats.cacheWrites;
                else ++sStats.skips[static_cast<unsigned>(NativeTextureSkip::Cache)];
            }
            ++sStats.reusedDds; ++sStats.eligible;
            return result;
        }
        // Composite baking consumes exact source colors. Only the DDS branch
        // above can reuse unchanged blocks; lossy conversion stays MainDraw.
        if (purpose!=TexturePurpose::MainDraw) { skip=NativeTextureSkip::Purpose; return {}; }
        // Explicit opaque color contract: 24-bit true-color TGA only. Alpha
        // data, normal maps, unknown readers and the legacy 16-bit alpha rule
        // remain on their exact existing loader paths.
        if (role!=TextureRole::Diffuse) { skip=NativeTextureSkip::Role; return {}; }
        if (source.size()<18 || source[1]!=0 || (source[2]!=2 && source[2]!=10) || source[16]!=24
            || !(path.ends_with(".tga") || path.ends_with(".targa")))
        { skip=NativeTextureSkip::Format; return {}; }
        const unsigned width=source[12]|(unsigned(source[13])<<8),height=source[14]|(unsigned(source[15])<<8);
        if (!width || !height || width>2048 || height>2048)
        { skip=NativeTextureSkip::Dimensions; return {}; }
#ifdef OPENMW_HAVE_DIRECTXTEX
        const auto cacheKey=DerivedAssets::key(digest,recipe(role,purpose,true));
        if (auto cached=DerivedAssets::cache().read(cacheKey,static_cast<std::size_t>(bc1Bytes(width,height)+128)))
        {
            auto result=parseDds(*cached,path,role,purpose,skip);
            if (result && result->mips.front().width==width && result->mips.front().height==height
                && result->format==GL_COMPRESSED_RGB_S3TC_DXT1_EXT)
            {
                result->sourceDigest=digest; result->recipeKey=cacheKey; result->converted=true;
                ++sStats.cacheHits; ++sStats.eligible;
                return result;
            }
        }
        ++sStats.cacheMisses;
        DirectX::ScratchImage decoded,mips,compressed;
        if (FAILED(DirectX::LoadFromTGAMemory(source.data(),source.size(),DirectX::TGA_FLAGS_IGNORE_SRGB,nullptr,decoded)))
        { skip=NativeTextureSkip::Encode; return {}; }
        const auto* image=decoded.GetImage(0,0,0);
        if (!image || image->width!=width || image->height!=height || image->format!=DXGI_FORMAT_R8G8B8A8_UNORM)
        { skip=NativeTextureSkip::Format; return {}; }
        for (unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x)
            if(image->pixels[y*image->rowPitch+x*4+3]!=255) { skip=NativeTextureSkip::Role; return {}; }
        const auto filter=static_cast<DirectX::TEX_FILTER_FLAGS>(DirectX::TEX_FILTER_BOX
            |DirectX::TEX_FILTER_WRAP|DirectX::TEX_FILTER_FORCE_NON_WIC);
        if (FAILED(DirectX::GenerateMipMaps(*image,filter,0,mips))
            || FAILED(DirectX::Compress(mips.GetImages(),mips.GetImageCount(),mips.GetMetadata(),
                DXGI_FORMAT_BC1_UNORM,DirectX::TEX_COMPRESS_DEFAULT,1.0f,compressed)))
        { skip=NativeTextureSkip::Encode; return {}; }
        DirectX::Blob blob;
        if (FAILED(DirectX::SaveToDDSMemory(compressed.GetImages(),compressed.GetImageCount(),compressed.GetMetadata(),
            DirectX::DDS_FLAGS_FORCE_DX9_LEGACY,blob))) { skip=NativeTextureSkip::Encode; return {}; }
        auto encoded=std::span(static_cast<const std::uint8_t*>(blob.GetBufferPointer()),blob.GetBufferSize());
        auto result=parseDds(encoded,path,role,purpose,skip);
        if (!result) return {};
        result->sourceDigest=digest; result->recipeKey=cacheKey; result->converted=true;
        if (DerivedAssets::cache().write(cacheKey,encoded)) ++sStats.cacheWrites;
        else ++sStats.skips[static_cast<unsigned>(NativeTextureSkip::Cache)];
        ++sStats.converted; ++sStats.eligible;
        return result;
#else
        skip=NativeTextureSkip::Backend; return {};
#endif
    }

    NativeTextureTicket::NativeTextureTicket(std::shared_ptr<NativeTextureEpoch> epoch,std::uint64_t value,
        std::uint64_t generation,std::string name,TextureRole r,TexturePurpose p)
        : path(std::move(name)),role(r),purpose(p),vfsGeneration(generation),mEpoch(std::move(epoch)),mExpectedEpoch(value) {}
    bool NativeTextureTicket::current() const noexcept
    { return mEpoch->alive.load(std::memory_order_acquire) && mEpoch->value.load(std::memory_order_acquire)==mExpectedEpoch; }
    std::shared_ptr<const NativeMipPayload> NativeTextureTicket::payload() const
    { std::lock_guard lock(mMutex); return current() ? mPayload : nullptr; }
    void NativeTextureTicket::publish(std::shared_ptr<const NativeMipPayload> value)
    {
        std::lock_guard lock(mMutex);
        if(current() && !ordinaryAllocated.load())
        { mPayload=std::move(value);mReason=NativeTextureSkip::None; mOutcome=NativeTextureOutcome::Ready; ++sStats.ready; }
        else if(!current()) ++sStats.skips[static_cast<unsigned>(NativeTextureSkip::Stale)];
    }
    void NativeTextureTicket::decline(NativeTextureSkip reason)
    {
        std::lock_guard lock(mMutex);
        ++sStats.skips[static_cast<unsigned>(reason)];mReason=reason;
        const bool temporary=reason==NativeTextureSkip::Queue || reason==NativeTextureSkip::HostAdmission
            || reason==NativeTextureSkip::RetainedLimit;
        const bool retry=temporary && mAttempts<3 && current() && !ordinaryAllocated.load();
        mOutcome=retry ? NativeTextureOutcome::TransientDecline : NativeTextureOutcome::TerminalDecline;
        mRetryAt=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count())+100;
        if(retry) ++sStats.transientDeclines;else ++sStats.terminalDeclines;
    }
    void NativeTextureTicket::markOrdinary()
    {
        if(ordinaryAllocated.exchange(true)) return;
        std::lock_guard lock(mMutex);
        mPayload.reset();mReason=NativeTextureSkip::OrdinaryAllocation;
        mOutcome=NativeTextureOutcome::TerminalDecline;
        ++sStats.terminalDeclines;++sStats.skips[static_cast<unsigned>(NativeTextureSkip::OrdinaryAllocation)];
    }
    NativeTextureOutcome NativeTextureTicket::outcome() const noexcept
    { return current() ? mOutcome.load(std::memory_order_acquire) : NativeTextureOutcome::TerminalDecline; }
    bool NativeTextureTicket::beginAttempt()
    {
        std::lock_guard lock(mMutex);
        const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if(!current() || ordinaryAllocated.load() || mOutcome!=NativeTextureOutcome::TransientDecline
            || now<mRetryAt || mAttempts>=3) return false;
        if(mAttempts++) ++sStats.retries;
        mOutcome=NativeTextureOutcome::Pending;return true;
    }
    NativeTexturePreparation::NativeTexturePreparation(const VFS::Manager* vfs) : mVfs(vfs),mWorker([this]{run();}) {}
    NativeTexturePreparation::~NativeTexturePreparation()
    {
        { std::lock_guard lock(mMutex); mStopping=true; mEpoch->alive=false; mQueue.clear(); }
        mCondition.notify_all(); mWorker.join();
    }
    void NativeTexturePreparation::clear()
    {
        std::lock_guard lock(mMutex); ++mEpoch->value; mQueue.clear(); mTickets.clear();
    }
    std::shared_ptr<NativeTextureTicket> NativeTexturePreparation::request(VFS::Path::NormalizedView path,
        TextureRole role,TexturePurpose purpose)
    {
        ++sStats.requested;
        std::lock_guard lock(mMutex);
        const auto key=nativeTextureKey(path.value(),role,purpose);
        std::shared_ptr<NativeTextureTicket> ticket;
        if(auto found=mTickets.find(key); found!=mTickets.end())
            if(found->second->current() && found->second->vfsGeneration==mVfs->getIndexGeneration()) ticket=found->second;
        if(!ticket)
        {
            // A queue refusal must still leave a stable producer/cache handle.
            // Strong pending handles also keep their immutable completion alive.
            if(mTickets.size()>64)
                for(auto it=mTickets.begin();it!=mTickets.end();)
                    if(it->second.use_count()==1 && it->second->outcome()!=NativeTextureOutcome::Pending)
                        it=mTickets.erase(it); else ++it;
            if(mStopping || mTickets.size()>=4096)
            { ++sStats.skips[static_cast<unsigned>(NativeTextureSkip::Queue)];return {}; }
            ticket=std::make_shared<NativeTextureTicket>(mEpoch,mEpoch->value.load(),mVfs->getIndexGeneration(),
                std::string(path.value()),role,purpose);
            mTickets[key]=ticket;
            const bool dds=path.value().ends_with(".dds");
            if(!dds && purpose!=TexturePurpose::MainDraw) ticket->decline(NativeTextureSkip::Purpose);
            else if(!dds && role!=TextureRole::Diffuse) ticket->decline(NativeTextureSkip::Role);
            else if(!dds && !AssetPolicy::enabled(AssetPolicy::TexturePreparation)) ticket->decline(NativeTextureSkip::Disabled);
            else if(!dds && !path.value().ends_with(".tga") && !path.value().ends_with(".targa"))
                ticket->decline(NativeTextureSkip::Format);
            else if(AssetPolicy::enabled(AssetPolicy::TextureStreaming) && !AssetPolicy::enabled(AssetPolicy::TexturePreparation)
                && (purpose!=TexturePurpose::MainDraw || (role!=TextureRole::Diffuse && role!=TextureRole::DiffuseSpecular)))
                ticket->decline(purpose!=TexturePurpose::MainDraw ? NativeTextureSkip::Purpose : NativeTextureSkip::Role);
        }
        if(mQueue.size()<4 && ticket->beginAttempt())
        {
            const auto priority=producerPriority(*ticket);
            const auto position=std::find_if(mQueue.begin(),mQueue.end(),[priority](const auto& other)
                { return producerPriority(*other)>priority; });
            mQueue.insert(position,ticket);mCondition.notify_one();
        }
        else if(mQueue.size()>=4 && ticket->outcome()==NativeTextureOutcome::TransientDecline
            && ticket->reason()==NativeTextureSkip::None) ticket->decline(NativeTextureSkip::Queue);
        return ticket;
    }
    void NativeTexturePreparation::run()
    {
        for(;;)
        {
            std::shared_ptr<NativeTextureTicket> ticket;
            {
                std::unique_lock lock(mMutex);
                mCondition.wait_for(lock,std::chrono::milliseconds(100),[this]{return mStopping || !mQueue.empty();});
                if(mStopping) return;
                // Transient tickets retain a bounded completion route even if
                // the chunk/texture caches stop calling the producer. One job
                // is admitted per wake; no GL/source work occurs under this lock.
                if(mQueue.empty())
                    for(unsigned priority=0;priority<3 && mQueue.empty();++priority)
                        for(const auto& [key,candidate]:mTickets)
                            if(producerPriority(*candidate)==priority && candidate->beginAttempt())
                            { mQueue.push_back(candidate);break; }
                if(mQueue.empty()) continue;
                ticket=std::move(mQueue.front());mQueue.pop_front();
            }
            Debug::HitchTrace::Scope work("resource.native_texture_prepare",{},ticket->path);
            try
            {
                if(!ticket->current() || mVfs->getIndexGeneration()!=ticket->vfsGeneration)
                { ticket->decline(NativeTextureSkip::Stale); continue; }
                const auto estimate=mVfs->getOpenEstimate(VFS::Path::Normalized(ticket->path));
                if(!estimate) { ticket->decline(NativeTextureSkip::SourceProvider); continue; }
                if(!estimate->sourceBytes || estimate->sourceBytes>SourceLimit)
                { ticket->decline(NativeTextureSkip::SourceSize); continue; }
                if(!estimate->openPeakBytes || estimate->openPeakBytes>AssetPolicy::CpuTransientLimit)
                { ticket->decline(NativeTextureSkip::HostAdmission); continue; }
                if(!UploadPool::ledger || !UploadPool::sample)
                { ticket->decline(NativeTextureSkip::HostAdmission); continue; }
                auto job=UploadPool::ledger->tryJob(UploadPool::sample(UploadPool::sampleOwner));
                if(!job) { ticket->decline(NativeTextureSkip::HostAdmission); continue; }
                SpeculativeScope scope(UploadPool::ledger,UploadPool::sample,UploadPool::sampleOwner);
                SpeculativeScope::Stage sourceOpen(estimate->openPeakBytes,0);
                auto stream=mVfs->get(VFS::Path::Normalized(ticket->path));
                stream->seekg(0,std::ios::end); const auto end=stream->tellg(); stream->seekg(0);
                if(end<=0 || static_cast<std::uint64_t>(end)>SourceLimit)
                { ticket->decline(NativeTextureSkip::SourceSize); continue; }
                if(static_cast<std::uint64_t>(end)!=estimate->sourceBytes)
                { ticket->decline(NativeTextureSkip::Stale); continue; }
                std::array<std::uint8_t,128> header{};
                const auto headerSize=(std::min)(static_cast<std::size_t>(end),header.size());
                if(!stream->read(reinterpret_cast<char*>(header.data()),static_cast<std::streamsize>(headerSize)))
                { ticket->decline(NativeTextureSkip::SourceRead); continue; }
                stream->seekg(0);
                const auto peak=preparationPeak(std::span(header.data(),headerSize),static_cast<std::uint64_t>(end));
                if(peak>AssetPolicy::CpuTransientLimit)
                { ticket->decline(NativeTextureSkip::HostAdmission); continue; }
                SpeculativeScope::Stage stage(peak,(std::min)(SourceLimit,static_cast<std::uint64_t>(end)));
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
                if(!stream->read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
                { ticket->decline(NativeTextureSkip::SourceRead); continue; }
                if(!AssetPolicy::enabled(AssetPolicy::TexturePreparation) && u32(bytes,0)!=0x20534444u)
                { ticket->decline(NativeTextureSkip::Disabled); continue; }
                NativeTextureSkip skip=NativeTextureSkip::None;
                auto payload=prepareNativeTexture(bytes,ticket->path,ticket->role,ticket->purpose,skip);
                if(!payload) { ticket->decline(skip); continue; }
                if(mVfs->getIndexGeneration()!=ticket->vfsGeneration) ticket->decline(NativeTextureSkip::Stale);
                else { work.setMetric(0,"native_bytes",payload->bytes.size()); work.retain(); ticket->publish(std::move(payload)); }
            }
            catch(const SpeculativeDeferred&) { ticket->decline(NativeTextureSkip::HostAdmission); }
            catch(const std::exception&) { ticket->decline(NativeTextureSkip::SourceRead); }
        }
    }
}
