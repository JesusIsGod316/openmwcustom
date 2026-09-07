#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDRESIDENCY_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDRESIDENCY_H

#include "framecompletion.hpp"
#include "staticworldplan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace RenderVsg
{
    [[nodiscard]] inline std::uint64_t staticInstanceKey(RenderCore::InstanceHandle handle) noexcept
    {
        return (static_cast<std::uint64_t>(handle.generation()) << 32u) | handle.slot();
    }

    struct StaticWorldMutation
    {
        std::uint64_t serial = 0;
        RenderCore::WorldEpoch worldEpoch;
        RenderCore::RenderWorldRevision worldRevision;
        std::vector<RenderCore::InstanceHandle> orderedInstances;
        std::vector<StaticInstancePlan> upserts;
        std::vector<RenderCore::InstanceHandle> removals;
        std::uint32_t simpleMeshInstancesDeferred = 0;
        std::uint32_t dynamicInstancesDeferred = 0;
        bool valid = false;
    };

    template <class Object>
    struct StaticWorldCommitResult
    {
        bool committed = false;
        std::vector<Object> immediatelyReleased;
    };

    // Persistent logical-instance to backend-object ownership. Planning and
    // realization are deliberately separate: callers compile every upsert first,
    // then atomically commit the mutation at a frame boundary. A failed shader,
    // pipeline, texture, or allocation compile leaves the live scene untouched.
    template <class Object>
    class StaticWorldResidency
    {
    public:
        static_assert(std::is_copy_constructible_v<Object>);
        static_assert(std::is_nothrow_move_constructible_v<Object>);

        [[nodiscard]] StaticWorldMutation prepare(
            const RenderCore::RenderWorld& world, StaticPlanOptions options = {})
        {
            StaticWorldMutation mutation;
            mutation.serial = ++mLastPreparedSerial;
            const StaticWorldPlan plan = buildStaticWorldPlan(world, options);
            mutation.worldEpoch = plan.worldEpoch;
            mutation.worldRevision = plan.worldRevision;
            mutation.simpleMeshInstancesDeferred = plan.simpleMeshInstancesDeferred;
            mutation.dynamicInstancesDeferred = plan.dynamicInstancesDeferred;
            mutation.valid = plan.valid();
            if (!mutation.valid)
                return mutation;

            mutation.orderedInstances.reserve(plan.instances.size());
            mutation.upserts.reserve(plan.instances.size());
            std::unordered_set<std::uint64_t> planned;
            planned.reserve(plan.instances.size());
            for (const StaticInstancePlan& candidate : plan.instances)
            {
                mutation.orderedInstances.push_back(candidate.instance);
                planned.insert(staticInstanceKey(candidate.instance));
                const Resident* resident = find(candidate.instance);
                if (!resident || resident->plan.instanceRevision != candidate.instanceRevision
                    || resident->plan.options != candidate.options || !staticInstancePlanCurrent(world, resident->plan))
                    mutation.upserts.push_back(candidate);
            }
            for (const Resident& resident : mResidents)
            {
                if (!planned.contains(staticInstanceKey(resident.plan.instance)))
                    mutation.removals.push_back(resident.plan.instance);
            }
            return mutation;
        }

        // realizedUpserts must correspond exactly to mutation.upserts. The world
        // is revalidated so an asynchronously realized mutation cannot overwrite
        // a newer resource or mod update.
        [[nodiscard]] StaticWorldCommitResult<Object> commit(const RenderCore::RenderWorld& world,
            const StaticWorldMutation& mutation, std::vector<Object> realizedUpserts)
        {
            StaticWorldCommitResult<Object> result;
            if (!mutation.valid || mutation.serial == 0 || mutation.serial <= mLastCommittedSerial
                || mutation.serial > mLastPreparedSerial || mutation.worldEpoch != world.epoch()
                || mutation.worldRevision != world.revision() || realizedUpserts.size() != mutation.upserts.size())
                return result;
            for (const StaticInstancePlan& plan : mutation.upserts)
            {
                if (!staticInstancePlanCurrent(world, plan))
                    return result;
            }

            std::unordered_map<std::uint64_t, std::size_t> upsertIndices;
            upsertIndices.reserve(mutation.upserts.size());
            for (std::size_t i = 0; i < mutation.upserts.size(); ++i)
            {
                if (!upsertIndices.emplace(staticInstanceKey(mutation.upserts[i].instance), i).second)
                    return result;
            }
            std::unordered_set<std::uint64_t> removalKeys;
            removalKeys.reserve(mutation.removals.size());
            for (const RenderCore::InstanceHandle handle : mutation.removals)
            {
                if (!removalKeys.insert(staticInstanceKey(handle)).second)
                    return result;
            }
            const auto replacedOrRemoved = [&](RenderCore::InstanceHandle handle) {
                const std::uint64_t key = staticInstanceKey(handle);
                return removalKeys.contains(key) || upsertIndices.contains(key);
            };

            // Rebuild in the deterministic planner order. This is observable for
            // legacy no-sort transparency, so replacing one instance must not
            // silently move it to the end of backend traversal order.
            std::vector<Resident> nextResidents;
            nextResidents.reserve(mutation.orderedInstances.size());
            std::unordered_set<std::uint64_t> orderedKeys;
            orderedKeys.reserve(mutation.orderedInstances.size());
            for (const RenderCore::InstanceHandle handle : mutation.orderedInstances)
            {
                const std::uint64_t key = staticInstanceKey(handle);
                if (!orderedKeys.insert(key).second)
                    return result;
                const auto replacement = upsertIndices.find(key);
                if (replacement != upsertIndices.end())
                {
                    const std::size_t index = replacement->second;
                    nextResidents.push_back(
                        Resident{ mutation.upserts[index], std::move(realizedUpserts[index]), std::nullopt });
                    continue;
                }
                const Resident* resident = find(handle);
                if (!resident || replacedOrRemoved(handle))
                    return result;
                nextResidents.push_back(*resident);
            }
            for (const StaticInstancePlan& upsert : mutation.upserts)
            {
                if (!orderedKeys.contains(staticInstanceKey(upsert.instance)))
                    return result;
            }
            for (const Resident& resident : mResidents)
            {
                if (!replacedOrRemoved(resident.plan.instance)
                    && !orderedKeys.contains(staticInstanceKey(resident.plan.instance)))
                    return result;
            }

            std::unordered_map<std::uint64_t, std::size_t> nextIndex;
            nextIndex.reserve(nextResidents.size());
            for (std::size_t i = 0; i < nextResidents.size(); ++i)
            {
                if (!nextIndex.emplace(staticInstanceKey(nextResidents[i].plan.instance), i).second)
                    return result;
            }

            // The complete replacement set exists before live ownership changes.
            // For vsg::ref_ptr this is a cheap reference copy and provides a
            // strong rollback path for allocation or plan-copy failures.
            std::size_t retirementCount = 0;
            std::size_t immediateCount = 0;
            for (const Resident& resident : mResidents)
            {
                if (replacedOrRemoved(resident.plan.instance))
                {
                    resident.lastUseFrame ? ++retirementCount : ++immediateCount;
                }
            }
            mRetirements.reserveAdditional(retirementCount);
            result.immediatelyReleased.reserve(immediateCount);

            for (Resident& resident : mResidents)
            {
                if (!replacedOrRemoved(resident.plan.instance))
                    continue;
                if (resident.lastUseFrame)
                    (void)mRetirements.queue(*resident.lastUseFrame, std::move(resident.object));
                else
                    result.immediatelyReleased.push_back(std::move(resident.object));
            }
            mResidents.swap(nextResidents);
            mResidentIndex.swap(nextIndex);
            mWorldEpoch = mutation.worldEpoch;
            mWorldRevision = mutation.worldRevision;
            mLastCommittedSerial = mutation.serial;
            result.committed = true;
            return result;
        }

        // Call only after the frame was successfully submitted. This records the
        // last GPU use of all objects reachable from the active scene root.
        [[nodiscard]] bool markSubmitted(RenderCore::FrameId frame)
        {
            if (!frame.valid() || (mLastSubmittedFrame && frame <= *mLastSubmittedFrame))
                return false;
            for (Resident& resident : mResidents)
                resident.lastUseFrame = frame;
            mLastSubmittedFrame = frame;
            return true;
        }

        [[nodiscard]] std::vector<Object> collect(RenderCore::FrameId completedFrame)
        {
            return mRetirements.collect(completedFrame);
        }

        [[nodiscard]] std::size_t residentCount() const noexcept { return mResidents.size(); }
        [[nodiscard]] std::size_t pendingRetirementCount() const noexcept { return mRetirements.size(); }
        [[nodiscard]] RenderCore::WorldEpoch worldEpoch() const noexcept { return mWorldEpoch; }
        [[nodiscard]] RenderCore::RenderWorldRevision worldRevision() const noexcept { return mWorldRevision; }

        [[nodiscard]] const Object* residentObject(RenderCore::InstanceHandle handle) const noexcept
        {
            const Resident* resident = find(handle);
            return resident ? &resident->object : nullptr;
        }

        template <class Visitor>
        void forEachResident(Visitor&& visitor) const
        {
            for (const Resident& resident : mResidents)
                visitor(resident.plan, resident.object);
        }

    private:
        struct Resident
        {
            StaticInstancePlan plan;
            Object object;
            std::optional<RenderCore::FrameId> lastUseFrame;
        };

        [[nodiscard]] Resident* find(RenderCore::InstanceHandle handle)
        {
            const auto found = mResidentIndex.find(staticInstanceKey(handle));
            if (found == mResidentIndex.end() || found->second >= mResidents.size()
                || mResidents[found->second].plan.instance != handle)
                return nullptr;
            return &mResidents[found->second];
        }

        [[nodiscard]] const Resident* find(RenderCore::InstanceHandle handle) const
        {
            const auto found = mResidentIndex.find(staticInstanceKey(handle));
            if (found == mResidentIndex.end() || found->second >= mResidents.size()
                || mResidents[found->second].plan.instance != handle)
                return nullptr;
            return &mResidents[found->second];
        }

        std::vector<Resident> mResidents;
        std::unordered_map<std::uint64_t, std::size_t> mResidentIndex;
        FrameRetirementQueue<Object> mRetirements;
        RenderCore::WorldEpoch mWorldEpoch;
        RenderCore::RenderWorldRevision mWorldRevision;
        std::optional<RenderCore::FrameId> mLastSubmittedFrame;
        std::uint64_t mLastPreparedSerial = 0;
        std::uint64_t mLastCommittedSerial = 0;
    };
}

#endif
