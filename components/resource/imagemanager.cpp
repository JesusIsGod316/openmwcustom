#include <components/debug/hitchtrace.hpp>
#include "imagemanager.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <vector>
#include <osgDB/Registry>

#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "objectcache.hpp"
#include "preloadestimate.hpp"

#ifdef OSG_LIBRARY_STATIC
// This list of plugins should match with the list in the top-level CMakelists.txt.
USE_OSGPLUGIN(png)
USE_OSGPLUGIN(tga)
USE_OSGPLUGIN(dds)
USE_OSGPLUGIN(jpeg)
USE_OSGPLUGIN(bmp)
USE_OSGPLUGIN(osg)
USE_SERIALIZER_WRAPPER_LIBRARY(osg)
#endif

namespace
{

    osg::ref_ptr<osg::Image> createWarningImage()
    {
        osg::ref_ptr<osg::Image> warningImage = new osg::Image;

        int width = 8, height = 8;
        warningImage->allocateImage(width, height, 1, GL_RGB, GL_UNSIGNED_BYTE);
        assert(warningImage->isDataContiguous());
        unsigned char* data = warningImage->data();
        for (int i = 0; i < width * height; ++i)
        {
            data[3 * i] = (255);
            data[3 * i + 1] = (0);
            data[3 * i + 2] = (255);
        }
        return warningImage;
    }

    bool isS3TC(osg::Image* image)
    {
        switch (image->getPixelFormat())
        {
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                return true;
        }
        return false;
    }

    bool checkSupported(osg::Image* image)
    {
        // not bothering with checks for other compression formats right now
        if (!isS3TC(image))
            return true;

        // hashtag yolo (CS might not have context when loading assets)
        if (!SceneUtil::glExtensionsReady())
            return true;

        return SceneUtil::getGLExtensions().isTextureCompressionS3TCSupported;
    }

}

namespace Resource
{

    ImageManager::ImageManager(const VFS::Manager* vfs, double expiryDelay)
        : ResourceManager(vfs, expiryDelay)
        , mWarningImage(createWarningImage())
        , mOptions(new osgDB::Options("dds_dxt1_detect_rgba ignoreTga2Fields"))
    {
    }

    ImageManager::~ImageManager()
    {
        if (mColdCache)
        {
            const auto v=mColdCache->stats();
            Log(Debug::Info) << "OptimizedMW cold image cache: compressed=" << v.compressed
                << " restored=" << v.restored << " refused=" << v.refused
                << " poor_ratio=" << v.poorRatio << " invalid=" << v.invalid
                << " busy=" << v.busy << " source_bytes=" << v.sourceBytes
                << " compressed_bytes=" << v.compressedBytes << " retained_bytes=" << v.retainedBytes
                << " peak_transient_bytes=" << v.peakReservedBytes;
        }
    }

    std::uint64_t ImageManager::sourceRevision(bool disableFlip) const noexcept
    {
        // Options are immutable for this manager's lifetime. Reserve zero for
        // unknown/legacy publications. Do not alias a wrapped VFS generation.
        const auto generation = mVFS->getIndexGeneration();
        return generation <= (UINT64_MAX - 2) / 2 ? generation * 2 + (disableFlip ? 2 : 1) : 0;
    }

    ObjectCacheLease ImageManager::getImageLease(VFS::Path::NormalizedView path, const osg::Image* expected,
        bool disableFlip)
    {
        const auto revision = sourceRevision(disableFlip);
        if (!revision) return {};
        auto result = mCache->getLeaseFromObjectCache(path, expected, revision);
        if (result.mValue == mWarningImage || !result.bytes()) return {};
        const auto* image = dynamic_cast<const osg::Image*>(result.mValue.get());
        if (!image || image->getDataVariance() == osg::Object::DYNAMIC || image->requiresUpdateCall()) return {};
        return result;
    }

    bool ImageManager::imageLeaseCurrent(VFS::Path::NormalizedView path, const ObjectCacheLease& lease, bool disableFlip)
    {
        if (!lease || lease.mSourceRevision != sourceRevision(disableFlip)) return false;
        const auto* image = dynamic_cast<const osg::Image*>(lease.mValue.get());
        if (!image) return false;
        const auto current = getImageLease(path, image, disableFlip);
        return current && current.mRevision == lease.mRevision && current.mCharge == lease.mCharge;
    }

    ObjectCacheLease ImageManager::prepareImage(VFS::Path::NormalizedView path, std::uint64_t maximumEstimate,
        std::uint64_t retainedLimit)
    {
        if (!SpeculativeScope::active() || !maximumEstimate) throw SpeculativeDeferred{};
        if (auto ready = getImageLease(path))
        {
            if (ready.bytes() > maximumEstimate || ready.bytes() > retainedLimit) throw SpeculativeDeferred{};
            return ready;
        }
        // This is an admission estimate, not a bound on arbitrary plugin
        // internals. Unknown headers decline optional work before decoding.
        auto stream = mVFS->get(path);
        const auto estimate = estimateStream(*stream, true, mPreciseRawDdsPreload);
        if (!estimate.knownDimensions || estimate.peak > maximumEstimate || estimate.retained > maximumEstimate
            || estimate.retained > retainedLimit)
            throw SpeculativeDeferred{};
        stream.reset();
        const auto image = getImage(path);
        auto lease = getImageLease(path, image);
        if (!lease || lease.bytes() > maximumEstimate || lease.bytes() > retainedLimit) throw SpeculativeDeferred{};
        return lease;
    }

    osg::ref_ptr<osg::Image> ImageManager::getImage(VFS::Path::NormalizedView path, bool disableFlip)
    {
        Debug::HitchTrace::Scope hitchImage("resource.image_lookup", {}, path.value());
        const auto epoch = mColdCache ? mColdEpoch.load(std::memory_order_acquire) : 0;
        const auto lookupRevision = mColdCache ? sourceRevision(disableFlip) : 0;
        const auto cacheWarning = [&] {
            if (mColdCache)
            {
                std::lock_guard lock(mColdPublicationMutex);
                if (epoch == mColdEpoch.load(std::memory_order_acquire)
                    && lookupRevision == sourceRevision(disableFlip))
                    mCache->addEntryToObjectCache(path.value(), mWarningImage,0.0,lookupRevision);
            }
            else mCache->addEntryToObjectCache(path.value(), mWarningImage);
        };
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(path);
        if (mColdCache && obj)
        {
            const auto currentRevision = sourceRevision(disableFlip);
            const auto lease = mCache->getLeaseFromObjectCache(path, obj.get());
            if (!lease || lease.mSourceRevision != currentRevision)
            {
                if (lease) mCache->removeIfSame(path.value(), lease);
                obj = nullptr;
            }
            else if (auto* cold = dynamic_cast<ColdImage*>(obj.get()))
            {
                auto restored = mColdCache->restore(*cold, path.value(), currentRevision,
                    mColdSample(mColdSampleOwner), mCache->speculativeBudget());
                if (restored.status == ColdImageCache::RestoreStatus::Invalid)
                    mCache->removeIfSame(path.value(), lease);
                if (restored.image)
                {
                    std::lock_guard lock(mColdPublicationMutex);
                    if (currentRevision == sourceRevision(disableFlip)
                        && epoch == mColdEpoch.load(std::memory_order_acquire))
                    {
                        auto [winner, inserted] = mCache->replaceIfSameOrInsert(
                            path.value(), cold, restored.image, currentRevision);
                        if (auto* image = dynamic_cast<osg::Image*>(winner.get())) return image;
                        return restored.image;
                    }
                }
                // Busy/denied/invalid restoration is optional. Required demand
                // makes progress through the authoritative ordinary reader.
                obj = nullptr;
            }
        }
        if (obj)
            return osg::ref_ptr<osg::Image>(static_cast<osg::Image*>(obj.get()));
        else
        {
            const auto publicationRevision = sourceRevision(disableFlip);
            auto claim = SpeculativeScope::claim(SpeculativeScope::active()
                ? std::to_string(mVFS->getIndexGeneration()) + "|image|" + std::string(path.value())
                    + (disableFlip ? "|no-flip" : "|flip") : std::string{});
            Debug::RuntimeDiagnostics::Operation diagnostic("image_cache_miss", path.value());
            Files::IStreamPtr stream;
            try
            {
                stream = mVFS->get(path);
            }
            catch (std::exception& e)
            {
                Log(Debug::Error) << "Failed to open image: " << e.what();
                cacheWarning();
                return mWarningImage;
            }

            std::string ext(Misc::getFileExtension(path.value()));

            // Non-standard, but Morrowind supports this
            if (ext == "targa")
                ext = "tga";

            osgDB::ReaderWriter* reader;
            { Debug::HitchTrace::Scope discovery("resource.image_reader_discovery", {}, ext);
              reader = osgDB::Registry::instance()->getReaderWriterForExtension(ext); }
            if (!reader)
            {
                Log(Debug::Error) << "Error loading " << path << ": no readerwriter for '" << ext << "' found";
                cacheWarning();
                return mWarningImage;
            }

            const auto estimate = SpeculativeScope::active() ? estimateStream(*stream, true, mPreciseRawDdsPreload) : PreloadEstimate{};
            SpeculativeScope::Stage memoryStage(estimate.peak, estimate.retained);
            bool killAlpha = false;
            if (reader->supportedExtensions().count("tga"))
            {
                // Morrowind ignores the alpha channel of 16bpp TGA files even when the header says not to
                unsigned char header[18];
                stream->read((char*)header, 18);
                if (stream->gcount() != 18)
                {
                    Log(Debug::Error) << "Error loading " << path << ": couldn't read TGA header";
                    cacheWarning();
                    return mWarningImage;
                }
                int type = header[2];
                int depth;
                if (type == 1 || type == 9)
                    depth = header[7];
                else
                    depth = header[16];
                int alphaBPP = header[17] & 0x0F;
                killAlpha = depth == 16 && alphaBPP == 1;
                stream->seekg(0);
            }

            osgDB::ReaderWriter::ReadResult result;
            { Debug::HitchTrace::Scope hitchDecode("resource.image_decode", {}, path.value());
              result = reader->readImage(*stream, mOptions); }
            if (!result.success())
            {
                Log(Debug::Error) << "Error loading " << path << ": " << result.message() << " code "
                                  << result.status();
                cacheWarning();
                return mWarningImage;
            }

            osg::ref_ptr<osg::Image> image = result.getImage();
            {
                Debug::HitchTrace::Scope detail("resource.image_postprocess", Debug::HitchTrace::CpuTiming::Scoped);

                image->setFileName(std::string(path.value()));
                if (!checkSupported(image))
                {
                    static bool uncompress = (getenv("OPENMW_DECOMPRESS_TEXTURES") != nullptr);
                    if (!uncompress)
                    {
                        Log(Debug::Error) << "Error loading " << path << ": no S3TC texture compression support installed";
                        cacheWarning();
                        return mWarningImage;
                    }
                    else
                    {
                        // decompress texture in software if not supported by GPU
                        // requires update to getColor() to be released with OSG 3.6
                        osg::ref_ptr<osg::Image> newImage = new osg::Image;
                        newImage->setFileName(image->getFileName());
                        newImage->setOrigin(image->getOrigin());
                        newImage->allocateImage(image->s(), image->t(), image->r(),
                            image->isImageTranslucent() ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE);
                        for (int s = 0; s < image->s(); ++s)
                            for (int t = 0; t < image->t(); ++t)
                                for (int r = 0; r < image->r(); ++r)
                                    newImage->setColor(image->getColor(s, t, r), s, t, r);
                        image = newImage;
                    }
                }
                else if (killAlpha)
                {
                    osg::ref_ptr<osg::Image> newImage = new osg::Image;
                    newImage->setFileName(image->getFileName());
                    newImage->setOrigin(image->getOrigin());
                    newImage->allocateImage(image->s(), image->t(), image->r(), GL_RGB, GL_UNSIGNED_BYTE);
                    // OSG just won't write the alpha as there's nowhere to put it.
                    for (int s = 0; s < image->s(); ++s)
                        for (int t = 0; t < image->t(); ++t)
                            for (int r = 0; r < image->r(); ++r)
                                newImage->setColor(image->getColor(s, t, r), s, t, r);
                    image = newImage;
                }

                // OSG might not set the right origin for DDS
                if (ext == "dds")
                    image->setOrigin(osg::Image::TOP_LEFT);

                // Convert the image to the convention we expect
                if (image->getOrigin() == osg::Image::BOTTOM_LEFT && !disableFlip)
                {
                    if (image->isCompressed() && !isS3TC(image))
                    {
                        // This is most likely a KTX texture that OSG can't flip
                        // We don't want it to be corrupted or displayed incorrectly, so bail
                        // OSGoS *can* flip RGTC, but we can't verify that (yet?)
                        Log(Debug::Error) << "Error loading " << path << ": cannot flip non-S3TC compressed texture";
                        cacheWarning();
                        return mWarningImage;
                    }

                    image->flipVertical();
                    image->setOrigin(osg::Image::TOP_LEFT);
                }

                if (mColdCache)
                {
                    std::lock_guard lock(mColdPublicationMutex);
                    if (publicationRevision == sourceRevision(disableFlip)
                        && epoch == mColdEpoch.load(std::memory_order_acquire))
                    {
                        // The expected value may be an immutable cold entry.
                        // Ordinary concurrent loaders/restores share one winner.
                        const auto expected = mCache->getLeaseFromObjectCache(path);
                        const auto* cold = expected ? dynamic_cast<const ColdImage*>(expected.mValue.get()) : nullptr;
                        auto [winner, inserted] = mCache->replaceIfSameOrInsert(
                            path.value(), cold, image, publicationRevision);
                        if (auto* canonical = dynamic_cast<osg::Image*>(winner.get())) image = canonical;
                    }
                }
                else
                    mCache->addEntryToObjectCache(path.value(), image, 0.0,
                        publicationRevision == sourceRevision(disableFlip) ? publicationRevision : 0);
            }

            return image;
        }
    }

    osg::Image* ImageManager::getWarningImage()
    {
        return mWarningImage;
    }

    void ImageManager::configureColdCache(ColdImageCache::Config config,
        SpeculativeScope::Sample sample, void* owner)
    {
        mColdSample = sample; mColdSampleOwner = owner;
        mColdCache = config.enabled && sample && owner ? std::make_unique<ColdImageCache>(config) : nullptr;
    }

    void ImageManager::updateCache(double referenceTime)
    {
        if (mColdCache && mCache->speculativeBudget())
        {
            const auto sample = mColdSample(mColdSampleOwner);
            if (SpeculativeBudget::newOwnerWorkUsable(sample, SpeculativePriority::Background))
            {
                std::optional<CacheType::ColdCandidate> candidate;
                std::uint64_t epoch=0;
                {
                    std::lock_guard lock(mColdPublicationMutex);
                    epoch=mColdEpoch.load(std::memory_order_acquire);
                    candidate=mCache->detachColdCandidate(referenceTime, mColdCache->config().minimumAge,
                    [&](const osg::Object* object) {
                        const auto* image = dynamic_cast<const osg::Image*>(object);
                        return image && image != mWarningImage.get() && ColdImageCache::eligible(*image, mColdCache->config());
                    });
                }
                if (candidate)
                {
                    auto* source = static_cast<osg::Image*>(candidate->lease.mValue.get());
                    // Detached payload has one exclusive owner during the codec;
                    // concurrent demand cannot mutate or wait for it.
                    auto encoded = mColdCache->encode(*source, candidate->lease.mSourceRevision,
                        sample, mCache->speculativeBudget());
                    std::lock_guard lock(mColdPublicationMutex);
                    const bool current = candidate->lease.mSourceRevision == sourceRevision(false)
                        || candidate->lease.mSourceRevision == sourceRevision(true);
                    if (current && epoch == mColdEpoch.load(std::memory_order_acquire))
                    {
                        const bool published = encoded.image && mCache->publishDetachedCold(candidate->key,
                            candidate->lease, encoded.image, encoded.image->allocatedBytes(),
                            encoded.reservation ? &*encoded.reservation : nullptr);
                        if (!published)
                            mCache->replaceIfSameOrInsert(candidate->key, nullptr, source, candidate->lease.mSourceRevision);
                    }
                }
            }
        }
        ResourceManager::updateCache(referenceTime);
    }

    void ImageManager::clearCache()
    {
        if (mColdCache)
        {
            std::lock_guard lock(mColdPublicationMutex);
            mColdEpoch.fetch_add(1, std::memory_order_acq_rel);
            ResourceManager::clearCache();
        }
        else ResourceManager::clearCache();
    }

    void ImageManager::reportRuntimeDiagnostics(double referenceTime) const noexcept
    {
        if (!mDiagnosticSampler.due()) return;
        mCache->reportRuntimeDiagnostics("images", referenceTime, mExpiryDelay);
        if (!mColdCache) return;
        const auto s = mColdCache->stats();
        Debug::RuntimeDiagnostics::recordEvent("cold_image_cache", "images", "Unique backing capacity; not process RAM", {
            {"retained_bytes", s.retainedBytes}, {"reserved_bytes", s.reservedBytes},
            {"reserved_cold_bytes", s.reservedColdBytes}, {"jobs", s.jobs},
            {"compressed", s.compressed}, {"restored", s.restored}, {"refused", s.refused},
            {"poor_ratio", s.poorRatio}, {"invalid", s.invalid}, {"busy", s.busy},
            {"source_bytes_cumulative", s.sourceBytes}, {"compressed_bytes_cumulative", s.compressedBytes},
            {"peak_reserved_bytes", s.peakReservedBytes},
            {"retained_limit", mColdCache->config().retainedLimit},
            {"maximum_image", mColdCache->config().maximumImage}});
    }

    void ImageManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Image", frameNumber, mCache->getStats(), *stats);

        static Debug::V3Diagnostics::CsvWriter v36Writer("OPENMW_V36_RESIDENCY_FILE",
            "frame,epoch_ms,event,path,estimated_source_mb,width,height,mip_levels,compressed,ref_count,"
            "cache_entries,last_usage,total_estimated_source_mb");
        if (!v36Writer.enabled() || frameNumber % 300 != 0)
            return;

        struct Entry
        {
            std::string mPath;
            unsigned int mBytes = 0;
            int mWidth = 0;
            int mHeight = 0;
            unsigned int mMipLevels = 0;
            bool mCompressed = false;
            int mRefCount = 0;
            double mLastUsage = 0.0;
        };
        std::vector<Entry> entries;
        std::uint64_t totalBytes = 0;
        mCache->callWithUsage([&](const auto& key, osg::Object* object, double lastUsage) {
            const osg::Image* image = dynamic_cast<const osg::Image*>(object);
            if (!image)
                return;
            const unsigned int bytes = image->getTotalSizeInBytesIncludingMipmaps();
            totalBytes += bytes;
            entries.push_back({ key, bytes, image->s(), image->t(), image->getNumMipmapLevels(),
                image->isCompressed(), image->referenceCount(), lastUsage });
        });
        std::ranges::sort(entries, [](const Entry& left, const Entry& right) { return left.mBytes > right.mBytes; });
        constexpr double MiB = 1024.0 * 1024.0;
        const double totalMb = static_cast<double>(totalBytes) / MiB;
        std::ostringstream summary;
        summary << frameNumber << ',' << Debug::V3Diagnostics::epochMs() << ",summary,"
                << Debug::V3Diagnostics::csvQuote("") << ',' << std::fixed << std::setprecision(3)
                << "0,0,0,0,0,0," << entries.size() << ",0," << totalMb;
        v36Writer.writeLine(summary.str());
        const std::size_t limit = std::min<std::size_t>(entries.size(), 32);
        for (std::size_t i = 0; i < limit; ++i)
        {
            const Entry& entry = entries[i];
            std::ostringstream row;
            row << frameNumber << ',' << Debug::V3Diagnostics::epochMs() << ",largest,"
                << Debug::V3Diagnostics::csvQuote(entry.mPath) << ',' << std::fixed << std::setprecision(3)
                << (static_cast<double>(entry.mBytes) / MiB) << ',' << entry.mWidth << ',' << entry.mHeight << ','
                << entry.mMipLevels << ',' << (entry.mCompressed ? 1 : 0) << ',' << entry.mRefCount << ','
                << entries.size() << ',' << entry.mLastUsage << ',' << totalMb;
            v36Writer.writeLine(row.str());
        }
    }

}
