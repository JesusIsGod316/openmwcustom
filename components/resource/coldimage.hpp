#ifndef OPENMW_COMPONENTS_RESOURCE_COLDIMAGE_H
#define OPENMW_COMPONENTS_RESOURCE_COLDIMAGE_H

#include "speculativebudget.hpp"
#include <components/debug/runtimediagnostics.hpp>

#include <osg/Image>
#include <osg/Object>

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace Resource
{
    class ColdImageCache;
    class ColdImage final : public osg::Object, public Debug::RuntimeDiagnostics::PayloadSource
    {
    public:
        struct Storage;
        ColdImage() = default;
        ColdImage(const ColdImage& other, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY);
        META_Object(Resource, ColdImage)
        std::uint64_t sourceRevision() const noexcept;
        std::uint64_t allocatedBytes() const noexcept;
        std::uint64_t decodedBytes() const noexcept;
        Debug::RuntimeDiagnostics::PayloadInfo diagnosticPayload() const noexcept override;
#ifdef OPENMW_COLD_IMAGE_TEST_HOOKS
        void corruptForTest();
        void lockRestoreForTest() { mRestoreMutex.lock(); }
        void unlockRestoreForTest() { mRestoreMutex.unlock(); }
#endif
    private:
        friend class ColdImageCache;
        explicit ColdImage(std::shared_ptr<const Storage> storage) : mStorage(std::move(storage)) {}
        std::shared_ptr<const Storage> mStorage;
        // Busy consumers use the ordinary loader; no demand waits for a codec.
        std::mutex mRestoreMutex;
    };

    class ColdImageCache
    {
    public:
        struct Config
        {
            bool enabled = false;
            std::uint64_t retainedLimit = 256ull * 1024 * 1024;
            std::uint64_t transientLimit = 16ull * 1024 * 1024;
            std::uint64_t maximumImage = 4ull * 1024 * 1024;
            std::uint64_t minimumImage = 64ull * 1024;
            std::uint64_t minimumSaving = 4096;
            double minimumAge = 30.0;
        };
        struct Stats
        {
            std::uint64_t retainedBytes = 0, reservedBytes = 0, reservedColdBytes = 0, jobs = 0;
            std::uint64_t compressed = 0, restored = 0, refused = 0, poorRatio = 0, invalid = 0, busy = 0;
            std::uint64_t sourceBytes = 0, compressedBytes = 0, peakReservedBytes = 0;
        };
        struct State;
        struct Encoded
        {
            std::optional<SpeculativeBudget::Stage> reservation;
            osg::ref_ptr<ColdImage> image;
        };
        enum class RestoreStatus { Ready, Busy, Deferred, Invalid };
        struct Restored
        {
            // Manager publication adopts the same charge by backing identity.
            // Never attach accounting metadata to cloned scene/image userdata.
            SpeculativeBudget::ChargePtr charge;
            osg::ref_ptr<osg::Image> image;
            RestoreStatus status = RestoreStatus::Deferred;
            Restored(osg::ref_ptr<osg::Image> value = {}, RestoreStatus result = RestoreStatus::Deferred,
                SpeculativeBudget::ChargePtr accounting = {})
                : charge(std::move(accounting)), image(std::move(value)), status(result) {}
            Restored(Restored&&) = default;
            Restored& operator=(Restored&& other) noexcept
            {
                if (this != &other)
                {
                    image = nullptr; charge.reset();
                    charge = std::move(other.charge); image = std::move(other.image); status = other.status;
                }
                return *this;
            }
        };

        explicit ColdImageCache(Config config);
        const Config& config() const noexcept { return mConfig; }
        // Caller owns a detached cache-only lease, or an immutable private
        // fixture buffer. Never call on an externally mutable shared image.
        Encoded encode(const osg::Image& image, std::uint64_t sourceRevision,
            const OpenGlPressureSample& sample, SpeculativeBudget* budget);
        Restored restore(ColdImage& image, std::string_view filename, std::uint64_t sourceRevision,
            const OpenGlPressureSample& sample, SpeculativeBudget* budget);
        Stats stats() const;
        static bool eligible(const osg::Image& image, const Config& config) noexcept;
    private:
        Config mConfig;
        std::shared_ptr<State> mState;
    };
}
#endif
