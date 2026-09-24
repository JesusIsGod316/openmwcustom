#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDSYNCSTATE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICWORLDSYNCSTATE_H

#include "staticworldplan.hpp"

#include <cstdint>
#include <cstdlib>
#include <unordered_set>
#include <vector>

namespace RenderVsg
{
    // Exact input stamps for the last successfully synchronized static scene.
    // RenderWorld's global revision also advances for actors/lights every frame;
    // it cannot decide whether immutable static geometry needs replanning.
    // This owns no payloads or GPU resources and never hashes vertex/index data.
    class StaticWorldSyncState
    {
    public:
        [[nodiscard]] bool unchanged(const RenderCore::RenderWorld& world, StaticPlanOptions options)
        {
            mPendingRevision = world.staticRevision();
            mPendingSource = &world;
            mFastUnchanged = std::getenv("OPENMW_V4_STATIC_REVISION_GATE") && mSynchronized
                && mSource == &world && mEpoch == world.epoch() && mOptions == options
                && mRevision == mPendingRevision;
            if (mFastUnchanged) return true;
            mPending.clear();
            mModels.clear();
            mPendingEpoch = world.epoch();
            mPendingOptions = options;
            world.forEachInstance([&](RenderCore::InstanceHandle handle, const RenderCore::InstanceRecord& instance) {
                // Match the planner's classification, including unsupported
                // mesh-only records, which must still reach its rejection path.
                if (!(instance.mesh.valid() && !instance.model) && (instance.skeleton || instance.attachment))
                    return;
                append(world, Kind::Instance, handle);
                if (instance.model)
                    appendModel(world, *instance.model);
            });
            world.forEachChunk([&](RenderCore::ChunkHandle handle, const RenderCore::ChunkRecord& chunk) {
                if (!chunk.population)
                    return;
                append(world, Kind::Chunk, handle);
                for (const auto& group : chunk.population->groups)
                    appendModel(world, group.model);
            });
            const bool same = mSynchronized && mEpoch == mPendingEpoch
                && mOptions == mPendingOptions && mCurrent == mPending;
            // An irrelevant asset publication may advance the conservative
            // token without changing this scene. A complete comparison proves
            // that token current without requiring a redundant realization.
            if (same) { mRevision = mPendingRevision; mSource = &world; }
            return same;
        }

        // Only acknowledge after both residency updates and scene publication
        // succeeded (or the full planner confirmed there were no mutations).
        // A failed realization must remain dirty on the next attempt.
        void synchronized() noexcept
        {
            if (mFastUnchanged) return;
            mCurrent.swap(mPending);
            mEpoch = mPendingEpoch;
            mOptions = mPendingOptions;
            mSynchronized = true;
            mRevision = mPendingRevision;
            mSource = mPendingSource;
        }

    private:
        enum class Kind : std::uint8_t { Instance, Chunk, Model, Mesh, Material, Texture };
        struct Stamp
        {
            Kind kind;
            std::uint32_t slot;
            std::uint32_t generation;
            RenderCore::ResourceRevision revision;
            friend bool operator==(const Stamp&, const Stamp&) = default;
        };

        template <class Handle>
        void append(const RenderCore::RenderWorld& world, Kind kind, Handle handle)
        {
            const auto* record = world.get(handle);
            mPending.push_back({ kind, handle.slot(), handle.generation(),
                record ? record->revision : RenderCore::ResourceRevision{} });
        }

        void appendModel(const RenderCore::RenderWorld& world, RenderCore::ModelHandle handle)
        {
            const auto key = (static_cast<std::uint64_t>(handle.generation()) << 32u) | handle.slot();
            if (!mModels.insert(key).second)
                return;
            append(world, Kind::Model, handle);
            const auto* model = world.get(handle);
            if (!model || !model->payload)
                return;
            // Track ALL nodes, not just active draws: hidden/LOD/switch
            // materials participate in the legacy loader's global sort state.
            for (const auto& node : model->payload->nodes)
            {
                if (node.mesh)
                    append(world, Kind::Mesh, *node.mesh);
                for (const auto materialHandle : node.materials)
                {
                    append(world, Kind::Material, materialHandle);
                    const auto* material = world.get(materialHandle);
                    if (!material)
                        continue;
                    for (const auto& binding : material->textures)
                        append(world, Kind::Texture, binding.texture);
                }
            }
        }

        std::vector<Stamp> mCurrent;
        std::vector<Stamp> mPending;
        std::unordered_set<std::uint64_t> mModels;
        RenderCore::WorldEpoch mEpoch;
        RenderCore::WorldEpoch mPendingEpoch;
        StaticPlanOptions mOptions;
        StaticPlanOptions mPendingOptions;
        bool mSynchronized = false;
        bool mFastUnchanged = false;
        RenderCore::RenderWorldRevision mRevision;
        RenderCore::RenderWorldRevision mPendingRevision;
        const RenderCore::RenderWorld* mSource = nullptr;
        const RenderCore::RenderWorld* mPendingSource = nullptr;
    };
}

#endif
