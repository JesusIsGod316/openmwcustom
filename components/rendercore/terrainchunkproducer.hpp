#ifndef OPENMW_COMPONENTS_RENDERCORE_TERRAINCHUNKPRODUCER_H
#define OPENMW_COMPONENTS_RENDERCORE_TERRAINCHUNKPRODUCER_H

#include "updatebatch.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace RenderCore
{
    struct TerrainChunkSource
    {
        std::string identity;
        std::string worldspaceIdentity;
        std::int32_t gridX = 0;
        std::int32_t gridY = 0;
        std::uint32_t lodLevel = 0;
        std::uint8_t stitchMask = 0;
        WorldTransform transform;
        AxisAlignedBounds localBounds;
        std::shared_ptr<const MeshPayload> mesh;
        MaterialRecord material;
    };

    enum class TerrainChunkPublishStatus : std::uint8_t
    {
        Applied,
        AlreadyPresent,
        InvalidSource,
        ReservationFailed,
        SequenceExhausted,
        BatchBuildFailed,
        PublishRejected,
    };

    // CP4A owns one current LAND surface while establishing the permanent
    // neutral publication seam. CP4B can widen this to an active/predicted set
    // without changing chunk/resource identities or backend ownership.
    class TerrainChunkProducer final
    {
    public:
        TerrainChunkProducer(RenderWorld& world, RenderWorldPublisher& publisher)
            : mWorld(world)
            , mPublisher(publisher)
            , mObservedEpoch(world.epoch())
        {
        }

        [[nodiscard]] TerrainChunkPublishStatus synchronize(const std::optional<TerrainChunkSource>& source)
        {
            synchronizeEpoch();
            if (source && !valid(*source))
                return TerrainChunkPublishStatus::InvalidSource;
            if (source && mActive && source->identity == mActive->identity)
                return live(*mActive) ? TerrainChunkPublishStatus::AlreadyPresent
                                      : TerrainChunkPublishStatus::PublishRejected;
            if (!source && !mActive)
                return TerrainChunkPublishStatus::AlreadyPresent;

            std::optional<Binding> replacement;
            if (source)
            {
                replacement = reserve(source->identity);
                if (!replacement)
                    return TerrainChunkPublishStatus::ReservationFailed;
            }

            const UpdateSequence sequence = mPublisher.nextSequence();
            if (!sequence.valid())
            {
                cancel(replacement);
                return TerrainChunkPublishStatus::SequenceExhausted;
            }
            RenderWorldUpdateBatch batch(mWorld.epoch(), sequence, source ? source->identity : mActive->identity);
            bool built = true;
            if (mActive)
                built = addRetirement(batch, *mActive);
            if (built && source)
                built = addCreation(batch, *source, *replacement);
            built = built && batch.seal();
            if (!built)
            {
                cancel(replacement);
                return TerrainChunkPublishStatus::BatchBuildFailed;
            }

            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
            {
                cancel(replacement);
                return TerrainChunkPublishStatus::PublishRejected;
            }
            mActive = std::move(replacement);
            return TerrainChunkPublishStatus::Applied;
        }

        [[nodiscard]] std::string_view activeIdentity() const noexcept
        {
            return mActive ? std::string_view(mActive->identity) : std::string_view{};
        }

        [[nodiscard]] bool contains(std::string_view identity)
        {
            synchronizeEpoch();
            return mActive && identity == mActive->identity && live(*mActive);
        }

    private:
        struct Binding
        {
            std::string identity;
            MeshHandle mesh;
            MaterialHandle material;
            ModelHandle model;
            ChunkHandle chunk;
            InstanceHandle instance;
        };

        [[nodiscard]] static bool valid(const TerrainChunkSource& source) noexcept
        {
            return !source.identity.empty() && !source.worldspaceIdentity.empty() && source.mesh
                && validMeshPayload(*source.mesh) && !source.mesh->positions.empty() && !source.mesh->surfaces.empty()
                && semantic_detail::finite(source.localBounds.minimum)
                && semantic_detail::finite(source.localBounds.maximum)
                && semantic_detail::finite(source.transform.translation)
                && semantic_detail::finite(source.transform.rotation)
                && semantic_detail::finite(source.transform.scale);
        }

        [[nodiscard]] std::optional<Binding> reserve(std::string identity)
        {
            Binding result;
            result.identity = std::move(identity);
            const auto mesh = mWorld.reserveMesh();
            const auto material = mWorld.reserveMaterial();
            const auto model = mWorld.reserveModel();
            const auto chunk = mWorld.reserveChunk();
            const auto instance = mWorld.reserveInstance();
            if (!mesh || !material || !model || !chunk || !instance)
            {
                if (mesh)
                    mWorld.cancel(*mesh);
                if (material)
                    mWorld.cancel(*material);
                if (model)
                    mWorld.cancel(*model);
                if (chunk)
                    mWorld.cancel(*chunk);
                if (instance)
                    mWorld.cancel(*instance);
                return std::nullopt;
            }
            result.mesh = *mesh;
            result.material = *material;
            result.model = *model;
            result.chunk = *chunk;
            result.instance = *instance;
            return result;
        }

        void cancel(const std::optional<Binding>& binding) noexcept
        {
            if (!binding)
                return;
            mWorld.cancel(binding->instance);
            mWorld.cancel(binding->chunk);
            mWorld.cancel(binding->model);
            mWorld.cancel(binding->material);
            mWorld.cancel(binding->mesh);
        }

        [[nodiscard]] bool addCreation(
            RenderWorldUpdateBatch& batch, const TerrainChunkSource& source, const Binding& binding) const
        {
            MeshRecord mesh;
            mesh.sourceIdentity = source.identity + ":mesh";
            mesh.bounds = source.localBounds;
            mesh.surfaceCount = static_cast<std::uint32_t>(source.mesh->surfaces.size());
            mesh.payload = source.mesh;

            MaterialRecord material = source.material;
            material.sourceIdentity = source.identity + ":material";

            auto modelPayload = std::make_shared<ModelPayload>();
            ModelNodeRecord geometry;
            geometry.name = "terrain";
            geometry.sourceRecordId = 0;
            geometry.kind = ModelNodeKind::Geometry;
            geometry.mesh = binding.mesh;
            geometry.materials.push_back(binding.material);
            modelPayload->nodes.push_back(std::move(geometry));
            modelPayload->roots.push_back(ModelNodeIndex{ 0 });
            ModelRecord model;
            model.sourceIdentity = source.identity + ":model";
            model.bounds = source.localBounds;
            model.payload = std::move(modelPayload);

            ChunkRecord chunk;
            chunk.producerIdentity = source.identity;
            chunk.worldspaceIdentity = source.worldspaceIdentity;
            chunk.bounds.minimum = source.localBounds.minimum + glm::vec3(source.transform.translation);
            chunk.bounds.maximum = source.localBounds.maximum + glm::vec3(source.transform.translation);
            chunk.semanticFlags
                = semanticFlag(InstanceSemanticFlag::OrdinaryWorld) | semanticFlag(InstanceSemanticFlag::Terrain);
            chunk.kind = ChunkRecord::Kind::Terrain;
            chunk.gridX = source.gridX;
            chunk.gridY = source.gridY;
            chunk.lodLevel = source.lodLevel;
            chunk.stitchMask = source.stitchMask;

            InstanceRecord instance;
            instance.chunk = binding.chunk;
            instance.model = binding.model;
            instance.transform = source.transform;
            instance.localBounds = source.localBounds;
            instance.semanticFlags = chunk.semanticFlags;

            return batch.add(CreateMesh{ binding.mesh, std::move(mesh) })
                && batch.add(CreateMaterial{ binding.material, std::move(material) })
                && batch.add(CreateModel{ binding.model, std::move(model) })
                && batch.add(CreateChunk{ binding.chunk, std::move(chunk) })
                && batch.add(CreateInstance{ binding.instance, std::move(instance) });
        }

        [[nodiscard]] static bool addRetirement(RenderWorldUpdateBatch& batch, const Binding& binding)
        {
            return batch.add(RetireInstance{ binding.instance }) && batch.add(RetireChunk{ binding.chunk })
                && batch.add(RetireModel{ binding.model }) && batch.add(RetireMaterial{ binding.material })
                && batch.add(RetireMesh{ binding.mesh });
        }

        [[nodiscard]] bool live(const Binding& binding) const noexcept
        {
            return mWorld.get(binding.mesh) && mWorld.get(binding.material) && mWorld.get(binding.model)
                && mWorld.get(binding.chunk) && mWorld.get(binding.instance);
        }

        void synchronizeEpoch()
        {
            if (mObservedEpoch == mWorld.epoch())
                return;
            mObservedEpoch = mWorld.epoch();
            mActive.reset();
        }

        RenderWorld& mWorld;
        RenderWorldPublisher& mPublisher;
        WorldEpoch mObservedEpoch;
        std::optional<Binding> mActive;
    };
}

#endif
