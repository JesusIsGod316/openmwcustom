#ifndef OPENMW_COMPONENTS_RESOURCE_ANIMATIONSOURCEPLAN_H
#define OPENMW_COMPONENTS_RESOURCE_ANIMATIONSOURCEPLAN_H

#include "animationdependencies.hpp"
#include "speculativebudget.hpp"
#include <components/debug/hitchtrace.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Resource
{
    // Owned path metadata only. VFS rebuild/reset still requires its documented
    // reader quiescence; generation checks do not make borrowed index reads safe.
    class AnimationSourcePlanCache
    {
        static constexpr std::size_t MaximumPlans = 64;
        static constexpr std::uint64_t MaximumPlanBytes = 256 * SpeculativeBudget::MiB / 1024;
        static constexpr std::uint64_t MaximumResidentBytes = 2 * SpeculativeBudget::MiB;
        struct Pool
        {
            std::atomic<std::uint64_t> bytes{0};
            std::atomic<std::size_t> plans{0};
        };
        struct Resident
        {
            std::shared_ptr<Pool> pool;
            std::uint64_t bytes = 0;
            ~Resident()
            {
                if (bytes)
                {
                    pool->bytes.fetch_sub(bytes, std::memory_order_relaxed);
                    pool->plans.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        };
        struct Plan
        {
            Resident resident; // last release, after paths and matching charge
            SpeculativeBudget::ChargePtr charge;
            std::vector<VFS::Path::Normalized> paths; // includes non-KF entries for exact scan accounting
            std::uint64_t bytes = sizeof(Plan) + 256;
        };

    public:
        void setBudget(SpeculativeBudget* budget) noexcept { mBudget = budget; } // startup only
        void clear()
        {
            decltype(mPlans) release;
            { std::lock_guard lock(mMutex); release.swap(mPlans); mGeneration = 0; }
        }

        template <class Visitor, class Continue, class Sample>
        bool visit(const VFS::Manager& vfs, VFS::Path::NormalizedView model,
            Visitor&& visit, Continue&& keepGoing, Sample&& sample)
        {
            if (!animationSourcePlanEnabled() || !mBudget)
                return visitAdditionalAnimationSources(vfs, model, visit, keepGoing);
            const std::uint64_t generation = vfs.getIndexGeneration();
            std::shared_ptr<const Plan> ready;
            decltype(mPlans) release;
            {
                std::lock_guard lock(mMutex);
                if (generation != mGeneration)
                { release.swap(mPlans); mGeneration = generation; }
                const auto it = mPlans.find(model.value());
                if (it != mPlans.end()) ready = it->second;
            }
            release.clear(); // no payload/charge final release under the plan lock
            Debug::HitchTrace::Scope receipt("animation.source_plan", {}, model.value());
            receipt.retain();
            receipt.setMetric(0, "cache_hit", bool(ready));
            receipt.setMetric(1, "vfs_generation", generation);
            if (ready)
            {
                receipt.setMetric(2, "scanned_paths", ready->paths.size());
                for (const auto& path : ready->paths)
                {
                    if (!keepGoing()) return false;
                    if (path.extension() == VFS::Path::ExtensionView("kf") && !visit(path)) return false;
                }
                return true;
            }

            const auto directory = additionalAnimationDirectory(model);
            if (directory.empty()) return true;
            std::optional<SpeculativeBudget::Stage> stage;
            std::shared_ptr<Plan> building;
            try
            {
                // Reserve metadata before collecting it. This uses a direct
                // stage, not a speculative TLS scope around required callbacks.
                stage.emplace(mBudget->reserve(sample(), 2 * MaximumPlanBytes,
                    SpeculativeScope::active() ? SpeculativeScope::capture().priority : SpeculativePriority::Background));
                building = std::make_shared<Plan>();
            }
            catch (const SpeculativeDeferred&) {} // required traversal continues unchanged
            catch (const std::bad_alloc&) {}
            std::size_t scanned = 0;
            for (const auto& path : vfs.getRecursiveDirectoryIterator(directory))
            {
                if (!keepGoing()) return false;
                ++scanned;
                if (building)
                {
                    // Conservative 2x string/vector slack is bounded before any
                    // append. An oversized plan is abandoned, never truncated.
                    const auto bytes = sizeof(VFS::Path::Normalized) * 2 + 2 * (path.value().size() + 1);
                    if (scanned > MaximumAnimationPreloadScannedPaths || path.value().size() > 4096
                        || bytes > MaximumPlanBytes - (std::min)(MaximumPlanBytes, building->bytes))
                        building.reset();
                    else
                    {
                        try { building->paths.push_back(path); building->bytes += bytes; }
                        catch (const std::bad_alloc&) { building.reset(); }
                    }
                }
                if (path.extension() == VFS::Path::ExtensionView("kf") && !visit(path)) return false;
            }
            receipt.setMetric(2, "scanned_paths", scanned);
            try
            {
                if (building && generation == vfs.getIndexGeneration())
                {
                    // Includes the key and conservative node overhead. Pool
                    // limits include plans still held after eviction.
                    building->bytes += model.value().size() + 1;
                    if (building->bytes <= MaximumPlanBytes)
                    {
                        building->charge = mBudget->track({building.get(), 4}, building->bytes, false, &*stage);
                        std::lock_guard lock(mMutex);
                        if (!mPool) mPool = std::make_shared<Pool>();
                        if (mGeneration == generation && !mPlans.contains(model.value())
                            && mPool->plans.load(std::memory_order_relaxed) < MaximumPlans
                            && building->bytes <= MaximumResidentBytes
                                - (std::min)(MaximumResidentBytes, mPool->bytes.load(std::memory_order_relaxed)))
                        {
                            // Insertion failure releases building outside lock.
                            building->resident.pool = mPool;
                            building->resident.bytes = building->bytes;
                            mPool->bytes.fetch_add(building->bytes, std::memory_order_relaxed);
                            mPool->plans.fetch_add(1, std::memory_order_relaxed);
                            mPlans.emplace(std::string(model.value()), building);
                            receipt.setMetric(3, "published", 1);
                        }
                    }
                }
            }
            catch (const std::bad_alloc&) {} // optional metadata cannot fail required actor binding
            catch (const std::overflow_error&) {} // saturated accounting means no new cached plan
            return true;
        }

    private:
        SpeculativeBudget* mBudget = nullptr;
        std::mutex mMutex;
        std::uint64_t mGeneration = 0;
        std::shared_ptr<Pool> mPool;
        std::map<std::string, std::shared_ptr<const Plan>, std::less<>> mPlans;
    };
}

#endif
