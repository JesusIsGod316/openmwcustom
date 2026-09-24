#ifndef OPENMW_COMPONENTS_RESOURCE_OPENGLPRESSURE_H
#define OPENMW_COMPONENTS_RESOURCE_OPENGLPRESSURE_H

#include <components/misc/hostmemory.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace Resource
{
    // GL-P1A governs OPTIONAL ownership only. It is not a process allocation
    // cap, an OOM guarantee, or a replacement for the GL-P1B byte ledger.
    enum class OpenGlPressure { Normal, Caution, Critical, Recovering, Degraded };

    struct OpenGlPressureConfig
    {
        static constexpr std::uint64_t MiB = 1024ull * 1024;
        static constexpr std::uint64_t SampleMilliseconds = 1000;
        static constexpr std::uint64_t StaleMilliseconds = 3000;
        static constexpr std::uint64_t RecoveryMilliseconds = 5000;
        std::uint64_t physicalReserve = 0; // zero: max(512 MiB, usable RAM / 8)
        std::uint64_t commitReserve = 1024 * MiB; // independent of physical RAM
    };

    struct OpenGlPressureLimits
    {
        std::uint64_t physicalReserve = 0, physicalCritical = 0, physicalRecovery = 0;
        std::uint64_t commitReserve = 0, commitCritical = 0, commitRecovery = 0;
    };

    struct OpenGlPressureDecision
    {
        OpenGlPressure state = OpenGlPressure::Degraded;
        OpenGlPressureLimits limits;
        unsigned admissionsPerSample = 0; // bounded even if jobs finish between samples
    };

    class OpenGlPressurePolicy
    {
    public:
        explicit OpenGlPressurePolicy(OpenGlPressureConfig config = {}) : mConfig(config) {}

        OpenGlPressureLimits limits(std::uint64_t total) const noexcept
        {
            // Bounded inputs also make recovery-threshold arithmetic safe.
            constexpr auto maximum = 65536 * OpenGlPressureConfig::MiB;
            const auto physical = std::clamp(mConfig.physicalReserve ? mConfig.physicalReserve : total / 8,
                512 * OpenGlPressureConfig::MiB, maximum);
            const auto commit = std::clamp(mConfig.commitReserve, 256 * OpenGlPressureConfig::MiB, maximum);
            return {physical, physical / 2, physical + physical / 4,
                commit, commit / 4, commit + commit / 2};
        }

        OpenGlPressureDecision update(const Misc::HostMemoryStatus& memory, std::uint64_t now) noexcept
        {
            const bool physical = memory.physicalValid && memory.physicalTotal != 0
                && memory.physicalAvailable <= memory.physicalTotal;
            if (physical) mLastPhysicalTotal = memory.physicalTotal;
            const auto budget = limits(mLastPhysicalTotal);
            if (mLastSample && (now < *mLastSample
                    || now - *mLastSample > OpenGlPressureConfig::StaleMilliseconds))
            {
                mNeedsRecovery = true;
                mHealthySince.reset();
            }
            mLastSample = now;
            const bool critical = (physical && memory.physicalAvailable < budget.physicalCritical)
                || (memory.commitValid && memory.commitAvailable < budget.commitCritical)
                || (memory.systemCommitValid && memory.systemCommitAvailable < budget.commitCritical)
                || (memory.lowMemoryValid && memory.lowMemory);
            const bool caution = (physical && memory.physicalAvailable < budget.physicalReserve)
                || (memory.commitValid && memory.commitAvailable < budget.commitReserve)
                || (memory.systemCommitValid && memory.systemCommitAvailable < budget.commitReserve);
            // Evaluate independent valid signals BEFORE deciding whether the
            // remaining sample is adequate. PrivateUsage is diagnostic only.
            if (critical || caution)
            {
                mNeedsRecovery = true;
                mHealthySince.reset();
                mRampSince.reset();
                return {critical ? OpenGlPressure::Critical : OpenGlPressure::Caution, budget, 0};
            }
            if (!physical || (!memory.commitValid && !memory.systemCommitValid))
            {
                mNeedsRecovery = true;
                mHealthySince.reset();
                mRampSince.reset();
                // Unknown does not justify destructive trimming, nor new
                // speculation. A later valid sample can recover normally.
                return {OpenGlPressure::Degraded, budget, 0};
            }
            if (mNeedsRecovery)
            {
                const bool recovered = memory.physicalAvailable >= budget.physicalRecovery
                    && (!memory.commitValid || memory.commitAvailable >= budget.commitRecovery)
                    && (!memory.systemCommitValid || memory.systemCommitAvailable >= budget.commitRecovery);
                if (!recovered) mHealthySince.reset();
                else if (!mHealthySince || now < *mHealthySince) mHealthySince = now;
                if (!mHealthySince || now - *mHealthySince < OpenGlPressureConfig::RecoveryMilliseconds)
                    return {OpenGlPressure::Recovering, budget, 0};
                mNeedsRecovery = false;
                mHealthySince.reset();
                mRampSince = now;
            }
            const bool ramp = mRampSince && now >= *mRampSince
                && now - *mRampSince < OpenGlPressureConfig::RecoveryMilliseconds;
            return {OpenGlPressure::Normal, budget, ramp ? 1u : 4u};
        }

    private:
        OpenGlPressureConfig mConfig;
        std::uint64_t mLastPhysicalTotal = 0;
        std::optional<std::uint64_t> mLastSample, mHealthySince, mRampSince;
        bool mNeedsRecovery = false;
    };

    struct OpenGlPressureSample
    {
        Misc::HostMemoryStatus memory;
        OpenGlPressureDecision decision;
        std::uint64_t generation = 0, sampledAtMs = 0;
    };

    // One sampler per enabled ResourceSystem, NOT one worker per cache/job.
    // It has no scene, GL-context or diagnostics dependency. Isolating this
    // small OS-query owner avoids starving samples behind a long cache sweep
    // or terrain build, including during loading screens.
    class OpenGlPressureMonitor
    {
    public:
        using Query = Misc::HostMemoryStatus (*)() noexcept;
        using Clock = std::uint64_t (*)() noexcept;
        static std::uint64_t milliseconds() noexcept
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        explicit OpenGlPressureMonitor(OpenGlPressureConfig config = {},
            Query query = &Misc::queryOpenGlHostMemoryStatus, Clock clock = &milliseconds, bool background = true)
            : mPolicy(config), mQuery(query), mClock(clock)
        {
            // Seed at engine initialization, before gameplay/preload workers.
            // No consumer ever queries the OS. Manual mode supports native tests.
            refresh();
            if (background) mThread = std::thread([this] { run(); });
        }
        OpenGlPressureMonitor(const OpenGlPressureMonitor&) = delete;
        OpenGlPressureMonitor& operator=(const OpenGlPressureMonitor&) = delete;
        ~OpenGlPressureMonitor()
        {
            {
                std::lock_guard lock(mWaitMutex);
                mStop = true;
            }
            mWake.notify_one();
            if (mThread.joinable()) mThread.join();
        }

        OpenGlPressureSample read() const
        {
            // Bounded try-lock copy only; OS calls never hold this mutex.
            // A contended publication or old sample denies new optional work
            // without inventing pressure or waiting on the frame thread.
            std::unique_lock lock(mPublicationMutex, std::try_to_lock);
            if (!lock.owns_lock()) return {};
            auto sample = mPublished;
            lock.unlock();
            const auto now = mClock();
            if (!sample.generation || now < sample.sampledAtMs
                || now - sample.sampledAtMs > OpenGlPressureConfig::StaleMilliseconds)
                return {};
            return sample;
        }

        // Single serialized sampling entry point, also usable by deterministic
        // native tests. Frame/render/resource consumers must call read(), not this.
        void refresh()
        {
            std::lock_guard sampling(mSamplingMutex);
            OpenGlPressureSample next;
            next.sampledAtMs = mClock(); // do not label a slow query freshly sampled
            next.memory = mQuery();
            next.decision = mPolicy.update(next.memory, next.sampledAtMs);
            std::lock_guard publication(mPublicationMutex);
            next.generation = mPublished.generation + 1;
            mPublished = next;
        }

    private:
        void run()
        {
            std::unique_lock lock(mWaitMutex);
            while (!mWake.wait_for(lock, std::chrono::milliseconds(OpenGlPressureConfig::SampleMilliseconds),
                [this] { return mStop; }))
            {
                lock.unlock();
                refresh();
                lock.lock();
            }
        }
        OpenGlPressurePolicy mPolicy;
        Query mQuery;
        Clock mClock;
        std::mutex mSamplingMutex;
        mutable std::mutex mPublicationMutex;
        OpenGlPressureSample mPublished;
        std::mutex mWaitMutex;
        std::condition_variable mWake;
        bool mStop = false;
        std::thread mThread;
    };
}
#endif
