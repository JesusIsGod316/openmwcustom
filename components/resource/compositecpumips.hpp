#ifndef OPENMW_RESOURCE_COMPOSITECPUMIPS_H
#define OPENMW_RESOURCE_COMPOSITECPUMIPS_H

#include "preparedterraintexture.hpp"

#include <osg/GLExtensions>
#include <osg/Image>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace Resource::CompositeCpuMips
{
    struct Capabilities
    {
        osg::observer_ptr<osg::GraphicsContext> context;
        unsigned contextId = std::numeric_limits<unsigned>::max();
        int maxDimension = 0;
        bool npotMipSupported = false;
    };

    inline std::mutex sCapabilitiesMutex;
    inline Capabilities sCapabilities;
    inline void publishCapabilities(osg::GraphicsContext& context)
    {
        const auto* state = context.getState();
        const auto* extensions = state ? state->get<osg::GLExtensions>() : nullptr;
        Capabilities value;
        if (state && extensions)
            value = { &context, state->getContextID(), extensions->maxTextureSize,
                extensions->isNonPowerOfTwoTextureSupported(GL_LINEAR_MIPMAP_LINEAR) };
        std::lock_guard lock(sCapabilitiesMutex);
        sCapabilities = value;
    }
    inline Capabilities startupCapabilities()
    {
        std::lock_guard lock(sCapabilitiesMutex);
        return sCapabilities;
    }
    inline bool requested()
    {
        const char* value = std::getenv("OPENMW_HITCH_COMPOSITE_CPU_MIPS");
        return value && std::string_view(value) == "1";
    }

    struct Stats
    {
        std::uint64_t attempted, prepared, declined, allocationFailures, sourceBytes, mipBytes, controlApplies;
        std::uint64_t suppliedApplies, uploadSubmissions, producerDrawUses, firstProducerUses,
            requiredFirstUses, optionalFirstUses, controlProducerUses, controlFirstUses, unprovenProducerUses;
    };
    inline std::atomic<std::uint64_t> sAttempted{ 0 }, sPrepared{ 0 }, sDeclined{ 0 }, sAllocationFailures{ 0 },
        sSourceBytes{ 0 }, sMipBytes{ 0 }, sControlApplies{ 0 };
    inline std::atomic<std::uint64_t> sSuppliedApplies{ 0 }, sUploadSubmissions{ 0 }, sProducerDrawUses{ 0 },
        sFirstProducerUses{ 0 }, sRequiredFirstUses{ 0 }, sOptionalFirstUses{ 0 }, sControlProducerUses{ 0 },
        sControlFirstUses{ 0 }, sUnprovenProducerUses{ 0 };
    inline Stats stats() noexcept
    {
        return { sAttempted.load(std::memory_order_relaxed), sPrepared.load(std::memory_order_relaxed),
            sDeclined.load(std::memory_order_relaxed), sAllocationFailures.load(std::memory_order_relaxed),
            sSourceBytes.load(std::memory_order_relaxed), sMipBytes.load(std::memory_order_relaxed),
            sControlApplies.load(std::memory_order_relaxed), sSuppliedApplies.load(std::memory_order_relaxed),
            sUploadSubmissions.load(std::memory_order_relaxed), sProducerDrawUses.load(std::memory_order_relaxed),
            sFirstProducerUses.load(std::memory_order_relaxed), sRequiredFirstUses.load(std::memory_order_relaxed),
            sOptionalFirstUses.load(std::memory_order_relaxed), sControlProducerUses.load(std::memory_order_relaxed),
            sControlFirstUses.load(std::memory_order_relaxed), sUnprovenProducerUses.load(std::memory_order_relaxed) };
    }
#ifdef OPENMW_COMPOSITE_MIPS_TEST_HOOKS
    inline std::atomic<bool> failNextAllocation{ false };
#endif

    struct PreparedImage
    {
        osg::ref_ptr<osg::Image> source;
        osg::ref_ptr<osg::Image> mipmapped;
        Capabilities capabilities;
    };

    // This is intentionally a small, producer-owned alpha-image operation, not
    // a texture/image-loader replacement. Existing required work never depends
    // on successful optional allocation. The original image remains available.
    inline PreparedImage prepare(
        osg::Image* source, const Capabilities& capabilities, bool composite, bool esm3) noexcept
    {
        sAttempted.fetch_add(1, std::memory_order_relaxed);
        const auto decline = [&] {
            sDeclined.fetch_add(1, std::memory_order_relaxed);
            return PreparedImage{};
        };
        constexpr std::uint64_t maxBaseBytes = 1024 * 1024;
        if (!composite || !esm3 || !source || !source->data() || !capabilities.npotMipSupported
            || capabilities.maxDimension <= 0 || source->s() <= 0 || source->t() <= 0 || source->r() != 1
            || source->s() > capabilities.maxDimension || source->t() > capabilities.maxDimension
            || source->getPixelFormat() != GL_ALPHA || source->getDataType() != GL_UNSIGNED_BYTE
            || (source->getInternalTextureFormat() != GL_ALPHA && source->getInternalTextureFormat() != GL_ALPHA8)
            || source->getPacking() != 1 || source->getRowLength() != 0 || source->isMipmap()
            || source->getDataVariance() != osg::Object::STATIC || source->requiresUpdateCall()
            || source->getBufferObject())
            return decline();
        const std::uint64_t baseBytes = std::uint64_t(source->s()) * std::uint64_t(source->t());
        if (baseBytes > maxBaseBytes || source->getTotalDataSize() != baseBytes || baseBytes == 1)
            return decline(); // 1x1 has no extra level; keep OSG's original path.
        try
        {
            struct Level
            {
                unsigned width, height;
                std::size_t offset;
            };
            std::vector<Level> levels{ { unsigned(source->s()), unsigned(source->t()), 0 } };
            osg::Image::MipmapDataType offsets;
            std::uint64_t totalBytes = baseBytes;
            while (levels.back().width > 1 || levels.back().height > 1)
            {
                const unsigned width = std::max(1u, levels.back().width / 2);
                const unsigned height = std::max(1u, levels.back().height / 2);
                if (totalBytes > std::numeric_limits<unsigned>::max())
                    return decline();
                offsets.push_back(unsigned(totalBytes));
                levels.push_back({ width, height, std::size_t(totalBytes) });
                totalBytes += std::uint64_t(width) * height;
            }
            if (totalBytes > 2 * maxBaseBytes || totalBytes > std::numeric_limits<unsigned>::max())
                return decline();
#ifdef OPENMW_COMPOSITE_MIPS_TEST_HOOKS
            if (failNextAllocation.exchange(false))
                throw std::bad_alloc();
#endif
            auto data = std::make_unique<unsigned char[]>(std::size_t(totalBytes));
            std::memcpy(data.get(), source->data(), std::size_t(baseBytes));
            for (std::size_t level = 1; level < levels.size(); ++level)
            {
                const auto previous = levels[level - 1];
                const auto current = levels[level];
                const unsigned char* input = data.get() + previous.offset;
                unsigned char* output = data.get() + current.offset;
                const std::uint64_t denominator = std::uint64_t(previous.width) * previous.height;
                for (unsigned y = 0; y < current.height; ++y)
                {
                    const std::uint64_t y0 = std::uint64_t(y) * previous.height;
                    const std::uint64_t y1 = std::uint64_t(y + 1) * previous.height;
                    for (unsigned x = 0; x < current.width; ++x)
                    {
                        const std::uint64_t x0 = std::uint64_t(x) * previous.width;
                        const std::uint64_t x1 = std::uint64_t(x + 1) * previous.width;
                        std::uint64_t sum = 0;
                        for (unsigned sy = unsigned(y0 / current.height); std::uint64_t(sy) * current.height < y1; ++sy)
                        {
                            const auto wy = std::min(y1, std::uint64_t(sy + 1) * current.height)
                                - std::max(y0, std::uint64_t(sy) * current.height);
                            for (unsigned sx = unsigned(x0 / current.width); std::uint64_t(sx) * current.width < x1;
                                ++sx)
                            {
                                const auto wx = std::min(x1, std::uint64_t(sx + 1) * current.width)
                                    - std::max(x0, std::uint64_t(sx) * current.width);
                                sum += std::uint64_t(input[std::size_t(sy) * previous.width + sx]) * wx * wy;
                            }
                        }
                        output[std::size_t(y) * current.width + x]
                            = static_cast<unsigned char>((sum + denominator / 2) / denominator);
                    }
                }
            }
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->setImage(source->s(), source->t(), 1, source->getInternalTextureFormat(), GL_ALPHA, GL_UNSIGNED_BYTE,
                data.release(), osg::Image::USE_NEW_DELETE, 1, 0);
            image->setOrigin(source->getOrigin());
            image->setMipmapLevels(offsets);
            image->dirty(); // mip metadata is complete before publication.
            sPrepared.fetch_add(1, std::memory_order_relaxed);
            sSourceBytes.fetch_add(baseBytes, std::memory_order_relaxed);
            sMipBytes.fetch_add(totalBytes, std::memory_order_relaxed);
            return { source, image, capabilities };
        }
        catch (const std::bad_alloc&)
        {
            sAllocationFailures.fetch_add(1, std::memory_order_relaxed);
            return decline();
        }
    }

    class Texture final : public Resource::PreparedTerrainTexture
    {
    public:
        Texture() = default;
        explicit Texture(const PreparedImage& prepared)
            : PreparedTerrainTexture(prepared.mipmapped)
            , mSource(prepared.source)
            , mMipmapped(prepared.mipmapped)
            , mCapabilities(prepared.capabilities)
            , mSourceRevision(mSource ? mSource->getModifiedCount() : 0)
            , mMipRevision(mMipmapped ? mMipmapped->getModifiedCount() : 0)
            , mControl(mSource ? new PreparedTerrainTexture(mSource) : nullptr)
        {
            if (mControl)
                mControl->setPreparationMemoEnabled(false);
        }
        Texture(const Texture& other, const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY)
            : PreparedTerrainTexture(other, copy)
            , mSource(other.mSource)
            , mMipmapped(getImage())
            , mCapabilities(other.mCapabilities)
            , mSourceRevision(other.mSourceRevision)
            , mMipRevision(mMipmapped ? mMipmapped->getModifiedCount() : 0)
            , mControl(mSource ? new PreparedTerrainTexture(mSource) : nullptr)
            , mReleased(other.mReleased.load(std::memory_order_acquire))
        {
            if (mControl)
                mControl->setPreparationMemoEnabled(false);
        }
        META_StateAttribute(Resource, Texture, TEXTURE);

        void apply(osg::State& state) const override
        {
            const unsigned context = state.getContextID();
            // These records share OSG's draw-context ownership and sizing.
            // They retain no GL object and perform no driver queries or waits.
            DrawProof* proof = context < mDrawProofs.size() ? &mDrawProofs[context] : nullptr;
            if (proof)
                proof->owner = DrawOwner::Unknown;
            if (!mControl || getImage() != mMipmapped.get() || getSubloadCallback())
            {
                PreparedTerrainTexture::apply(state);
                return;
            }
            if (preparationContextEligible(state))
            {
                const auto* before = getTextureObject(context);
                const bool uploadNeeded = !before || !before->isAllocated() || isDirty(context);
                PreparedTerrainTexture::apply(state);
                sSuppliedApplies.fetch_add(1, std::memory_order_relaxed);
                if (acceptedObject(*this, state, true))
                {
                    if (uploadNeeded)
                        sUploadSubmissions.fetch_add(1, std::memory_order_relaxed);
                    if (proof)
                        rememberDrawProof(*proof, state, *this, DrawOwner::Supplied);
                }
                return;
            }
            mReleased.store(true, std::memory_order_release);
            // Exact original-image control, with its own OSG-managed object.
            // Never swap a shared Image pointer or alias two texture owners.
            synchronizeControl();
            mControl->apply(state);
            state.haveAppliedTextureAttribute(state.getActiveTextureUnit(), mControl);
            sControlApplies.fetch_add(1, std::memory_order_relaxed);
            if (proof && acceptedObject(*mControl, state, false))
                rememberDrawProof(*proof, state, *mControl, DrawOwner::Control);
        }
        void resizeGLObjectBuffers(unsigned size) override
        {
            PreparedTerrainTexture::resizeGLObjectBuffers(size);
            mDrawProofs.resize(size);
            if (mControl)
                mControl->resizeGLObjectBuffers(size);
        }
        void releaseGLObjects(osg::State* state = nullptr) const override
        {
            // A release may be followed by a different native context behind
            // the same GraphicsContext/ID. Keep original uploads thereafter.
            mReleased.store(true, std::memory_order_release);
            PreparedTerrainTexture::releaseGLObjects(state);
            if (mControl)
                mControl->releaseGLObjects(state);
        }
        std::uint64_t retainedSourceBytes() const noexcept override
        {
            const std::array<const osg::Image*, 4> images{ getImage(), mSource.get(), mMipmapped.get(),
                mControl ? mControl->getImage() : nullptr };
            std::uint64_t bytes = 0;
            for (std::size_t i = 0; i < images.size(); ++i)
                if (images[i] && std::find(images.begin(), images.begin() + i, images[i]) == images.begin() + i)
                    bytes += images[i]->getTotalDataSize();
            return bytes;
        }
        const PreparedTerrainTexture* controlTexture() const { return mControl; }

        void recordProducerDrawUse(osg::State& state, bool required) const noexcept override
        {
            const unsigned context = state.getContextID();
            if (context >= mDrawProofs.size())
            {
                sUnprovenProducerUses.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto& proof = mDrawProofs[context];
            const bool supplied = proof.owner == DrawOwner::Supplied;
            const auto* owner = supplied ? this : mControl.get();
            if (proof.owner == DrawOwner::Unknown || !owner || !proof.object.valid() || !proof.context.valid()
                || proof.context.get() != state.getGraphicsContext()
                || proof.object.get() != owner->getTextureObject(context) || !owner->getImage()
                || proof.revision != owner->getImage()->getModifiedCount()
                || (supplied && !preparationContextEligible(state)) || !acceptedObject(*owner, state, supplied))
            {
                sUnprovenProducerUses.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (supplied)
            {
                sProducerDrawUses.fetch_add(1, std::memory_order_relaxed);
                if (!proof.suppliedUsed)
                {
                    proof.suppliedUsed = true;
                    sFirstProducerUses.fetch_add(1, std::memory_order_relaxed);
                    (required ? sRequiredFirstUses : sOptionalFirstUses).fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                sControlProducerUses.fetch_add(1, std::memory_order_relaxed);
                if (!proof.controlUsed)
                {
                    proof.controlUsed = true;
                    sControlFirstUses.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        bool preparationAdmissionEligible(const osg::State& state) const noexcept override
        {
            return mMipmapped && getImage() == mMipmapped.get() && mSource && mCapabilities.context.valid()
                && !mReleased.load(std::memory_order_acquire)
                && state.getGraphicsContext() == mCapabilities.context.get()
                && state.getContextID() == mCapabilities.contextId && mCapabilities.npotMipSupported
                && mSource->getModifiedCount() == mSourceRevision && mMipmapped->getModifiedCount() == mMipRevision
                && getDataVariance() != osg::Object::DYNAMIC && !getSubloadCallback() && !getUpdateCallback()
                && !getEventCallback() && !getReadPBuffer() && !getUnRefImageDataAfterApply()
                && getInternalFormatMode() == osg::Texture::USE_IMAGE_DATA_FORMAT && !getSourceFormat()
                && !getSourceType() && !getClientStorageHint() && !getShadowComparison()
                && !getResizeNonPowerOfTwoHint() && !getBorderWidth();
        }

        bool preparationDispatchEligible(const osg::State& state) const override
        {
            return preparationContextEligible(state);
        }

    protected:
        bool preparationContextEligible(const osg::State& state) const override
        {
            return preparationAdmissionEligible(state) && !_texMipmapGenerationDirtyList[state.getContextID()];
        }

    private:
        enum class DrawOwner { Unknown, Supplied, Control };
        struct DrawProof
        {
            DrawOwner owner = DrawOwner::Unknown;
            osg::observer_ptr<osg::Texture::TextureObject> object;
            osg::observer_ptr<osg::GraphicsContext> context;
            unsigned revision = 0;
            bool suppliedUsed = false, controlUsed = false;
        };
        static bool acceptedObject(const PreparedTerrainTexture& owner, const osg::State& state, bool supplied) noexcept
        {
            const unsigned context = state.getContextID();
            const auto* image = owner.getImage();
            const auto* object = owner.getTextureObject(context);
            return state.getGraphicsContext() && image && image->data() && object && object->id()
                && object->isAllocated() && !owner.isDirty(context) && !owner.getSubloadCallback()
                && object->_profile._target == GL_TEXTURE_2D && object->_profile._width == image->s()
                && object->_profile._height == image->t()
                && (!supplied || (object->_profile._numMipmapLevels > 0
                    && static_cast<unsigned>(object->_profile._numMipmapLevels) == image->getNumMipmapLevels()));
        }
        static void rememberDrawProof(DrawProof& proof, osg::State& state,
            const PreparedTerrainTexture& owner, DrawOwner kind) noexcept
        {
            proof.owner = kind;
            proof.object = owner.getTextureObject(state.getContextID());
            proof.context = state.getGraphicsContext();
            proof.revision = owner.getImage()->getModifiedCount();
        }
        void synchronizeControl() const
        {
            // A deliberately dirtied supplied image uses its current base on
            // the control path. The producer source wins if it changed itself.
            if (mSource->getModifiedCount() == mSourceRevision && mMipmapped->getModifiedCount() != mMipRevision
                && mControlMipRevision != mMipmapped->getModifiedCount())
            {
                osg::ref_ptr<osg::Image> base = new osg::Image;
                base->allocateImage(mMipmapped->s(), mMipmapped->t(), mMipmapped->r(), mMipmapped->getPixelFormat(),
                    mMipmapped->getDataType(), mMipmapped->getPacking());
                std::memcpy(base->data(), mMipmapped->data(), base->getTotalDataSize());
                base->setInternalTextureFormat(mMipmapped->getInternalTextureFormat());
                base->setOrigin(mMipmapped->getOrigin());
                mControl->setImage(base);
                mControlMipRevision = mMipmapped->getModifiedCount();
            }
            else if (mSource->getModifiedCount() != mSourceRevision && mControl->getImage() != mSource.get())
                mControl->setImage(mSource);
            for (auto wrap : { osg::Texture::WRAP_S, osg::Texture::WRAP_T, osg::Texture::WRAP_R })
                mControl->setWrap(wrap, getWrap(wrap));
            for (auto filter : { osg::Texture::MIN_FILTER, osg::Texture::MAG_FILTER })
                mControl->setFilter(filter, getFilter(filter));
            mControl->setBorderWidth(getBorderWidth());
            mControl->setBorderColor(getBorderColor());
            mControl->setMaxAnisotropy(getMaxAnisotropy());
            mControl->setMinLOD(getMinLOD());
            mControl->setMaxLOD(getMaxLOD());
            mControl->setLODBias(getLODBias());
            mControl->setResizeNonPowerOfTwoHint(getResizeNonPowerOfTwoHint());
            mControl->setUseHardwareMipMapGeneration(getUseHardwareMipMapGeneration());
            mControl->setTextureSize(getTextureWidth(), getTextureHeight());
            if (mControl->getInternalFormatMode() != getInternalFormatMode()
                || mControl->getInternalFormat() != getInternalFormat())
            {
                mControl->setInternalFormat(getInternalFormat());
                mControl->setInternalFormatMode(getInternalFormatMode());
                mControl->dirtyTextureObject();
            }
            mControl->setSourceFormat(getSourceFormat());
            mControl->setSourceType(getSourceType());
            mControl->setClientStorageHint(getClientStorageHint());
            // This wrapper explicitly owns both images; unref-after-apply may
            // clear OSG's base pointer but must not discard required fallback.
            mControl->setUnRefImageDataAfterApply(false);
            if (mControl->getSwizzle() != getSwizzle())
                mControl->setSwizzle(getSwizzle());
            mControl->setShadowComparison(getShadowComparison());
            mControl->setShadowCompareFunc(getShadowCompareFunc());
            mControl->setShadowTextureMode(getShadowTextureMode());
            mControl->setShadowAmbient(getShadowAmbient());
        }

        osg::ref_ptr<osg::Image> mSource, mMipmapped;
        Capabilities mCapabilities;
        unsigned mSourceRevision = 0, mMipRevision = 0;
        osg::ref_ptr<PreparedTerrainTexture> mControl;
        mutable unsigned mControlMipRevision = 0;
        mutable std::atomic<bool> mReleased{ false };
        mutable osg::buffered_object<DrawProof> mDrawProofs;
    };
}
#endif
