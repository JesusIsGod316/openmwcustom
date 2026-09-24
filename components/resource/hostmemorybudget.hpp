#ifndef OPENMW_COMPONENTS_RESOURCE_HOSTMEMORYBUDGET_H
#define OPENMW_COMPONENTS_RESOURCE_HOSTMEMORYBUDGET_H

#include <components/misc/hostmemory.hpp>
#include "preloadadmission.hpp"
#include "openglpressure.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace Resource
{
    enum class HostMemoryPressure : unsigned char { Normal, Trim, Critical };

    struct HostMemoryLimits
    {
        std::uint64_t reserve = 0, criticalReserve = 0, recoveryReserve = 0;
        std::uint64_t privateSoft = 0, privateCritical = 0, privateRecovery = 0;
    };

    // Soft limits govern OPTIONAL retention only. They are not a hard cap on
    // process memory: live scenes, required loading and GPU use remain protected.
    class HostMemoryPolicy
    {
    public:
        static constexpr std::uint64_t MiB = 1024ull * 1024;
        static constexpr std::uint64_t RecoveryMilliseconds = 5000;
        static HostMemoryLimits limits(std::uint64_t total) noexcept
        {
            const auto reserve = (std::max)(512 * MiB, total / 8);
            const auto critical = (std::max)(128 * MiB, total / 32);
            const auto soft = total - total / 4;
            return { reserve, critical, reserve + critical, soft,
                total - total / 8, soft > critical ? soft - critical : 0 };
        }

        HostMemoryPressure update(const Misc::HostMemoryStatus& memory, std::uint64_t nowMs) noexcept
        {
            if (!memory.physicalValid || memory.physicalTotal == 0
                || memory.physicalAvailable > memory.physicalTotal)
            {
                mHealthySince.reset();
                return mPressure;
            }
            const auto budget = limits(memory.physicalTotal);
            const bool critical = memory.physicalAvailable < budget.criticalReserve
                || (memory.processValid && memory.privateCommit >= budget.privateCritical)
                || (memory.commitValid && memory.commitAvailable < budget.criticalReserve);
            const bool pressure = memory.physicalAvailable < budget.reserve
                || (memory.processValid && memory.privateCommit >= budget.privateSoft)
                || (memory.commitValid && memory.commitAvailable < budget.reserve);
            if (critical)
            {
                mPressure = HostMemoryPressure::Critical;
                mHealthySince.reset();
            }
            else if (pressure)
            {
                mPressure = HostMemoryPressure::Trim;
                mHealthySince.reset();
            }
            else if (mPressure != HostMemoryPressure::Normal)
            {
                const bool recovered = memory.processValid && memory.commitValid
                    && memory.physicalAvailable >= budget.recoveryReserve
                    && memory.privateCommit < budget.privateRecovery
                    && memory.commitAvailable >= budget.recoveryReserve;
                if (!recovered)
                    mHealthySince.reset();
                else if (!mHealthySince || nowMs < *mHealthySince)
                    mHealthySince = nowMs;
                else if (nowMs - *mHealthySince >= RecoveryMilliseconds)
                {
                    mPressure = HostMemoryPressure::Normal;
                    mHealthySince.reset();
                }
            }
            return mPressure;
        }
    private:
        HostMemoryPressure mPressure = HostMemoryPressure::Normal;
        std::optional<std::uint64_t> mHealthySince;
    };

    // Shared by preload workers and cache maintenance. At most one inexpensive
    // OS sample per second, including during a main-thread loading screen wait.
    // It observes no live graph and creates no worker, GPU wait or recorder.
    class HostMemoryBudget
    {
    public:
        using Query = Misc::HostMemoryStatus (*)() noexcept;
        explicit HostMemoryBudget(Query query = &Misc::queryHostMemoryStatus) : mQuery(query) {}
        void setEnabled(bool enabled) noexcept { mEnabled.store(enabled, std::memory_order_release); }
        bool enabled() const noexcept { return mEnabled.load(std::memory_order_acquire); }
        // Startup-only; off/Vulkan keep the original coordinator and policy.
        void enableOpenGl(OpenGlPressureConfig config = {}, const std::atomic<std::uint64_t>* watermark = nullptr)
        {
            mOpenGl = std::make_unique<OpenGlPressureMonitor>(config, &Misc::queryOpenGlHostMemoryStatus,
                &OpenGlPressureMonitor::milliseconds, true, watermark);
            mEnabled.store(true, std::memory_order_release);
        }
        bool openGlEnabled() const noexcept { return enabled() && mOpenGl != nullptr; }
        OpenGlPressureSample openGlSample() const { return mOpenGl ? mOpenGl->read() : OpenGlPressureSample{}; }
        HostMemoryPressure pressure()
        {
            if (!enabled()) return HostMemoryPressure::Normal;
            if (mOpenGl)
            {
                switch (mOpenGl->read().decision.state)
                {
                    case OpenGlPressure::Critical: return HostMemoryPressure::Critical;
                    case OpenGlPressure::Caution: return HostMemoryPressure::Trim;
                    // Missing data or recovery suppresses admission, not hot
                    // cache ownership. Never invent pressure from failed probes.
                    default: return HostMemoryPressure::Normal;
                }
            }
            const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            if (now >= mNextSample.load(std::memory_order_relaxed))
            {
                std::unique_lock lock(mMutex, std::try_to_lock);
                if (lock.owns_lock() && now >= mNextSample.load(std::memory_order_relaxed))
                {
                    mLastMemory = mQuery();
                    mPressure.store(mPolicy.update(mLastMemory, now), std::memory_order_release);
                    mNextSample.store(now + 1000, std::memory_order_relaxed);
                }
            }
            return mPressure.load(std::memory_order_acquire);
        }
        Misc::HostMemoryStatus snapshot() const
        {
            if (mOpenGl) return mOpenGl->read().memory;
            std::lock_guard lock(mMutex);
            return mLastMemory;
        }
        std::optional<PreloadAdmission::Reservation> reserveOptionalPreload()
        {
            if (!enabled()) return PreloadAdmission::Reservation{};
            if (mOpenGl)
            {
                const auto sample = mOpenGl->read();
                return mPreloads.reserveOpenGl(sample.memory, sample.generation,
                    sample.decision.admissionsPerSample, sample.decision.limits.physicalReserve,
                    sample.decision.limits.commitReserve);
            }
            const auto currentPressure = pressure();
            const auto memory = snapshot();
            const auto budget = HostMemoryPolicy::limits(memory.physicalTotal);
            return mPreloads.reserve(memory, currentPressure != HostMemoryPressure::Normal,
                budget.reserve, budget.privateSoft);
        }
        PreloadAdmission::Stats preloadAdmissionStats() const { return mPreloads.stats(); }
    private:
        Query mQuery;
        std::atomic<bool> mEnabled{false};
        std::atomic<HostMemoryPressure> mPressure{HostMemoryPressure::Normal};
        std::atomic<std::uint64_t> mNextSample{0};
        mutable std::mutex mMutex;
        HostMemoryPolicy mPolicy;
        Misc::HostMemoryStatus mLastMemory;
        PreloadAdmission mPreloads;
        // Only allocated/started when the explicit OpenGL switch is enabled.
        std::unique_ptr<OpenGlPressureMonitor> mOpenGl;
    };
}
#endif
