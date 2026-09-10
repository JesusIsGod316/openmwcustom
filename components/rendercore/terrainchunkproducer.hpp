#ifndef OPENMW_COMPONENTS_RENDERCORE_TERRAINCHUNKPRODUCER_H
#define OPENMW_COMPONENTS_RENDERCORE_TERRAINCHUNKPRODUCER_H

#include "updatebatch.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

    [[nodiscard]] inline bool validTerrainChunkSource(const TerrainChunkSource& source) noexcept
    {
        return !source.identity.empty() && !source.worldspaceIdentity.empty() && source.mesh
            && validMeshPayload(*source.mesh) && !source.mesh->positions.empty() && !source.mesh->surfaces.empty()
            && semantic_detail::finite(source.localBounds.minimum)
            && semantic_detail::finite(source.localBounds.maximum)
            && semantic_detail::finite(source.transform.translation)
            && semantic_detail::finite(source.transform.rotation) && semantic_detail::finite(source.transform.scale)
            && source.localBounds.minimum.x <= source.localBounds.maximum.x
            && source.localBounds.minimum.y <= source.localBounds.maximum.y
            && source.localBounds.minimum.z <= source.localBounds.maximum.z;
    }

    enum class TerrainChunkPublishStatus : std::uint8_t
    {
        Applied,
        PartiallyApplied,
        AlreadyPresent,
        InvalidSource,
        ReservationFailed,
        SequenceExhausted,
        BatchBuildFailed,
        PublishRejected,
    };

    struct TerrainPublicationLimits
    {
        std::size_t maxNewChunks = std::numeric_limits<std::size_t>::max();
        std::uint64_t maxNewMeshBytes = std::numeric_limits<std::uint64_t>::max();
    };

    [[nodiscard]] inline std::optional<std::uint64_t> terrainMeshPayloadBytes(const MeshPayload& payload) noexcept
    {
        std::uint64_t total = 0;
        const auto add = [&](std::size_t count, std::size_t size) {
            const std::uint64_t bytes = static_cast<std::uint64_t>(count) * size;
            if (bytes > std::numeric_limits<std::uint64_t>::max() - total)
                return false;
            total += bytes;
            return true;
        };
        if (!add(payload.positions.size(), sizeof(glm::vec3)) || !add(payload.normals.size(), sizeof(glm::vec3))
            || !add(payload.tangents.size(), sizeof(glm::vec3)) || !add(payload.bitangents.size(), sizeof(glm::vec3))
            || !add(payload.colors.size(), sizeof(glm::vec4)) || !add(payload.indices.size(), sizeof(std::uint32_t))
            || !add(payload.surfaces.size(), sizeof(MeshSurface)))
            return std::nullopt;
        for (const auto& coordinates : payload.texCoordSets)
            if (!add(coordinates.size(), sizeof(glm::vec2)))
                return std::nullopt;
        return total;
    }

    // CP4A uses the single-source wrapper for one current LAND surface. The
    // ordered set path is the CP4B ownership seam for active and predicted
    // chunks without changing resource identities or backend ownership.
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
            if (!source)
                return synchronize(std::span<const TerrainChunkSource>{});
            return synchronize(std::span<const TerrainChunkSource>(&*source, 1));
        }

        [[nodiscard]] TerrainChunkPublishStatus synchronize(
            std::span<const TerrainChunkSource> sources, TerrainPublicationLimits limits = {})
        {
            synchronizeEpoch();
            if (!validSet(sources))
                return TerrainChunkPublishStatus::InvalidSource;

            bool unchanged = sources.size() == mActive.size();
            for (const TerrainChunkSource& source : sources)
            {
                const auto active = mActive.find(source.identity);
                if (active != mActive.end() && !live(active->second))
                    return TerrainChunkPublishStatus::PublishRejected;
                unchanged = unchanged && active != mActive.end();
            }
            if (unchanged)
                return TerrainChunkPublishStatus::AlreadyPresent;

            std::map<std::string, Binding, std::less<>> additions;
            std::size_t admittedChunks = 0;
            std::uint64_t admittedBytes = 0;
            for (const TerrainChunkSource& source : sources)
            {
                if (mActive.contains(source.identity))
                    continue;
                const std::optional<std::uint64_t> bytes = terrainMeshPayloadBytes(*source.mesh);
                if (!bytes || admittedChunks >= limits.maxNewChunks || *bytes > limits.maxNewMeshBytes - admittedBytes)
                    continue;
                std::optional<Binding> binding = reserve(source);
                if (!binding)
                {
                    cancel(additions);
                    return TerrainChunkPublishStatus::ReservationFailed;
                }
                additions.emplace(source.identity, std::move(*binding));
                ++admittedChunks;
                admittedBytes += *bytes;
            }

            const UpdateSequence sequence = mPublisher.nextSequence();
            if (!sequence.valid())
            {
                cancel(additions);
                return TerrainChunkPublishStatus::SequenceExhausted;
            }
            RenderWorldUpdateBatch batch(mWorld.epoch(), sequence, "terrain:active-set");
            bool built = true;
            std::vector<std::string> retiredIdentities;
            const bool targetCompleteAfterBatch = std::ranges::all_of(sources, [&](const TerrainChunkSource& source) {
                return mActive.contains(source.identity) || additions.contains(source.identity);
            });
            for (const auto& [identity, binding] : mActive)
            {
                const bool retained = std::ranges::any_of(
                    sources, [&](const TerrainChunkSource& source) { return source.identity == identity; });
                const bool replaced = std::ranges::any_of(sources, [&](const TerrainChunkSource& source) {
                    return additions.contains(source.identity) && source.gridX == binding.gridX
                        && source.gridY == binding.gridY;
                });
                if (!retained && (targetCompleteAfterBatch || replaced))
                {
                    if (built && addRetirement(batch, binding))
                        retiredIdentities.push_back(identity);
                    else
                        built = false;
                }
            }
            for (const TerrainChunkSource& source : sources)
            {
                const auto addition = additions.find(source.identity);
                if (addition != additions.end())
                    built = built && addCreation(batch, source, addition->second);
            }
            built = built && batch.seal();
            if (!built)
            {
                cancel(additions);
                return TerrainChunkPublishStatus::BatchBuildFailed;
            }

            const PublishStatus published = mPublisher.apply(batch);
            if (published != PublishStatus::Applied)
            {
                cancel(additions);
                return TerrainChunkPublishStatus::PublishRejected;
            }
            std::erase_if(mActive, [&](const auto& entry) {
                return std::ranges::find(retiredIdentities, entry.first) != retiredIdentities.end();
            });
            mActive.merge(additions);
            const bool complete = mActive.size() == sources.size()
                && std::ranges::all_of(
                    sources, [&](const TerrainChunkSource& source) { return mActive.contains(source.identity); });
            return complete ? TerrainChunkPublishStatus::Applied : TerrainChunkPublishStatus::PartiallyApplied;
        }

        [[nodiscard]] std::string_view activeIdentity() const noexcept
        {
            return mActive.size() == 1 ? std::string_view(mActive.begin()->first) : std::string_view{};
        }

        [[nodiscard]] bool contains(std::string_view identity)
        {
            synchronizeEpoch();
            const auto active = mActive.find(identity);
            return active != mActive.end() && live(active->second);
        }

        [[nodiscard]] std::size_t activeCount() const noexcept { return mActive.size(); }

    private:
        struct Binding
        {
            std::string identity;
            MeshHandle mesh;
            MaterialHandle material;
            ModelHandle model;
            ChunkHandle chunk;
            InstanceHandle instance;
            std::int32_t gridX = 0;
            std::int32_t gridY = 0;
        };

        [[nodiscard]] static bool valid(const TerrainChunkSource& source) noexcept
        {
            return validTerrainChunkSource(source);
        }

        [[nodiscard]] static bool validSet(std::span<const TerrainChunkSource> sources) noexcept
        {
            for (std::size_t i = 0; i < sources.size(); ++i)
            {
                if (!valid(sources[i]))
                    return false;
                if (i != 0 && sources[i].worldspaceIdentity != sources.front().worldspaceIdentity)
                    return false;
                for (std::size_t j = i + 1; j < sources.size(); ++j)
                {
                    if (sources[i].identity == sources[j].identity
                        || (sources[i].gridX == sources[j].gridX && sources[i].gridY == sources[j].gridY
                            && sources[i].lodLevel == sources[j].lodLevel))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] std::optional<Binding> reserve(const TerrainChunkSource& source)
        {
            Binding result;
            result.identity = source.identity;
            result.gridX = source.gridX;
            result.gridY = source.gridY;
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

        void cancel(const Binding& binding) noexcept
        {
            mWorld.cancel(binding.instance);
            mWorld.cancel(binding.chunk);
            mWorld.cancel(binding.model);
            mWorld.cancel(binding.material);
            mWorld.cancel(binding.mesh);
        }

        void cancel(const std::map<std::string, Binding, std::less<>>& bindings) noexcept
        {
            for (const auto& [identity, binding] : bindings)
                cancel(binding);
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
            mActive.clear();
        }

        RenderWorld& mWorld;
        RenderWorldPublisher& mPublisher;
        WorldEpoch mObservedEpoch;
        std::map<std::string, Binding, std::less<>> mActive;
    };
}

#endif
