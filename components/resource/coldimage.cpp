#include "coldimage.hpp"

#include <components/debug/hitchtrace.hpp>

#include <lz4.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <typeinfo>
#include <vector>

namespace Resource
{
    struct ColdImageCache::State
    {
        mutable std::mutex mutex;
        Stats stats;
        bool reserve(std::uint64_t peak, std::uint64_t retained, const Config& config)
        {
            std::lock_guard lock(mutex);
            if (stats.jobs >= 4 || peak > config.transientLimit
                || stats.reservedBytes > config.transientLimit - peak
                || retained > config.retainedLimit || stats.retainedBytes > config.retainedLimit - retained
                || stats.reservedColdBytes > config.retainedLimit - retained - stats.retainedBytes)
            { ++stats.refused; return false; }
            ++stats.jobs;
            stats.reservedBytes += peak; stats.reservedColdBytes += retained;
            stats.peakReservedBytes = (std::max)(stats.peakReservedBytes, stats.reservedBytes);
            return true;
        }
        void release(std::uint64_t peak, std::uint64_t retained)
        {
            std::lock_guard lock(mutex);
            --stats.jobs; stats.reservedBytes -= peak; stats.reservedColdBytes -= retained;
        }
    };

    struct ColdImage::Storage
    {
        // Not a serialized OSG object: one explicit immutable typed byte block.
        static constexpr unsigned Version = 1;
        unsigned version = Version;
        int width = 0, height = 0, depth = 0, internalFormat = 0, packing = 0, rowLength = 0;
        GLenum pixelFormat = 0, type = 0;
        osg::Image::Origin origin = osg::Image::BOTTOM_LEFT;
        osg::Image::WriteHint writeHint = osg::Image::NO_PREFERENCE;
        float pixelAspect = 1;
        osg::Object::DataVariance variance = osg::Object::UNSPECIFIED;
        osg::Image::MipmapDataType mipOffsets;
        std::uint64_t revision = 0, checksum = 0, decoded = 0, compressed = 0, allocated = 0;
        std::unique_ptr<unsigned char[]> bytes;
        std::shared_ptr<ColdImageCache::State> state;
        ~Storage()
        {
            if (state)
            {
                bytes.reset();
                std::lock_guard lock(state->mutex);
                state->stats.retainedBytes -= allocated;
            }
        }
    };

    namespace
    {
        std::uint64_t checksum(const unsigned char* data, std::size_t size) noexcept
        {
            std::uint64_t result = 14695981039346656037ull;
            for (std::size_t i = 0; i < size; ++i) { result ^= data[i]; result *= 1099511628211ull; }
            return result;
        }
        struct LocalReservation
        {
            std::shared_ptr<ColdImageCache::State> state;
            std::uint64_t peak, retained;
            ~LocalReservation() { if (state) state->release(peak, retained); }
        };
        bool formatSupported(GLenum format) noexcept
        {
            return format == GL_ALPHA || format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA
                || format == GL_RGB || format == GL_RGBA;
        }
        bool offsetsValid(const osg::Image::MipmapDataType& offsets, std::uint64_t size) noexcept
        {
            std::uint64_t previous = 0;
            for (const auto offset : offsets)
            {
                if (offset <= previous || offset >= size) return false;
                previous = offset;
            }
            return offsets.size() <= 32;
        }
    }

    ColdImage::ColdImage(const ColdImage& other, const osg::CopyOp& copyop)
        : osg::Object(other, copyop), mStorage(other.mStorage) {}
    std::uint64_t ColdImage::sourceRevision() const noexcept { return mStorage ? mStorage->revision : 0; }
    std::uint64_t ColdImage::allocatedBytes() const noexcept { return mStorage ? mStorage->allocated : 0; }
    std::uint64_t ColdImage::decodedBytes() const noexcept { return mStorage ? mStorage->decoded : 0; }
    Debug::RuntimeDiagnostics::PayloadInfo ColdImage::diagnosticPayload() const noexcept
    {
        return {mStorage.get(), allocatedBytes(), 1, mStorage && mStorage.use_count() > 1};
    }
#ifdef OPENMW_COLD_IMAGE_TEST_HOOKS
    void ColdImage::corruptForTest()
    {
        if (mStorage && mStorage->compressed)
            const_cast<Storage*>(mStorage.get())->bytes[0] ^= 0xff;
    }
#endif

    ColdImageCache::ColdImageCache(Config config) : mConfig(config), mState(std::make_shared<State>())
    {
        constexpr auto limit = 4ull * 1024 * 1024; // one non-preemptible codec block
        mConfig.maximumImage = std::clamp<std::uint64_t>(mConfig.maximumImage, 1, limit);
        mConfig.minimumImage = (std::min)(mConfig.minimumImage, mConfig.maximumImage);
        if (!std::isfinite(mConfig.minimumAge) || mConfig.minimumAge < 0) mConfig.minimumAge = 30;
    }

    bool ColdImageCache::eligible(const osg::Image& image, const Config& config) noexcept
    {
        const auto size = image.getTotalDataSize();
        return image.data() && size >= config.minimumImage && size <= config.maximumImage
            && size <= LZ4_MAX_INPUT_SIZE && size <= std::uint64_t((std::numeric_limits<int>::max)())
            && image.s() > 0 && image.t() > 0 && image.r() == 1 && image.s() <= 16384 && image.t() <= 16384
            && formatSupported(image.getPixelFormat()) && image.getDataType() == GL_UNSIGNED_BYTE
            && !image.isCompressed() && image.isDataContiguous()
            && typeid(image) == typeid(osg::Image)
            && image.getDataVariance() != osg::Object::DYNAMIC && !image.requiresUpdateCall()
            && !image.getBufferObject() && !image.getUserDataContainer()
            && image.getAllocationMode() != osg::Image::NO_DELETE
            && image.getRowLength() == 0
            && (image.getPacking() == 1 || image.getPacking() == 2 || image.getPacking() == 4 || image.getPacking() == 8)
            && offsetsValid(image.getMipmapLevels(), size);
    }

    ColdImageCache::Encoded ColdImageCache::encode(const osg::Image& image, std::uint64_t revision,
        const OpenGlPressureSample& sample, SpeculativeBudget* budget)
    {
        Debug::HitchTrace::Scope trace("resource.cold_image_encode");
        if (!mConfig.enabled || !revision || !budget || !eligible(image, mConfig)) return {};
        const auto decoded = std::uint64_t(image.getTotalDataSize());
        const auto bound = std::uint64_t(LZ4_compressBound(static_cast<int>(decoded)));
        const auto metadata = sizeof(ColdImage::Storage) + sizeof(ColdImage)
            + image.getMipmapLevels().capacity() * sizeof(unsigned);
        // Old backing + bound destination + compact final copy coexist. Old
        // owners are not credited as released during this reservation.
        const auto peak = decoded + bound + bound + metadata;
        if (!mState->reserve(peak, bound + metadata, mConfig)) return {};
        LocalReservation local{mState, peak, bound + metadata};
        Encoded result;
        try
        {
            result.reservation.emplace(budget->reserve(sample, peak));
            const auto modified = image.getModifiedCount();
            std::vector<char> temporary(static_cast<std::size_t>(bound));
            const int compressed = LZ4_compress_default(reinterpret_cast<const char*>(image.data()),
                temporary.data(), static_cast<int>(decoded), static_cast<int>(bound));
            if (compressed <= 0) return {};
            const auto allocated = std::uint64_t(compressed) + metadata;
            if (allocated >= decoded || mConfig.minimumSaving > decoded - allocated)
            {
                std::lock_guard lock(mState->mutex); ++mState->stats.poorRatio;
                return {};
            }
            auto storage = std::make_shared<ColdImage::Storage>();
            storage->width = image.s(); storage->height = image.t(); storage->depth = image.r();
            storage->internalFormat = image.getInternalTextureFormat();
            storage->pixelFormat = image.getPixelFormat(); storage->type = image.getDataType();
            storage->packing = image.getPacking(); storage->rowLength = image.getRowLength();
            storage->origin = image.getOrigin(); storage->writeHint = image.getWriteHint();
            storage->pixelAspect = image.getPixelAspectRatio();
            storage->variance = image.getDataVariance();
            storage->mipOffsets = image.getMipmapLevels();
            storage->revision = revision; storage->decoded = decoded; storage->compressed = compressed;
            storage->checksum = checksum(image.data(), static_cast<std::size_t>(decoded));
            storage->bytes = std::make_unique<unsigned char[]>(compressed); // exact allocated capacity
            std::memcpy(storage->bytes.get(), temporary.data(), compressed);
            storage->allocated = std::uint64_t(compressed) + sizeof(ColdImage::Storage) + sizeof(ColdImage)
                + storage->mipOffsets.capacity() * sizeof(unsigned);
            if (image.getModifiedCount() != modified) return {};
            {
                std::lock_guard lock(mState->mutex);
                mState->stats.retainedBytes += storage->allocated;
                ++mState->stats.compressed; mState->stats.sourceBytes += decoded;
                mState->stats.compressedBytes += compressed;
            }
            storage->state = mState;
            result.image = new ColdImage(std::move(storage));
            trace.setMetric(0, "source_bytes", decoded); trace.setMetric(1, "allocated_bytes", result.image->allocatedBytes());
            return result;
        }
        catch (const std::exception&)
        {
            std::lock_guard lock(mState->mutex); ++mState->stats.refused;
            return {};
        }
    }

    ColdImageCache::Restored ColdImageCache::restore(ColdImage& cold, std::string_view filename,
        std::uint64_t revision, const OpenGlPressureSample& sample, SpeculativeBudget* budget)
    {
        Debug::HitchTrace::Scope trace("resource.cold_image_restore");
        std::unique_lock flight(cold.mRestoreMutex, std::try_to_lock);
        if (!flight.owns_lock())
        {
            std::lock_guard lock(mState->mutex); ++mState->stats.busy;
            return {{}, RestoreStatus::Busy};
        }
        const auto storage = cold.mStorage;
        if (!storage || storage->version != ColdImage::Storage::Version || !revision
            || storage->revision != revision || !storage->bytes || !storage->compressed || !storage->decoded
            || storage->compressed > mConfig.maximumImage || storage->decoded > mConfig.maximumImage
            || storage->decoded > LZ4_MAX_INPUT_SIZE || !formatSupported(storage->pixelFormat)
            || storage->type != GL_UNSIGNED_BYTE || !offsetsValid(storage->mipOffsets, storage->decoded))
        {
            std::lock_guard lock(mState->mutex); ++mState->stats.invalid;
            return {{}, RestoreStatus::Invalid};
        }
        if (!mConfig.enabled || !budget) return {};
        const auto peak = storage->allocated + storage->decoded + sizeof(osg::Image)
            + storage->mipOffsets.capacity() * sizeof(unsigned) + filename.size();
        if (!mState->reserve(peak, 0, mConfig)) return {};
        LocalReservation local{mState, peak, 0};
        try
        {
            auto reservation = budget->reserve(sample, peak, SpeculativeScope::active()
                ? SpeculativeScope::capture().priority : SpeculativePriority::Background);
            auto bytes = std::make_unique<unsigned char[]>(static_cast<std::size_t>(storage->decoded));
            const int decoded = LZ4_decompress_safe(reinterpret_cast<const char*>(storage->bytes.get()),
                reinterpret_cast<char*>(bytes.get()), static_cast<int>(storage->compressed), static_cast<int>(storage->decoded));
            if (decoded < 0 || std::uint64_t(decoded) != storage->decoded
                || checksum(bytes.get(), static_cast<std::size_t>(storage->decoded)) != storage->checksum)
            {
                std::lock_guard lock(mState->mutex); ++mState->stats.invalid;
                return {{}, RestoreStatus::Invalid};
            }
            osg::ref_ptr<osg::Image> result = new osg::Image;
            result->setImage(storage->width, storage->height, storage->depth, storage->internalFormat,
                storage->pixelFormat, storage->type, bytes.get(), osg::Image::USE_NEW_DELETE,
                storage->packing, storage->rowLength);
            bytes.release();
            result->setMipmapLevels(storage->mipOffsets);
            result->setOrigin(storage->origin); result->setWriteHint(storage->writeHint);
            result->setPixelAspectRatio(storage->pixelAspect);
            result->setDataVariance(storage->variance); result->setFileName(std::string(filename));
            if (result->getTotalDataSize() != storage->decoded)
            {
                std::lock_guard lock(mState->mutex); ++mState->stats.invalid;
                return {{}, RestoreStatus::Invalid};
            }
            // Demand has no outer Stage. Track restored backing before releasing
            // its reservation, so publication cannot create uncharged growth.
            auto charge = budget->track({result->data(), 1u}, result->getTotalDataSize(), true, &reservation);
            { std::lock_guard lock(mState->mutex); ++mState->stats.restored; }
            trace.setMetric(0, "decoded_bytes", storage->decoded);
            return {std::move(result), RestoreStatus::Ready, std::move(charge)};
        }
        catch (const std::exception&)
        {
            std::lock_guard lock(mState->mutex); ++mState->stats.refused;
            return {};
        }
    }

    ColdImageCache::Stats ColdImageCache::stats() const
    {
        std::lock_guard lock(mState->mutex); return mState->stats;
    }
}
