#ifndef OPENMW_COMPONENTS_RESOURCE_ANIMATIONPREPARATION_H
#define OPENMW_COMPONENTS_RESOURCE_ANIMATIONPREPARATION_H

#include "animationdependencies.hpp"
#include "objectcache.hpp"

#include <atomic>
#include <span>
#include <vector>

namespace Resource
{
    // One sample per decision. Invalid probes deny NEW optional work; they do
    // not invent pressure to destroy an already owned immutable resource.
    inline bool animationNewWorkAllowed(const OpenGlPressureSample& sample,
        SpeculativePriority priority) noexcept
    {
        const bool valid = sample.generation && sample.memory.physicalValid && sample.memory.physicalTotal
            && sample.memory.physicalAvailable <= sample.memory.physicalTotal
            && (sample.memory.commitValid || sample.memory.systemCommitValid)
            && !(sample.memory.lowMemoryValid && sample.memory.lowMemory);
        if (!valid)
            return false;
        return priority == SpeculativePriority::NearFuture
            ? sample.decision.state == OpenGlPressure::Normal || sample.decision.state == OpenGlPressure::Caution
                || sample.decision.state == OpenGlPressure::Recovering
            : sample.decision.state == OpenGlPressure::Normal && sample.decision.admissionsPerSample != 0;
    }

    inline bool animationOwnedWorkMayRemain(const OpenGlPressureSample& sample, bool currentTarget) noexcept
    {
        if (sample.decision.state == OpenGlPressure::Critical
            || (sample.memory.lowMemoryValid && sample.memory.lowMemory))
            return false;
        return sample.decision.state != OpenGlPressure::Caution || currentTarget;
    }

    // Worker-owned until complete() publishes immutable progress. Resume uses
    // a new admitted WorkItem and copies only a completed ticket's ready leases.
    class AnimationPreparation : public osg::Referenced
    {
    public:
        struct Entry
        {
            VFS::Path::Normalized mPath;
            ObjectCacheLease mLease;
        };
        static constexpr std::uint64_t MetadataEstimate = 6 * SpeculativeBudget::MiB;

        AnimationPreparation(SpeculativeBudget& budget, const OpenGlPressureSample& sample,
            SpeculativePriority priority, std::uint64_t generation, bool additional,
            std::span<const VFS::Path::Normalized> sources, std::uint64_t copiedOwnerMetadata = 0)
            : mGeneration(generation)
            , mAdditional(additional)
            , mCopiedOwnerMetadata(copiedOwnerMetadata)
        {
            if (!generation || !animationNewWorkAllowed(sample, priority)
                || sources.size() > MaximumAnimationPreloadSources)
                throw SpeculativeDeferred{};
            for (const auto& source : sources)
                if (source.value().size() > 4096)
                    throw SpeculativeDeferred{};
            // Covers capacities and all bounded path strings, including resume
            // copies. Payloads have their own exact shared charges.
            auto stage = budget.reserve(sample, 2 * MetadataEstimate, priority);
            mMetadataCharge = budget.track({this, 5}, MetadataEstimate, false, &stage);
            mSources.assign(sources.begin(), sources.end());
            mEntries.reserve(MaximumAnimationPreloadDependencies);
        }

        bool matches(std::uint64_t generation, bool additional,
            std::span<const VFS::Path::Normalized> sources) const noexcept
        {
            return published() && mGeneration == generation && mAdditional == additional
                && std::equal(mSources.begin(), mSources.end(), sources.begin(), sources.end());
        }
        bool published() const noexcept { return mPublished.load(std::memory_order_acquire); }
        bool cancelled() const noexcept { return mCancelled.load(std::memory_order_acquire); }
        void cancel() noexcept { mCancelled.store(true, std::memory_order_release); }
        void complete(bool retryable) noexcept
        {
            mRetryable = retryable;
            mPublished.store(true, std::memory_order_release);
        }
        bool retryable() const noexcept { return published() && mRetryable && !cancelled(); }
        std::uint64_t generation() const noexcept { return mGeneration; }
        bool additionalSources() const noexcept { return mAdditional; }
        std::uint64_t metadataBytes() const noexcept { return MetadataEstimate; }
        std::uint64_t retainedBytes() const noexcept
        {
            return published() ? MetadataEstimate + mPayloadBytes : MaximumAnimationPreloadEstimate;
        }
        std::uint64_t remainingBytes() const noexcept
        {
            const auto used = MetadataEstimate + mCopiedOwnerMetadata + mPayloadBytes;
            return used < MaximumAnimationPreloadEstimate ? MaximumAnimationPreloadEstimate - used : 0;
        }
        const Entry* find(VFS::Path::NormalizedView path) const noexcept
        {
            const auto it = std::find_if(mEntries.begin(), mEntries.end(),
                [&](const Entry& entry) { return entry.mPath == path; });
            return it != mEntries.end() ? &*it : nullptr;
        }
        bool adopt(VFS::Path::NormalizedView path, ObjectCacheLease lease)
        {
            if (cancelled() || published() || !lease || !lease.mCharge || !lease.bytes()
                || lease.mSourceRevision != mGeneration || path.value().size() > 4096)
                return false;
            if (find(path))
                return true;
            const bool shared = std::any_of(mEntries.begin(), mEntries.end(),
                [&](const Entry& entry) { return entry.mLease.mCharge == lease.mCharge; });
            const auto bytes = shared ? 0 : lease.bytes();
            if (mEntries.size() >= MaximumAnimationPreloadDependencies || bytes > remainingBytes())
                return false;
            mEntries.push_back({VFS::Path::Normalized(path), std::move(lease)});
            mPayloadBytes += bytes;
            return true;
        }
        void forget(VFS::Path::NormalizedView path)
        {
            if (published()) return;
            const auto it = std::find_if(mEntries.begin(), mEntries.end(),
                [&](const Entry& entry) { return entry.mPath == path; });
            if (it == mEntries.end()) return;
            const auto charge = it->mLease.mCharge;
            const bool shared = std::any_of(mEntries.begin(), mEntries.end(),
                [&](const Entry& entry) { return &entry != &*it && entry.mLease.mCharge == charge; });
            if (!shared) mPayloadBytes -= charge->bytes();
            mEntries.erase(it); // optional worker, outside resource/cache locks
        }
        std::size_t importReady(const AnimationPreparation& previous)
        {
            if (!previous.published() || previous.cancelled() || previous.mGeneration != mGeneration || previous.mAdditional != mAdditional
                || previous.mSources != mSources)
                return 0;
            std::size_t copied = 0;
            for (const auto& entry : previous.mEntries)
                if (adopt(entry.mPath, entry.mLease))
                    ++copied;
            return copied;
        }
        std::size_t size() const noexcept { return mEntries.size(); } // worker/published readers only

    private:
        // Declaration order ensures payload/path release precedes metadata charge.
        SpeculativeBudget::ChargePtr mMetadataCharge;
        std::vector<VFS::Path::Normalized> mSources;
        std::vector<Entry> mEntries;
        const std::uint64_t mGeneration;
        const bool mAdditional;
        const std::uint64_t mCopiedOwnerMetadata;
        std::uint64_t mPayloadBytes = 0;
        bool mRetryable = false;
        std::atomic<bool> mCancelled{false}, mPublished{false};
    };
}

#endif
