#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDRESIDENCY_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDRESIDENCY_H

#include "framecompletion.hpp"
#include "staticworldplan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace RenderVsg
{
    struct StaticWorldMutation
    {
        std::uint64_t serial = 0;
        RenderCore::WorldEpoch worldEpoch;
        RenderCore::RenderWorldRevision worldRevision;
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

            for (const StaticInstancePlan& candidate : plan.instances)
            {
                const Resident* resident = find(candidate.instance);
                if (!resident || resident->plan.instanceRevision != candidate.instanceRevision
                    || resident->plan.options != candidate.options || !staticInstancePlanCurrent(world, resident->plan))
                    mutation.upserts.push_back(candidate);
            }
            for (const Resident& resident : mResidents)
            {
                const auto found = std::find_if(plan.instances.begin(), plan.instances.end(), [&](const auto& candidate) {
                    return candidate.instance == resident.plan.instance;
                });
                if (found == plan.instances.end())
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

            const auto replacedOrRemoved = [&](RenderCore::InstanceHandle handle) {
                return std::find(mutation.removals.begin(), mutation.removals.end(), handle)
                        != mutation.removals.end()
                    || std::find_if(mutation.upserts.begin(), mutation.upserts.end(),
                           [&](const StaticInstancePlan& plan) { return plan.instance == handle; })
                        != mutation.upserts.end();
            };

            // Build the complete replacement set before touching live ownership.
            // For the production vsg::ref_ptr object type this is a cheap reference
            // copy and gives allocation/plan-copy failures a strong rollback path.
            std::vector<Resident> nextResidents;
            nextResidents.reserve(mResidents.size() + mutation.upserts.size());
            std::size_t retirementCount = 0;
            std::size_t immediateCount = 0;
            for (const Resident& resident : mResidents)
            {
                if (replacedOrRemoved(resident.plan.instance))
                {
                    resident.lastUseFrame ? ++retirementCount : ++immediateCount;
                    continue;
                }
                nextResidents.push_back(resident);
            }
            for (std::size_t i = 0; i < mutation.upserts.size(); ++i)
                nextResidents.push_back(Resident{ mutation.upserts[i], std::move(realizedUpserts[i]), std::nullopt });
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
            const auto found = std::find_if(mResidents.begin(), mResidents.end(),
                [&](const Resident& resident) { return resident.plan.instance == handle; });
            return found == mResidents.end() ? nullptr : &*found;
        }

        [[nodiscard]] const Resident* find(RenderCore::InstanceHandle handle) const
        {
            const auto found = std::find_if(mResidents.begin(), mResidents.end(),
                [&](const Resident& resident) { return resident.plan.instance == handle; });
            return found == mResidents.end() ? nullptr : &*found;
        }

        std::vector<Resident> mResidents;
        FrameRetirementQueue<Object> mRetirements;
        RenderCore::WorldEpoch mWorldEpoch;
        RenderCore::RenderWorldRevision mWorldRevision;
        std::optional<RenderCore::FrameId> mLastSubmittedFrame;
        std::uint64_t mLastPreparedSerial = 0;
        std::uint64_t mLastCommittedSerial = 0;
    };
}

#endif
