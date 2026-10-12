#ifndef OPENMW_WORKQUEUE_PUBLICATION_TEST_HOOKS
#error Compile this fixture and workqueue.cpp with OPENMW_WORKQUEUE_PUBLICATION_TEST_HOOKS
#endif

#include <components/resource/loadingnifmaintenance.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/cachemaintenance.hpp>
#include <components/nif/niffile.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sceneutil/workqueuepublication.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/vfs/manager.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Resource
{
    struct LoadingNifMaintenanceTestAccess
    {
        static void setPhase(LoadingNifMaintenancePolicy& policy, std::uint64_t state)
        { policy.mPhaseState.store(state); }
    };
}

namespace
{
    unsigned checks = 0;
    void require(bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); ++checks; }

    Resource::OpenGlPressureSample pressure(std::uint64_t time = 1000)
    {
        Resource::OpenGlPressureSample sample;
        sample.generation = 1; sample.sampledAtMs = time;
        sample.memory.physicalValid = sample.memory.commitValid = true;
        sample.memory.physicalTotal = 32ull << 30;
        sample.decision.limits = Resource::OpenGlPressurePolicy{}.limits(sample.memory.physicalTotal);
        sample.memory.physicalAvailable = sample.decision.limits.physicalReserve - 1;
        sample.memory.commitAvailable = 8ull << 30;
        sample.decision.state = Resource::OpenGlPressure::Caution;
        return sample;
    }

    class MemoryFile final : public VFS::File
    {
    public:
        MemoryFile()
        {
            bytes = "NetImmerse File Format, Version 4.0.0.2\n";
            const auto word = [&](std::uint32_t value) {
                for (unsigned shift = 0; shift < 32; shift += 8)
                    bytes.push_back(static_cast<char>(value >> shift));
            };
            word(0x04000002); word(0); word(0); // actual Reader: version, records, roots
        }
        Files::IStreamPtr open() override
        { ++opens; return std::make_unique<std::istringstream>(bytes, std::ios::in | std::ios::binary); }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "native parsed NIF maintenance fixture"; }
        std::atomic<unsigned> opens{ 0 };
        std::string bytes;
    };
    class MemoryArchive final : public VFS::Archive
    {
    public:
        MemoryFile file;
        void listResources(VFS::FileMap& output) override
        { output.emplace(VFS::Path::Normalized("test.nif"), &file); }
        bool contains(VFS::Path::NormalizedView path) const override { return path.value() == "test.nif"; }
        std::string getDescription() const override { return "private parsed NIF maintenance fixture"; }
    };

    class CountingItem final : public SceneUtil::WorkItem
    {
    public:
        explicit CountingItem(std::atomic<unsigned>& calls) : mCalls(calls) {}
        void doWork() override { ++mCalls; }
    private:
        std::atomic<unsigned>& mCalls;
    };
    struct Gate
    {
        std::mutex mutex;
        std::condition_variable condition;
        bool started = false, released = false;
        void wait()
        {
            std::unique_lock lock(mutex);
            started = true; condition.notify_all();
            condition.wait(lock, [&] { return released; });
        }
        void waitStarted()
        {
            std::unique_lock lock(mutex);
            if (!condition.wait_for(lock, std::chrono::seconds(5), [&] { return started; }))
                throw std::runtime_error("fixture worker did not start within five seconds");
        }
        void release()
        {
            std::lock_guard lock(mutex);
            released = true; condition.notify_all();
        }
    };
    class BlockingItem final : public SceneUtil::WorkItem
    {
    public:
        explicit BlockingItem(Gate& gate) : mGate(gate) {}
        void doWork() override { mGate.wait(); }
    private:
        Gate& mGate;
    };
    // This calls the exact production admission/error-completion and queue
    // ownership helpers. ResourceSystem's manager graph is not substituted or
    // constructed by this isolated fixture; its game wiring is a later gate.
    class OptionalItem final : public SceneUtil::WorkItem
    {
    public:
        OptionalItem(Resource::LoadingNifMaintenancePolicy& policy, std::uint64_t phase,
            Resource::OpenGlPressureSample sample, std::uint64_t now, std::atomic<unsigned>& calls,
            bool& borrowedOwnerAlive, Gate* gate = nullptr, bool fail = false)
            : mPolicy(policy), mPhase(phase), mSample(sample), mNow(now), mCalls(calls),
              mOwnerAlive(borrowedOwnerAlive), mGate(gate), mFail(fail) {}
        void doWork() override
        {
            outcome = mPolicy.runOwned(mPhase, [&] {
                if (mGate) mGate->wait();
                if (!mOwnerAlive) throw std::runtime_error("borrowed state died before the owned task");
                if (!mPolicy.canRun(mPhase, mSample, mNow))
                    return Resource::LoadingNifMaintenancePolicy::Outcome::Skipped;
                if (mFail) throw std::bad_alloc();
                ++mCalls;
                return Resource::LoadingNifMaintenancePolicy::Outcome::Completed;
            });
        }
        Resource::LoadingNifMaintenancePolicy::Outcome outcome =
            Resource::LoadingNifMaintenancePolicy::Outcome::Error;
    private:
        Resource::LoadingNifMaintenancePolicy& mPolicy;
        std::uint64_t mPhase;
        Resource::OpenGlPressureSample mSample;
        std::uint64_t mNow;
        std::atomic<unsigned>& mCalls;
        bool& mOwnerAlive;
        Gate* mGate;
        bool mFail;
    };
    void failAllocation() { throw std::bad_alloc(); }
}

int main() try
{
    using Policy = Resource::LoadingNifMaintenancePolicy;
    auto sample = pressure();
    Policy disabled;
    disabled.setLoading(true);
    require(!disabled.tryBegin(sample, 1000, true), "default-off policy queued work");
    Policy policy(true);
    require(!policy.tryBegin(sample, 1000, true), "inactive loading admitted trim");
    policy.setLoading(true);
    require(!policy.tryBegin(sample, 1000, false), "missing existing queue admitted trim");
    for (unsigned invalid = 0; invalid < 4; ++invalid)
    {
        auto stale = sample;
        auto now = 1000ull;
        if (invalid == 0) stale.generation = 0;
        if (invalid == 1) stale.sampledAtMs = 0;
        if (invalid == 2) now = 999;
        if (invalid == 3) now = 2501;
        require(!policy.tryBegin(stale, now, true), "invalid or stale sample authorized release");
    }
    for (auto state : { Resource::OpenGlPressure::Normal, Resource::OpenGlPressure::Recovering,
             Resource::OpenGlPressure::Degraded })
    {
        auto other = sample; other.decision.state = state;
        require(!policy.tryBegin(other, 1000, true), "non-pressure state authorized release");
    }
    auto unproved = sample;
    unproved.memory.physicalAvailable = 16ull << 30;
    require(!policy.tryBegin(unproved, 1000, true), "pressure label without observed signal authorized release");
    auto unknown = sample;
    unknown.memory.physicalValid = unknown.memory.commitValid = false;
    require(!policy.tryBegin(unknown, 1000, true), "unknown observations authorized release");
    auto impossible = sample;
    impossible.memory.physicalAvailable = impossible.memory.physicalTotal + 1;
    require(!policy.tryBegin(impossible, 1000, true), "malformed physical sample authorized release");
    impossible = sample; impossible.decision.limits.physicalReserve = 0;
    require(!policy.tryBegin(impossible, 1000, true), "missing physical reserve authorized release");
    auto countOnly = unknown;
    countOnly.memory.privateCommit = std::numeric_limits<std::uint64_t>::max();
    countOnly.memory.processValid = true;
    require(!policy.tryBegin(countOnly, 1000, true), "process allocation count alone authorized release");
    const auto first = policy.tryBegin(sample, 1000, true);
    require(bool(first) && policy.pending(), "fresh pressure failed to queue one item");
    require(!policy.tryBegin(sample, 1100, true), "running/queued item admitted duplicate");
    require(policy.canRun(first.phase, sample, 1000), "fresh worker rejected its own phase");
    require(!policy.canRun(first.phase, sample, 2501), "delayed worker used stale evidence");
    require(!policy.canRun(first.phase, unproved, 1000), "worker ignored pressure recovery");
    policy.setLoading(false);
    policy.setLoading(true);
    require(!policy.canRun(first.phase, sample, 1000), "old queued phase ran in a new load");
    policy.finish(first.phase);
    require(!policy.pending(), "old owned completion leaked coalescing slot");
    require(!policy.tryBegin(sample, 1099, true), "cadence became one job per draw");
    auto next = policy.tryBegin(sample, 1100, true);
    require(bool(next) && next.phase != first.phase, "new loading phase reused old identity");
    policy.finish(first.phase);
    require(policy.pending(), "stale completion cleared a newer admission");
    policy.finish(next.phase);
    unsigned unownedCalls = 0;
    require(policy.runOwned(0, [&] { ++unownedCalls; return Policy::Outcome::Completed; })
            == Policy::Outcome::Skipped && unownedCalls == 0,
        "unowned phase executed an optional task");
    require(Policy::fresh(sample, 2500) && !Policy::fresh(sample, 2501),
        "sample freshness boundary changed");

    Policy concurrent(true);
    concurrent.setLoading(true);
    std::atomic<unsigned> accepted{ 0 };
    std::atomic<std::uint64_t> admittedPhase{ 0 };
    std::vector<std::thread> producers;
    for (unsigned i = 0; i < 8; ++i)
        producers.emplace_back([&] {
            const auto admission = concurrent.tryBegin(sample, 1000, true);
            if (admission) { ++accepted; admittedPhase = admission.phase; }
        });
    for (auto& thread : producers) thread.join();
    require(accepted == 1, "simultaneous producers admitted multiple pending items");
    concurrent.finish(admittedPhase);
    require(!concurrent.pending(), "concurrent pending admission failed to retire");

    Policy exhausted(true);
    Resource::LoadingNifMaintenanceTestAccess::setPhase(exhausted,
        std::numeric_limits<std::uint64_t>::max() - 2);
    const auto lastPhase = exhausted.tryBegin(sample, 1000, true);
    require(bool(lastPhase), "last legitimate active phase was rejected");
    exhausted.setLoading(false);
    exhausted.setLoading(true);
    require(!exhausted.canRun(lastPhase.phase, sample, 1000), "exhausted phase still authorized queued work");
    exhausted.finish(lastPhase.phase);
    for (unsigned i = 0; i < 4; ++i)
    {
        exhausted.setLoading(false); exhausted.setLoading(true);
        require(!exhausted.tryBegin(sample, 1400 + i * 100, true), "exhausted phase recycled an old identity");
    }

    auto commit = sample;
    commit.memory.physicalValid = false;
    commit.memory.commitAvailable = 1;
    next = policy.tryBegin(commit, 1200, true);
    require(bool(next), "independently valid commit pressure was ignored");
    policy.finish(next.phase);
    auto system = sample;
    system.memory.physicalValid = system.memory.commitValid = false;
    system.memory.systemCommitValid = true;
    system.memory.systemCommitAvailable = 1;
    next = policy.tryBegin(system, 1300, true);
    require(bool(next), "independently valid system commit pressure was ignored");
    policy.finish(next.phase);
    auto low = unknown;
    low.memory.lowMemoryValid = true; low.memory.lowMemory = true;
    low.decision.state = Resource::OpenGlPressure::Critical;
    next = policy.tryBegin(low, 1400, true);
    require(bool(next), "independently valid OS low-memory signal was ignored");
    policy.finish(next.phase);
    low.memory.lowMemoryValid = false;
    require(!policy.tryBegin(low, 1500, true), "unqualified OS low-memory flag authorized release");

    // A real NifFileManager/Reader/cache, not a substitute refcount model.
    auto archive = std::make_unique<MemoryArchive>();
    auto* source = &archive->file;
    VFS::Manager vfs;
    vfs.addArchive(std::move(archive)); vfs.buildIndex();
    constexpr VFS::Path::NormalizedView path("test.nif");
    Resource::NifFileManager ordinary(&vfs, nullptr);
    auto heldControl = ordinary.get(path);
    require(heldControl->mVersion == 0x04000002, "real NIF parser did not consume version");
    require(ordinary.trimUnusedForLoading(8) == 0, "disabled manager trimmed loading sources");
    Resource::NifFileManager candidate(&vfs, nullptr);
    candidate.setLoadingTrimEnabled(true);
    auto held = candidate.get(path);
    std::weak_ptr<const Nif::NIFFile> weak = held;
    const auto opens = source->opens.load();
    for (unsigned i = 0; i < 3; ++i)
    {
        Resource::CacheMaintenanceBudget budget(128, 8, std::chrono::milliseconds(50));
        Resource::CacheMaintenanceScope scope(budget);
        require(candidate.trimUnusedForLoading(8) == 0, "live shared NIF source retired");
    }
    auto other = candidate.get(path);
    require(other == held && source->opens.load() == opens, "live source protection reparsed/replaced identity");
    held.reset();
    require(candidate.trimUnusedForLoading(8) == 0, "second real consumer was ignored");
    other.reset();
    {
        Resource::CacheMaintenanceBudget zeroBudget(0, 0, std::chrono::milliseconds(50));
        Resource::CacheMaintenanceScope scope(zeroBudget);
        require(candidate.trimUnusedForLoading(8) == 0 && !weak.expired(),
            "zero shared release budget retired a source");
    }
    unsigned removed = 0;
    for (unsigned i = 0; i < 3 && removed == 0; ++i)
    {
        Resource::CacheMaintenanceBudget budget(128, 1, std::chrono::milliseconds(50));
        Resource::CacheMaintenanceScope scope(budget);
        removed += static_cast<unsigned>(candidate.trimUnusedForLoading(8));
        require(budget.released <= 1 && budget.scanned <= 128, "release escaped actual shared budget");
    }
    require(removed == 1 && weak.expired(), "cache-only source payload remained held after real retirement");
    auto recreated = candidate.get(path);
    require(recreated->mVersion == 0x04000002 && source->opens.load() == opens + 1,
        "retired source failed ordinary recreation");
    candidate.setColdRetentionSeconds(30, true);
    require(candidate.effectiveExpiryDelay() == 30, "loading policy changed existing cold TTL");
    candidate.setColdRetentionSeconds(0);
    require(candidate.effectiveExpiryDelay() == 0, "loading policy changed healthy zero TTL");
    candidate.updateCache(10); candidate.updateCache(20);
    require(candidate.get(path) == recreated && source->opens.load() == opens + 1,
        "ordinary zero-TTL maintenance dropped a live candidate source identity");
    Resource::NifFileManager coldCandidate(&vfs, nullptr);
    // Cold retention is a startup setting. Populate only AFTER its source
    // revision checks are configured; zero-TTL entries legitimately carry no
    // cold lease generation and may be reparsed when cold mode is enabled.
    coldCandidate.setColdRetentionSeconds(30, true);
    coldCandidate.setLoadingTrimEnabled(true);
    auto coldLive = coldCandidate.get(path);
    coldCandidate.setLoadingTrimEnabled(false);
    require(coldCandidate.trimUnusedForLoading(8) == 0, "disabled trim ignored its configuration");
    coldCandidate.updateCache(100); coldCandidate.updateCache(200);
    require(coldCandidate.get(path) == coldLive, "disabling loading trim removed the cold external-owner guard");

    // Exact-zero TTL must preserve only actual candidate source consumers.
    // Refreshing lastUsage alone is insufficient because <= referenceTime
    // still expires an externally owned wrapper at delay0. Both real cache
    // algorithms must keep live source identity while retiring cache-only
    // sources immediately, without a positive TTL/recent-hit workaround.
    for (bool budgeted : { false, true })
    {
        const auto update = [budgeted](Resource::NifFileManager& manager, double time) {
            if (budgeted)
            {
                Resource::CacheMaintenanceBudget budget(128, 8, std::chrono::milliseconds(50));
                Resource::CacheMaintenanceScope scope(budget);
                manager.updateCache(time);
                require(budget.scanned <= 128 && budget.released <= 8,
                    "zero-TTL source update escaped its shared budget");
            }
            else manager.updateCache(time);
        };
        Resource::NifFileManager unchanged(&vfs, nullptr);
        auto controlSource = unchanged.get(path);
        update(unchanged, 10);
        require(unchanged.get(path) != controlSource,
            "candidate zero-TTL protection changed ordinary source cache behavior");
        Resource::NifFileManager protectedSource(&vfs, nullptr);
        protectedSource.setLoadingTrimEnabled(true);
        auto live = protectedSource.get(path);
        const auto originalIdentity = live.get();
        const auto originalOpenCount = source->opens.load();
        update(protectedSource, 10); update(protectedSource, 20);
        require(protectedSource.get(path).get() == originalIdentity
                && source->opens.load() == originalOpenCount,
            "candidate zero-TTL update lost a real live shared source identity");
        std::weak_ptr<const Nif::NIFFile> cacheOnly = live;
        live.reset();
        update(protectedSource, 30);
        require(cacheOnly.expired() && protectedSource.effectiveExpiryDelay() == 0,
            "source protection changed immediate cache-only zero-TTL retirement");
        auto restored = protectedSource.get(path);
        require(restored->mVersion == 0x04000002 && source->opens.load() == originalOpenCount + 1,
            "zero-TTL retired source failed its ordinary parser recreation");
    }

    // Actual WorkQueue: failure before publish propagates without enqueue;
    // diagnostic allocation failure after publish must still complete once.
    osg::ref_ptr<SceneUtil::WorkQueue> ownedQueue = new SceneUtil::WorkQueue(1);
    auto& queue = *ownedQueue;
    std::atomic<unsigned> executed{ 0 };
    osg::ref_ptr<CountingItem> rejected = new CountingItem(executed);
    SceneUtil::WorkQueuePublicationTestHooks::beforePublication = &failAllocation;
    bool rejectedBeforePublication = false;
    try { queue.addWorkItem(rejected); }
    catch (const std::bad_alloc&) { rejectedBeforePublication = true; }
    SceneUtil::WorkQueuePublicationTestHooks::beforePublication = nullptr;
    require(rejectedBeforePublication && executed == 0 && !rejected->isDone() && queue.getNumItems() == 0,
        "pre-publication failure published or hid its failure");
    osg::ref_ptr<CountingItem> published = new CountingItem(executed);
    SceneUtil::WorkQueuePublicationTestHooks::afterPublicationDiagnostic = &failAllocation;
    queue.addWorkItem(published);
    SceneUtil::WorkQueuePublicationTestHooks::afterPublicationDiagnostic = nullptr;
    published->waitTillDone(); // Only an actually published owned item is waited.
    require(executed == 1 && published->isDone() && SceneUtil::workQueuePublicationDiagnosticFailures() == 1,
        "post-publication diagnostic failure abandoned/doubled the real item");
    osg::ref_ptr<CountingItem> following = new CountingItem(executed);
    queue.addWorkItem(following); following->waitTillDone();
    require(executed == 2 && SceneUtil::workQueuePublicationDiagnosticFailures() == 1,
        "following ordinary queue work regressed");

    // Exact owner helper publication failure cannot leave an unpublished item
    // behind for shutdown to wait on. Exception-bound optional work releases
    // its admission and subsequent work can execute on the same real queue.
    Policy taskPolicy(true); taskPolicy.setLoading(true);
    bool borrowedOwnerAlive = true;
    Resource::LoadingNifMaintenanceQueueOwner owner(taskPolicy);
    owner.attach(&queue);
    require(owner.available() && !owner.busy(), "existing idle queue attachment failed");
    auto taskAdmission = taskPolicy.tryBegin(sample, 1000, owner.available());
    osg::ref_ptr<OptionalItem> notPublished = new OptionalItem(taskPolicy, taskAdmission.phase,
        sample, 1000, executed, borrowedOwnerAlive);
    SceneUtil::WorkQueuePublicationTestHooks::beforePublication = &failAllocation;
    bool ownerRejected = false;
    try { owner.publish(notPublished); }
    catch (const std::bad_alloc&) { ownerRejected = true; taskPolicy.finish(taskAdmission.phase); }
    SceneUtil::WorkQueuePublicationTestHooks::beforePublication = nullptr;
    require(ownerRejected && !owner.busy() && !taskPolicy.pending() && !notPublished->isDone(),
        "unpublished item was retained for a shutdown wait");
    taskAdmission = taskPolicy.tryBegin(sample, 1100, owner.available());
    osg::ref_ptr<OptionalItem> faulted = new OptionalItem(taskPolicy, taskAdmission.phase,
        sample, 1100, executed, borrowedOwnerAlive, nullptr, true);
    owner.publish(faulted); faulted->waitTillDone();
    require(faulted->outcome == Policy::Outcome::Error && !taskPolicy.pending() && executed == 2,
        "optional item exception escaped or leaked its admission");
    taskAdmission = taskPolicy.tryBegin(sample, 1200, owner.available());
    osg::ref_ptr<OptionalItem> success = new OptionalItem(taskPolicy, taskAdmission.phase,
        sample, 1200, executed, borrowedOwnerAlive);
    const auto diagnosticFailures = SceneUtil::workQueuePublicationDiagnosticFailures();
    SceneUtil::WorkQueuePublicationTestHooks::afterPublicationDiagnostic = &failAllocation;
    owner.publish(success);
    SceneUtil::WorkQueuePublicationTestHooks::afterPublicationDiagnostic = nullptr;
    success->waitTillDone();
    require(success->outcome == Policy::Outcome::Completed && !taskPolicy.pending() && executed == 3
            && SceneUtil::workQueuePublicationDiagnosticFailures() == diagnosticFailures + 1,
        "diagnostic loss abandoned the owner's successfully published task");
    owner.detach();
    require(!owner.available() && !owner.busy(), "completed owner detach retained its queue/item");

    // Hold the actual worker, queue an owned task behind it, then tear down the
    // exact production owner. Detach invalidates the phase and drains ONLY its
    // task before the queue owner is allowed to stop the queue. The borrowed
    // resource state remains alive throughout and can be destroyed afterward.
    Gate blockerGate;
    osg::ref_ptr<BlockingItem> blocker = new BlockingItem(blockerGate);
    queue.addWorkItem(blocker); blockerGate.waitStarted();
    Policy queuedPolicy(true); queuedPolicy.setLoading(true);
    osg::ref_ptr<OptionalItem> queued;
    std::uint64_t queuedPhase = 0;
    std::thread releaseBlocker;
    {
        Resource::LoadingNifMaintenanceQueueOwner queuedOwner(queuedPolicy);
        queuedOwner.attach(&queue);
        const auto queuedAdmission = queuedPolicy.tryBegin(sample, 1000, queuedOwner.available());
        queuedPhase = queuedAdmission.phase;
        queued = new OptionalItem(queuedPolicy, queuedAdmission.phase, sample, 1000, executed, borrowedOwnerAlive);
        queuedOwner.publish(queued);
        require(queuedOwner.busy() && queue.getNumItems() == 1, "queued ownership fixture failed to queue behind blocker");
        osg::ref_ptr<SceneUtil::WorkQueue> foreignQueue = new SceneUtil::WorkQueue(1);
        require(!queuedOwner.detach(foreignQueue.get()) && queuedOwner.available(),
            "foreign queue detach disturbed the owned admission");
        releaseBlocker = std::thread([&] {
            while (queuedPolicy.canRun(queuedPhase, sample, 1000)) std::this_thread::yield();
            blockerGate.release();
        });
    } // Real owner destructor waits for the queued owned item before state teardown.
    releaseBlocker.join();
    require(queued->isDone() && queued->outcome == Policy::Outcome::Skipped
            && !queuedPolicy.pending() && executed == 3,
        "owner teardown dropped queued work or used a stale loading phase");

    Gate runningGate;
    Policy runningPolicy(true); runningPolicy.setLoading(true);
    osg::ref_ptr<OptionalItem> running;
    std::uint64_t runningPhase = 0;
    std::thread releaseRunning;
    {
        Resource::LoadingNifMaintenanceQueueOwner runningOwner(runningPolicy);
        runningOwner.attach(&queue);
        const auto runningAdmission = runningPolicy.tryBegin(sample, 1000, runningOwner.available());
        runningPhase = runningAdmission.phase;
        running = new OptionalItem(runningPolicy, runningAdmission.phase, sample, 1000, executed,
            borrowedOwnerAlive, &runningGate);
        runningOwner.publish(running); runningGate.waitStarted();
        releaseRunning = std::thread([&] {
            while (runningPolicy.canRun(runningPhase, sample, 1000)) std::this_thread::yield();
            runningGate.release();
        });
    }
    releaseRunning.join();
    require(running->isDone() && running->outcome == Policy::Outcome::Skipped && !runningPolicy.pending(),
        "owner teardown failed to drain its already running task");
    borrowedOwnerAlive = false; // Safe only after both real owned items complete.
    queue.stop();
    std::cout << "Loading parsed NIF maintenance native checks: " << checks << "\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "Loading parsed NIF maintenance fixture failed: " << error.what() << "\n";
    return 1;
}
