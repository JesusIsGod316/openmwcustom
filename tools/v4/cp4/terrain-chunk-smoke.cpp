#include <components/rendercore/terrainchunkproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "CP4A terrain chunk smoke: FAIL: " << message << '\n';
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
    require(producer.synchronize(std::nullopt) == RenderCore::TerrainChunkPublishStatus::Applied,
        "LAND chunk was not retired");
    require(world.valid() && world.chunkCount() == 0 && world.instanceCount() == 0,
        "retirement left live terrain ownership");

    std::cout << "CP4A terrain chunk smoke: PASS\n";
}
