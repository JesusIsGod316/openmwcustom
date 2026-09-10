#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICPOPULATIONRESIDENCY_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICPOPULATIONRESIDENCY_H

#include "framecompletion.hpp"
#include "staticworldplan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace RenderVsg
{
    struct StaticPopulationIdentity
    {
        RenderCore::ChunkHandle chunk;
        RenderCore::ModelHandle model;

        friend bool operator==(const StaticPopulationIdentity&, const StaticPopulationIdentity&) = default;
    };

    [[nodiscard]] inline StaticPopulationIdentity populationIdentity(const StaticPopulationPlan& plan) noexcept
    {
        return { plan.chunk, plan.model };
    }

    struct StaticPopulationMutation
    {
        std::uint64_t serial = 0;
        RenderCore::WorldEpoch worldEpoch;
        RenderCore::RenderWorldRevision worldRevision;
        std::vector<StaticPopulationIdentity> orderedPopulations;
        std::vector<StaticPopulationPlan> upserts;
        std::vector<StaticPopulationIdentity> removals;
        bool valid = false;
    };

    template <class Object>
    struct StaticPopulationCommitResult
    {
        bool committed = false;
        std::vector<Object> immediatelyReleased;
    };

    // Persistent chunk/model-group ownership for high-volume exterior
    // placements. Replacements are compiled in isolation and become visible
    // together at a frame boundary; objects used by submitted frames remain
    // alive until completion is observed.
    template <class Object>
    class StaticPopulationResidency
    {
    public:
        static_assert(std::is_copy_constructible_v<Object>);
        static_assert(std::is_nothrow_move_constructible_v<Object>);

        [[nodiscard]] StaticPopulationMutation prepare(
            const RenderCore::RenderWorld& world, StaticPlanOptions options = {})
        {
            return prepare(world, buildStaticWorldPlan(world, options));
        }

        [[nodiscard]] StaticPopulationMutation prepare(
            const RenderCore::RenderWorld& world, const StaticWorldPlan& plan)
        {
            StaticPopulationMutation mutation;
            mutation.serial = ++mLastPreparedSerial;
            mutation.worldEpoch = plan.worldEpoch;
            mutation.worldRevision = plan.worldRevision;
            mutation.valid = plan.valid();
            if (!mutation.valid)
                return mutation;

            mutation.orderedPopulations.reserve(plan.populations.size());
            mutation.upserts.reserve(plan.populations.size());
            for (const StaticPopulationPlan& candidate : plan.populations)
            {
                const StaticPopulationIdentity identity = populationIdentity(candidate);
                if (contains(mutation.orderedPopulations, identity))
                {
                    mutation.valid = false;
                    return mutation;
                }
                mutation.orderedPopulations.push_back(identity);
                const Resident* resident = find(identity);
                if (!resident || resident->plan.chunkRevision != candidate.chunkRevision
                    || resident->plan.options != candidate.options || !staticPopulationPlanCurrent(world, resident->plan))
                    mutation.upserts.push_back(candidate);
            }
            for (const Resident& resident : mResidents)
            {
                const StaticPopulationIdentity identity = populationIdentity(resident.plan);
                if (!contains(mutation.orderedPopulations, identity))
                    mutation.removals.push_back(identity);
            }
            return mutation;
        }

        [[nodiscard]] StaticPopulationCommitResult<Object> commit(const RenderCore::RenderWorld& world,
            const StaticPopulationMutation& mutation, std::vector<Object> realizedUpserts)
        {
            StaticPopulationCommitResult<Object> result;
            if (!mutation.valid || mutation.serial == 0 || mutation.serial <= mLastCommittedSerial
                || mutation.serial > mLastPreparedSerial || mutation.worldEpoch != world.epoch()
                || mutation.worldRevision != world.revision() || realizedUpserts.size() != mutation.upserts.size())
                return result;
            for (const StaticPopulationPlan& plan : mutation.upserts)
            {
                if (!staticPopulationPlanCurrent(world, plan))
                    return result;
            }
            for (std::size_t i = 0; i < mutation.orderedPopulations.size(); ++i)
            {
                if (std::find(mutation.orderedPopulations.begin() + i + 1, mutation.orderedPopulations.end(),
                        mutation.orderedPopulations[i])
                    != mutation.orderedPopulations.end())
                    return result;
            }
            for (std::size_t i = 0; i < mutation.upserts.size(); ++i)
            {
                const StaticPopulationIdentity identity = populationIdentity(mutation.upserts[i]);
                if (!contains(mutation.orderedPopulations, identity)
                    || std::ranges::any_of(mutation.upserts.begin() + i + 1, mutation.upserts.end(),
                        [&](const StaticPopulationPlan& plan) { return populationIdentity(plan) == identity; }))
                    return result;
            }
            for (std::size_t i = 0; i < mutation.removals.size(); ++i)
            {
                if (contains(mutation.orderedPopulations, mutation.removals[i])
                    || std::find(mutation.removals.begin() + i + 1, mutation.removals.end(), mutation.removals[i])
                        != mutation.removals.end())
                    return result;
            }

            std::vector<Resident> nextResidents;
            nextResidents.reserve(mutation.orderedPopulations.size());
            for (const StaticPopulationIdentity identity : mutation.orderedPopulations)
            {
                const auto replacement = std::find_if(mutation.upserts.begin(), mutation.upserts.end(),
                    [&](const StaticPopulationPlan& plan) { return populationIdentity(plan) == identity; });
                if (replacement != mutation.upserts.end())
                {
                    const std::size_t index = static_cast<std::size_t>(replacement - mutation.upserts.begin());
                    nextResidents.push_back(
                        Resident{ *replacement, std::move(realizedUpserts[index]), std::nullopt });
                    continue;
                }
                const Resident* resident = find(identity);
                if (!resident || contains(mutation.removals, identity))
                    return result;
                nextResidents.push_back(*resident);
            }
            for (const StaticPopulationPlan& plan : mutation.upserts)
            {
                if (!contains(mutation.orderedPopulations, populationIdentity(plan)))
                    return result;
            }

            std::size_t retirementCount = 0;
            std::size_t immediateCount = 0;
            for (const Resident& resident : mResidents)
            {
                const StaticPopulationIdentity identity = populationIdentity(resident.plan);
                if (contains(mutation.removals, identity) || containsUpsert(mutation.upserts, identity))
                    resident.lastUseFrame ? ++retirementCount : ++immediateCount;
            }
            mRetirements.reserveAdditional(retirementCount);
            result.immediatelyReleased.reserve(immediateCount);
            for (Resident& resident : mResidents)
            {
                const StaticPopulationIdentity identity = populationIdentity(resident.plan);
                if (!contains(mutation.removals, identity) && !containsUpsert(mutation.upserts, identity))
                    continue;
                if (resident.lastUseFrame)
                    static_cast<void>(mRetirements.queue(*resident.lastUseFrame, std::move(resident.object)));
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

        [[nodiscard]] const Object* residentObject(StaticPopulationIdentity identity) const noexcept
        {
            const Resident* resident = find(identity);
            return resident ? &resident->object : nullptr;
        }

        [[nodiscard]] std::size_t residentCount() const noexcept { return mResidents.size(); }
        [[nodiscard]] std::size_t pendingRetirementCount() const noexcept { return mRetirements.size(); }

    private:
        struct Resident
        {
            StaticPopulationPlan plan;
            Object object;
            std::optional<RenderCore::FrameId> lastUseFrame;
        };

        template <class Value>
        [[nodiscard]] static bool contains(
            const std::vector<Value>& values, const StaticPopulationIdentity& identity) noexcept
        {
            return std::find(values.begin(), values.end(), identity) != values.end();
        }

        [[nodiscard]] static bool containsUpsert(
            const std::vector<StaticPopulationPlan>& values, StaticPopulationIdentity identity) noexcept
        {
            return std::ranges::any_of(
                values, [&](const StaticPopulationPlan& plan) { return populationIdentity(plan) == identity; });
        }

        [[nodiscard]] Resident* find(StaticPopulationIdentity identity) noexcept
        {
            const auto found = std::find_if(mResidents.begin(), mResidents.end(),
                [&](const Resident& resident) { return populationIdentity(resident.plan) == identity; });
            return found == mResidents.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] const Resident* find(StaticPopulationIdentity identity) const noexcept
        {
            const auto found = std::find_if(mResidents.begin(), mResidents.end(),
                [&](const Resident& resident) { return populationIdentity(resident.plan) == identity; });
            return found == mResidents.end() ? nullptr : std::addressof(*found);
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
