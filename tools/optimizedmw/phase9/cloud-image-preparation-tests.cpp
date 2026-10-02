#include <components/resource/cloudimagepreparation.hpp>
#include <components/resource/deferredrelease.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <osgDB/Registry>
#include <osg/Observer>
#include <osg/observer_ptr>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace
{
    using namespace Resource;
    unsigned checks = 0;
    void require(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); ++checks; }
    OpenGlPressureSample healthy(SpeculativeBudget& budget)
    {
        OpenGlPressureSample s;
        s.generation = 1; s.growthWatermarkValid = true; s.accountedGrowth = budget.watermark()->load();
        s.memory.physicalValid = s.memory.commitValid = true;
        s.memory.physicalTotal = 32 * 1024 * SpeculativeBudget::MiB;
        s.memory.physicalAvailable = 16 * 1024 * SpeculativeBudget::MiB;
        s.memory.commitAvailable = 24 * 1024 * SpeculativeBudget::MiB;
        s.decision.state = OpenGlPressure::Normal; s.decision.admissionsPerSample = 4;
        s.decision.limits = {4 * 1024 * SpeculativeBudget::MiB, 2 * 1024 * SpeculativeBudget::MiB,
            5 * 1024 * SpeculativeBudget::MiB, 1024 * SpeculativeBudget::MiB,
            256 * SpeculativeBudget::MiB, 1536 * SpeculativeBudget::MiB};
        return s;
    }
    OpenGlPressureSample sample(void* p) { return healthy(*static_cast<SpeculativeBudget*>(p)); }
    OpenGlPressureSample critical(void* p)
    { auto s = sample(p); s.decision.state = OpenGlPressure::Critical; return s; }
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool entered = false, released = false;
        void block()
        {
            std::unique_lock lock(mutex); entered = true; condition.notify_all();
            condition.wait(lock, [&] { return released; });
        }
        void waitEntered() { std::unique_lock lock(mutex); condition.wait(lock, [&] { return entered; }); }
        void release() { std::lock_guard lock(mutex); released = true; condition.notify_all(); }
    };
    struct MemoryFile final : VFS::File
    {
        std::string bytes;
        Gate* gate = nullptr;
        std::atomic_uint opens{0};
        Files::IStreamPtr open() override
        {
            ++opens;
            if (gate) gate->block();
            return std::make_unique<std::istringstream>(bytes, std::ios::in | std::ios::binary);
        }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "cloud fixture"; }
    };
    struct DeleteObserver final : osg::Observer
    {
        explicit DeleteObserver(osg::Object* object) : observed(object) { object->addObserver(this); }
        ~DeleteObserver() override
        {
            osg::ref_ptr<osg::Object> object;
            if (observed.lock(object)) object->removeObserver(this);
        }
        osg::observer_ptr<osg::Object> observed;
        const std::thread::id mainThread = std::this_thread::get_id();
        std::atomic_bool deleted{false}, deletedOnMain{false};
        void objectDeleted(void*) override
        {
            deletedOnMain.store(std::this_thread::get_id() == mainThread);
            deleted.store(true);
        }
    };
    class MemoryArchive final : public VFS::Archive
    {
    public:
        std::map<std::string, std::unique_ptr<MemoryFile>> files;
        void listResources(VFS::FileMap& out) override
        { for (auto& [path, file] : files) out.emplace(VFS::Path::Normalized(path), file.get()); }
        bool contains(VFS::Path::NormalizedView path) const override { return files.contains(std::string(path.value())); }
        std::string getDescription() const override { return "private real-reader fixture"; }
    };
    std::string tga(unsigned width = 4, unsigned height = 2)
    {
        std::string bytes(18, '\0');
        bytes[2] = 2; bytes[12] = static_cast<char>(width & 255); bytes[13] = static_cast<char>(width >> 8);
        bytes[14] = static_cast<char>(height & 255); bytes[15] = static_cast<char>(height >> 8);
        bytes[16] = 24;
        if (width == 4 && height == 2)
            for (unsigned i = 0; i < width * height; ++i)
            { bytes.push_back(0); bytes.push_back(i < width ? 0 : static_cast<char>(255)); bytes.push_back(i < width ? static_cast<char>(255) : 0); }
        return bytes;
    }
    class FixtureImageManager : public ImageManager
    {
    public:
        using ImageManager::ImageManager;
        void seed(VFS::Path::NormalizedView path, osg::Image* image)
        { mCache->addEntryToObjectCache(std::string(path.value()), image, 0.0, getVFS()->getIndexGeneration() * 2 + 1); }
    };
}

int main() try
{
    require(osgDB::Registry::instance()->getReaderWriterForExtension("tga") != nullptr, "real TGA reader unavailable");
    VFS::Manager vfs;
    auto archive = std::make_unique<MemoryArchive>();
    auto* rawArchive = archive.get();
    for (const auto* path : {"textures/cloud.tga", "textures/cancelled.tga", "textures/blocked.tga", "textures/retired.tga"})
    { auto file = std::make_unique<MemoryFile>(); file->bytes = tga(); archive->files.emplace(path, std::move(file)); }
    auto oversized = std::make_unique<MemoryFile>(); oversized->bytes = tga(4096, 4096);
    archive->files.emplace("textures/oversized.tga", std::move(oversized));
    auto bad = std::make_unique<MemoryFile>(); bad->bytes = "invalid image";
    archive->files.emplace("textures/invalid.tga", std::move(bad));
    vfs.addArchive(std::move(archive)); vfs.buildIndex();
    SpeculativeBudget budget;
    FixtureImageManager images(&vfs, 10);
    images.setSpeculativeBudget(&budget);
    const SpeculativeScope::Context context{&budget, sample, &budget, SpeculativePriority::Background};
    osg::ref_ptr<SceneUtil::WorkQueue> queue = new SceneUtil::WorkQueue(1);
    constexpr VFS::Path::NormalizedView path("textures/cloud.tga");
    osg::ref_ptr<CloudImagePreparation> work = new CloudImagePreparation(&images, context, VFS::Path::Normalized(path), vfs.getIndexGeneration());
    queue->addWorkItem(work); work->waitTillDone();
    require(work->ready() && work->lease().bytes() > 0, "worker did not publish real decoded image/charge");
    const auto lease = work->lease();
    require(images.imageLeaseCurrent(path, lease), "matching publication not adoptable");
    const auto ownerBytes = budget.stats().knownOwnerBytes;
    auto second = images.getImageLease(path);
    require(second.mCharge == lease.mCharge && budget.stats().knownOwnerBytes == ownerBytes,
        "cache-hit lease double charged its existing payload");
    require(!images.getImageLease(path, static_cast<osg::Image*>(lease.mValue.get()), true), "decode-option mismatch accepted");
    ImageManager control(&vfs, 10);
    auto controlImage = control.getImage(path);
    auto* preparedImage = static_cast<osg::Image*>(lease.mValue.get());
    require(preparedImage->getOrigin() == controlImage->getOrigin()
        && preparedImage->getTotalDataSize() == controlImage->getTotalDataSize()
        && std::equal(preparedImage->data(), preparedImage->data() + preparedImage->getTotalDataSize(), controlImage->data()),
        "optional decode changed original flip/pixels");
    preparedImage->dirty();
    require(!work->current(), "mutated prepared image accepted without revision proof");
    images.clearCache();
    require(!images.imageLeaseCurrent(path, lease) && lease.mValue.valid(), "clear left stale adoption or destroyed owned lease");
    auto replacement = images.getImage(path);
    auto replacementLease = images.getImageLease(path, replacement);
    require(replacementLease && replacementLease.mRevision != lease.mRevision
        && !images.getImageLease(path, preparedImage), "replacement mixed old payload/new revision or charge");
    vfs.buildIndex(); // All workers joined; never mutate a live worker's VFS.
    require(!images.imageLeaseCurrent(path, replacementLease), "VFS generation mismatch accepted");

    bool pressureDenied = false;
    try
    {
        osg::ref_ptr<CloudImagePreparation> denied = new CloudImagePreparation(&images,
            {&budget, critical, &budget, SpeculativePriority::Background},
            VFS::Path::Normalized("textures/cancelled.tga"), vfs.getIndexGeneration());
    }
    catch (const SpeculativeDeferred&) { pressureDenied = true; }
    require(pressureDenied && rawArchive->files.at("textures/cancelled.tga")->opens == 0,
        "critical pressure started optional decode");

    osg::ref_ptr<CloudImagePreparation> cancelled = new CloudImagePreparation(&images, context,
        VFS::Path::Normalized("textures/cancelled.tga"), vfs.getIndexGeneration());
    cancelled->abort(); queue->addWorkItem(cancelled); cancelled->waitTillDone();
    require(!cancelled->ready() && rawArchive->files.at("textures/cancelled.tga")->opens == 0, "cancelled request performed decoding");
    for (const auto* rejected : {"textures/oversized.tga", "textures/invalid.tga"})
    {
        osg::ref_ptr<CloudImagePreparation> item = new CloudImagePreparation(&images, context,
            VFS::Path::Normalized(rejected), vfs.getIndexGeneration());
        queue->addWorkItem(item); item->waitTillDone();
        require(!item->ready() && rawArchive->files.at(rejected)->opens == 1,
            "unknown/oversized header entered decoder or produced a ready lease");
    }
    // Actual byte cap is independent of the predecode estimate/cache-hit delta.
    osg::ref_ptr<osg::Image> large = new osg::Image;
    large->allocateImage(4096, 2049, 1, GL_RGBA, GL_UNSIGNED_BYTE);
    images.seed(VFS::Path::NormalizedView("textures/large.tga"), large);
    bool refused = false;
    { SpeculativeScope scope(context); try { images.prepareImage(VFS::Path::NormalizedView("textures/large.tga"), CloudImagePreparation::MaximumEstimate); }
      catch (const SpeculativeDeferred&) { refused = true; } }
    require(refused, "cache-hit actual retained bytes bypassed local cap");

    Gate gate;
    rawArchive->files.at("textures/blocked.tga")->gate = &gate;
    osg::ref_ptr<CloudImagePreparation> blocked = new CloudImagePreparation(&images, context,
        VFS::Path::Normalized("textures/blocked.tga"), vfs.getIndexGeneration());
    queue->addWorkItem(blocked); gate.waitEntered();
    bool releaseCalled = false;
    require(!CloudImagePreparation::retireCompleted(blocked, [&](auto&, auto) { releaseCalled = true; return false; })
        && !releaseCalled && blocked.valid(), "running worker entered deferred release or lost original slot");
    require(!blocked->ready() && !images.getImageLease(VFS::Path::NormalizedView("textures/blocked.tga")),
        "unready adoption waited for/claimed in-progress decode");
    blocked->abort(); gate.release(); blocked->waitTillDone();
    require(!blocked->ready(), "cancelled in-progress decode published ready owner");
    osg::ref_ptr<CloudImagePreparation> retired = new CloudImagePreparation(&images, context,
        VFS::Path::Normalized("textures/retired.tga"), vfs.getIndexGeneration());
    queue->addWorkItem(retired); retired->waitTillDone();
    require(retired->ready(), "retirement fixture image did not decode");
    DeleteObserver deletion(retired->lease().mValue);
    retired->abort();
    images.clearCache(); // Ticket is now the only prepared payload owner.
    DeferredReleaseQueue full(0, 0), deferred(1);
    const auto* originalSlot = retired.get();
    require(!CloudImagePreparation::retireCompleted(retired,
        [&](auto& slot, auto bytes) { return full.push(slot, bytes); })
        && retired.get() == originalSlot && !deletion.deleted.load(),
        "refused deferred retirement moved/dropped sole prepared payload");
    require(CloudImagePreparation::retireCompleted(retired,
        [&](auto& slot, auto bytes) { return deferred.push(slot, bytes); })
        && !retired && deferred.stats().owners == 1 && !deletion.deleted.load(),
        "accepted retirement did not transfer the original retaining slot");
    work = nullptr; cancelled = nullptr; blocked = nullptr;
    queue = nullptr;
    std::thread maintenance([&] {
        CacheMaintenanceBudget pass(64, 64, std::chrono::seconds(1));
        deferred.drain(pass);
    });
    maintenance.join();
    require(deletion.deleted.load() && !deletion.deletedOnMain.load() && deferred.stats().owners == 0,
        "cache-cleared canceled cloud performed final image free on main");
    require(budget.stats().jobs == 0 && budget.stats().futureBytes == 0, "worker teardown leaked job/transient reservation");
    std::cout << "PASS " << checks << " real decoder/cache/charge/ownership checks\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
