#include <components/resource/coldimage.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/cachemaintenance.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>

#include <osgDB/Registry>
#include <osg/Group>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace Resource;
    unsigned checks = 0;
    void require(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); ++checks; }
    OpenGlPressureSample sample(void* owner)
    {
        auto& budget = *static_cast<SpeculativeBudget*>(owner);
        OpenGlPressureSample s;
        s.generation = 1; s.growthWatermarkValid = true; s.accountedGrowth = budget.watermark()->load();
        s.memory.physicalValid = s.memory.commitValid = true;
        s.memory.physicalTotal = 32ull * 1024 * SpeculativeBudget::MiB;
        s.memory.physicalAvailable = 16ull * 1024 * SpeculativeBudget::MiB;
        s.memory.commitAvailable = 24ull * 1024 * SpeculativeBudget::MiB;
        s.decision.state = OpenGlPressure::Normal; s.decision.admissionsPerSample = 4;
        s.decision.limits = OpenGlPressurePolicy{}.limits(s.memory.physicalTotal);
        return s;
    }
    osg::ref_ptr<osg::Image> image()
    {
        osg::ref_ptr<osg::Image> result = new osg::Image;
        result->allocateImage(256, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
        std::memset(result->data(), 63, result->getTotalDataSize());
        result->setInternalTextureFormat(GL_RGBA8);
        result->setOrigin(osg::Image::TOP_LEFT); result->setPixelAspectRatio(1.25f);
        result->setWriteHint(osg::Image::STORE_INLINE);
        return result;
    }
    struct MemoryFile final : VFS::File
    {
        std::string bytes;
        std::atomic_uint opens{0};
        std::mutex openMutex;
        std::condition_variable openChanged;
        bool blockOpen = false, openEntered = false, openReleased = false;
        void armOpen()
        {
            std::lock_guard lock(openMutex);
            blockOpen = true; openEntered = false; openReleased = false;
        }
        bool waitOpen()
        {
            std::unique_lock lock(openMutex);
            return openChanged.wait_for(lock, std::chrono::seconds(5), [&] { return openEntered; });
        }
        void releaseOpen()
        {
            std::lock_guard lock(openMutex);
            openReleased = true; blockOpen = false; openChanged.notify_all();
        }
        Files::IStreamPtr open() override
        {
            ++opens;
            {
                std::unique_lock lock(openMutex);
                if (blockOpen)
                {
                    openEntered = true; openChanged.notify_all();
                    openChanged.wait(lock, [&] { return openReleased; });
                }
            }
            return std::make_unique<std::istringstream>(bytes, std::ios::in | std::ios::binary);
        }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "cold image native fixture"; }
    };
    class MemoryArchive final : public VFS::Archive
    {
    public:
        std::map<std::string, std::unique_ptr<MemoryFile>> files;
        void listResources(VFS::FileMap& out) override
        { for (auto& [path, file] : files) out.emplace(VFS::Path::Normalized(path), file.get()); }
        bool contains(VFS::Path::NormalizedView path) const override { return files.contains(std::string(path.value())); }
        std::string getDescription() const override { return "private cold image real-reader fixture"; }
    };
    std::string tga()
    {
        std::string bytes(18, '\0');
        bytes[2] = 2; bytes[13] = 1; bytes[15] = 1; bytes[16] = 24;
        bytes.append(256 * 256 * 3, '\x3f');
        return bytes;
    }
    class FixtureManager final : public ImageManager
    {
    public:
        using ImageManager::ImageManager;
        osg::ref_ptr<ColdImage> cold(VFS::Path::NormalizedView path)
        {
            auto lease = mCache->getLeaseFromObjectCache(path);
            return dynamic_cast<ColdImage*>(lease.mValue.get());
        }
        std::size_t entries() const { return mCache->getStats().mSize; }
    };
    void sweep(FixtureManager& manager, double time)
    {
        CacheMaintenanceBudget budget(4096, 64, std::chrono::seconds(1));
        CacheMaintenanceScope scope(budget);
        manager.updateCache(time);
    }
}

int main() try
{
    using namespace Resource;
    SpeculativeBudget budget;
    ColdImageCache::Config config; config.enabled = true;
    ColdImageCache cache(config);
    auto original = image();
    auto encoded = cache.encode(*original, 3, sample(&budget), &budget);
    require(encoded.image.valid(), "eligible typed backing did not compress");
    require(encoded.image->allocatedBytes() < original->getTotalDataSize() / 8,
        "stored compressed vector retained its compressBound capacity");
    require(cache.stats().retainedBytes == encoded.image->allocatedBytes()
        && cache.stats().reservedBytes == 0 && cache.stats().jobs == 0, "cold allocation capacity or transient accounting incorrect");
    encoded.reservation.reset();
    auto restored = cache.restore(*encoded.image, "image", 3, sample(&budget), &budget);
    require(restored.status == ColdImageCache::RestoreStatus::Ready && restored.image.valid(), "lossless restoration failed");
    require(restored.image->getTotalDataSize() == original->getTotalDataSize()
        && std::memcmp(restored.image->data(), original->data(), original->getTotalDataSize()) == 0,
        "decoded bytes differed from original");
    require(restored.image->getInternalTextureFormat() == original->getInternalTextureFormat()
        && restored.image->getOrigin() == original->getOrigin()
        && restored.image->getPacking() == original->getPacking()
        && restored.image->getPixelAspectRatio() == original->getPixelAspectRatio()
        && restored.image->getWriteHint() == original->getWriteHint()
        && restored.image->getDataVariance() == original->getDataVariance(),
        "image metadata changed");
    require(restored.image->getUserDataContainer() == nullptr, "accounting metadata was copied into image userdata");
    restored = {};
    require(cache.restore(*encoded.image, "image", 4, sample(&budget), &budget).status
        == ColdImageCache::RestoreStatus::Invalid, "source generation/options mismatch accepted");
    auto critical = sample(&budget); critical.decision.state = OpenGlPressure::Critical;
    require(!cache.encode(*original, 3, critical, &budget).image, "Critical pressure allocated optional compression");
    require(cache.restore(*encoded.image, "image", 3, critical, &budget).status
        == ColdImageCache::RestoreStatus::Deferred, "Critical pressure allocated optional restoration");
    encoded.image->lockRestoreForTest();
    require(cache.restore(*encoded.image, "image", 3, sample(&budget), &budget).status
        == ColdImageCache::RestoreStatus::Busy, "single-flight restoration waited or duplicated codec work");
    encoded.image->unlockRestoreForTest();

    original->setDataVariance(osg::Object::DYNAMIC);
    require(!ColdImageCache::eligible(*original, config), "dynamic image entered cold tier");
    original->setDataVariance(osg::Object::UNSPECIFIED);
    original->setUserData(new osg::Group);
    require(!ColdImageCache::eligible(*original, config), "image with opaque userdata entered cold tier");
    original->setUserData(nullptr);
    original = image();
    std::uint64_t random = 0x192837465abcdefull;
    for (unsigned i = 0; i < original->getTotalDataSize(); ++i)
    {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        original->data()[i] = static_cast<unsigned char>(random);
    }
    require(!cache.encode(*original, 3, sample(&budget), &budget).image && cache.stats().poorRatio == 1,
        "incompressible input was stored at a larger net allocation");
    std::vector<unsigned char> externalBytes(256 * 256 * 4, 0);
    osg::ref_ptr<osg::Image> external = new osg::Image;
    external->setImage(256, 256, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
        externalBytes.data(), osg::Image::NO_DELETE, 1);
    require(!ColdImageCache::eligible(*external, config), "external unowned buffer entered the cold tier");
    osg::ref_ptr<osg::Image> blocks = new osg::Image;
    blocks->setImage(256, 256, 1, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
        GL_UNSIGNED_BYTE, new unsigned char[65536], osg::Image::USE_NEW_DELETE, 1);
    require(!ColdImageCache::eligible(*blocks, config), "already compressed DDS blocks were re-encoded");
    osg::ref_ptr<ColdImage> empty = new ColdImage;
    require(cache.restore(*empty, "empty", 3, sample(&budget), &budget).status
        == ColdImageCache::RestoreStatus::Invalid, "empty typed descriptor was accepted");

    // Preserve the complete mip chain, not just its duplicated base level.
    const std::size_t mipBytes = 256 * 256 * 4 + 128 * 128 * 4 + 64 * 64 * 4;
    auto mipData = std::make_unique<unsigned char[]>(mipBytes);
    std::memset(mipData.get(), 12, mipBytes);
    osg::ref_ptr<osg::Image> mips = new osg::Image;
    mips->setImage(256, 256, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
        mipData.get(), osg::Image::USE_NEW_DELETE, 1); mipData.release();
    mips->setMipmapLevels({256 * 256 * 4, 256 * 256 * 4 + 128 * 128 * 4});
    auto mipEncoded = cache.encode(*mips, 3, sample(&budget), &budget);
    require(mipEncoded.image.valid(), "eligible typed mip chain did not encode");
    mipEncoded.reservation.reset();
    auto mipRestored = cache.restore(*mipEncoded.image, "mips", 3, sample(&budget), &budget);
    require(mipRestored.image && mipRestored.image->getMipmapLevels() == mips->getMipmapLevels()
        && mipRestored.image->getTotalDataSize() == mipBytes
        && std::memcmp(mipRestored.image->data(), mips->data(), mipBytes) == 0,
        "mip metadata or bytes changed during restoration");
    mipRestored = {};
    auto tinyLimit = config; tinyLimit.retainedLimit = 1;
    ColdImageCache limited(tinyLimit);
    auto uniform = image();
    require(!limited.encode(*uniform, 3, sample(&budget), &budget).image
        && limited.stats().retainedBytes == 0 && limited.stats().jobs == 0, "cold cap overflow leaked reservation");
    auto noScratch = config; noScratch.transientLimit = 1;
    ColdImageCache constrained(noScratch);
    require(!constrained.encode(*uniform, 3, sample(&budget), &budget).image
        && constrained.stats().reservedBytes == 0, "scratch peak cap was bypassed");
    ColdImageCache off({});
    require(!off.encode(*uniform, 3, sample(&budget), &budget).image && off.stats().retainedBytes == 0,
        "off path created a cold entry");

    // Actual real-reader publication, cache-only retirement, restoration,
    // fallback, and concurrent canonical load behavior.
    require(osgDB::Registry::instance()->getReaderWriterForExtension("tga") != nullptr, "real TGA reader unavailable");
    VFS::Manager vfs;
    auto archive = std::make_unique<MemoryArchive>();
    auto* raw = archive.get();
    for (const auto* name : {"textures/cold.tga", "textures/busy.tga", "textures/race.tga",
        "textures/pinned.tga", "textures/clear.tga", "textures/invalid.tga"})
    {
        auto file = std::make_unique<MemoryFile>(); file->bytes = tga(); archive->files.emplace(name, std::move(file));
    }
    raw->files.at("textures/invalid.tga")->bytes = "bad"; // Real TGA header failure.
    vfs.addArchive(std::move(archive)); vfs.buildIndex();
    FixtureManager manager(&vfs, 1800);
    manager.setSpeculativeBudget(&budget);
    manager.configureColdCache(config, sample, &budget);
    constexpr VFS::Path::NormalizedView path("textures/cold.tga");
    auto loaded = manager.getImage(path);
    std::vector<unsigned char> expected(loaded->data(), loaded->data() + loaded->getTotalDataSize());
    const auto expectedOrigin = loaded->getOrigin();
    loaded = nullptr;
    sweep(manager, 1); sweep(manager, 40); sweep(manager, 41);
    require(manager.cold(path).valid(), "actual ImageManager did not replace cold cache-only decoded backing");
    require(!manager.getImageLease(path), "compressed cache object was exposed as an osg::Image lease");
    auto loadedAgain = manager.getImage(path);
    require(loadedAgain->getOrigin() == expectedOrigin && loadedAgain->getTotalDataSize() == expected.size()
        && std::equal(expected.begin(), expected.end(), loadedAgain->data()), "manager round trip changed reader pixels");
    require(raw->files.at("textures/cold.tga")->opens == 1 && !manager.cold(path),
        "cold restore reread source or failed canonical decoded publication");
    loadedAgain = nullptr;
    sweep(manager, 42); sweep(manager, 80); sweep(manager, 81);
    auto corrupt = manager.cold(path);
    require(corrupt.valid(), "second cold publication failed");
    corrupt->corruptForTest();
    auto recovered = manager.getImage(path);
    require(recovered->getTotalDataSize() == expected.size()
        && std::equal(expected.begin(), expected.end(), recovered->data())
        && raw->files.at("textures/cold.tga")->opens == 2, "corrupt optional entry did not reload authoritative source");
    corrupt = nullptr; recovered = nullptr;

    constexpr VFS::Path::NormalizedView busyPath("textures/busy.tga");
    manager.getImage(busyPath);
    sweep(manager, 82); sweep(manager, 120); sweep(manager, 121);
    auto busy = manager.cold(busyPath);
    require(busy.valid(), "busy fixture was not cold");
    busy->lockRestoreForTest();
    auto busyFallback = manager.getImage(busyPath);
    busy->unlockRestoreForTest();
    require(busyFallback.valid() && raw->files.at("textures/busy.tga")->opens == 2,
        "busy codec blocked demanded progress instead of ordinary reload");
    busy = nullptr; busyFallback = nullptr;

    auto pinned = manager.getImage(VFS::Path::NormalizedView("textures/pinned.tga"));
    sweep(manager, 122); sweep(manager, 170); sweep(manager, 171); sweep(manager, 172); sweep(manager, 173);
    require(!manager.cold(VFS::Path::NormalizedView("textures/pinned.tga")) && pinned->data(),
        "externally pinned image was compressed or its backing was removed");

    constexpr VFS::Path::NormalizedView racePath("textures/race.tga");
    osg::ref_ptr<osg::Image> a, b;
    std::thread first([&] { a = manager.getImage(racePath); });
    std::thread second([&] { b = manager.getImage(racePath); });
    first.join(); second.join();
    require(a && b && a == b, "concurrent ordinary loaders did not publish one canonical image");
    a = nullptr; b = nullptr;
    const auto before = raw->files.at("textures/race.tga")->opens.load();
    vfs.buildIndex(); // No worker mutates VFS concurrently.
    auto regenerated = manager.getImage(racePath);
    require(regenerated && raw->files.at("textures/race.tga")->opens == before + 1,
        "source revision changed but stale hot/cold publication was accepted");
    manager.clearCache();
    require(manager.entries() == 0 && pinned->data(), "clear destroyed live image ownership or left cache entries");
    constexpr VFS::Path::NormalizedView invalidPath("textures/invalid.tga");
    auto warning = manager.getImage(invalidPath);
    auto cachedWarning = manager.getImage(invalidPath);
    require(warning == manager.getWarningImage() && cachedWarning == warning
        && raw->files.at("textures/invalid.tga")->opens == 1 && manager.entries() == 1,
        "C5 cached failed-reader warning was treated as stale and repeatedly reopened");
    vfs.buildIndex();
    auto newGenerationWarning = manager.getImage(invalidPath);
    require(newGenerationWarning == warning && raw->files.at("textures/invalid.tga")->opens == 2,
        "warning retained stale source generation rather than retrying the new source");
    manager.clearCache();
    // Clear while the authoritative reader is blocked, after getImage captured
    // its publication epoch. Codec/reader work must not hold the publication
    // mutex, and an older completed read may return its private image without
    // repopulating the cleared canonical cache.
    constexpr VFS::Path::NormalizedView clearPath("textures/clear.tga");
    auto& clearing = *raw->files.at("textures/clear.tga");
    clearing.armOpen();
    auto pendingRead = std::async(std::launch::async, [&] { return manager.getImage(clearPath); });
    const bool blocked = clearing.waitOpen();
    if (blocked) manager.clearCache();
    clearing.releaseOpen(); // Always release before assertions/future cleanup.
    auto heldRead = pendingRead.get();
    require(blocked && heldRead && heldRead->data(), "deterministic blocked ordinary-read fixture failed");
    require(manager.entries() == 0 && clearing.opens == 1,
        "ordinary read admitted before clear republished into the cleared cache");
    auto afterClear = manager.getImage(clearPath);
    require(afterClear && afterClear != heldRead && clearing.opens == 2 && manager.entries() == 1,
        "fresh post-clear demand did not publish its own canonical image");
    manager.clearCache();
    encoded.image->corruptForTest();
    require(cache.restore(*encoded.image, "image", 3, sample(&budget), &budget).status
        == ColdImageCache::RestoreStatus::Invalid, "malformed compressed bytes bypassed exact decode/checksum validation");
    require(cache.stats().reservedBytes == 0 && cache.stats().jobs == 0
        && budget.stats().futureBytes == 0, "codec/restore failure leaked transient reservations");
    std::cout << "Cold image native checks passed: " << checks << '\n';
    return 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
