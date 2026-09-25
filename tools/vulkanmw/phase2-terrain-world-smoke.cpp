#include <components/render/native/terrainworldservice.hpp>

#include <cstdlib>
#include <memory>

namespace
{
    [[noreturn]] void fail() { std::abort(); }

    std::optional<RenderCore::TerrainChunkSource> build(
        const RenderCore::TerrainPreparationRequest& request, std::stop_token stop)
    {
        if (stop.stop_requested())
            return std::nullopt;

        auto mesh = std::make_shared<RenderCore::MeshPayload>();
        mesh->positions = { {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f} };
        mesh->normals = { {0.f, 0.f, 1.f}, {0.f, 0.f, 1.f}, {0.f, 0.f, 1.f} };
        mesh->indices = { 0, 1, 2 };
        mesh->surfaces.push_back({
            .topology = RenderCore::PrimitiveTopology::Triangles,
            .firstIndex = 0,
            .indexCount = 3,
            .materialSlot = 0,
        });

        RenderCore::TerrainChunkSource source;
        source.identity = request.identity;
        source.worldspaceIdentity = request.worldspaceIdentity;
        source.gridX = request.gridX;
        source.gridY = request.gridY;
        source.lodLevel = request.lodLevel;
        source.stitchMask = request.stitchMask;
        source.localBounds.minimum = {0.f, 0.f, 0.f};
        source.localBounds.maximum = {1.f, 1.f, 0.f};
        source.mesh = std::move(mesh);
        return source;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::RenderWorldPublisher publisher(world);
    RenderNative::TerrainWorldService terrain(world, publisher);

    const auto residency = terrain.updateResidency("test", 0, 0);
    if (residency.empty())
        fail();

    std::vector<RenderCore::TerrainPreparationRequest> desired;
    desired.reserve(residency.size());
    for (const auto& cell : residency)
    {
        RenderCore::TerrainPreparationRequest request;
        request.worldspaceIdentity = "test";
        request.gridX = cell.gridX;
        request.gridY = cell.gridY;
        request.lodLevel = cell.lodLevel;
        request.stitchMask = cell.stitchMask;
        request.required = cell.required;
        request.identity = "terrain:test:" + std::to_string(cell.gridX) + "," + std::to_string(cell.gridY)
            + ":lod" + std::to_string(cell.lodLevel) + ":stitch" + std::to_string(cell.stitchMask);
        desired.push_back(std::move(request));
    }

    const RenderNative::TerrainWorldSyncResult synchronized = terrain.synchronize(desired, build);
    if (!synchronized.accepted())
        fail();

    std::size_t terrainChunks = 0;
    world.forEachChunk([&](auto, const RenderCore::ChunkRecord& chunk) {
        if (chunk.kind == RenderCore::ChunkRecord::Kind::Terrain)
            ++terrainChunks;
    });
    if (terrainChunks == 0)
        fail();

    const RenderNative::TerrainWorldSyncResult cleared = terrain.clear();
    if (!cleared.accepted())
        fail();

    terrainChunks = 0;
    world.forEachChunk([&](auto, const RenderCore::ChunkRecord& chunk) {
        if (chunk.kind == RenderCore::ChunkRecord::Kind::Terrain)
            ++terrainChunks;
    });
    if (terrainChunks != 0)
        fail();

    terrain.stopBackgroundPreparation();
    return 0;
}
