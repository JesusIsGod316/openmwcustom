#ifndef OPENMW_COMPONENTS_RENDERCORE_UPDATEBATCH_H
#define OPENMW_COMPONENTS_RENDERCORE_UPDATEBATCH_H

#include "renderworld.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace RenderCore
{
    struct CreateMesh { MeshHandle handle; MeshRecord record; };
    struct UpdateMesh { MeshHandle handle; MeshRecord record; };
    struct RetireMesh { MeshHandle handle; };
    struct CreateMaterial { MaterialHandle handle; MaterialRecord record; };
    struct UpdateMaterial { MaterialHandle handle; MaterialRecord record; };
    struct RetireMaterial { MaterialHandle handle; };
    struct CreateTexture { TextureHandle handle; TextureRecord record; };
    struct UpdateTexture { TextureHandle handle; TextureRecord record; };
    struct RetireTexture { TextureHandle handle; };
    struct CreateSkeleton { SkeletonHandle handle; SkeletonRecord record; };
    struct UpdateSkeleton { SkeletonHandle handle; SkeletonRecord record; };
    struct RetireSkeleton { SkeletonHandle handle; };
    struct CreateInstance { InstanceHandle handle; InstanceRecord record; };
    struct UpdateInstance { InstanceHandle handle; InstanceRecord record; };
    struct ReparentInstance { InstanceHandle handle; std::optional<ChunkHandle> chunk; };
    struct RetireInstance { InstanceHandle handle; };
    struct CreateChunk { ChunkHandle handle; ChunkRecord record; };
    struct UpdateChunk { ChunkHandle handle; ChunkRecord record; };
    struct RetireChunk { ChunkHandle handle; };
    struct CreateLight { LightHandle handle; LightRecord record; };
    struct UpdateLight { LightHandle handle; LightRecord record; };
    struct RetireLight { LightHandle handle; };

    using RenderWorldUpdateOperation = std::variant<CreateMesh, UpdateMesh, RetireMesh, CreateMaterial, UpdateMaterial,
        RetireMaterial, CreateTexture, UpdateTexture, RetireTexture, CreateSkeleton, UpdateSkeleton, RetireSkeleton,
        CreateInstance, UpdateInstance, ReparentInstance, RetireInstance, CreateChunk, UpdateChunk, RetireChunk,
        CreateLight, UpdateLight, RetireLight>;

    class RenderWorldUpdateBatch final
    {
    public:
        RenderWorldUpdateBatch(WorldEpoch epoch, UpdateSequence sequence, std::string sourceIdentity = {})
            : mEpoch(epoch)
            , mSequence(sequence)
            , mSourceIdentity(std::move(sourceIdentity))
        {
        }

        bool add(RenderWorldUpdateOperation operation)
        {
            if (mSealed)
                return false;
            mOperations.push_back(std::move(operation));
            return true;
        }

        bool seal() noexcept
        {
            if (mSealed || !mEpoch.valid() || !mSequence.valid())
                return false;
            mSealed = true;
            return true;
        }

        [[nodiscard]] bool sealed() const noexcept { return mSealed; }
        [[nodiscard]] WorldEpoch epoch() const noexcept { return mEpoch; }
        [[nodiscard]] UpdateSequence sequence() const noexcept { return mSequence; }
        [[nodiscard]] const std::string& sourceIdentity() const noexcept { return mSourceIdentity; }
        [[nodiscard]] const std::vector<RenderWorldUpdateOperation>& operations() const noexcept { return mOperations; }

    private:
        WorldEpoch mEpoch;
        UpdateSequence mSequence;
        std::string mSourceIdentity;
        std::vector<RenderWorldUpdateOperation> mOperations;
        bool mSealed = false;
    };

    enum class PublishStatus : std::uint8_t
    {
        Applied,
        UnsealedBatch,
        StaleEpoch,
        OutOfOrder,
        OperationRejected,
        InvariantFailure,
        Exception,
    };

    class RenderWorldPublisher final
    {
    public:
        explicit RenderWorldPublisher(RenderWorld& world)
            : mWorld(world)
            , mObservedEpoch(world.epoch())
        {
        }

        [[nodiscard]] PublishStatus apply(const RenderWorldUpdateBatch& batch) noexcept
        {
            if (!batch.sealed())
                return PublishStatus::UnsealedBatch;

            if (mObservedEpoch != mWorld.epoch())
            {
                mObservedEpoch = mWorld.epoch();
                mLastSequence = UpdateSequence{};
            }

            if (batch.epoch() != mWorld.epoch())
                return PublishStatus::StaleEpoch;

            const UpdateSequence expected = expectedSequence();
            if (!expected.valid() || batch.sequence() != expected)
                return PublishStatus::OutOfOrder;

            try
            {
                // CP3A deliberately prioritizes all-or-nothing semantic publication.
                // Payloads are immutable/shared where large; this shadow copy is a
                // correctness baseline and may be replaced by an undo-log/COW commit
                // without changing the batch contract before high-volume CP3C use.
                RenderWorld candidate = mWorld;
                for (const RenderWorldUpdateOperation& operation : batch.operations())
                {
                    if (!std::visit([&candidate](const auto& value) { return applyOperation(candidate, value); }, operation))
                        return PublishStatus::OperationRejected;
                }
                if (!candidate.valid())
                    return PublishStatus::InvariantFailure;

                mWorld = std::move(candidate);
                mLastSequence = batch.sequence();
                return PublishStatus::Applied;
            }
            catch (...)
            {
                return PublishStatus::Exception;
            }
        }

        [[nodiscard]] UpdateSequence lastSequence() const noexcept { return mLastSequence; }
        [[nodiscard]] WorldEpoch observedEpoch() const noexcept { return mObservedEpoch; }

    private:
        [[nodiscard]] UpdateSequence expectedSequence() const noexcept
        {
            if (!mLastSequence.valid())
                return InitialUpdateSequence;
            const auto next = advanceMonotonic(mLastSequence);
            return next ? *next : UpdateSequence{};
        }

        static bool applyOperation(RenderWorld& world, const CreateMesh& value) { return world.commit(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const UpdateMesh& value) { return world.update(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const RetireMesh& value) { return world.retire(value.handle); }
        static bool applyOperation(RenderWorld& world, const CreateMaterial& value) { return world.commit(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const UpdateMaterial& value) { return world.update(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const RetireMaterial& value) { return world.retire(value.handle); }
        static bool applyOperation(RenderWorld& world, const CreateTexture& value) { return world.commit(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const UpdateTexture& value) { return world.update(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const RetireTexture& value) { return world.retire(value.handle); }
        static bool applyOperation(RenderWorld& world, const CreateSkeleton& value) { return world.commit(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const UpdateSkeleton& value) { return world.update(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const RetireSkeleton& value) { return world.retire(value.handle); }
        static bool applyOperation(RenderWorld& world, const CreateInstance& value) { return world.commit(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const UpdateInstance& value) { return world.update(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const ReparentInstance& value) { return world.reparentInstance(value.handle, value.chunk); }
        static bool applyOperation(RenderWorld& world, const RetireInstance& value) { return world.retire(value.handle); }
        static bool applyOperation(RenderWorld& world, const CreateChunk& value) { return world.commit(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const UpdateChunk& value) { return world.update(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const RetireChunk& value) { return world.retire(value.handle); }
        static bool applyOperation(RenderWorld& world, const CreateLight& value) { return world.commit(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const UpdateLight& value) { return world.update(value.handle, value.record); }
        static bool applyOperation(RenderWorld& world, const RetireLight& value) { return world.retire(value.handle); }

        RenderWorld& mWorld;
        WorldEpoch mObservedEpoch;
        UpdateSequence mLastSequence;
    };
}

#endif
