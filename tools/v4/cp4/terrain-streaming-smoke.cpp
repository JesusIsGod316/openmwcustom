#include <components/rendercore/terrainpreparationservice.hpp>
#include <components/rendercore/terrainresidencyplanner.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace
{
    [[noreturn]] void fail(const char* message)
    {
        std::cerr << "CP4B terrain streaming smoke: FAIL: " << message << '\n';
        std::exit(1);
    }

    std::optional<RenderCore::TerrainChunkSource> build(const RenderCore::TerrainPreparationRequest& request)
    {
        auto mesh = std::make_shared<RenderCore::MeshPayload>();
        mesh->positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
        mesh->indices = { 0, 1, 2 };
        mesh->surfaces.push_back({ RenderCore::PrimitiveTopology::Triangles, 0, 3, 0 });
        RenderCore::TerrainChunkSource source;
        source.identity = request.identity;
        source.worldspaceIdentity = request.worldspaceIdentity;
        source.gridX = request.gridX;
        source.gridY = request.gridY;
        source.lodLevel = request.lodLevel;
        source.stitchMask = request.stitchMask;
        source.localBounds.maximum = { 1, 1, 0 };
        source.mesh = std::move(mesh);
        return source;
    }
}

int main()
{
    RenderCore::TerrainResidencyPlanner planner;
    const auto initial = planner.update("world", 0, 0);
    if (initial.size() != 25)
        fail("initial mixed-LOD ring size changed");
    static_cast<void>(planner.update("world", 0, 0));
    const auto predicted = planner.update("world", 1, 0);
    if (predicted.size() != 30)
        fail("predictive ring size changed");

    RenderCore::TerrainPreparationService service([](const auto& request, std::stop_token) { return build(request); });
    std::vector<RenderCore::TerrainPreparationRequest> requests;
    requests.reserve(predicted.size());
    for (const auto& cell : predicted)
    {
        requests.push_back({ .identity = "terrain:" + std::to_string(cell.gridX) + "," + std::to_string(cell.gridY),
            .worldspaceIdentity = "world",
            .gridX = cell.gridX,
            .gridY = cell.gridY,
            .lodLevel = cell.lodLevel,
            .stitchMask = cell.stitchMask,
            .required = cell.required });
    }
    if (service.request(requests) != RenderCore::TerrainPreparationRequestStatus::Accepted)
        fail("valid predicted set was rejected");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (const auto ready = service.takeReady())
        {
            if (!ready->requiredChunksReady || ready->chunks.size() != 30 || ready->budgetLimited)
                fail("prepared set violated readiness or budget contract");
            std::cout << "CP4B terrain streaming smoke: PASS\n";
            return 0;
        }
        std::this_thread::yield();
    }
    fail("background preparation timed out");
}
