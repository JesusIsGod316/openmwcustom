#include <components/rendercore/terrainchunkproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "CP4 terrain chunk smoke: FAIL: " << message << '\n';
        std::exit(1);
    }

    void require(bool condition, const char* message)
    {
        if (!condition)
            fail(message);
    }

    RenderCore::TerrainChunkSource source(const char* identity, int x)
    {
        auto mesh = std::make_shared<RenderCore::MeshPayload>();
        mesh->positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
        mesh->normals.assign(3, glm::vec3(0.0f, 0.0f, 1.0f));
        mesh->colors.assign(3, glm::vec4(1.0f));
        mesh->indices = { 0, 1, 2 };
        mesh->surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3 });

        RenderCore::TerrainChunkSource result;
        result.identity = identity;
        result.worldspaceIdentity = "world";
        result.gridX = x;
        result.localBounds.minimum = { -1.0f, -1.0f, 0.0f };
        result.localBounds.maximum = { 1.0f, 1.0f, 0.0f };
        result.mesh = std::move(mesh);
        result.material.vertexColorMode = RenderCore::VertexColorMode::AmbientDiffuse;
        return result;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::RenderWorldPublisher publisher(world);
    RenderCore::TerrainChunkProducer producer(world, publisher);

    require(producer.synchronize(source("terrain:0,0", 0)) == RenderCore::TerrainChunkPublishStatus::Applied,
        "first LAND chunk was not published");
    require(world.valid() && world.chunkCount() == 1 && world.instanceCount() == 1,
        "first publication violated world invariants");
    require(producer.synchronize(source("terrain:1,0", 1)) == RenderCore::TerrainChunkPublishStatus::Applied,
        "replacement LAND chunk was not published");
    require(world.valid() && world.chunkCount() == 1 && world.instanceCount() == 1, "replacement was not atomic");
    std::vector<RenderCore::TerrainChunkSource> active{
        source("terrain:1,0", 1),
        source("terrain:2,0", 2),
    };
    require(producer.synchronize(std::span<const RenderCore::TerrainChunkSource>(active))
            == RenderCore::TerrainChunkPublishStatus::Applied,
        "active LAND set was not published");
    require(world.valid() && world.chunkCount() == 2 && world.instanceCount() == 2,
        "active LAND set publication was not atomic");
    active = { source("terrain:2,0", 2), source("terrain:3,0", 3) };
    require(producer.synchronize(std::span<const RenderCore::TerrainChunkSource>(active))
            == RenderCore::TerrainChunkPublishStatus::Applied,
        "active LAND set churn failed");
    require(!producer.contains("terrain:1,0") && producer.contains("terrain:2,0") && producer.contains("terrain:3,0"),
        "active LAND set retained the wrong identities");
    require(world.valid() && world.chunkCount() == 2 && world.instanceCount() == 2,
        "active LAND set churn violated world invariants");
    require(producer.synchronize(std::nullopt) == RenderCore::TerrainChunkPublishStatus::Applied,
        "LAND chunk was not retired");
    require(world.valid() && world.chunkCount() == 0 && world.instanceCount() == 0,
        "retirement left live terrain ownership");

    std::cout << "CP4 terrain chunk smoke: PASS\n";
}
