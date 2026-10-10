#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICPOPULATIONRESIDENCY_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICPOPULATIONRESIDENCY_H

#include "framecompletion.hpp"
#include "staticworldplan.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

    struct StaticPopulationIdentityHash
    {
        std::size_t operator()(StaticPopulationIdentity value) const noexcept
        {
            std::size_t seed = value.chunk.slot();
            for (const auto part : {value.chunk.generation(), value.model.slot(), value.model.generation()})
                seed ^= part + std::size_t{0x9e3779b9} + (seed << 6) + (seed >> 2);
            return seed;
        }
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
        const RenderCore::RenderWorld* sourceWorld = nullptr;
        std::vector<StaticPopulationIdentity> orderedPopulations;
        std::vector<StaticPopulationPlan> upserts;
        std::vector<StaticPopulationIdentity> removals;
        std::size_t newGroups = 0;
        std::size_t changedChunks = 0;
        std::size_t changedOptions = 0;
        std::size_t staleDependencies = 0;
        std::size_t reusedPlans = 0;
        std::size_t chunksInspected = 0;
        std::size_t cachedAssetPlans = 0;
        bool targetedChunks = false;
        bool incremental = false;
        // Bounded first causes, not an unbounded dump of the population.
        std::vector<PopulationStaleCause> causes;
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
        static_assert(std::is_nothrow_move_constructible_v<StaticPopulationPlan>);

        explicit StaticPopulationResidency(bool indexedLookup = true) : mIndexedLookup(indexedLookup) {}

        [[nodiscard]] StaticPopulationMutation prepare(
            const RenderCore::RenderWorld& world, StaticPlanOptions options = {})
        {
            return prepare(world, buildStaticWorldPlan(world, options));
        }

        // Reconcile persistent residents directly against published group data.
        // No candidate plan (and no copied asset/placement vectors) is made for
        // an unchanged group. Unknown/new dependencies keep the full builder.
        [[nodiscard]] StaticPopulationMutation prepareIncremental(
            const RenderCore::RenderWorld& world, StaticPlanOptions options = {})
        {
            StaticPopulationMutation mutation;
            mutation.serial = ++mLastPreparedSerial;
            mutation.worldEpoch = world.epoch();
            mutation.worldRevision = world.revision();
            mutation.sourceWorld = &world;
            mutation.incremental = true;
            mutation.valid = mutation.worldEpoch.valid() && mutation.worldRevision.valid();
            IdentitySet ordered;
            world.forEachChunk([&](RenderCore::ChunkHandle handle, const RenderCore::ChunkRecord& chunk) {
                ++mutation.chunksInspected;
                if (!chunk.population) return;
                for (const auto& population : chunk.population->groups)
                {
                    const StaticPopulationIdentity identity{handle, population.model};
                    if (!ordered.insert(identity).second) { mutation.valid = false; continue; }
                    mutation.orderedPopulations.push_back(identity);
                    const auto* resident = mWorldEpoch == world.epoch() ? find(identity) : nullptr;
                    PopulationStaleCause cause;
                    const bool optionsCurrent = resident && resident->plan.options == populationPlanOptions(options, population);
                    const bool dependenciesCurrent = resident && staticPopulationPlanCurrent(world, resident->plan,
                        true, mutation.causes.size() < 8 ? &cause : nullptr);
                    mutation.newGroups += !resident;
                    mutation.changedChunks += resident && resident->plan.chunkRevision != chunk.revision;
                    mutation.changedOptions += resident && !optionsCurrent;
                    mutation.staleDependencies += resident && !dependenciesCurrent;
                    if (resident && !dependenciesCurrent && mutation.causes.size() < 8)
                        mutation.causes.push_back(std::move(cause));
                    if (optionsCurrent && dependenciesCurrent) { ++mutation.reusedPlans; continue; }
                    auto plan = buildStaticPopulationPlan(world, handle, population, options);
                    if (!plan) { mutation.valid = false; continue; }
                    mutation.upserts.push_back(std::move(*plan));
                }
            });
            for (const auto& resident : mResidents)
            {
                const auto identity = populationIdentity(resident.plan);
                if (!ordered.contains(identity)) mutation.removals.push_back(identity);
            }
            return mutation;
        }

        // The input is the complete, committed Chunk delta debt from
        // GpuSceneTables, not a camera-visible subset. Asset/options/epoch
        // changes retain full discovery. Untouched chunks do not inspect or
        // reconstruct their model/placement payloads.
        [[nodiscard]] StaticPopulationMutation prepareChangedChunks(const RenderCore::RenderWorld& world,
            StaticPlanOptions options, std::span<const RenderCore::ChunkHandle> changedChunks)
        {
            if (mSourceWorld != &world || mWorldEpoch != world.epoch() || mAssetRevision != world.assetRevision())
                return prepareIncremental(world, options);
            for (const auto& resident : mResidents)
            {
                auto effective = options;
                effective.includeDeformableMeshes = false;
                effective.herbalismHarvested = false;
                effective.dayNightSwitchesEnabled = effective.dayNightSwitchesEnabled
                    && !resident.plan.placements.empty() && (resident.plan.placements.front().semanticFlags
                        & RenderCore::NightDaySwitchCapabilitySemanticFlag) != 0;
                if (effective != resident.plan.options) return prepareIncremental(world, options);
            }
            StaticPopulationMutation mutation;
            mutation.serial = ++mLastPreparedSerial;
            mutation.worldEpoch = world.epoch();
            mutation.worldRevision = world.revision();
            mutation.sourceWorld = &world;
            mutation.incremental = true;
            mutation.targetedChunks = true;
            mutation.valid = mutation.worldEpoch.valid() && mutation.worldRevision.valid();
            const auto chunkKey = [](RenderCore::ChunkHandle handle) {
                return (static_cast<std::uint64_t>(handle.generation()) << 32u) | handle.slot();
            };
            std::unordered_set<std::uint64_t> touched;
            for (const auto handle : changedChunks) touched.insert(chunkKey(handle));
            IdentitySet ordered;
            for (const auto& resident : mResidents)
            {
                if (touched.contains(chunkKey(resident.plan.chunk))) continue;
                const auto identity = populationIdentity(resident.plan);
                ordered.insert(identity);
                mutation.orderedPopulations.push_back(identity);
                ++mutation.reusedPlans;
            }
            std::unordered_set<std::uint64_t> inspected;
            for (const auto handle : changedChunks)
            {
                if (!inspected.insert(chunkKey(handle)).second) continue;
                ++mutation.chunksInspected;
                const auto* chunk = world.get(handle);
                if (!chunk || !chunk->population) continue; // retirement, including an old slot generation
                for (std::size_t groupIndex = 0; groupIndex < chunk->population->groups.size(); ++groupIndex)
                {
                    const auto& population = chunk->population->groups[groupIndex];
                    const StaticPopulationIdentity identity{handle, population.model};
                    if (!ordered.insert(identity).second) { mutation.valid = false; continue; }
                    mutation.orderedPopulations.push_back(identity);
                    const auto* resident = find(identity);
                    const auto effective = populationPlanOptions(options, population);
                    mutation.newGroups += !resident;
                    mutation.changedChunks += resident && resident->plan.chunkRevision != chunk->revision;
                    mutation.changedOptions += resident && resident->plan.options != effective;
                    if (resident && resident->plan.options == effective
                        && staticPopulationPlanCurrent(world, resident->plan, true))
                    { ++mutation.reusedPlans; continue; }
                    constexpr auto capabilityMask = RenderCore::NightDaySwitchCapabilitySemanticFlag
                        | RenderCore::HerbalismSwitchCapabilitySemanticFlag;
                    const bool uniformCapabilities = !population.instances.empty()
                        && std::all_of(population.instances.begin(), population.instances.end(), [&](const auto& row) {
                            return (row.semanticFlags & capabilityMask)
                                    == (population.instances.front().semanticFlags & capabilityMask)
                                && (row.semanticFlags & RenderCore::HerbalismHarvestedSemanticFlag) == 0;
                        });
                    if (resident && resident->plan.options == effective && resident->plan.kind == chunk->kind
                        && uniformCapabilities)
                    {
                        // The authoritative asset revision proves these bindings
                        // unchanged. Copy the existing plan contract, not another
                        // model-node/material discovery pass for a moved object.
                        auto plan = resident->plan;
                        plan.chunkRevision = chunk->revision;
                        plan.groupIndex = groupIndex;
                        plan.placements = population.instances;
                        mutation.upserts.push_back(std::move(plan));
                        ++mutation.cachedAssetPlans;
                    }
                    else
                    {
                        auto plan = buildStaticPopulationPlan(world, handle, population, options);
                        if (!plan) { mutation.valid = false; continue; }
                        mutation.upserts.push_back(std::move(*plan));
                    }
                }
            }
            // RenderWorld enumerates chunk slots in order; preserve authored
            // group order inside each chunk, independent of delta arrival order.
            std::stable_sort(mutation.orderedPopulations.begin(), mutation.orderedPopulations.end(),
                [](const auto& left, const auto& right) { return left.chunk.slot() < right.chunk.slot(); });
            for (const auto& resident : mResidents)
            {
                const auto identity = populationIdentity(resident.plan);
                if (!ordered.contains(identity)) mutation.removals.push_back(identity);
            }
            return mutation;
        }

        [[nodiscard]] StaticPopulationMutation prepare(
            const RenderCore::RenderWorld& world, const StaticWorldPlan& plan, bool coarseChunkInvalidation = false)
        {
            StaticPopulationMutation mutation;
            mutation.serial = ++mLastPreparedSerial;
            mutation.worldEpoch = plan.worldEpoch;
            mutation.worldRevision = plan.worldRevision;
            mutation.sourceWorld = &world;
            mutation.valid = plan.valid();
            if (!mutation.valid)
                return mutation;

            mutation.orderedPopulations.reserve(plan.populations.size());
            mutation.upserts.reserve(plan.populations.size());
            IdentitySet ordered;
            ordered.reserve(plan.populations.size());
            for (const StaticPopulationPlan& candidate : plan.populations)
            {
                const StaticPopulationIdentity identity = populationIdentity(candidate);
                if (!ordered.insert(identity).second)
                {
                    mutation.valid = false;
                    return mutation;
                }
                mutation.orderedPopulations.push_back(identity);
                const Resident* resident = find(identity);
                if (!resident)
                {
                    ++mutation.newGroups;
                    mutation.upserts.push_back(candidate);
                    continue;
                }
                const bool chunkChanged = resident->plan.chunkRevision != candidate.chunkRevision;
                const bool optionsChanged = resident->plan.options != candidate.options;
                PopulationStaleCause cause;
                const bool dependenciesCurrent = staticPopulationPlanCurrent(world, resident->plan,
                    !coarseChunkInvalidation, mutation.causes.size() < 8 ? &cause : nullptr);
                if (!dependenciesCurrent && mutation.causes.size() < 8)
                    mutation.causes.push_back(std::move(cause));
                mutation.changedChunks += chunkChanged;
                mutation.changedOptions += optionsChanged;
                mutation.staleDependencies += !dependenciesCurrent;
                if ((coarseChunkInvalidation && chunkChanged) || optionsChanged || !dependenciesCurrent)
                    mutation.upserts.push_back(candidate);
            }
            for (const Resident& resident : mResidents)
            {
                const StaticPopulationIdentity identity = populationIdentity(resident.plan);
                if (!ordered.contains(identity))
                    mutation.removals.push_back(identity);
            }
            return mutation;
        }

        [[nodiscard]] StaticPopulationCommitResult<Object> commit(const RenderCore::RenderWorld& world,
            const StaticPopulationMutation& mutation, std::vector<Object> realizedUpserts)
        {
            StaticPopulationCommitResult<Object> result;
            if (!mutation.valid || mutation.sourceWorld != &world || mutation.serial == 0 || mutation.serial <= mLastCommittedSerial
                || mutation.serial > mLastPreparedSerial || mutation.worldEpoch != world.epoch()
                || mutation.worldRevision != world.revision() || realizedUpserts.size() != mutation.upserts.size())
                return result;
            for (const StaticPopulationPlan& plan : mutation.upserts)
            {
                if (!staticPopulationPlanCurrent(world, plan))
                    return result;
            }
            IdentitySet ordered;
            ordered.reserve(mutation.orderedPopulations.size());
            for (const auto identity : mutation.orderedPopulations)
            {
                if (!ordered.insert(identity).second)
                    return result;
            }
            IdentityIndex replacements;
            replacements.reserve(mutation.upserts.size());
            for (std::size_t i = 0; i < mutation.upserts.size(); ++i)
            {
                const StaticPopulationIdentity identity = populationIdentity(mutation.upserts[i]);
                if (!ordered.contains(identity) || !replacements.emplace(identity, i).second)
                    return result;
            }
            IdentitySet removals;
            removals.reserve(mutation.removals.size());
            for (const auto identity : mutation.removals)
            {
                if (ordered.contains(identity) || !removals.insert(identity).second || !find(identity))
                    return result;
            }
            // Omitting a submitted resident without declaring its removal
            // would bypass fence retirement when nextResidents is published.
            for (const Resident& resident : mResidents)
            {
                const auto identity = populationIdentity(resident.plan);
                if (!ordered.contains(identity) && !removals.contains(identity))
                    return result;
            }

            std::vector<Resident> nextResidents;
            nextResidents.reserve(mutation.orderedPopulations.size());
            IdentityIndex nextIndex;
            nextIndex.reserve(mutation.orderedPopulations.size());
            // Validate and allocate everything before moving any live owner.
            // A rejected/stale transaction must leave residency and fences intact.
            std::vector<Resident> preparedReplacements;
            if (mutation.incremental)
            {
                preparedReplacements.reserve(mutation.upserts.size());
                for (std::size_t i = 0; i < mutation.upserts.size(); ++i)
                    preparedReplacements.push_back({mutation.upserts[i], std::move(realizedUpserts[i]), std::nullopt});
                for (const auto identity : mutation.orderedPopulations)
                {
                    nextIndex.emplace(identity, nextIndex.size());
                    if (replacements.contains(identity)) continue;
                    const auto* resident = find(identity);
                    if (!resident || removals.contains(identity)
                        || !staticPopulationPlanCurrent(world, resident->plan, true)) return result;
                }
                mRetirements.reserveAdditional(mResidents.size());
                result.immediatelyReleased.reserve(mResidents.size());
            }
            for (const StaticPopulationIdentity identity : mutation.orderedPopulations)
            {
                if (!mutation.incremental) nextIndex.emplace(identity, nextResidents.size());
                const auto replacement = replacements.find(identity);
                if (replacement != replacements.end())
                {
                    const std::size_t index = replacement->second;
                    if (mutation.incremental) nextResidents.push_back(std::move(preparedReplacements[index]));
                    else nextResidents.push_back(
                        Resident{ mutation.upserts[index], std::move(realizedUpserts[index]), std::nullopt });
                    continue;
                }
                Resident* resident = find(identity);
                if (!resident || removals.contains(identity))
                    return result;
                if (!mutation.incremental && !staticPopulationPlanCurrent(world, resident->plan, true))
                    return result;
                if (mutation.incremental) nextResidents.push_back(std::move(*resident));
                else nextResidents.push_back(*resident);
                // The group's exact placements and dependencies were checked.
                // Acknowledge its containing cell's newer revision without
                // changing its packing origin, graph, or last-use fence.
                nextResidents.back().plan.chunkRevision = world.get(identity.chunk)->revision;
                nextResidents.back().plan.sourceWorld = &world;
                nextResidents.back().plan.sourceEpoch = world.epoch();
                nextResidents.back().plan.assetRevision = world.assetRevision();
            }
            std::size_t retirementCount = 0;
            std::size_t immediateCount = 0;
            for (const Resident& resident : mResidents)
            {
                const StaticPopulationIdentity identity = populationIdentity(resident.plan);
                if (removals.contains(identity) || replacements.contains(identity))
                    resident.lastUseFrame ? ++retirementCount : ++immediateCount;
            }
            mRetirements.reserveAdditional(retirementCount);
            result.immediatelyReleased.reserve(immediateCount);
            for (Resident& resident : mResidents)
            {
                const StaticPopulationIdentity identity = populationIdentity(resident.plan);
                if (!removals.contains(identity) && !replacements.contains(identity))
                    continue;
                if (resident.lastUseFrame)
                    static_cast<void>(mRetirements.queue(*resident.lastUseFrame, std::move(resident.object)));
                else
                    result.immediatelyReleased.push_back(std::move(resident.object));
            }

            mResidents.swap(nextResidents);
            mResidentIndex.swap(nextIndex);
            mWorldEpoch = mutation.worldEpoch;
            mWorldRevision = mutation.worldRevision;
            mSourceWorld = &world;
            mAssetRevision = world.assetRevision();
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
        [[nodiscard]] const StaticPopulationPlan* residentPlan(RenderCore::WorldEpoch epoch,
            StaticPopulationIdentity identity) const noexcept
        {
            if (epoch != mWorldEpoch) return nullptr;
            const Resident* resident = find(identity);
            return resident ? &resident->plan : nullptr;
        }

        [[nodiscard]] std::size_t residentCount() const noexcept { return mResidents.size(); }
        [[nodiscard]] std::size_t pendingRetirementCount() const noexcept { return mRetirements.size(); }

        template <class Visitor>
        void forEachResident(Visitor&& visitor)
        {
            for (Resident& resident : mResidents)
                visitor(resident.plan, resident.object);
        }

    private:
        using IdentitySet = std::unordered_set<StaticPopulationIdentity, StaticPopulationIdentityHash>;
        using IdentityIndex = std::unordered_map<StaticPopulationIdentity, std::size_t, StaticPopulationIdentityHash>;
        struct Resident
        {
            StaticPopulationPlan plan;
            Object object;
            std::optional<RenderCore::FrameId> lastUseFrame;
        };

        [[nodiscard]] Resident* find(StaticPopulationIdentity identity) noexcept
        {
            if (mIndexedLookup)
            {
                const auto found = mResidentIndex.find(identity);
                return found == mResidentIndex.end() ? nullptr : &mResidents[found->second];
            }
            const auto found = std::find_if(mResidents.begin(), mResidents.end(),
                [&](const Resident& resident) { return populationIdentity(resident.plan) == identity; });
            return found == mResidents.end() ? nullptr : std::addressof(*found);
        }

        [[nodiscard]] const Resident* find(StaticPopulationIdentity identity) const noexcept
        {
            if (mIndexedLookup)
            {
                const auto found = mResidentIndex.find(identity);
                return found == mResidentIndex.end() ? nullptr : &mResidents[found->second];
            }
            const auto found = std::find_if(mResidents.begin(), mResidents.end(),
                [&](const Resident& resident) { return populationIdentity(resident.plan) == identity; });
            return found == mResidents.end() ? nullptr : std::addressof(*found);
        }

        std::vector<Resident> mResidents;
        IdentityIndex mResidentIndex;
        bool mIndexedLookup;
        FrameRetirementQueue<Object> mRetirements;
        RenderCore::WorldEpoch mWorldEpoch;
        RenderCore::RenderWorldRevision mWorldRevision;
        const RenderCore::RenderWorld* mSourceWorld = nullptr;
        RenderCore::RenderWorldRevision mAssetRevision;
        std::optional<RenderCore::FrameId> mLastSubmittedFrame;
        std::uint64_t mLastPreparedSerial = 0;
        std::uint64_t mLastCommittedSerial = 0;
    };
}

#endif
