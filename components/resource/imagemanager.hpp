#ifndef OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H

#include <osg/Image>
#include <osg/Texture2D>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "resourcemanager.hpp"
#include "coldimage.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace osgDB
{
    class Options;
}

namespace Resource
{

    /// @brief Handles loading/caching of Images.
    /// @note May be used from any thread.
    class ImageManager : public ResourceManager
    {
    public:
        explicit ImageManager(const VFS::Manager* vfs, double expiryDelay);
        ~ImageManager();

        /// Create or retrieve an Image
        /// Returns the dummy image if the given image is not found.
        osg::ref_ptr<osg::Image> getImage(VFS::Path::NormalizedView path, bool disableFlip = false);

        // Optional owners must match publication, decode options and VFS
        // generation. Copying the matching charge does not double-charge bytes.
        ObjectCacheLease getImageLease(VFS::Path::NormalizedView path, const osg::Image* expected = nullptr,
            bool disableFlip = false);
        bool imageLeaseCurrent(VFS::Path::NormalizedView path, const ObjectCacheLease& lease,
            bool disableFlip = false);
        ObjectCacheLease prepareImage(VFS::Path::NormalizedView path, std::uint64_t maximumEstimate,
            std::uint64_t retainedLimit = UINT64_MAX);
        void setPreciseRawDdsPreload(bool enabled) { mPreciseRawDdsPreload = enabled; }
        // Startup-only. Required loaders still decode independently; this only
        // coalesces completed immutable publications for the exact source.
        // The ordinary control and existing cold-cache path are unchanged.
        void configureCanonicalPublication(bool enabled);
        struct PublicationStats
        {
            bool enabled = false;
            std::uint64_t publications = 0, winners = 0, discardedImages = 0, discardedBytes = 0;
            std::uint64_t revisionDeclines = 0, epochDeclines = 0, ineligible = 0, lookupRevisionMisses = 0;
        };
        PublicationStats publicationStats() const noexcept;

        osg::Image* getWarningImage();
        // Startup-only. No codec, cold index, sampler, or allocation when off.
        void configureColdCache(ColdImageCache::Config config, SpeculativeScope::Sample sample, void* owner);
        void updateCache(double referenceTime) override;
        void clearCache() override;
        void reportRuntimeDiagnostics(double referenceTime) const noexcept override;

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

    private:
        std::uint64_t sourceRevision(bool disableFlip) const noexcept;
        osg::ref_ptr<osg::Image> mWarningImage;
        bool mPreciseRawDdsPreload = false; // configured before workers start
        osg::ref_ptr<osgDB::Options> mOptions;
        std::unique_ptr<ColdImageCache> mColdCache;
        SpeculativeScope::Sample mColdSample = nullptr;
        void* mColdSampleOwner = nullptr;
        std::atomic<std::uint64_t> mColdEpoch{0};
        // Publication only; never held while reading, encoding or decoding.
        mutable std::mutex mColdPublicationMutex;
        bool mCanonicalPublication = false; // configured before workers start
        std::atomic<std::uint64_t> mPublicationEpoch{0};
        // Only completed publication/clear; never held across VFS or decoding.
        mutable std::mutex mPublicationMutex;
        std::atomic<std::uint64_t> mPublications{0}, mPublicationWinners{0}, mDiscardedImages{0}, mDiscardedBytes{0};
        std::atomic<std::uint64_t> mPublicationRevisionDeclines{0}, mPublicationEpochDeclines{0};
        std::atomic<std::uint64_t> mPublicationIneligible{0}, mLookupRevisionMisses{0};

        ImageManager(const ImageManager&);
        void operator=(const ImageManager&);
    };

}

#endif
