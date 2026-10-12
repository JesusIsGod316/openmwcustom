#include <components/resource/animationpreparation.hpp>
#include <components/resource/animationsourceplan.hpp>
#include <components/vfs/archive.hpp>

#include <osg/Group>
#include <atomic>
#include <functional>
#include <iostream>
#include <sstream>
#include <thread>

namespace
{
    using namespace Resource;
    constexpr auto MiB = SpeculativeBudget::MiB;
    unsigned checks = 0;
    void require(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); ++checks; }
    OpenGlPressureSample healthy(std::uint64_t watermark = 0)
    {
        OpenGlPressureSample s;
        s.generation = 1; s.growthWatermarkValid = true; s.accountedGrowth = watermark;
        s.memory.physicalValid = s.memory.commitValid = true;
        s.memory.physicalTotal = 32 * 1024 * MiB; s.memory.physicalAvailable = 16 * 1024 * MiB;
        s.memory.commitAvailable = 24 * 1024 * MiB;
        s.decision.state = OpenGlPressure::Normal; s.decision.admissionsPerSample = 4;
        s.decision.limits = {4 * 1024 * MiB, 2 * 1024 * MiB, 5 * 1024 * MiB,
            1024 * MiB, 256 * MiB, 1536 * MiB};
        return s;
    }
    OpenGlPressureSample sampleFn(void* p) { return *static_cast<OpenGlPressureSample*>(p); }
    struct Reentrant : osg::Group
    {
        explicit Reentrant(std::function<void()> callback) : mCallback(std::move(callback)) {}
        ~Reentrant() override { mCallback(); }
        std::function<void()> mCallback;
    };
    class PathsArchive : public VFS::Archive
    {
    public:
        explicit PathsArchive(std::vector<std::string> paths) : mPaths(std::move(paths)) {}
        void listResources(VFS::FileMap& out) override
        { for (const auto& path : mPaths) out.insert_or_assign(VFS::Path::Normalized(path), nullptr); }
        bool contains(VFS::Path::NormalizedView path) const override
        { return std::find(mPaths.begin(), mPaths.end(), path.value()) != mPaths.end(); }
        std::string getDescription() const override { return "owned fixture paths, no file access"; }
    private:
        std::vector<std::string> mPaths;
    };
    std::vector<std::string> collect(AnimationSourcePlanCache& plans, const VFS::Manager& vfs,
        SpeculativeBudget& budget, std::size_t* scannedOut = nullptr, unsigned* samplesOut = nullptr)
    {
        std::vector<std::string> result;
        std::size_t scanned = 0;
        require(plans.visit(vfs, VFS::Path::NormalizedView("meshes/xbase.kf"),
            [&](const auto& path) { result.emplace_back(path.value()); return true; },
            [&] { ++scanned; return true; }, [&] {
                if (samplesOut) ++*samplesOut;
                return healthy(budget.watermark()->load());
            }), "complete required discovery");
        if (scannedOut) *scannedOut = scanned;
        return result;
    }
    ObjectCacheLease chargedLease(SpeculativeBudget& budget, std::uint64_t bytes,
        std::uint64_t revision = 1, std::uint64_t sourceRevision = 7)
    {
        ObjectCacheLease result;
        result.mValue = new osg::Group;
        result.mCharge = budget.track({result.mValue.get(), 2}, bytes, false);
        result.mRevision = revision;
        result.mSourceRevision = sourceRevision;
        return result;
    }
}

int main(int argc, char** argv)
{
    using namespace Resource;
    try
    {
        if (argc > 1)
        {
            const std::string mode(argv[1]);
            require(animationSourcePlanEnabled() == (mode == "plan" || mode == "both"), "exact plan switch");
            require(animationDependencyLifetimeEnabled() == (mode == "lifetime" || mode == "both"),
                "exact lifetime switch and legacy ANIM dependency");
        }
        for (const auto state : {OpenGlPressure::Normal, OpenGlPressure::Caution, OpenGlPressure::Recovering,
                 OpenGlPressure::Critical, OpenGlPressure::Degraded})
        {
            auto s = healthy(); s.decision.state = state;
            require(animationNewWorkAllowed(s, SpeculativePriority::NearFuture)
                == (state == OpenGlPressure::Normal || state == OpenGlPressure::Caution
                    || state == OpenGlPressure::Recovering), "urgent pressure policy");
            require(animationNewWorkAllowed(s, SpeculativePriority::Background) == (state == OpenGlPressure::Normal),
                "background pressure policy");
            require(animationOwnedWorkMayRemain(s, true) == (state != OpenGlPressure::Critical), "target hot ownership");
            require(animationOwnedWorkMayRemain(s, false)
                == (state != OpenGlPressure::Critical && state != OpenGlPressure::Caution), "distant hot ownership");
        }
        auto invalid = healthy(); invalid.generation = 0;
        require(!animationNewWorkAllowed(invalid, SpeculativePriority::NearFuture)
            && animationOwnedWorkMayRemain(invalid, true), "missing sample denies new, preserves hot ownership");
        invalid = healthy(); invalid.memory.lowMemoryValid = invalid.memory.lowMemory = true;
        require(!animationNewWorkAllowed(invalid, SpeculativePriority::NearFuture)
            && !animationOwnedWorkMayRemain(invalid, true), "explicit low memory cancels optional ownership");
        invalid = healthy(); invalid.memory.commitValid = invalid.memory.systemCommitValid = false;
        require(!animationNewWorkAllowed(invalid, SpeculativePriority::NearFuture), "missing commit denies optional work");

        {
            SpeculativeBudget budget;
            GenericObjectCache<std::string> cache; cache.setSpeculativeBudget(&budget);
            auto s = healthy(); bool chargedInLastDestructor = false;
            {
                SpeculativeScope scope(&budget, &sampleFn, &s);
                SpeculativeScope::Stage stage(64, 32);
                cache.addEntryToObjectCache("last", new Reentrant([&] {
                    chargedInLastDestructor = budget.stats().estimatedOwnerBytes == 32;
                }), 1, 7);
            }
            auto only = cache.getLeaseFromObjectCache("last");
            cache.clear(); only = {};
            require(chargedInLastDestructor && budget.stats().estimatedOwnerBytes == 0,
                "final lease assignment releases payload before charge");
        }

        {
            SpeculativeBudget budget;
            GenericObjectCache<std::string> cache;
            cache.setSpeculativeBudget(&budget);
            auto s = healthy(); bool destroyedOutsideLock = false, chargedThroughDestructor = false;
            {
                SpeculativeScope scope(&budget, &sampleFn, &s);
                SpeculativeScope::Stage stage(8 * MiB, 4 * MiB);
                cache.addEntryToObjectCache("a", new Reentrant([&] {
                    destroyedOutsideLock = cache.getStats().mSize == 0;
                    chargedThroughDestructor = budget.stats().estimatedOwnerBytes >= 4 * MiB;
                }), 1, 7);
            }
            auto old = cache.getLeaseFromObjectCache("a", nullptr, 7);
            require(old && old.bytes() == 4 * MiB, "atomic ready lease has nonzero revision and charge");
            require(!cache.getLeaseFromObjectCache("a", nullptr, 8), "wrong source revision denied");
            auto replacement = chargedLease(budget, 4 * MiB, 2);
            cache.addEntryToObjectCache("a", replacement.mValue, 2, 7);
            require(!cache.getLeaseFromObjectCache("a", old.mValue.get(), 7), "replacement cannot pair old value/new charge");
            require(cache.getLeaseFromObjectCache("a").mRevision != old.mRevision, "publication revisions differ");
            const std::vector<VFS::Path::Normalized> sources{VFS::Path::Normalized("meshes/xbase.nif")};
            auto ticket = osg::ref_ptr<AnimationPreparation>(new AnimationPreparation(budget,
                healthy(budget.watermark()->load()), SpeculativePriority::NearFuture, 7, true, sources));
            require(ticket->adopt(VFS::Path::NormalizedView("animations/xbase/a.kf"), old), "adopt charged hit");
            require(ticket->adopt(VFS::Path::NormalizedView("animations/xbase/alias.kf"), old), "adopt same payload alias");
            require(ticket->remainingBytes() == MaximumAnimationPreloadEstimate
                - AnimationPreparation::MetadataEstimate - 4 * MiB, "unique hit bytes count locally once");
            require(!ticket->adopt(VFS::Path::NormalizedView("wrong.kf"), chargedLease(budget, 4 * MiB, 3, 8)),
                "stale ready source denied");
            require(!ticket->adopt(VFS::Path::NormalizedView("unknown.kf"), chargedLease(budget, 0)),
                "zero-byte unknown cannot become free pin");
            ticket->complete(true);
            require(!ticket->adopt(VFS::Path::NormalizedView("late.kf"), old), "published progress immutable");
            auto newJob = budget.tryJob(healthy(budget.watermark()->load()), SpeculativePriority::NearFuture);
            require(bool(newJob), "resume newly admitted job");
            auto resumed = osg::ref_ptr<AnimationPreparation>(new AnimationPreparation(budget,
                healthy(budget.watermark()->load()), SpeculativePriority::NearFuture, 7, true, sources,
                ticket->metadataBytes()));
            require(resumed->importReady(*ticket) == 2 && ticket->size() == 2, "immutable ready progress copied for new job");
            require(resumed->remainingBytes() == MaximumAnimationPreloadEstimate
                - 2 * AnimationPreparation::MetadataEstimate - 4 * MiB, "copied owner metadata stays conservative");
            resumed->cancel(); require(!resumed->adopt(VFS::Path::NormalizedView("cancel.kf"), old), "cancel is never reset");
            resumed->complete(true); require(!resumed->retryable(), "cancelled ticket cannot be revived");
            cache.clear(); old = {}; replacement = {}; resumed = nullptr;
            require(!destroyedOutsideLock, "ready target survives cache retirement");
            ticket = nullptr;
            require(destroyedOutsideLock && chargedThroughDestructor, "payload released outside lock before final charge");
            require(budget.stats().estimatedOwnerBytes == 0 && budget.stats().unknownOwners == 0,
                "all payload and metadata charges retire");
            require(budget.stats().observedGrowth != 0, "logical release gives no stale physical credit");
        }
        {
            SpeculativeBudget budget;
            const std::vector<VFS::Path::Normalized> sources{VFS::Path::Normalized("meshes/xbase.nif")};
            auto ticket = osg::ref_ptr<AnimationPreparation>(new AnimationPreparation(budget, healthy(),
                SpeculativePriority::NearFuture, 7, true, sources));
            for (unsigned i = 0; i < 62; ++i)
                require(ticket->adopt(VFS::Path::Normalized("animations/xbase/" + std::to_string(i) + ".kf"),
                    chargedLease(budget, 4 * MiB, i + 1)), "bounded payload adoption");
            require(!ticket->adopt(VFS::Path::NormalizedView("overflow.kf"), chargedLease(budget, 4 * MiB)),
                "cache hits cannot bypass256MiB local bound");
            ticket->complete(false);
            require(ticket->retainedBytes() <= MaximumAnimationPreloadEstimate, "retained total within existing cap");
        }
        {
            SpeculativeBudget budget;
            const std::vector<VFS::Path::Normalized> sources{VFS::Path::Normalized("meshes/xbase.nif")};
            auto ticket = osg::ref_ptr<AnimationPreparation>(new AnimationPreparation(budget, healthy(),
                SpeculativePriority::NearFuture, 7, true, sources));
            const auto lease = chargedLease(budget, 4 * MiB);
            require(ticket->adopt(VFS::Path::NormalizedView("a.kf"), lease)
                && ticket->adopt(VFS::Path::NormalizedView("alias.kf"), lease), "alias readiness established");
            const auto before = ticket->remainingBytes();
            ticket->forget(VFS::Path::NormalizedView("a.kf"));
            require(ticket->remainingBytes() == before, "removing alias does not refund local shared payload");
            ticket->forget(VFS::Path::NormalizedView("alias.kf"));
            require(ticket->remainingBytes() == before + 4 * MiB && budget.stats().observedGrowth != 0,
                "stale copied readiness removed without physical credit");
        }
        {
            // Atomic acquisition/replacement races use exact charge identity,
            // never borrowed cache-entry memory. No VFS rebuild races are legal.
            SpeculativeBudget budget;
            GenericObjectCache<std::string> cache; cache.setSpeculativeBudget(&budget);
            std::atomic<bool> stop{false}, okay{true};
            std::thread reader([&] {
                while (!stop.load())
                {
                    auto lease = cache.getLeaseFromObjectCache("race", nullptr, 7);
                    if (lease && (!lease.mCharge || lease.bytes() != 32)) okay = false;
                }
            });
            auto s = healthy();
            for (unsigned i = 0; i < 2000; ++i)
            {
                SpeculativeScope scope(&budget, &sampleFn, &s);
                SpeculativeScope::Stage stage(64, 32);
                cache.addEntryToObjectCache("race", new osg::Group, 1, 7);
                if (i % 17 == 0) cache.clear();
            }
            stop = true; reader.join(); cache.clear();
            require(okay && budget.stats().estimatedOwnerBytes == 0, "concurrent lease/replacement/clear accounting");
        }
        {
            SpeculativeBudget budget; AnimationSourcePlanCache plans; plans.setBudget(&budget);
            VFS::Manager vfs;
            vfs.addArchive(std::make_unique<PathsArchive>(std::vector<std::string>{
                "animations/xbase/a.kf", "animations/xbase/b.nif", "animations/xbase/c.kf",
                "animations/xbase/sub/d.kf", "animations/other/e.kf"}));
            vfs.buildIndex(); std::size_t scanned = 0; unsigned samples = 0;
            const auto first = collect(plans, vfs, budget, &scanned, &samples);
            require(first == std::vector<std::string>{"animations/xbase/a.kf", "animations/xbase/c.kf",
                "animations/xbase/sub/d.kf"} && scanned == 4, "exact ordered KF demand and non-KF scan accounting");
            const auto charged = budget.stats().estimatedOwnerBytes;
            const auto requests = samples;
            require(collect(plans, vfs, budget, &scanned, &samples) == first && scanned == 4,
                "ordered metadata reuse preserves semantics");
            require(!animationSourcePlanEnabled() || (samples == requests && charged > 0), "plan hit avoids rebuilding/sample");
            require(animationSourcePlanEnabled() || (samples == 0 && charged == 0), "disabled control has no plan allocation");
            vfs.addArchive(std::make_unique<PathsArchive>(std::vector<std::string>{"animations/xbase/new.kf"}));
            vfs.buildIndex(); // all readers have quiesced, as required by VFS contract
            auto rebuilt = collect(plans, vfs, budget);
            require(rebuilt.size() == 4 && std::find(rebuilt.begin(), rebuilt.end(), "animations/xbase/new.kf") != rebuilt.end(),
                "quiescent VFS generation replacement invalidates plan");
            plans.clear(); require(budget.stats().estimatedOwnerBytes == 0 && budget.stats().futureBytes == 0,
                "plan payload/charge/future retire");
        }
        {
            SpeculativeBudget budget; AnimationSourcePlanCache plans; plans.setBudget(&budget);
            VFS::Manager vfs; std::vector<std::string> paths;
            for (unsigned i = 0; i < 9000; ++i) paths.push_back("animations/xbase/" + std::to_string(i) + ".kf");
            vfs.addArchive(std::make_unique<PathsArchive>(std::move(paths))); vfs.buildIndex();
            require(collect(plans, vfs, budget).size() == 9000, "oversized required discovery is never truncated");
            require(budget.stats().estimatedOwnerBytes == 0 && budget.stats().futureBytes == 0,
                "oversized plan does not retain metadata or reservation");
            unsigned scans = 0;
            require(!plans.visit(vfs, VFS::Path::NormalizedView("meshes/xbase.kf"), [](const auto&) { return true; },
                [&] { return ++scans < 4; }, [&] { return healthy(budget.watermark()->load()); }), "optional early stop preserved");
            require(budget.stats().estimatedOwnerBytes == 0, "incomplete plan is not published");
        }
        {
            SpeculativeBudget budget; AnimationSourcePlanCache plans; plans.setBudget(&budget);
            VFS::Manager vfs;
            vfs.addArchive(std::make_unique<PathsArchive>(std::vector<std::string>{"animations/xbase/a.kf"}));
            vfs.buildIndex(); bool threw = false;
            try
            {
                plans.visit(vfs, VFS::Path::NormalizedView("meshes/xbase.kf"), [](const auto&) -> bool {
                    throw std::runtime_error("required binding error");
                }, [] { return true; }, [&] { return healthy(); });
            }
            catch (const std::runtime_error&) { threw = true; }
            require(threw && budget.stats().futureBytes == 0 && budget.stats().estimatedOwnerBytes == 0,
                "required callback errors propagate without publishing/leaking metadata");
            unsigned values = 0;
            auto unknown = healthy(); unknown.generation = 0;
            require(plans.visit(vfs, VFS::Path::NormalizedView("meshes/xbase.kf"),
                [&](const auto&) { ++values; return true; }, [] { return true; }, [&] { return unknown; }) && values == 1,
                "invalid optional sample retains required demand fallback");
        }
        {
            SpeculativeBudget budget; AnimationSourcePlanCache plans; plans.setBudget(&budget);
            VFS::Manager vfs;
            vfs.addArchive(std::make_unique<PathsArchive>(std::vector<std::string>{
                "animations/xbase/a.kf", "animations/xbase/b.nif", "animations/xbase/c.kf"}));
            vfs.buildIndex(); std::atomic<bool> okay{true};
            std::vector<std::thread> workers;
            for (unsigned i = 0; i < 8; ++i)
                workers.emplace_back([&] {
                    for (unsigned j = 0; j < 40; ++j)
                    {
                        std::vector<std::string> result;
                        std::size_t scanned = 0;
                        const bool complete = plans.visit(vfs, VFS::Path::NormalizedView("meshes/xbase.kf"),
                            [&](const auto& path) { result.emplace_back(path.value()); return true; },
                            [&] { ++scanned; return true; }, [&] { return healthy(budget.watermark()->load()); });
                        if (!complete || scanned != 3 || result != std::vector<std::string>{
                                "animations/xbase/a.kf", "animations/xbase/c.kf"}) okay = false;
                    }
                });
            for (auto& worker : workers) worker.join();
            plans.clear();
            require(okay && budget.stats().estimatedOwnerBytes == 0 && budget.stats().futureBytes == 0,
                "concurrent owned-plan publication preserves order and settles charges");
        }
        {
            // The cache API's real uncharged owner (not chargedLease) is the
            // same starting condition as a required-loaded KF publication.
            SpeculativeBudget budget;
            GenericObjectCache<std::string> cache; cache.setSpeculativeBudget(&budget);
            osg::ref_ptr<osg::Group> value = new osg::Group;
            cache.addEntryToObjectCache("primary", value, 0, 7, 4 * MiB);
            cache.addEntryToObjectCache("alias", value, 0, 7, 4 * MiB);
            auto primary = cache.getLeaseFromObjectCache("primary", value.get(), 7);
            auto alias = cache.getLeaseFromObjectCache("alias", value.get(), 7);
            require(primary.mCharge == alias.mCharge && !primary.bytes() && budget.stats().unknownOwners == 1,
                "uncharged canonical aliases");
            auto first = budget.reserve(healthy(), 4 * MiB);
            auto second = budget.reserve(healthy(), 4 * MiB);
            require(cache.promoteReadyLease("primary", primary, 4 * MiB, first)
                && cache.promoteReadyLease("alias", alias, 4 * MiB, second), "concurrent winner aliases promote once");
            require(primary.bytes() == 4 * MiB && alias.bytes() == 4 * MiB && first.future() == 0
                && second.future() == 4 * MiB && budget.stats().estimatedOwnerBytes == 4 * MiB
                && budget.stats().observedGrowth == 4 * MiB && budget.stats().unknownOwners == 0,
                "one transfer and immutable positive canonical bytes");
            require(!budget.promoteUnknown(primary.mCharge, 8 * MiB, second)
                && primary.bytes() == 4 * MiB && budget.stats().observedGrowth == 4 * MiB,
                "positive charge never grows and incompatible larger metadata is declined");
            cache.addEntryToObjectCache("primary", new osg::Group, 0, 7, 4 * MiB);
            require(!cache.promoteReadyLease("primary", primary, 4 * MiB, second), "stale value/revision rejected");
            auto wrongSource = alias; wrongSource.mSourceRevision = 8;
            require(!cache.promoteReadyLease("alias", wrongSource, 4 * MiB, second), "stale generation rejected");
            auto wrongEstimate = alias; wrongEstimate.mOwnerEstimate = 8 * MiB;
            require(!cache.promoteReadyLease("alias", wrongEstimate, 8 * MiB, second), "estimate must match publication");
        }
        {
            SpeculativeBudget budget;
            auto unknown = budget.track({&budget, 2}, 0, false);
            auto insufficient = budget.reserve(healthy(), 1);
            require(!budget.promoteUnknown(unknown, 4 * MiB, insufficient) && !unknown->bytes()
                && budget.stats().unknownOwners == 1, "no unreserved promotion");
            unknown->pending();
            auto reservation = budget.reserve(healthy(), 4 * MiB);
            require(!budget.promoteUnknown(unknown, 4 * MiB, reservation), "pending retired owner cannot be resurrected");
        }
        std::cout << "PASS animation preparation " << checks << " checks\n";
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << "FAIL animation preparation: " << error.what() << '\n'; return 1; }
}
