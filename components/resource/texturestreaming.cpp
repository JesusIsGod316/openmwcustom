#include "texturestreaming.hpp"
#include "assetpolicy.hpp"
#include "assetgpubudget.hpp"
#include "uploadpool.hpp"
#include <components/debug/hitchtrace.hpp>
#include <components/debug/runtimediagnostics.hpp>
#include <components/sceneutil/glcalltrace.hpp>
#include <osg/ContextData>
#include <osg/FrameStamp>
#include <osg/GraphicsContext>
#include <osg/GLExtensions>
#include <osg/State>
#include <osg/StateSet>
#include <osg/observer_ptr>
#include <algorithm>
#include <array>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Resource::TextureStreaming
{
    struct Descriptor
    {
        explicit Descriptor(std::shared_ptr<NativeTextureTicket> t,bool stream) : ticket(std::move(t)),streaming(stream) {}
        const bool streaming;
        std::atomic_bool nativeRecorded{false};
        const std::shared_ptr<NativeTextureTicket> ticket;
        osg::observer_ptr<Texture> texture;
        osg::observer_ptr<osg::Image> image;
        unsigned modified=0;
        GLint width=0,height=0,depth=0,internal=0;
        GLenum pixel=0,type=0;
        unsigned packing=0;
        int rowLength=0;
        const unsigned char* data=nullptr;
        std::size_t imageBytes=0;
        osg::Image::MipmapDataType mipmaps;
        osg::Image::Origin origin=osg::Image::TOP_LEFT;
        void capture(osg::Image* value)
        {
            image=value;
            if(!value) return;
            modified=value->getModifiedCount();width=value->s();height=value->t();depth=value->r();
            internal=value->getInternalTextureFormat();pixel=value->getPixelFormat();type=value->getDataType();
            mipmaps=value->getMipmapLevels();origin=value->getOrigin();
            packing=value->getPacking();rowLength=value->getRowLength();data=value->data();
            imageBytes=value->getTotalSizeInBytesIncludingMipmaps();
        }
        std::atomic_bool alive{true};
        std::atomic_bool fallbackRecorded{false};
    };
    namespace
    {
        constexpr std::uint64_t MiB=AssetPolicy::MiB;
        constexpr unsigned ChunkBytes=512*1024, ChunksPerFrame=2;
        constexpr unsigned TrackedTextureUnits=32;
        // Windows' system GL header stops at 1.1. These core 1.2 tokens
        // describe immutable storage bounds, not a residency clamp policy.
        constexpr GLenum TextureBaseLevel=0x813c,TextureMaxLevel=0x813d;
        constexpr double QualityHold=10.0, HealthyHold=5.0;
        Counters sCounters;
        thread_local bool sCompileActive=false,sCompileAllowed=false;
        std::mutex sRegistryMutex;
        std::vector<std::weak_ptr<Descriptor>> sRegistry;
        std::atomic_uint64_t sConstructing{0},sOwnerLive{0};
#ifdef OPENMW_NATIVE_TEXTURE_TEST_HOOKS
        std::atomic_bool sFailNextUpload{false};
        std::atomic_bool sHoldTestRetirements{false};
#endif
        std::shared_ptr<Descriptor> registerTexture(std::shared_ptr<NativeTextureTicket> ticket,bool stream)
        {
            if(stream) ++sCounters.requested;
            if(!ticket) return {};
            if(stream && ticket->purpose!=TexturePurpose::MainDraw)
            { ++sCounters.skips[static_cast<unsigned>(Skip::Purpose)]; return {}; }
            if(stream && ticket->role!=TextureRole::Diffuse && ticket->role!=TextureRole::DiffuseSpecular)
            { ++sCounters.skips[static_cast<unsigned>(Skip::Role)]; return {}; }
            std::lock_guard lock(sRegistryMutex);
            sRegistry.erase(std::remove_if(sRegistry.begin(),sRegistry.end(),[](const auto& x){return x.expired();}),sRegistry.end());
            if(sRegistry.size()>=4096) { ++sCounters.skips[static_cast<unsigned>(Skip::AllocationLimit)]; return {}; }
            auto descriptor=std::make_shared<Descriptor>(std::move(ticket),stream);
            sRegistry.push_back(descriptor); if(stream) ++sCounters.eligible; return descriptor;
        }
        using Sync=decltype(std::declval<osg::GLExtensions>().glFenceSync(GLenum{},GLbitfield{}));
        struct Allocation
        {
            GLuint name=0;
            Sync fence=nullptr;
            std::uint64_t bytes=0;
            unsigned first=0,level=0,row=0;
            bool constructing=false, uncertain=false,drawn=false,applied=false,streaming=true,demoted=false;
            std::shared_ptr<AssetGpuBudget::Reservation> gpu;
            SpeculativeBudget::ChargePtr host;
            std::shared_ptr<const unsigned char> identity;
        };
        struct Entry
        {
            std::weak_ptr<Descriptor> descriptor;
            std::shared_ptr<const NativeMipPayload> payload;
            osg::ref_ptr<osg::Image> nativeImage;
            osg::ref_ptr<Texture> frozenOwner;
            Allocation active,building;
            double publishedAt=0,healthySince=-1;
            bool demoting=false,failed=false;
        };
        struct UnpackScope
        {
            osg::State& state;
            osg::GLExtensions& ext;
            unsigned unit;
            GLint binding=0,pbo=0,alignment=0,row=0,rows=0,pixels=0;
            explicit UnpackScope(osg::State& s) : state(s),ext(*s.get<osg::GLExtensions>()),unit(s.getActiveTextureUnit())
            {
                state.setActiveTextureUnit(0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
                glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB,&pbo);
                glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment); glGetIntegerv(GL_UNPACK_ROW_LENGTH,&row);
                glGetIntegerv(GL_UNPACK_SKIP_ROWS,&rows); glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&pixels);
                ext.glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,0);
                glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
                glPixelStorei(GL_UNPACK_SKIP_ROWS,0); glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
            }
            ~UnpackScope()
            {
                glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(binding));
                ext.glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,static_cast<GLuint>(pbo));
                glPixelStorei(GL_UNPACK_ALIGNMENT,alignment); glPixelStorei(GL_UNPACK_ROW_LENGTH,row);
                glPixelStorei(GL_UNPACK_SKIP_ROWS,rows); glPixelStorei(GL_UNPACK_SKIP_PIXELS,pixels);
                state.haveAppliedTextureAttribute(0,osg::StateAttribute::TEXTURE);
                state.setActiveTextureUnit(unit);
            }
        };
        class Controller final : public osg::GraphicsObjectManager
        {
        public:
            explicit Controller(unsigned context) : osg::GraphicsObjectManager("OptimizedMW.NativeTextureOwner",context) {}
            bool owner(osg::State& state,bool assign=false)
            {
                auto* gc=state.getGraphicsContext();
                if(mClosed || !gc || !gc->isCurrent() || state.getContextID()!=_contextID) return false;
                if(!mAssigned && assign) { mOwner=gc; mAssigned=true; }
                return mAssigned && mOwner.get()==gc;
            }
            bool bind(osg::State& state,const Descriptor* descriptor)
            {
                if(!owner(state)) return false;
                const auto unit=state.getActiveTextureUnit();
                if(unit>=TrackedTextureUnits)
                { ++sCounters.skips[static_cast<unsigned>(Skip::TextureUnit)];return false; }
                mUsedUnits|=std::uint32_t(1)<<unit;
                auto found=mEntries.find(descriptor);
                if(found==mEntries.end() || !found->second.active.name) return false;
                const auto exactOwner=found->second.descriptor.lock();
                osg::ref_ptr<Texture> texture;
                if(!exactOwner || exactOwner.get()!=descriptor || found->second.failed || !descriptor->texture.lock(texture) || !valid(*descriptor,*texture))
                { found->second.failed=true;return false; }
                if(!descriptor->streaming && !found->second.active.applied)
                { found->second.active.applied=true;++nativeTextureStats().fullApplies; }
                // Publication cannot change between main/refraction draws.
                glBindTexture(GL_TEXTURE_2D,found->second.active.name); return true;
            }
            void draw(osg::State& state,const std::shared_ptr<Descriptor>& descriptor)
            {
                if(!descriptor || !owner(state)) return;
                auto found=mEntries.find(descriptor.get());
                if(found==mEntries.end() || !found->second.active.name) return;
                auto& entry=found->second;
                const auto exactOwner=entry.descriptor.lock();
                osg::ref_ptr<Texture> texture;
                if(!exactOwner || exactOwner!=descriptor || entry.failed || !descriptor->texture.lock(texture) || !valid(*descriptor,*texture)) return;
                const auto* stamp=state.getFrameStamp();
                const std::uint64_t frame=stamp ? stamp->getFrameNumber() : mFrame;
                descriptor->ticket->lastDrawFrame=frame;
                descriptor->ticket->lastDrawTime=mTime;
                if(AssetPolicy::enabled(AssetPolicy::TexturePreparation) && entry.active.first==0)
                {
                    if(descriptor->ticket->purpose==TexturePurpose::MainDraw) ++nativeTextureStats().consumedMainDraws;
                    else ++nativeTextureStats().consumedCompositeDraws;
                }
                if(!descriptor->streaming) return;
                auto& first=entry.active.first ? descriptor->ticket->firstCoarseDraw : descriptor->ticket->firstFineDraw;
                auto& counter=entry.active.first ? sCounters.coarseDraws : sCounters.fineDraws;
                auto& firstCounter=entry.active.first ? sCounters.firstCoarseDraws : sCounters.firstFineDraws;
                ++counter;
                if(entry.active.demoted) ++sCounters.demotionDraws;
                std::uint64_t zero=0;first.compare_exchange_strong(zero,frame+1);
                if(!entry.active.drawn)
                {
                    entry.active.drawn=true;
                    ++firstCounter;
                    Debug::RuntimeDiagnostics::recordEvent("texture_first_draw","terrain",descriptor->ticket->path,
                        {{"context",_contextID},{"frame",frame},{"name",entry.active.name},
                         {"first_mip",entry.active.first},{"allocation_bytes",entry.active.bytes},
                         {"format",entry.payload->format},{"role",static_cast<unsigned>(descriptor->ticket->role)},
                         {"image_revision",descriptor->modified}});
                }
            }
            void boundary(osg::State& state)
            {
                if(!owner(state,true)) { ++sCounters.skips[static_cast<unsigned>(Skip::Context)]; return; }
                const auto* stamp=state.getFrameStamp();
                const auto frame=stamp ? stamp->getFrameNumber() : mFrame+1;
                if(mHasFrame && frame==mFrame) return;
                mFrame=frame; mHasFrame=true;
                // FrameStamp reference time may be simulation-scaled. Quality
                // holds use the monotonic owner clock in production.
                mTime=static_cast<double>(AssetGpuBudget::milliseconds())/1000.0;
#ifdef OPENMW_NATIVE_TEXTURE_TEST_HOOKS
                if(testTime>=0) mTime=testTime;
#endif
                ++sCounters.boundaries;
                // Release last epoch's temporary keepalive before deciding
                // which producers still have a real scene/cache owner.
                // The OSG last-applied pointer must not outlive a producer that
                // loses its last reference during this release.
                invalidate(state);
                for(auto& [key,entry]:mEntries) entry.frozenOwner=nullptr;
                auto* ext=state.get<osg::GLExtensions>();
                if(!ext || !ext->glTexStorage2D || !ext->glCompressedTexSubImage2D || !ext->glBindBuffer
                    || !ext->glFenceSync || !ext->glClientWaitSync || !ext->glDeleteSync
                    || !osg::isGLExtensionSupported(_contextID,"GL_EXT_texture_compression_s3tc"))
                { ++sCounters.skips[static_cast<unsigned>(Skip::Capability)]; return; }
                flush(state);
                {
                    std::unique_lock lock(sRegistryMutex,std::try_to_lock);
                    unsigned ingested=0;
                    if(lock.owns_lock()) for(const auto& weak:sRegistry)
                        if(auto descriptor=weak.lock();descriptor && descriptor->alive.load())
                            if(!mEntries.contains(descriptor.get()) && mEntries.size()<4096 && ingested<32)
                            {
                                Entry entry;entry.descriptor=descriptor;
                                mEntries.emplace(descriptor.get(),std::move(entry));
                                ++ingested;
                            }
                }
                const auto pressure=Debug::V3GpuMemory::admissionSample();
                const auto now=AssetGpuBudget::milliseconds();
                const bool healthy=pressure.valid && pressure.pressure==Debug::V3GpuMemory::PressureState::Comfortable
                    && now>=pressure.sampledAtMs && now-pressure.sampledAtMs<=2000
                    && pressure.headroomBytes>=AssetGpuBudget::HeadroomReserve;
                unsigned chunks=0,allocations=0;
                for(auto it=mEntries.begin();it!=mEntries.end();)
                {
                    auto descriptor=it->second.descriptor.lock(); auto& entry=it->second;
                    if(!descriptor || !descriptor->alive.load() || !descriptor->ticket->current())
                    {
                        if(retirementSlots()>=unsigned(bool(entry.active.name))+unsigned(bool(entry.building.name)))
                        { retire(entry.active,*ext); retire(entry.building,*ext); it=mEntries.erase(it); continue; }
                        ++it; continue;
                    }
                    ++it;
                    osg::ref_ptr<Texture> texture;
                    if(!descriptor->texture.lock(texture)) continue;
                    entry.frozenOwner=texture;
                    if(!valid(*descriptor,*texture) && !descriptor->ticket->ordinaryAllocated.load())
                    {
                        if(retirementSlots()>=unsigned(bool(entry.active.name))+unsigned(bool(entry.building.name)))
                        { retire(entry.active,*ext); retire(entry.building,*ext); }
                        ++sCounters.skips[static_cast<unsigned>(Skip::Stale)]; entry.failed=true; continue;
                    }
                    if(descriptor->ticket->ordinaryAllocated.load() || entry.failed)
                    {
                        if(retirementSlots()>=unsigned(bool(entry.active.name))+unsigned(bool(entry.building.name)))
                        { retire(entry.building,*ext);retire(entry.active,*ext); }
                        if(!entry.active.name && !entry.building.name)
                        { entry.nativeImage=nullptr;entry.payload.reset(); }
                        ++sCounters.skips[static_cast<unsigned>(descriptor->ticket->ordinaryAllocated.load()
                            ? Skip::OrdinaryAllocation : descriptor->ticket->outcome()==NativeTextureOutcome::TerminalDecline
                                ? Skip::TerminalPreparation : Skip::Stale)];continue;
                    }
                    if(!entry.payload) entry.payload=descriptor->ticket->payload();
                    if(!entry.payload)
                    {
                        const auto outcome=descriptor->ticket->outcome();
                        ++sCounters.skips[static_cast<unsigned>(outcome==NativeTextureOutcome::TerminalDecline
                            ? Skip::TerminalPreparation : Skip::NotReady)];
                        if(outcome==NativeTextureOutcome::TerminalDecline) entry.failed=true;
                        continue;
                    }
                    if(!entry.nativeImage)
                    {
                        entry.nativeImage=nativeTextureImage(entry.payload);
                        if(!descriptor->nativeRecorded.exchange(true))
                        { ++nativeTextureStats().nativeImages;++nativeTextureStats().handoffs; }
                    }
                    if(entry.payload->suffixBytes(0)>AssetPolicy::NewTextureLimit)
                    {
                        ++sCounters.skips[static_cast<unsigned>(Skip::AllocationLimit)];entry.failed=true;continue;
                    }
                    if(healthy) { if(entry.healthySince<0) entry.healthySince=mTime; }
                    else entry.healthySince=-1;
                    if(entry.building.name)
                    {
                        if(!healthy && !entry.demoting) continue;
                        upload(state,entry,chunks);
                        if(entry.failed) continue;
                        publish(state,entry,descriptor,*ext);
                        continue;
                    }
                    if(!retirementSlots())
                    { ++sCounters.skips[static_cast<unsigned>(Skip::RetirementDebt)]; continue; }
                    if(allocations || chunks>=ChunksPerFrame) continue;
                    unsigned first=descriptor->streaming ? entry.payload->coarseLevel() : 0; bool want=false;
                    if(!entry.active.name) want=true;
                    else if(descriptor->streaming)
                    {
                        const auto last=descriptor->ticket->lastDrawTime.load();
                        const bool held=mTime-entry.publishedAt<QualityHold;
                        if(!held && entry.active.first && healthy && entry.healthySince>=0
                            && mTime-entry.healthySince>=HealthyHold && last>=entry.publishedAt && mTime-last<1.0)
                        { first=0; want=true; }
                        else if(!held && !entry.active.first && first>0 && AssetPolicy::enabled(AssetPolicy::TextureDemotion)
                            && mTime-last>=QualityHold)
                        { ++sCounters.demotionEligible;want=true; entry.demoting=true; }
                        else if(held) ++sCounters.skips[static_cast<unsigned>(Skip::Hold)];
                    }
                    if(want && allocate(state,entry,first,descriptor->streaming))
                    { ++allocations; upload(state,entry,chunks); publish(state,entry,descriptor,*ext); }
                }
            }
            void close(osg::State& state)
            {
                if(!owner(state)) return;
                invalidate(state);
                // Called after draw threads stop. GL deletion is targeted and
                // releases names immediately; GL retains in-flight storage as
                // required by its own lifetime rules, without engine recycling.
                auto* ext=state.get<osg::GLExtensions>();
                for(auto& [key,entry]:mEntries) { destroy(entry.active,*ext); destroy(entry.building,*ext); }
                for(auto& retired:mRetired) destroy(retired,*ext);
                mEntries.clear(); mClosed=true;
            }
            // Ordinary OSG sweeping must not change the frozen frame epoch.
            void flushDeletedGLObjects(double,double&) override {}
            void flushAllDeletedGLObjects() override {}
            void deleteAllGLObjects() override {}
            void discardAllGLObjects() override
            {
                osg::ref_ptr<osg::GraphicsContext> owner;
                if(mOwner.lock(owner) && owner->getState()) invalidate(*owner->getState());
                // Context discard proves lifetime end even when no GL owner is
                // available. Numeric context reuse cannot inherit this manager.
                for(auto& [key,entry]:mEntries) { discard(entry.active); discard(entry.building); }
                for(auto& retired:mRetired) discard(retired);
                mEntries.clear(); mClosed=true; ++sCounters.contextDiscards;
            }
#ifdef OPENMW_NATIVE_TEXTURE_TEST_HOOKS
            inline static double testTime=-1;
            std::optional<TestBackings> testBackings(const Descriptor* descriptor) const
            {
                const auto found=mEntries.find(descriptor);
                if(found==mEntries.end() || found->second.descriptor.lock().get()!=descriptor) return {};
                const auto& entry=found->second;
                return TestBackings{entry.active.name,entry.building.name,entry.active.first,entry.building.first,
                    entry.active.bytes,entry.building.bytes};
            }
            bool expireTestEntryOwner(const Descriptor* descriptor)
            {
                const auto found=mEntries.find(descriptor);
                if(found==mEntries.end() || !found->second.active.name) return false;
                // Model an expired old owner at a reused raw key, without an
                // unsafe fixture allocator or changing production ownership.
                found->second.descriptor.reset();return true;
            }
#endif
        private:
            static bool valid(const Descriptor& descriptor,const Texture& texture)
            {
                const auto* image=texture.getImage();
                return descriptor.alive.load() && descriptor.ticket->current()
                    && !descriptor.ticket->ordinaryAllocated.load() && image
                    && descriptor.image.get()==image && image->getModifiedCount()==descriptor.modified
                    && image->getDataVariance()!=osg::Object::DYNAMIC && !image->requiresUpdateCall()
                    && image->s()==descriptor.width && image->t()==descriptor.height && image->r()==descriptor.depth
                    && image->getInternalTextureFormat()==descriptor.internal && image->getPixelFormat()==descriptor.pixel
                    && image->getDataType()==descriptor.type && image->getOrigin()==descriptor.origin
                    && image->getMipmapLevels()==descriptor.mipmaps && image->getPacking()==descriptor.packing
                    && image->getRowLength()==descriptor.rowLength && image->data()==descriptor.data
                    && image->getTotalSizeInBytesIncludingMipmaps()==descriptor.imageBytes && !texture.getUpdateCallback() && !texture.getEventCallback()
                    && texture.getDataVariance()!=osg::Object::DYNAMIC
                    && !texture.getSubloadCallback()
                    && !texture.getReadPBuffer() && texture.getBorderWidth()==0;
            }
            void invalidate(osg::State& state) const
            {
                for(unsigned unit=0;unit<TrackedTextureUnits;++unit)
                    if(mUsedUnits&(std::uint32_t(1)<<unit))
                        state.haveAppliedTextureAttribute(unit,osg::StateAttribute::TEXTURE);
            }
            unsigned retirementSlots() const
            { return static_cast<unsigned>(std::count_if(mRetired.begin(),mRetired.end(),[](const auto& x){return !x.name;})); }
            static void finishConstruction(Allocation& slot)
            { if(slot.constructing) { sConstructing.fetch_sub(1); slot.constructing=false; } }
            static void discard(Allocation& slot)
            {
                if(!slot.name) return;
                finishConstruction(slot);sOwnerLive.fetch_sub(slot.bytes);
                if(slot.streaming) sCounters.liveBytes.fetch_sub(slot.bytes);
                else nativeTextureStats().fullLiveBytes.fetch_sub(slot.bytes);
                slot={};
            }
            static void destroy(Allocation& slot,osg::GLExtensions& ext)
            {
                if(!slot.name) return;
                if(slot.fence) ext.glDeleteSync(slot.fence);
                glDeleteTextures(1,&slot.name);
                if(slot.streaming) { ++sCounters.deletions;sCounters.deletedBytes+=slot.bytes; }
                else ++nativeTextureStats().fullDeletions;
                Debug::RuntimeDiagnostics::recordEvent("texture_delete","terrain",{},
                    {{"name",slot.name},{"allocation_bytes",slot.bytes}});
                discard(slot); // reservations release strictly after deletion
            }
            void flush(osg::State& state)
            {
#ifdef OPENMW_NATIVE_TEXTURE_TEST_HOOKS
                if(sHoldTestRetirements.load()) return;
#endif
                auto& ext=*state.get<osg::GLExtensions>();
                for(auto& slot:mRetired) if(slot.name && !slot.uncertain)
                {
                    const auto result=slot.fence ? ext.glClientWaitSync(slot.fence,0,0) : GL_WAIT_FAILED;
                    if(result==GL_ALREADY_SIGNALED || result==GL_CONDITION_SATISFIED) destroy(slot,ext);
                }
            }
            void retire(Allocation& slot,osg::GLExtensions& ext)
            {
                if(!slot.name) return;
                if(!retirementSlots()) return;
                finishConstruction(slot);
                if(slot.fence) ext.glDeleteSync(slot.fence);
                slot.fence=ext.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
                slot.uncertain=!slot.fence;
                if(slot.host) slot.host->pending();
                for(auto& pending:mRetired) if(!pending.name) { pending=std::move(slot); slot={}; ++sCounters.retirements; break; }
            }
            void fail(Entry& entry,osg::GLExtensions& ext)
            {
                ++sCounters.skips[static_cast<unsigned>(Skip::GlFailure)];
                retire(entry.building,ext); entry.failed=true;
            }
            void publish(osg::State& state,Entry& entry,const std::shared_ptr<Descriptor>& descriptor,
                osg::GLExtensions& ext)
            {
                if(entry.failed || !entry.building.name || entry.building.level!=entry.payload->mips.size()
                    || (entry.active.name && !retirementSlots())) return;
                // Client-memory compressed uploads have returned before this
                // owner-boundary publication. Later draws in this exact GL
                // context are ordered after those commands; no CPU/GPU wait is
                // needed and no other context may use these raw names.
                retire(entry.active,ext);
                entry.active=std::move(entry.building); entry.building={};
                finishConstruction(entry.active); entry.publishedAt=mTime;
                if(!descriptor->streaming) ++nativeTextureStats().fullPublished;
                else if(entry.active.first) ++sCounters.coarsePublished; else ++sCounters.finePublished;
                if(entry.demoting) { entry.active.demoted=true;++sCounters.demotions; }
                entry.demoting=false;
                invalidate(state);
                Debug::RuntimeDiagnostics::recordEvent("texture_publish","terrain",descriptor->ticket->path,
                    {{"context",_contextID},{"frame",mFrame},{"name",entry.active.name},
                     {"first_mip",entry.active.first},{"allocation_bytes",entry.active.bytes}});
            }
            bool allocate(osg::State& state,Entry& entry,unsigned first,bool streaming)
            {
                const auto bytes=entry.payload->suffixBytes(first);
                if(!bytes || bytes>AssetPolicy::NewTextureLimit || sOwnerLive.load()>AssetPolicy::TextureAllocationLimit-bytes)
                { ++sCounters.skips[static_cast<unsigned>(Skip::AllocationLimit)]; return false; }
                if(!UploadPool::ledger || !UploadPool::sample)
                { ++sCounters.skips[static_cast<unsigned>(Skip::Admission)]; return false; }
                std::uint64_t zero=0;
                if(!sConstructing.compare_exchange_strong(zero,1)) return false;
                Allocation slot; slot.constructing=true;slot.streaming=streaming;
                try
                {
                    SpeculativeScope scope(UploadPool::ledger,UploadPool::sample,UploadPool::sampleOwner);
                    SpeculativeScope::Stage stage(bytes,bytes);
                    slot.gpu=AssetGpuBudget::reserve(bytes);
                    if(!slot.gpu) throw SpeculativeDeferred{};
                    slot.identity=std::make_shared<const unsigned char>(0);
                    slot.host=SpeculativeScope::track(UploadPool::ledger,{slot.identity.get(),0x54324750u},bytes,true);
                }
                catch(...) { finishConstruction(slot); ++sCounters.skips[static_cast<unsigned>(Skip::Admission)]; return false; }
                auto* ext=state.get<osg::GLExtensions>();
                UnpackScope restore(state);
                glGenTextures(1,&slot.name);
                if(!slot.name) { finishConstruction(slot); ++sCounters.skips[static_cast<unsigned>(Skip::GlFailure)]; return false; }
                slot.bytes=bytes; slot.first=first; slot.level=first;
                sOwnerLive+=bytes;
                auto& liveCounter=streaming ? sCounters.liveBytes : nativeTextureStats().fullLiveBytes;
                auto& peakCounter=streaming ? sCounters.peakBytes : nativeTextureStats().fullPeakBytes;
                liveCounter+=bytes;auto peak=peakCounter.load(),live=liveCounter.load();
                while(peak<live && !peakCounter.compare_exchange_weak(peak,live)) {}
                glBindTexture(GL_TEXTURE_2D,slot.name);
                const auto& top=entry.payload->mips[first];
                ext->glTexStorage2D(GL_TEXTURE_2D,static_cast<GLsizei>(entry.payload->mips.size()-first),
                    entry.payload->format,top.width,top.height);
                glTexParameteri(GL_TEXTURE_2D,TextureBaseLevel,0);
                glTexParameteri(GL_TEXTURE_2D,TextureMaxLevel,static_cast<GLint>(entry.payload->mips.size()-first-1));
                entry.building=std::move(slot);
                if(streaming) { ++sCounters.allocations;sCounters.allocationBytes+=bytes; }
                else ++nativeTextureStats().fullAllocations;
                if(glGetError()!=GL_NO_ERROR) { fail(entry,*ext); return false; }
                return true;
            }
            void upload(osg::State& state,Entry& entry,unsigned& chunks)
            {
                auto& slot=entry.building;
                if(!slot.name || entry.failed || chunks>=ChunksPerFrame) return;
                auto* ext=state.get<osg::GLExtensions>();
#ifdef OPENMW_NATIVE_TEXTURE_TEST_HOOKS
                if(sFailNextUpload.exchange(false)) { fail(entry,*ext);return; }
#endif
                UnpackScope restore(state); glBindTexture(GL_TEXTURE_2D,slot.name);
                Debug::HitchTrace::Scope timing("terrain.native_texture_upload",Debug::HitchTrace::CpuTiming::WallEnvelope,
                    entry.payload->path);
                std::uint64_t submitted=0;
                while(slot.level<entry.payload->mips.size() && chunks<ChunksPerFrame)
                {
                    unsigned packet=0;
                    while(slot.level<entry.payload->mips.size() && packet<ChunkBytes)
                    {
                        const auto& mip=entry.payload->mips[slot.level];
                        const unsigned block=entry.payload->format==GL_COMPRESSED_RGB_S3TC_DXT1_EXT
                            || entry.payload->format==GL_COMPRESSED_RGBA_S3TC_DXT1_EXT ? 8 : 16;
                        const unsigned rowBytes=((mip.width+3)/4)*block,blockRows=(mip.height+3)/4;
                        const unsigned rows=(std::min)(blockRows-slot.row,(ChunkBytes-packet)/rowBytes);
                        if(!rows) break;
                        const unsigned bytes=rows*rowBytes;
                        const unsigned y=slot.row*4,height=(std::min)(rows*4,mip.height-y);
                        ext->glCompressedTexSubImage2D(GL_TEXTURE_2D,slot.level-slot.first,0,y,mip.width,height,
                            entry.payload->format,bytes,entry.payload->bytes.data()+mip.offset+slot.row*rowBytes);
                        if(slot.streaming) { ++sCounters.uploads;sCounters.uploadedBytes+=bytes; }
                        else nativeTextureStats().fullUploadBytes+=bytes;
                        submitted+=bytes;packet+=bytes;
                        if(glGetError()!=GL_NO_ERROR) { fail(entry,*ext); break; }
                        slot.row+=rows;
                        if(slot.row==blockRows) { ++slot.level; slot.row=0; }
                    }
                    ++chunks;
                    if(entry.failed) break;
                }
                timing.setMetric(0,"bytes",submitted); timing.retain();
            }
            osg::observer_ptr<osg::GraphicsContext> mOwner;
            bool mAssigned=false,mClosed=false,mHasFrame=false;
            std::uint32_t mUsedUnits=1; // raw upload state also touches unit zero
            std::uint64_t mFrame=0;
            double mTime=0;
            std::unordered_map<const Descriptor*,Entry> mEntries;
            std::array<Allocation,4> mRetired{};
        };
    }
    Counters& counters() { return sCounters; }
    CompileScope::CompileScope(bool allowDeferral)
    {
        if(!AssetPolicy::enabled(AssetPolicy::TexturePreparation|AssetPolicy::TextureStreaming)) return;
        mChanged=true;mPreviousActive=sCompileActive;mPreviousAllowed=sCompileAllowed;
        sCompileActive=true;sCompileAllowed=allowDeferral;
    }
    CompileScope::~CompileScope()
    { if(mChanged) { sCompileActive=mPreviousActive;sCompileAllowed=mPreviousAllowed; } }
    const char* skipName(Skip reason)
    {
        static constexpr const char* names[]={"disabled","purpose","role","not_ready","ordinary_allocation",
            "context","capability","admission","allocation_limit","retirement_debt","hold","gl_failure","stale","texture_unit","terminal_preparation"};
        const auto i=static_cast<unsigned>(reason); return i<std::size(names) ? names[i] : "unknown";
    }
    Texture::Texture()=default;
    Texture::Texture(osg::Image* image,std::shared_ptr<NativeTextureTicket> ticket,bool streaming,bool nativeImageReady)
        : PreparedTerrainTexture(image),mDescriptor(registerTexture(std::move(ticket),streaming))
    {
        setDataVariance(osg::Object::STATIC); setResizeNonPowerOfTwoHint(false);
        setUseHardwareMipMapGeneration(false); setUnRefImageDataAfterApply(false);
        if(mDescriptor) { mDescriptor->texture=this;mDescriptor->capture(image);mDescriptor->nativeRecorded=nativeImageReady; }
    }
    Texture::Texture(const Texture& other,const osg::CopyOp& copy)
        : PreparedTerrainTexture(other,copy),mDescriptor(registerTexture(other.mDescriptor ? other.mDescriptor->ticket : nullptr,other.mDescriptor ? other.mDescriptor->streaming : true))
    { if(mDescriptor) { mDescriptor->texture=this; mDescriptor->capture(getImage());mDescriptor->nativeRecorded=other.mDescriptor->nativeRecorded.load(); } }
    Texture::~Texture() { if(mDescriptor) mDescriptor->alive=false; }
    int Texture::compare(const osg::StateAttribute& other) const
    {
        const auto compared=osg::Texture2D::compare(other);
        if(compared) return compared;
        const auto* rhs=dynamic_cast<const Texture*>(&other);
        if(!rhs || mDescriptor==rhs->mDescriptor) return 0;
        // State sharing must not collapse role/source-epoch/lifetime owners
        // just because their pending fallback images and samplers compare
        // equal. TextureManager already shares the exact canonical producer.
        return std::less<const Descriptor*>{}(mDescriptor.get(),rhs->mDescriptor.get()) ? -1 : 1;
    }
    void Texture::apply(osg::State& state) const
    {
        if(sCompileActive && sCompileAllowed && mDescriptor && mDescriptor->alive.load()
            && mDescriptor->ticket->current() && !mDescriptor->ticket->ordinaryAllocated.load())
        {
            ++sCounters.compileDeferred;
            state.haveAppliedTextureAttribute(state.getActiveTextureUnit(),osg::StateAttribute::TEXTURE);
            return;
        }
        SceneUtil::GLCallTrace::ScopedTextureApply ownership(state,*this);
        if(mDescriptor && state.get<Controller>()->bind(state,mDescriptor.get()))
        { applyTexParameters(GL_TEXTURE_2D,state); return; }
        osg::Texture2D::apply(state);
        // An ordinary allocated texture is an authoritative fallback. Never
        // create a second hidden full allocation alongside the OSG pool object.
        if(mDescriptor && getTextureObject(state.getContextID()))
        {
            mDescriptor->ticket->markOrdinary();
            if(!mDescriptor->fallbackRecorded.exchange(true))
            {
                if(sCompileActive) ++sCounters.compileFallbacks;else ++sCounters.coldVisibleFallbacks;
                Debug::RuntimeDiagnostics::recordEvent(sCompileActive ? "texture_compile_fallback" : "texture_cold_visible_fallback",
                    "terrain",mDescriptor->ticket->path,
                    {{"context",state.getContextID()},{"name",getTextureObject(state.getContextID())->id()},
                     {"payload_ready",bool(mDescriptor->ticket->payload())},
                     {"ticket_current",mDescriptor->ticket->current()},{"image_revision",mDescriptor->modified}});
            }
        }
    }
    void Texture::compileGLObjects(osg::State& state) const
    {
        if(mDescriptor && mDescriptor->alive.load() && mDescriptor->ticket->current()
            && !mDescriptor->ticket->ordinaryAllocated.load()
            && AssetPolicy::enabled(AssetPolicy::TexturePreparation|AssetPolicy::TextureStreaming))
        {
            // ICO may run before the owner boundary. Its optional compile may
            // not turn a registered producer into an ordinary full allocation.
            // The boundary alone owns candidate upload; visible apply retains
            // the exact ordinary path if preparation/admission is not ready.
            ++sCounters.compileDeferred;return;
        }
        osg::Texture2D::compileGLObjects(state);
    }
    void Texture::releaseGLObjects(osg::State* state) const
    {
        // Context-specific teardown belongs to the engine live-owner hook.
        // Cache releases can run on workers; they perform no raw GL work.
        if(mDescriptor) mDescriptor->alive=false;
        osg::Texture2D::releaseGLObjects(state);
    }
    void frameBoundary(osg::State& state)
    {
        if(!AssetPolicy::enabled(AssetPolicy::TexturePreparation|AssetPolicy::TextureStreaming)) return;
        state.get<Controller>()->boundary(state);
    }
    void releaseContext(osg::State& state) { state.get<Controller>()->close(state); }
    void recordDrawUse(osg::State& state,const osg::StateSet* pass)
    {
        if(!pass || !AssetPolicy::enabled(AssetPolicy::TexturePreparation|AssetPolicy::TextureStreaming)) return;
        // Only the production shader's pass-defined sampled roles qualify.
        // Cached attributes on unsampled units from previous draws are not use.
        for(const unsigned unit:{0u,2u})
        {
            const auto* texture=dynamic_cast<const Texture*>(pass->getTextureAttribute(unit,osg::StateAttribute::TEXTURE));
            if(!texture || !texture->descriptor()
                || state.getLastAppliedTextureAttribute(unit,osg::StateAttribute::TEXTURE)!=texture) continue;
            const auto& ticket=texture->descriptor()->ticket;
            const bool color=ticket->role==TextureRole::Diffuse || ticket->role==TextureRole::DiffuseSpecular;
            const bool normal=ticket->role==TextureRole::Normal || ticket->role==TextureRole::NormalHeight;
            if((unit==0 && color) || (unit==2 && normal && ticket->purpose==TexturePurpose::MainDraw))
                state.get<Controller>()->draw(state,texture->descriptor());
        }
    }
    void reportDiagnostics()
    {
        Debug::RuntimeDiagnostics::recordEvent("texture_stream_stats","terrain",{},
            {{"requested",sCounters.requested.load()},{"eligible",sCounters.eligible.load()},
             {"allocations",sCounters.allocations.load()},{"upload_bytes",sCounters.uploadedBytes.load()},
             {"coarse_publish",sCounters.coarsePublished.load()},{"fine_publish",sCounters.finePublished.load()},
             {"coarse_draws",sCounters.coarseDraws.load()},{"fine_draws",sCounters.fineDraws.load()},
             {"first_coarse_draw",sCounters.firstCoarseDraws.load()},{"first_fine_draw",sCounters.firstFineDraws.load()},
             {"demotions",sCounters.demotions.load()},{"demotion_eligible",sCounters.demotionEligible.load()},
             {"demotion_draws",sCounters.demotionDraws.load()},{"deletions",sCounters.deletions.load()},
             {"deleted_bytes",sCounters.deletedBytes.load()},{"live_bytes",sCounters.liveBytes.load()},
             {"peak_bytes",sCounters.peakBytes.load()},{"context_discard",sCounters.contextDiscards.load()}});
        Debug::RuntimeDiagnostics::recordEvent("texture_stream_cold","terrain",{},
            {{"compile_deferred",sCounters.compileDeferred.load()},{"compile_fallback",sCounters.compileFallbacks.load()},
             {"visible_fallback",sCounters.coldVisibleFallbacks.load()}});
        for(unsigned i=0;i<static_cast<unsigned>(Skip::Count);++i)
            Debug::RuntimeDiagnostics::recordEvent("texture_stream_skip","terrain",skipName(static_cast<Skip>(i)),
                {{"count",sCounters.skips[i].load()}});
    }
#ifdef OPENMW_NATIVE_TEXTURE_TEST_HOOKS
    std::optional<TestBackings> testBackings(osg::State& state,const Texture& texture)
    {
        auto* controller=state.get<Controller>();
        if(!texture.descriptor() || !controller->owner(state)) return {};
        return controller->testBackings(texture.descriptor().get());
    }
    bool expireTestEntryOwner(osg::State& state,const Texture& texture)
    {
        auto* controller=state.get<Controller>();
        return texture.descriptor() && controller->owner(state)
            && controller->expireTestEntryOwner(texture.descriptor().get());
    }
    void setTestTime(double seconds) { Controller::testTime=seconds; }
    void failNextUpload() { sFailNextUpload=true; }
    void holdTestRetirements(bool value) { sHoldTestRetirements=value; }
#endif
}
