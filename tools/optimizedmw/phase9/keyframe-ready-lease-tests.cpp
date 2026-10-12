#include <components/resource/animationpreparation.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>

#include <atomic>
#include <array>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    using namespace Resource;
    constexpr auto MiB = SpeculativeBudget::MiB;
    unsigned checks = 0;
    void require(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); ++checks; }
    OpenGlPressureSample healthy(SpeculativeBudget& budget)
    {
        OpenGlPressureSample result;
        result.generation = 1; result.growthWatermarkValid = true;
        result.accountedGrowth = budget.watermark()->load();
        result.memory.physicalValid = result.memory.commitValid = true;
        result.memory.physicalTotal = 32 * 1024 * MiB;
        result.memory.physicalAvailable = result.memory.commitAvailable = 16 * 1024 * MiB;
        result.decision.state = OpenGlPressure::Normal; result.decision.admissionsPerSample = 4;
        result.decision.limits = {4 * 1024 * MiB, 2 * 1024 * MiB, 5 * 1024 * MiB,
            1024 * MiB, 256 * MiB, 1536 * MiB};
        return result;
    }
    struct Probe
    {
        SpeculativeBudget& budget;
        OpenGlPressure pressure = OpenGlPressure::Normal;
        bool valid = true;
        static OpenGlPressureSample sample(void* owner)
        {
            const auto& probe = *static_cast<Probe*>(owner);
            auto result = healthy(probe.budget); result.decision.state = probe.pressure;
            result.memory.physicalValid = probe.valid;
            return result;
        }
    };
    struct Counters { std::atomic_uint opens{0}; std::atomic_uint64_t bytes{0}; };
    class Buffer final : public std::stringbuf
    {
        Counters& mCounters;
    public:
        Buffer(const std::string& value, Counters& counters) : std::stringbuf(value, std::ios::in), mCounters(counters) {}
        std::streamsize xsgetn(char* target, std::streamsize count) override
        {
            const auto result = std::stringbuf::xsgetn(target, count);
            mCounters.bytes.fetch_add(static_cast<std::uint64_t>(result));
            return result;
        }
    };
    class Stream final : public std::istream
    {
        Buffer mBuffer;
    public:
        Stream(const std::string& value, Counters& counters) : std::istream(nullptr), mBuffer(value, counters)
        { rdbuf(&mBuffer); }
    };
    class File final : public VFS::File
    {
        std::string mBytes;
        Counters& mCounters;
    public:
        File(std::string value, Counters& counters) : mBytes(std::move(value)), mCounters(counters) {}
        Files::IStreamPtr open() override
        { ++mCounters.opens; return std::make_unique<Stream>(mBytes, mCounters); }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "production warm KF fixture"; }
    };
    class Archive final : public VFS::Archive
    {
        VFS::File* mFile;
    public:
        explicit Archive(VFS::File& file) : mFile(&file) {}
        void listResources(VFS::FileMap& output) override
        {
            output.insert_or_assign(VFS::Path::Normalized("meshes/empty.kf"), mFile);
            output.insert_or_assign(VFS::Path::Normalized("meshes/alias.kf"), mFile);
        }
        bool contains(VFS::Path::NormalizedView path) const override
        { return path.value() == "meshes/empty.kf" || path.value() == "meshes/alias.kf"; }
        std::string getDescription() const override { return "counted production KF fixture"; }
    };
    std::string emptyKf(std::size_t padding = 0)
    {
        std::string result = "NetImmerse File Format, Version 4.0.0.2\n";
        const auto word = [&](std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) result.push_back(static_cast<char>(value >> (8 * i)));
        };
        word(0x04000002); word(0); word(0); // supported zero records/roots
        result.append(padding, '\0');
        return result;
    }
    class Manager final : public KeyframeManager
    {
    public:
        using KeyframeManager::KeyframeManager;
        ObjectCacheLease cached(VFS::Path::NormalizedView path)
        { return mCache->getLeaseFromObjectCache(path); }
        void alias(VFS::Path::NormalizedView path, const ObjectCacheLease& original, std::uint64_t estimate)
        { mCache->addEntryToObjectCache(path.value(), original.mValue, 0, original.mSourceRevision, estimate); }
    };
    constexpr VFS::Path::NormalizedView Primary("meshes/empty.kf"), Alias("meshes/alias.kf");
}

int main(int argc, char** argv)
{
    try
    {
        const std::string mode = argc > 1 ? argv[1] : "warm";
        const bool warm = mode == "warm";
        require(animationWarmLeaseEnabled() == warm, "exact independent warm-lease switch");
        SpeculativeBudget budget;
        Probe probe{budget};
        Counters counters;
        File file(emptyKf(), counters);
        VFS::Manager vfs;
        vfs.addArchive(std::make_unique<Archive>(file)); vfs.buildIndex();
        Manager manager(&vfs, nullptr, 300, nullptr); manager.setSpeculativeBudget(&budget);

        // ACTUAL required KeyframeManager/Nif::Reader/NifOsg::loadKf path first:
        // no optional Stage, canonical charge initially unknown, parsed once.
        const auto demanded = manager.get(Primary);
        require(demanded.valid() && counters.bytes.load() != 0, "real required KF parsed");
        auto original = manager.cached(Primary);
        require(original && original.bytes() == 0 && budget.stats().unknownOwners == 1,
            "required warm owner starts unknown");
        require(original.mOwnerEstimate == (warm ? 4 * MiB : 0), "estimate stamped only in repair path");
        const auto opens = counters.opens.load(); const auto bytes = counters.bytes.load();
        if (!warm)
        {
            SpeculativeScope scope(&budget, Probe::sample, &probe, SpeculativePriority::NearFuture);
            require(!manager.getReadyLease(Primary), "historical zero-charge gate preserved");
            require(counters.opens.load() == opens && counters.bytes.load() == bytes, "control warm no I/O");
            std::cout << "keyframe ready " << mode << " checks=" << checks << '\n';
            return 0;
        }

        // Invalid/Critical NEW optional retention cannot promote required data.
        for (const auto critical : {false, true})
        {
            probe.valid = critical; probe.pressure = critical ? OpenGlPressure::Critical : OpenGlPressure::Normal;
            bool denied = false;
            try { SpeculativeScope scope(&budget, Probe::sample, &probe, SpeculativePriority::NearFuture);
                  (void)manager.getReadyLease(Primary); }
            catch (const SpeculativeDeferred&) { denied = true; }
            require(denied && original.bytes() == 0, "invalid/Critical no promotion");
            require(manager.get(Primary) == demanded, "required warm unaffected under pressure");
        }
        probe.valid = true; probe.pressure = OpenGlPressure::Normal;
        {
            SpeculativeScope scope(&budget, Probe::sample, &probe, SpeculativePriority::NearFuture);
            require(!manager.getReadyLease(Primary, 3 * MiB), "local cap rejects before charge");
        }
        require(original.bytes() == 0 && budget.stats().estimatedOwnerBytes == 0, "cap denial leaves unknown");

        manager.alias(Alias, original, original.mOwnerEstimate);
        auto aliasBefore = manager.cached(Alias);
        require(aliasBefore.mCharge == original.mCharge && budget.stats().unknownOwners == 1,
            "real cached aliases share unknown canonical charge");
        probe.pressure = OpenGlPressure::Caution;
        {
            SpeculativeScope scope(&budget, Probe::sample, &probe, SpeculativePriority::Background);
            bool denied = false;
            try { (void)manager.getReadyLease(Primary); } catch (const SpeculativeDeferred&) { denied = true; }
            require(denied, "Caution background denied");
        }
        ObjectCacheLease ready;
        {
            SpeculativeScope scope(&budget, Probe::sample, &probe, SpeculativePriority::NearFuture);
            ready = manager.getReadyLease(Primary);
        }
        require(ready && ready.mValue.get() == demanded.get() && ready.mCharge == original.mCharge,
            "required warm KF adopts exact payload and canonical pointer");
        require(original.bytes() == 4 * MiB && aliasBefore.bytes() == 4 * MiB,
            "promotion visible through existing aliases");
        require(budget.stats().estimatedOwnerBytes == 4 * MiB && budget.stats().unknownOwners == 0
            && budget.stats().futureBytes == 0 && budget.stats().observedGrowth == 4 * MiB,
            "one exact charge transfer/growth");
        require(counters.opens.load() == opens && counters.bytes.load() == bytes, "warm repair zero reopen/hash/parse");
        {
            const std::vector<VFS::Path::Normalized> sources{VFS::Path::Normalized(Primary)};
            osg::ref_ptr<AnimationPreparation> progress = new AnimationPreparation(budget,
                Probe::sample(&probe), SpeculativePriority::NearFuture, vfs.getIndexGeneration(), true, sources);
            require(progress->adopt(Primary, ready) && progress->adopt(Alias, aliasBefore), "actual KF progress adoption");
            require(progress->remainingBytes() == MaximumAnimationPreloadEstimate - 6 * MiB - 4 * MiB,
                "two aliases consume one payload plus metadata");
            progress->complete(true);
            osg::ref_ptr<AnimationPreparation> resumed = new AnimationPreparation(budget,
                Probe::sample(&probe), SpeculativePriority::NearFuture, vfs.getIndexGeneration(), true,
                sources, progress->metadataBytes());
            require(resumed->importReady(*progress) == 2, "bounded resume imports real ready aliases");
            require(resumed->remainingBytes() == MaximumAnimationPreloadEstimate - 12 * MiB - 4 * MiB,
                "resume metadata and unique payload charged");
            resumed->cancel(); require(!resumed->adopt(Primary, ready), "cancel blocks new adoption");
        }
        require(budget.stats().estimatedOwnerBytes == 4 * MiB && budget.stats().futureBytes == 0,
            "progress metadata released independently");

        // Same payload with unknown size metadata is never a free extra owner.
        manager.alias(Alias, original, 0);
        { SpeculativeScope scope(&budget, Probe::sample, &probe, SpeculativePriority::NearFuture);
          require(manager.getReadyLease(Alias).bytes() == 4 * MiB, "positive canonical aliases need no upgrade"); }
        vfs.buildIndex();
        { SpeculativeScope scope(&budget, Probe::sample, &probe, SpeculativePriority::NearFuture);
          require(!manager.getReadyLease(Primary), "VFS generation invalidates warm lease"); }
        require(manager.get(Primary) == demanded && counters.opens.load() == opens,
            "generation denial does not disturb required/cache control");

        // Real parser source larger than the floor. Unknown estimate must never
        // be replaced by a blind4MiB assumption or by later filesystem content.
        {
            SpeculativeBudget largeBudget; Probe largeProbe{largeBudget}; Counters largeCounters;
            File largeFile(emptyKf(MiB), largeCounters); VFS::Manager largeVfs;
            largeVfs.addArchive(std::make_unique<Archive>(largeFile)); largeVfs.buildIndex();
            Manager large(&largeVfs, nullptr, 300, nullptr); large.setSpeculativeBudget(&largeBudget);
            const auto required = large.get(Primary); const auto cached = large.cached(Primary);
            require(required.valid() && cached.mOwnerEstimate > 8 * MiB, "actual source estimate exceeds floor");
            const auto largeOpens = largeCounters.opens.load();
            const auto largeReads = largeCounters.bytes.load();
            SpeculativeScope scope(&largeBudget, Probe::sample, &largeProbe, SpeculativePriority::NearFuture);
            require(!large.getReadyLease(Primary, 4 * MiB), "large warm source fails small local cap");
            const auto accepted = large.getReadyLease(Primary);
            require(accepted.bytes() == cached.mOwnerEstimate && accepted.mCharge == cached.mCharge,
                "actual publication estimate charged once");
            require(largeCounters.opens.load() == largeOpens && largeCounters.bytes.load() == largeReads,
                "large warm adoption no new source access");
        }

        {
            SpeculativeBudget unknownBudget; Probe unknownProbe{unknownBudget}; Counters unknownCounters;
            File unknownFile(emptyKf(), unknownCounters); VFS::Manager unknownVfs;
            unknownVfs.addArchive(std::make_unique<Archive>(unknownFile)); unknownVfs.buildIndex();
            Manager unknown(&unknownVfs, nullptr, 300, nullptr); unknown.setSpeculativeBudget(&unknownBudget);
            const auto required = unknown.get(Primary); const auto loaded = unknown.cached(Primary);
            unknown.alias(Alias, loaded, 0); // a same-object entry without a proven footprint
            SpeculativeScope scope(&unknownBudget, Probe::sample, &unknownProbe, SpeculativePriority::NearFuture);
            require(required.valid() && !unknown.getReadyLease(Alias), "unknown publication cannot be a free pin");
            require(loaded.bytes() == 0 && unknownBudget.stats().estimatedOwnerBytes == 0
                && unknownBudget.stats().futureBytes == 0, "unknown ownership remains fail closed");
        }

        // Concurrent production getReadyLease callers, preloaded REQUIRED once.
        {
            SpeculativeBudget raceBudget; Probe raceProbe{raceBudget}; Counters raceCounters;
            File raceFile(emptyKf(), raceCounters); VFS::Manager raceVfs;
            raceVfs.addArchive(std::make_unique<Archive>(raceFile)); raceVfs.buildIndex();
            Manager race(&raceVfs, nullptr, 300, nullptr); race.setSpeculativeBudget(&raceBudget);
            const auto required = race.get(Primary); const auto preexisting = race.cached(Primary);
            std::array<ObjectCacheLease, 8> leases;
            std::array<std::thread, 8> workers;
            std::atomic_uint successes{0};
            for (std::size_t i = 0; i < workers.size(); ++i)
                workers[i] = std::thread([&, i] {
                    try { SpeculativeScope scope(&raceBudget, Probe::sample, &raceProbe, SpeculativePriority::NearFuture);
                          leases[i] = race.getReadyLease(Primary); if (leases[i]) ++successes; }
                    catch (const SpeculativeDeferred&) {}
                });
            for (auto& worker : workers) worker.join();
            require(successes == workers.size(), "all concurrent warm lease callers succeed");
            for (const auto& lease : leases)
                require(lease.mValue.get() == required.get() && lease.mCharge == preexisting.mCharge
                    && lease.bytes() == 4 * MiB, "all concurrent callers keep same immutable positive charge");
            require(raceBudget.stats().estimatedOwnerBytes == 4 * MiB && raceBudget.stats().observedGrowth == 4 * MiB
                && raceBudget.stats().futureBytes == 0 && raceCounters.opens == 1,
                "concurrent promotion one reservation transfer, no duplicate parse");
        }
        manager.clearCache();
        require(budget.stats().estimatedOwnerBytes == 4 * MiB, "leases remain charged after cache release");
        ready = {}; original = {}; aliasBefore = {};
        require(budget.stats().estimatedOwnerBytes == 0 && budget.stats().unknownOwners == 0,
            "canonical charge releases after final lease");
        std::cout << "production keyframe warm-lease checks=" << checks << '\n';
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << error.what() << '\n'; return 1; }
}
