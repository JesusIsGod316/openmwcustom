#ifndef OPENMW_SCENEUTIL_TERRAINTARGETREADINESS_H
#define OPENMW_SCENEUTIL_TERRAINTARGETREADINESS_H

#include <components/loadinglistener/reporter.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace SceneUtil
{
    class TerrainTargetUnavailable final : public std::runtime_error
    {
    public:
        TerrainTargetUnavailable()
            : std::runtime_error("Terrain target was cancelled or deferred before readiness")
        {
        }
    };

    // One worker owns each private view and prepares complete views serially.
    // The main thread sees only target status/reporters, never mutable views.
    // Publication is per target: a ready first view cannot advertise its peers.
    class TerrainTargetReadiness
    {
    public:
        explicit TerrainTargetReadiness(std::size_t count)
        {
            mTargets.reserve(count);
            for (std::size_t i = 0; i < count; ++i)
                mTargets.push_back(std::make_unique<Target>());
        }

        template <class Prepare>
        bool run(std::size_t index, Prepare&& prepare)
        {
            return run(index, prepare, [](bool) noexcept {});
        }

        template <class Prepare, class BeforePublish>
        bool run(std::size_t index, Prepare&& prepare, BeforePublish&& beforePublish)
        {
            static_assert(std::is_nothrow_invocable_v<BeforePublish&, bool>);
            Target& target = *mTargets.at(index);
            {
                const std::lock_guard lock(target.mutex);
                if (target.status != Status::Pending)
                    return target.status == Status::Ready;
                target.status = Status::Running;
            }
            bool ready = false;
            std::exception_ptr failure;
            try
            {
                ready = prepare(target.reporter);
            }
            catch (...)
            {
                failure = std::current_exception();
            }
            // Completion metadata belongs to this same publication boundary,
            // rather than a later assignment after a waiting main thread wakes.
            beforePublish(ready);
            {
                const std::lock_guard lock(target.mutex);
                target.failure = failure;
                target.status = failure ? Status::Failed : ready ? Status::Ready : Status::Cancelled;
            }
            // Complete callback writes precede status publication and reporter
            // completion. Only this immutable readiness status is exposed; the
            // sole worker may still strengthen its private view afterward.
            // Failure/cancellation also wakes a demander.
            target.reporter.complete();
            return ready;
        }

        void wait(std::size_t index, Loading::Listener& listener)
        {
            mDemand.store(index, std::memory_order_release);
            Target& target = *mTargets.at(index);
            target.reporter.wait(listener);
            std::exception_ptr failure;
            Status status;
            {
                const std::lock_guard lock(target.mutex);
                failure = target.failure;
                status = target.status;
            }
            if (failure)
                std::rethrow_exception(failure);
            if (status != Status::Ready)
                throw TerrainTargetUnavailable{};
        }

        std::optional<std::size_t> nextPending() const
        {
            const std::size_t demand = mDemand.load(std::memory_order_acquire);
            if (demand < mTargets.size() && status(demand) == Status::Pending)
                return demand;
            for (std::size_t i = 0; i < mTargets.size(); ++i)
                if (status(i) == Status::Pending)
                    return i;
            return std::nullopt;
        }

        bool demanded(std::size_t index) const { return mDemand.load(std::memory_order_acquire) == index; }
        bool ready(std::size_t index) const { return status(index) == Status::Ready; }
        bool canServe(std::size_t index) const
        {
            const Status value = status(index);
            return value == Status::Ready || value == Status::Pending || value == Status::Running;
        }
        std::vector<std::size_t> completed() const
        {
            std::vector<std::size_t> result;
            for (std::size_t i = 0; i < mTargets.size(); ++i)
                if (ready(i))
                    result.push_back(i);
            return result;
        }
        // Called by the owning worker after its final callback, never while a
        // private view is still being written. Already ready targets stay ready.
        void cancelPending()
        {
            for (const auto& target : mTargets)
            {
                bool cancelled = false;
                {
                    const std::lock_guard lock(target->mutex);
                    if (target->status == Status::Pending)
                    {
                        target->status = Status::Cancelled;
                        cancelled = true;
                    }
                }
                if (cancelled)
                    target->reporter.complete();
            }
        }

    private:
        enum class Status
        {
            Pending,
            Running,
            Ready,
            Failed,
            Cancelled
        };
        struct Target
        {
            mutable std::mutex mutex;
            Status status = Status::Pending;
            std::exception_ptr failure;
            Loading::Reporter reporter;
        };
        Status status(std::size_t index) const
        {
            const Target& target = *mTargets.at(index);
            const std::lock_guard lock(target.mutex);
            return target.status;
        }
        std::vector<std::unique_ptr<Target>> mTargets;
        std::atomic<std::size_t> mDemand{ 0 };
    };
}

#endif
