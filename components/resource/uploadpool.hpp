#ifndef OPENMW_RESOURCE_UPLOADPOOL_H
#define OPENMW_RESOURCE_UPLOADPOOL_H

#include "openglpressure.hpp"
#include "speculativebudget.hpp"
#include <osg/BufferObject>
#include <osg/ContextData>
#include <osg/GraphicsContext>
#include <osg/GLExtensions>
#include <osg/Image>
#include <osg/State>
#include <osg/UserDataContainer>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Resource::UploadPool
{
    inline bool requested()
    {
        static const bool value = [] { const char* v = std::getenv("OPENMW_P9_UPLOAD_POOL");
            return v && std::strcmp(v, "1") == 0; }();
        return value;
    }
    using Sample = OpenGlPressureSample (*)(void*);
    // Set before draw threads start, clear after they stop. One existing sampler.
    inline Sample sample = nullptr;
    inline void* sampleOwner = nullptr;
    inline SpeculativeBudget* ledger = nullptr;
    inline std::atomic<std::uint64_t> liveBytes{0}, uploads{0}, uploadedBytes{0},
        busyFallbacks{0}, admissionFallbacks{0}, allocations{0};
    inline std::atomic<std::uint64_t> uploadFailures{0};
    constexpr std::uint64_t SlotBytes = 2 * 1024 * 1024;
    constexpr std::uint64_t GlobalBytes = 24 * 1024 * 1024;
#ifdef OPENMW_UPLOAD_POOL_TEST_HOOKS
    inline bool holdRetirement = false;
    inline int failUploadMip = -1;
#endif
    using Sync = decltype(std::declval<osg::GLExtensions>().glFenceSync(GLenum{},GLbitfield{}));
    struct Retired { GLuint name=0; Sync fence=nullptr; bool uncertain=false;
        std::shared_ptr<const unsigned char> identity;
        SpeculativeBudget::ChargePtr charge; };
    class Retirement final : public osg::GraphicsObjectManager
    {
    public:
        explicit Retirement(unsigned context) : osg::GraphicsObjectManager("OptimizedMW-UploadRetirement",context) {}
        void retire(Retired value)
        {
            if (!value.name) return;
            if (value.charge) value.charge->pending();
            std::lock_guard lock(mMutex);
            if (!mAlive) { liveBytes.fetch_sub(SlotBytes); return; }
            // The global 24 MiB allocation ledger proves <=12 live entries.
            for(auto& entry:mPending) if(!entry.name) { entry=std::move(value); return; }
            std::terminate(); // impossible ledger violation must not grant reuse credit
        }
        void flushDeletedGLObjects(double,double&) override { flushAllDeletedGLObjects(); }
        void flushAllDeletedGLObjects() override
        {
            // Frame-owner flush never waits behind a worker retirement enqueue.
            std::unique_lock lock(mMutex,std::try_to_lock);
            if(!lock.owns_lock()) return;
#ifdef OPENMW_UPLOAD_POOL_TEST_HOOKS
            if(holdRetirement) return;
#endif
            auto* ext=osg::get<osg::GLExtensions>(_contextID);
            if(!ext) return;
            for(auto& entry:mPending) if(entry.name && !entry.uncertain)
            {
                if(entry.fence)
                {
                    const auto status=ext->glClientWaitSync(entry.fence,0,0);
                    if(status!=GL_ALREADY_SIGNALED && status!=GL_CONDITION_SATISFIED) continue;
                    ext->glDeleteSync(entry.fence);
                }
                ext->glDeleteBuffers(1,&entry.name); liveBytes.fetch_sub(SlotBytes); entry={};
            }
        }
        void deleteAllGLObjects() override { flushAllDeletedGLObjects(); }
        void discardAllGLObjects() override
        {
            std::lock_guard lock(mMutex);
            mAlive=false;
            for(auto& entry:mPending) if(entry.name) { liveBytes.fetch_sub(SlotBytes); entry={}; }
        }
    private:
        std::mutex mMutex;
        std::array<Retired,12> mPending{};
        bool mAlive=true;
    };
    inline bool reserve() noexcept
    {
        if (!sample) return false;
        const auto value = sample(sampleOwner);
        if (!value.generation || value.decision.state != OpenGlPressure::Normal
            || !value.decision.admissionsPerSample) return false;
        auto bytes = liveBytes.load(std::memory_order_relaxed);
        while (bytes <= GlobalBytes - SlotBytes)
            if (liveBytes.compare_exchange_weak(bytes, bytes + SlotBytes)) return true;
        return false;
    }

    // Three immutable staging allocations per graphics context. The texture
    // remains OSG owned. Polling never waits; a busy pool uses ordinary upload.
    class Pool final : public osg::Object
    {
    public:
        struct Slot { GLuint name=0; unsigned char* mapped=nullptr; Sync fence=nullptr;
            bool poisoned=false, retiring=false; std::shared_ptr<const unsigned char> identity;
            SpeculativeBudget::ChargePtr charge; };
        Pool() { setName("OptimizedMW.BoundedUploadPool"); }
        Pool(const Pool&, const osg::CopyOp& = osg::CopyOp::SHALLOW_COPY) : Pool() {}
        META_Object(Resource, Pool)
        Pool(unsigned context) : Pool() { mContext = context; }
        ~Pool() override
        {
            retire(); // never GL calls on a worker, keep charges until proof
        }
        Slot* acquire(osg::State& state, std::size_t bytes)
        {
            if(state.getContextID()!=mContext || !state.getGraphicsContext()
                || !state.getGraphicsContext()->isCurrent()) return nullptr;
            if(mOwnerAssigned && mOwner.get()!=state.getGraphicsContext()) return nullptr;
            if(!mOwnerAssigned) { mOwner=state.getGraphicsContext(); mOwnerAssigned=true; }
            auto* ext = state.get<osg::GLExtensions>();
            if (!bytes || bytes > SlotBytes || !ext || !ext->glBufferStorage || !ext->glMapBufferRange
                || !ext->glFenceSync || !ext->glClientWaitSync || !ext->glDeleteSync) return nullptr;
            if (ext->glVersion<4.4f && !osg::isGLExtensionSupported(state.getContextID(),"GL_ARB_buffer_storage"))
                return nullptr;
            if(!mRetirement) mRetirement=state.get<Retirement>();
            mRetirement->flushAllDeletedGLObjects();
            for (auto& slot : mSlots)
            {
                if (slot.poisoned) continue;
                if (slot.fence)
                {
                    const auto result = ext->glClientWaitSync(slot.fence, 0, 0);
                    if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED) continue;
                    ext->glDeleteSync(slot.fence); slot.fence=nullptr;
                }
                if (slot.retiring)
                {
                    ext->glDeleteBuffers(1,&slot.name); liveBytes.fetch_sub(SlotBytes); slot={};
                }
                if (!slot.name)
                {
                    if (!ledger || !sample) { ++admissionFallbacks; return nullptr; }
                    std::optional<SpeculativeBudget::Stage> reservation;
                    try { reservation.emplace(ledger->reserve(sample(sampleOwner),SlotBytes)); }
                    catch (const SpeculativeDeferred&) { ++admissionFallbacks; return nullptr; }
                    if (!reserve()) { ++admissionFallbacks; return nullptr; }
                    try
                    {
                        // A slot can be reused while its previous allocation is
                        // pending deletion. That previous charge must never be
                        // returned as a shared claim for this distinct backing.
                        slot.identity=std::make_shared<const unsigned char>(0);
                        slot.charge=ledger->track({slot.identity.get(),0x50395550u},SlotBytes,true,&*reservation);
                    }
                    catch (const std::bad_alloc&)
                    { liveBytes.fetch_sub(SlotBytes); slot.identity.reset(); ++admissionFallbacks; return nullptr; }
                    GLint previous=0; glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB, &previous);
                    ext->glGenBuffers(1,&slot.name);
                    if (!slot.name) { liveBytes.fetch_sub(SlotBytes); slot.charge.reset(); slot.identity.reset(); return nullptr; }
                    ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,slot.name);
                    // ARB_buffer_storage values; Windows GL headers stop at 1.1.
                    constexpr GLbitfield flags = GL_MAP_WRITE_BIT | 0x0040u | 0x0080u;
                    ext->glBufferStorage(GL_PIXEL_UNPACK_BUFFER_ARB,SlotBytes,nullptr,flags);
                    slot.mapped=static_cast<unsigned char*>(ext->glMapBufferRange(GL_PIXEL_UNPACK_BUFFER_ARB,0,SlotBytes,flags));
                    ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,static_cast<GLuint>(previous));
                    if (!slot.mapped)
                    {
                        ext->glDeleteBuffers(1,&slot.name); slot.name=0; liveBytes.fetch_sub(SlotBytes);
                        slot.charge.reset();
                        slot.identity.reset();
                        return nullptr;
                    }
                    ++allocations;
                }
                return &slot;
            }
            ++busyFallbacks;
            return nullptr;
        }
        void submitted(osg::State& state, Slot& slot, std::size_t bytes)
        {
            slot.fence=state.get<osg::GLExtensions>()->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
            if (!slot.fence) slot.poisoned=true; // never overwrite an unproved lease
            ++uploads; uploadedBytes.fetch_add(bytes);
        }
        void release(osg::State& state)
        {
            retire();
            if(mRetirement && state.getContextID()==mContext && state.getGraphicsContext()
                && mOwner.get()==state.getGraphicsContext()
                && state.getGraphicsContext()->isCurrent()) mRetirement->flushAllDeletedGLObjects();
        }
    private:
        void retire()
        {
            for(auto& slot:mSlots) if(slot.name)
            {
                mRetirement->retire({slot.name,slot.fence,slot.poisoned,std::move(slot.identity),std::move(slot.charge)});
                slot={};
            }
        }
        unsigned mContext=0;
        std::array<Slot,3> mSlots{};
        osg::ref_ptr<Retirement> mRetirement;
        osg::observer_ptr<osg::GraphicsContext> mOwner;
        bool mOwnerAssigned=false;
    };
    inline Pool* find(osg::State& state, bool create)
    {
        auto* gc=state.getGraphicsContext();
        if (!gc) return nullptr;
        auto* data=gc->getOrCreateUserDataContainer();
        if (auto* value=dynamic_cast<Pool*>(data->getUserObject("OptimizedMW.BoundedUploadPool"))) return value;
        if (!create) return nullptr;
        osg::ref_ptr<Pool> value=new Pool(state.getContextID());
        data->addUserObject(value); return value.get();
    }
    inline void release(osg::State& state)
    { if (auto* pool=find(state,false)) pool->release(state); }

    // Exact, static, contiguous alpha mip-chain only. No DDS conversion, image
    // mutation, live texture rewrite, rescaling, or automatic mip generation.
    inline bool eligible(const osg::Image& image) noexcept
    {
        if (!image.data() || image.requiresUpdateCall() || image.getDataVariance()!=osg::Object::STATIC
            || !image.isDataContiguous() || image.getPixelFormat()!=GL_ALPHA
            || image.getDataType()!=GL_UNSIGNED_BYTE
            || (image.getInternalTextureFormat()!=GL_ALPHA && image.getInternalTextureFormat()!=GL_ALPHA8)
            || image.s()<=0 || image.t()<=0 || image.r()!=1 || image.getPacking()!=1
            || image.getRowLength()!=0 || !image.isMipmap()) return false;
        std::size_t offset=0; unsigned level=0;
        int w=image.s(), h=image.t();
        while (true)
        {
            if (level>=image.getNumMipmapLevels() || image.getMipmapOffset(level)!=offset) return false;
            offset+=static_cast<std::size_t>(w)*h;
            if (w==1 && h==1) break;
            w=(std::max)(1,w/2); h=(std::max)(1,h/2); ++level;
        }
        return level+1==image.getNumMipmapLevels() && offset==image.getTotalSizeInBytesIncludingMipmaps()
            && offset<=SlotBytes;
    }
    // The caller has bound an exact OSG texture and configured its parameters.
    inline bool upload(osg::State& state, Pool& pool, Pool::Slot& slot, const osg::Image& image, GLint internalFormat)
    {
        const auto bytes=image.getTotalSizeInBytesIncludingMipmaps();
        std::memcpy(slot.mapped,image.data(),bytes);
        GLint binding=0, alignment=0, rowLength=0, skipRows=0, skipPixels=0;
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB,&binding);
        glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment); glGetIntegerv(GL_UNPACK_ROW_LENGTH,&rowLength);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skipRows); glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&skipPixels);
        auto* ext=state.get<osg::GLExtensions>();
        ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,slot.name);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1); glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,0); glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        bool valid=glGetError()==GL_NO_ERROR; // incoming errors deny optional acceptance
        int w=image.s(),h=image.t();
        for (unsigned level=0;level<image.getNumMipmapLevels();++level)
        {
            int uploadWidth=w;
#ifdef OPENMW_UPLOAD_POOL_TEST_HOOKS
            if(static_cast<int>(level)==failUploadMip) uploadWidth=-1; // actual GL error, no valid mip allocation
#endif
            glTexImage2D(GL_TEXTURE_2D,level,internalFormat,uploadWidth,h,0,GL_ALPHA,GL_UNSIGNED_BYTE,
                reinterpret_cast<const void*>(static_cast<std::uintptr_t>(image.getMipmapOffset(level))));
            GLint actualWidth=0,actualHeight=0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_WIDTH,&actualWidth);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_HEIGHT,&actualHeight);
            valid=valid && actualWidth==w && actualHeight==h;
            w=(std::max)(1,w/2); h=(std::max)(1,h/2);
        }
        pool.submitted(state,slot,bytes);
        ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB,static_cast<GLuint>(binding));
        glPixelStorei(GL_UNPACK_ALIGNMENT,alignment); glPixelStorei(GL_UNPACK_ROW_LENGTH,rowLength);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,skipRows); glPixelStorei(GL_UNPACK_SKIP_PIXELS,skipPixels);
        valid=glGetError()==GL_NO_ERROR && valid;
        if(!valid) ++uploadFailures;
        return valid;
    }
}
#endif
