#include <components/resource/imagemanager.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/vfs/manager.hpp>

#include <osg/observer_ptr>
#include <osgDB/ReaderWriter>
#include <osgDB/Registry>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
    using namespace Resource;
    unsigned checks = 0;
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
        ++checks;
    }
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
    struct MemoryFile final : VFS::File
    {
        std::string bytes;
        std::mutex mutex;
        std::condition_variable changed;
        bool blocked = false, released = false;
        unsigned entered = 0;
        std::atomic<unsigned> opens{0};
        void arm()
        { std::lock_guard lock(mutex); blocked = true; released = false; entered = 0; }
        bool wait(unsigned count)
        {
            std::unique_lock lock(mutex);
            return changed.wait_for(lock, std::chrono::seconds(5), [&] { return entered >= count; });
        }
        void release()
        { std::lock_guard lock(mutex); released = true; blocked = false; changed.notify_all(); }
        Files::IStreamPtr open() override
        {
            ++opens;
            std::unique_lock lock(mutex);
            if (blocked)
            {
                ++entered; changed.notify_all();
                if (!changed.wait_for(lock, std::chrono::seconds(10), [&] { return released; }))
                    throw std::runtime_error("bounded loader gate timed out");
            }
            return std::make_unique<std::istringstream>(bytes, std::ios::in | std::ios::binary);
        }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "real image publication fixture"; }
    };
    struct MemoryArchive final : VFS::Archive
    {
        std::map<std::string, std::unique_ptr<MemoryFile>> files;
        void listResources(VFS::FileMap& output) override
        { for (auto& [path, file] : files) output.emplace(VFS::Path::Normalized(path), file.get()); }
        bool contains(VFS::Path::NormalizedView path) const override { return files.contains(std::string(path.value())); }
        std::string getDescription() const override { return "private ordinary image publication fixture"; }
    };
    class FixtureManager final : public ImageManager
    {
    public:
        using ImageManager::ImageManager;
        std::size_t entries() const { return mCache->getStats().mSize; }
    };
    struct Fixture
    {
        VFS::Manager vfs;
        SpeculativeBudget budget;
        std::unique_ptr<FixtureManager> manager;
        MemoryFile* file = nullptr;
        VFS::Path::Normalized path;
        Fixture(const std::string& extension, const std::string& bytes, bool enabled)
            : path("textures/publication." + extension)
        {
            auto archive = std::make_unique<MemoryArchive>();
            auto source = std::make_unique<MemoryFile>(); file = source.get(); file->bytes = bytes;
            archive->files.emplace(std::string(path.value()), std::move(source));
            vfs.addArchive(std::move(archive)); vfs.buildIndex();
            manager = std::make_unique<FixtureManager>(&vfs, 100);
            manager->setSpeculativeBudget(&budget);
            manager->configureCanonicalPublication(enabled);
        }
    };
    struct ReleaseProbe final : osg::Referenced
    {
        explicit ReleaseProbe(std::function<void()> action) : action(std::move(action)) {}
        ~ReleaseProbe() override { if (action) action(); }
        std::function<void()> action;
    };
    // This wraps the exact registered production plugin rather than replacing
    // its decoder. Weak observations do not retain either decoded image.
    class ObservedReader final : public osgDB::ReaderWriter
    {
    public:
        explicit ObservedReader(osgDB::ReaderWriter* original, const std::string& extension)
            : original(original)
        { supportsExtension(extension, "real-reader publication observation"); }
        ReadResult readImage(std::istream& stream, const osgDB::Options* options) const override
        {
            ReadResult result;
            {
                std::unique_lock lock(mutex);
                if (sharedImage)
                {
                    // Both real ImageManager calls first miss their cache. The
                    // second shared return waits for the first caller's entire
                    // postprocess/publication, avoiding unsupported concurrent
                    // mutation of one Image while preserving the shared-return
                    // custom decoder case under test.
                    if (!borrowedWrappers && sharedReturns++ && !sharedChanged.wait_for(lock, std::chrono::seconds(5),
                            [&] { return sharedCallerComplete; }))
                        throw std::runtime_error("shared reader caller completion timed out");
                    if (borrowedWrappers)
                    {
                        osg::ref_ptr<osg::Image> wrapper = new osg::Image;
                        wrapper->setImage(sharedImage->s(), sharedImage->t(), sharedImage->r(),
                            sharedImage->getInternalTextureFormat(), sharedImage->getPixelFormat(),
                            sharedImage->getDataType(), sharedImage->data(), osg::Image::NO_DELETE,
                            sharedImage->getPacking(), sharedImage->getRowLength());
                        wrapper->setOrigin(sharedImage->getOrigin());
                        wrapper->setMipmapLevels(sharedImage->getMipmapLevels());
                        result = ReadResult(wrapper.get());
                    }
                    else result = ReadResult(sharedImage.get());
                }
            }
            if (!result.validImage()) result = original->readImage(stream, options);
            if (result.validImage())
            {
                auto* image = result.getImage();
                if (dynamic.load()) image->setDataVariance(osg::Object::DYNAMIC);
                std::function<void()> action;
                {
                    std::lock_guard lock(mutex);
                    if (alternateDescriptor.load() && decoded.size() % 2)
                        image->setInternalTextureFormat(GL_SRGB8);
                    decoded.emplace_back(image); action = releaseAction;
                }
                if (action) image->setUserData(new ReleaseProbe(std::move(action)));
            }
            return result;
        }
        void reset()
        {
            std::lock_guard lock(mutex);
            decoded.clear(); releaseAction = {}; dynamic.store(false); alternateDescriptor.store(false);
            sharedImage = nullptr; sharedReturns = 0; sharedCallerComplete = false; borrowedWrappers = false;
        }
        std::vector<osg::observer_ptr<osg::Image>> observations() const
        { std::lock_guard lock(mutex); return decoded; }
        void onRelease(std::function<void()> action)
        { std::lock_guard lock(mutex); releaseAction = std::move(action); }
        void returnShared(osg::Image* image)
        { std::lock_guard lock(mutex); sharedImage = image; sharedReturns = 0; sharedCallerComplete = false; borrowedWrappers = false; }
        void returnBorrowedBacking(osg::Image* image)
        { std::lock_guard lock(mutex); sharedImage = image; borrowedWrappers = true; }
        void callerComplete()
        { std::lock_guard lock(mutex); sharedCallerComplete = true; sharedChanged.notify_all(); }
        osg::ref_ptr<osgDB::ReaderWriter> original;
        std::atomic<bool> dynamic{false};
        std::atomic<bool> alternateDescriptor{false};
    private:
        mutable std::mutex mutex;
        mutable std::vector<osg::observer_ptr<osg::Image>> decoded;
        std::function<void()> releaseAction;
        osg::ref_ptr<osg::Image> sharedImage;
        mutable std::condition_variable sharedChanged;
        mutable unsigned sharedReturns = 0;
        mutable bool sharedCallerComplete = false;
        bool borrowedWrappers = false;
    };
    struct ReaderSwap
    {
        osg::ref_ptr<ObservedReader> observed;
        explicit ReaderSwap(const std::string& extension)
        {
            auto* registry = osgDB::Registry::instance();
            osg::ref_ptr<osgDB::ReaderWriter> original = registry->getReaderWriterForExtension(extension);
            require(original.valid(), "exact production image plugin is missing");
            observed = new ObservedReader(original, extension);
            registry->removeReaderWriter(original); registry->addReaderWriter(observed);
            require(registry->getReaderWriterForExtension(extension) == observed.get(), "reader observation was not installed");
        }
        ~ReaderSwap()
        {
            auto* registry = osgDB::Registry::instance();
            registry->removeReaderWriter(observed); registry->addReaderWriter(observed->original);
        }
    };
    std::string tga()
    {
        std::string bytes(18, '\0');
        bytes[2] = 2; bytes[12] = 4; bytes[14] = 2; bytes[16] = 24;
        for (unsigned i = 0; i < 8; ++i)
        { bytes.push_back(static_cast<char>(i)); bytes.push_back(i < 4 ? 0 : static_cast<char>(255)); bytes.push_back(i < 4 ? static_cast<char>(255) : 0); }
        return bytes;
    }
    std::string generated(const std::string& extension)
    {
        auto* reader = osgDB::Registry::instance()->getReaderWriterForExtension(extension);
        require(reader != nullptr, "real image writer plugin is missing");
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(4, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
        image->setInternalTextureFormat(GL_RGBA8);
        for (unsigned i = 0; i < image->getTotalSizeInBytes(); ++i) image->data()[i] = static_cast<unsigned char>(i * 7);
        std::ostringstream stream(std::ios::out | std::ios::binary);
        require(reader->writeImage(*image, stream).success(), "exact production writer rejected image fixture");
        return stream.str();
    }
    std::string dxt1()
    {
        std::string bytes(136, '\0');
        const auto set = [&](unsigned offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) bytes[offset+i] = static_cast<char>(value >> (i*8));
        };
        bytes.replace(0, 4, "DDS "); set(4, 124); set(8, 0x81007); set(12, 4); set(16, 4); set(20, 8);
        set(76, 32); set(80, 4); set(84, 0x31545844); set(108, 0x1000);
        set(128, 0x07e0f800); set(132, 0xe4e4e4e4);
        return bytes;
    }
    std::uint64_t digest(const osg::Image& image)
    {
        std::uint64_t result = 14695981039346656037ull;
        const auto add = [&](const void* data, std::size_t count) {
            auto* p = static_cast<const unsigned char*>(data);
            for (std::size_t i = 0; i < count; ++i) { result ^= p[i]; result *= 1099511628211ull; }
        };
        const std::uint32_t values[] = {static_cast<unsigned>(image.s()), static_cast<unsigned>(image.t()),
            static_cast<unsigned>(image.r()), image.getPixelFormat(), image.getDataType(),
            static_cast<unsigned>(image.getInternalTextureFormat()), static_cast<unsigned>(image.getOrigin()), image.getNumMipmapLevels()};
        add(values, sizeof(values));
        int width = image.s(), height = image.t(), depth = image.r();
        for (unsigned level = 0; level < image.getNumMipmapLevels(); ++level)
        {
            const auto bytes = image.isCompressed()
                ? static_cast<std::size_t>((width+3)/4) * ((height+3)/4) * depth
                    * osg::Image::computeBlockSize(image.getPixelFormat(), image.getPacking())
                : osg::Image::computeImageSizeInBytes(width, height, depth, image.getPixelFormat(), image.getDataType(), image.getPacking());
            add(image.getMipmapData(level), bytes);
            width = std::max(1, width/2); height = std::max(1, height/2); depth = std::max(1, depth/2);
        }
        return result;
    }
    using Pair = std::pair<osg::ref_ptr<osg::Image>, osg::ref_ptr<osg::Image>>;
    Pair race(Fixture& f, bool flipSecond = false, bool optionalSecond = false,
        std::function<void()> beforeRelease = {}, ObservedReader* completion = nullptr)
    {
        f.file->arm();
        auto a = std::async(std::launch::async, [&] {
            auto image = f.manager->getImage(f.path);
            if (completion) completion->callerComplete();
            return image;
        });
        auto b = std::async(std::launch::async, [&] {
            if (optionalSecond)
            {
                SpeculativeScope scope(&f.budget, sample, &f.budget, SpeculativePriority::Background);
                auto image = f.manager->getImage(f.path, flipSecond);
                if (completion) completion->callerComplete();
                return image;
            }
            auto image = f.manager->getImage(f.path, flipSecond);
            if (completion) completion->callerComplete();
            return image;
        });
        const bool both = f.file->wait(2);
        if (both && beforeRelease) beforeRelease();
        f.file->release();
        auto first = a.get(); auto second = b.get();
        require(both, "two real loaders did not both reach the forced cache-miss gate");
        return {std::move(first), std::move(second)};
    }
    std::size_t alive(const ObservedReader& reader)
    {
        const auto observations = reader.observations();
        return std::count_if(observations.begin(), observations.end(), [](const auto& value) { return value.valid(); });
    }
    void ordinaryCases(const std::string& extension, const std::string& bytes, std::uint64_t exactPayload = 0)
    {
        Fixture baseline(extension, bytes, false);
        auto expected = baseline.manager->getImage(baseline.path);
        require(expected != baseline.manager->getWarningImage(), "real decoder rejected baseline fixture");
        const auto expectedDigest = digest(*expected);
        const auto expectedBytes = exactPayload ? exactPayload : expected->getTotalSizeInBytesIncludingMipmaps();
        require(Resource::imagePayloadExtent(*expected).bytes==expectedBytes,
            "production checked payload disagrees with the real reader fixture allocation");
        ReaderSwap readers(extension);
        {
            Fixture control(extension, bytes, false); readers.observed->reset();
            auto images = race(control);
            require(images.first != images.second && images.first->data() != images.second->data(), "ordinary control no longer has independent decoded misses");
            require(digest(*images.first) == expectedDigest && digest(*images.second) == expectedDigest, "ordinary race changed decoded metadata/pixels");
            require(alive(*readers.observed) == 2, "weak observation unexpectedly retained or lost a control image");
            require(control.manager->publicationStats().winners == 0, "off path engaged canonical publication");
            // A cache-only charge follows the winning Item, although both
            // returned raw Image owners remain alive. This is an actual native
            // accounting-gap reproduction, not a byte estimate from the log.
            require(control.budget.stats().knownOwnerBytes == expectedBytes,
                "ordinary replaced owner accounting differs from its one cache Item");
            images.first = nullptr; images.second = nullptr; control.manager->clearCache();
            require(alive(*readers.observed) == 0, "control image backing remained pinned after all owners retired");
        }
        {
            Fixture candidate(extension, bytes, true); readers.observed->reset();
            auto images = race(candidate);
            require(images.first == images.second && images.first->data() == images.second->data(), "same-source concurrent loaders did not share one canonical completed image");
            require(digest(*images.first) == expectedDigest, "canonical publication changed decoded metadata/pixels");
            require(readers.observed->observations().size() == 2 && alive(*readers.observed) == 1,
                "decoded loser was retained or actual two-loader decoding was not exercised");
            auto s = candidate.manager->publicationStats();
            require(s.enabled && s.publications == 1 && s.winners == 1 && s.discardedImages == 1
                && s.discardedBytes == expectedBytes, "publication counters do not describe the actual released decoded loser");
            auto lease = candidate.manager->getImageLease(candidate.path, images.first);
            require(lease && candidate.manager->imageLeaseCurrent(candidate.path, lease), "canonical publication lost exact-source lease/charge ownership");
            require(candidate.budget.stats().knownOwnerBytes == lease.bytes(), "canonical image backing was double charged");
            const auto opened = candidate.file->opens.load();
            require(candidate.manager->getImage(candidate.path) == images.first && candidate.file->opens == opened,
                "completed canonical cache hit decoded another image");
            candidate.manager->clearCache();
            require(!candidate.manager->imageLeaseCurrent(candidate.path, lease) && images.first.valid(),
                "clear destroyed an externally owned image or retained its stale lease");
            lease = {}; images.first = nullptr; images.second = nullptr;
            require(alive(*readers.observed) == 0 && candidate.budget.stats().knownOwnerBytes == 0,
                "canonical backing/charge remained alive after cache and consumers retired");
        }
        std::cout << "REAL-READER " << extension << " exact_decoded_payload=" << expectedBytes << " passed\n";
    }
}

int main() try
{
    const auto tgaBytes = tga(), pngBytes = generated("png"), ddsBytes = generated("dds");
    ordinaryCases("tga", tgaBytes);
    ordinaryCases("png", pngBytes);
    ordinaryCases("dds", ddsBytes);
    ordinaryCases("dds", dxt1(), 8);
    {
        Fixture baseline("dds", ddsBytes, false);
        auto alreadyShared = baseline.manager->getImage(baseline.path);
        require(alreadyShared != baseline.manager->getWarningImage(), "shared decoder seed failed real DDS decoding");
        ReaderSwap sharedReader("dds"); sharedReader.observed->returnShared(alreadyShared);
        Fixture f("dds", ddsBytes, true);
        auto images = race(f, false, false, {}, sharedReader.observed);
        const auto s = f.manager->publicationStats();
        require(images.first == alreadyShared && images.second == alreadyShared,
            "custom decoder did not return its actual already shared Image to both missed calls");
        const auto observations = sharedReader.observed->observations();
        require(observations.size() == 2 && observations[0].get() == alreadyShared.get()
            && observations[1].get() == alreadyShared.get(), "shared decoder fixture did not observe both real loader calls");
        require(s.publications == 1 && s.winners == 1 && s.discardedImages == 0 && s.discardedBytes == 0,
            "already shared decoder object was reported as a distinct discarded payload");
        require(f.manager->getImageLease(f.path, alreadyShared) && f.budget.stats().knownOwnerBytes
            == alreadyShared->getTotalSizeInBytesIncludingMipmaps(), "shared return changed exact image lease or double charged payload");
    }
    {
        Fixture baseline("dds", ddsBytes, false);
        auto backingOwner = baseline.manager->getImage(baseline.path);
        ReaderSwap borrowedReader("dds"); borrowedReader.observed->returnBorrowedBacking(backingOwner);
        Fixture f("dds", ddsBytes, true);
        auto images = race(f);
        const auto s = f.manager->publicationStats();
        require(images.first != images.second && images.first->data() == backingOwner->data()
            && images.second->data() == backingOwner->data(), "custom decoder did not return distinct borrowed wrappers over one actual backing");
        require(images.first->getAllocationMode() == osg::Image::NO_DELETE
            && images.second->getAllocationMode() == osg::Image::NO_DELETE,
            "borrowed wrapper fixture accidentally acquired the backing allocation");
        require(s.publications == 2 && s.winners == 0 && s.discardedImages == 0 && s.discardedBytes == 0 && s.ineligible == 2,
            "borrowed shared backing was coalesced or reported as a distinct discarded payload");
        require(digest(*images.first) == digest(*backingOwner) && digest(*images.second) == digest(*backingOwner),
            "ordinary borrowed fallback changed source metadata/pixels");
    }
    ReaderSwap readers("tga");
    {
        Fixture f("tga", tgaBytes, true); readers.observed->reset();
        auto images = race(f, false, true);
        require(images.first == images.second && f.manager->publicationStats().winners == 1,
            "required and optional real loaders did not share a completed exact winner");
        require(f.budget.stats().futureBytes == 0 && f.budget.stats().jobs == 0,
            "optional admission remained reserved after the decoded loser retired");
    }
    {
        Fixture flipped("tga", tgaBytes, false), unflipped("tga", tgaBytes, false);
        const auto flipDigest = digest(*flipped.manager->getImage(flipped.path));
        const auto noFlipDigest = digest(*unflipped.manager->getImage(unflipped.path, true));
        require(flipDigest != noFlipDigest, "TGA fixture does not distinguish vertical-flip options");
        Fixture f("tga", tgaBytes, true); readers.observed->reset();
        auto images = race(f, true);
        require(images.first != images.second && digest(*images.first) == flipDigest && digest(*images.second) == noFlipDigest,
            "concurrent different flip policies reused the wrong processed winner");
        require(f.manager->publicationStats().winners == 0 && f.manager->publicationStats().discardedBytes == 0,
            "option-mismatched publications were counted as discarded duplicates");
        auto first = f.manager->getImage(f.path);
        require(digest(*first) == flipDigest && f.manager->getImageLease(f.path, first), "flip revision cache miss did not restore the correct policy");
        auto second = f.manager->getImage(f.path, true);
        require(digest(*second) == noFlipDigest && f.manager->getImageLease(f.path, second, true), "opposite flip revision lease was not preserved");
        require(f.manager->publicationStats().lookupRevisionMisses >= 1, "flip revision mismatch was not diagnosed");
    }
    {
        Fixture f("tga", tgaBytes, true); readers.observed->reset();
        auto images = race(f, false, false, [&] { f.manager->clearCache(); });
        require(images.first != images.second && f.manager->entries() == 0, "in-flight pre-clear producer repopulated the cleared cache");
        require(f.manager->publicationStats().epochDeclines == 2 && f.manager->publicationStats().publications == 0,
            "clear epoch did not independently decline both in-flight publications");
        auto fresh = f.manager->getImage(f.path);
        require(fresh != images.first && fresh != images.second && f.manager->getImageLease(f.path, fresh),
            "fresh post-clear loader failed to publish its current lease");
    }
    {
        Fixture f("tga", tgaBytes, true); readers.observed->reset();
        auto old = f.manager->getImage(f.path); auto lease = f.manager->getImageLease(f.path, old);
        f.file->bytes.back() ^= 0x7f;
        f.vfs.buildIndex(); // All readers joined: VFS mutation is deliberately quiescent.
        require(!f.manager->imageLeaseCurrent(f.path, lease), "quiescent VFS generation change left an old image lease valid");
        auto fresh = f.manager->getImage(f.path);
        require(fresh != old && digest(*fresh) != digest(*old), "new source generation reused the old decoded winner");
        require(f.manager->getImageLease(f.path, fresh) && f.manager->publicationStats().lookupRevisionMisses == 1,
            "new source publication lost its revision or mismatch diagnosis");
    }
    {
        Fixture f("tga", tgaBytes, true); readers.observed->reset(); readers.observed->dynamic = true;
        auto images = race(f);
        require(images.first != images.second && alive(*readers.observed) == 2, "mutable plugin image was canonically shared");
        require(f.manager->publicationStats().winners == 0 && f.manager->publicationStats().ineligible == 2,
            "mutable fallback did not preserve the independent ordinary path");
        require(!f.manager->getImageLease(f.path, images.first) && !f.manager->getImageLease(f.path, images.second),
            "mutable fallback acquired an immutable preparation lease");
    }
    {
        Fixture f("tga", tgaBytes, true); readers.observed->reset(); readers.observed->alternateDescriptor = true;
        auto images = race(f);
        require(images.first != images.second && alive(*readers.observed) == 2, "different processed internal formats were canonically shared");
        require(images.first->getInternalTextureFormat() != images.second->getInternalTextureFormat()
            && f.manager->publicationStats().winners == 0 && f.manager->publicationStats().ineligible >= 1,
            "processed descriptor mismatch did not retain the ordinary result/fallback");
    }
    {
        Fixture f("tga", tgaBytes, true); readers.observed->reset();
        const auto armed = std::make_shared<std::atomic<bool>>(true);
        const auto released = std::make_shared<std::atomic<unsigned>>(0);
        readers.observed->onRelease([&, armed, released] {
            if (armed->load()) { ++*released; f.manager->clearCache(); }
        });
        auto images = race(f);
        armed->store(false);
        require(images.first == images.second && released->load() == 1 && alive(*readers.observed) == 1,
            "decoded loser destructor did not reenter publication/cache clear safely outside both locks");
    }
    {
        Fixture f("tga", tgaBytes, true); readers.observed->reset();
        ColdImageCache::Config config; config.enabled = true;
        f.manager->configureColdCache(config, sample, &f.budget);
        auto images = race(f);
        require(images.first == images.second, "existing cold-cache canonical behavior changed");
        require(!f.manager->publicationStats().enabled && f.manager->publicationStats().publications == 0
            && f.manager->publicationStats().winners == 0, "ordinary candidate intercepted the existing cold-cache path");
    }
    std::cout << "IMAGE-PUBLICATION PASS checks=" << checks
        << " scope=real-concurrent-loader-safety-and-payload-release-not-runtime-performance\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "IMAGE-PUBLICATION FAIL: " << error.what() << '\n';
    return 1;
}
