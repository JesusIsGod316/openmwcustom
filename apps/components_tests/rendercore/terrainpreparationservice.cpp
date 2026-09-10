#include <components/rendercore/terrainpreparationservice.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace
{
    RenderCore::TerrainPreparationRequest request(std::string identity, std::int32_t x, bool required = false)
    {
        return RenderCore::TerrainPreparationRequest{
            .identity = std::move(identity),
            .worldspaceIdentity = "world",
            .gridX = x,
            .required = required,
        };
    }

    std::optional<RenderCore::TerrainChunkSource> build(const RenderCore::TerrainPreparationRequest& request)
    {
        auto mesh = std::make_shared<RenderCore::MeshPayload>();
        mesh->positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        mesh->indices = { 0, 1, 2 };
        mesh->surfaces.push_back({ RenderCore::PrimitiveTopology::Triangles, 0, 3, 0 });
        RenderCore::TerrainChunkSource result;
        result.identity = request.identity;
        result.worldspaceIdentity = request.worldspaceIdentity;
        result.gridX = request.gridX;
        result.gridY = request.gridY;
        result.lodLevel = request.lodLevel;
        result.stitchMask = request.stitchMask;
        result.localBounds.maximum = { 1.0f, 1.0f, 0.0f };
        result.mesh = std::move(mesh);
        return result;
    }

    std::optional<RenderCore::PreparedTerrainSet> waitForReady(RenderCore::TerrainPreparationService& service)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto ready = service.takeReady())
                return ready;
            std::this_thread::yield();
        }
        return std::nullopt;
    }

    TEST(TerrainPreparationService, PreparesCoarseSetAndReusesUnchangedChunks)
    {
        std::atomic_uint32_t builds = 0;
        RenderCore::TerrainPreparationService service(
            [&](const RenderCore::TerrainPreparationRequest& item, std::stop_token) {
                ++builds;
                return build(item);
            });
        std::vector desired{ request("terrain:0", 0, true), request("terrain:1", 1) };

        EXPECT_EQ(service.request(std::span<const RenderCore::TerrainPreparationRequest>(desired)),
            RenderCore::TerrainPreparationRequestStatus::Accepted);
        auto first = waitForReady(service);
        ASSERT_TRUE(first);
        EXPECT_TRUE(first->requiredChunksReady);
        EXPECT_EQ(first->chunks.size(), 2u);
        EXPECT_EQ(builds, 2u);

        desired = { request("terrain:1", 1), request("terrain:2", 2, true) };
        EXPECT_EQ(service.request(std::span<const RenderCore::TerrainPreparationRequest>(desired)),
            RenderCore::TerrainPreparationRequestStatus::Accepted);
        auto second = waitForReady(service);
        ASSERT_TRUE(second);
        EXPECT_TRUE(second->requiredChunksReady);
        EXPECT_EQ(second->chunks.size(), 2u);
        EXPECT_EQ(builds, 3u);
    }

    TEST(TerrainPreparationService, RejectsInvalidSetsBeforeWorkerPublication)
    {
        RenderCore::TerrainPreparationService service(
            [](const RenderCore::TerrainPreparationRequest& item, std::stop_token) { return build(item); });
        std::vector duplicate{ request("terrain:0", 0), request("terrain:0", 1) };
        EXPECT_EQ(service.request(std::span<const RenderCore::TerrainPreparationRequest>(duplicate)),
            RenderCore::TerrainPreparationRequestStatus::Invalid);

        std::vector duplicateAddress{ request("terrain:0", 0), request("terrain:other", 0) };
        EXPECT_EQ(service.request(std::span<const RenderCore::TerrainPreparationRequest>(duplicateAddress)),
            RenderCore::TerrainPreparationRequestStatus::Invalid);
    }

    TEST(TerrainPreparationService, ReportsRequiredFailureWithoutPublishingMalformedSource)
    {
        RenderCore::TerrainPreparationService service(
            [](const RenderCore::TerrainPreparationRequest& item,
                std::stop_token) -> std::optional<RenderCore::TerrainChunkSource> {
                if (item.required)
                {
                    auto malformed = build(item);
                    malformed->mesh.reset();
                    return malformed;
                }
                return build(item);
            });
        std::vector desired{ request("terrain:required", 0, true), request("terrain:optional", 1) };
        ASSERT_EQ(service.request(std::span<const RenderCore::TerrainPreparationRequest>(desired)),
            RenderCore::TerrainPreparationRequestStatus::Accepted);
        auto ready = waitForReady(service);
        ASSERT_TRUE(ready);
        EXPECT_FALSE(ready->requiredChunksReady);
        ASSERT_EQ(ready->failedIdentities.size(), 1u);
        EXPECT_EQ(ready->failedIdentities.front(), "terrain:required");
        ASSERT_EQ(ready->chunks.size(), 1u);
        EXPECT_EQ(ready->chunks.front().identity, "terrain:optional");
    }

    TEST(TerrainPreparationService, CoalescesToNewestDesiredSetWithoutDeliveringStaleWork)
    {
        std::atomic_bool firstStarted = false;
        std::atomic_bool releaseFirst = false;
        RenderCore::TerrainPreparationService service(
            [&](const RenderCore::TerrainPreparationRequest& item, std::stop_token stop) {
                if (item.identity == "terrain:old")
                {
                    firstStarted = true;
                    while (!releaseFirst && !stop.stop_requested())
                        std::this_thread::yield();
                }
                return build(item);
            });

        const std::vector oldSet{ request("terrain:old", 0, true) };
        ASSERT_EQ(service.request(std::span<const RenderCore::TerrainPreparationRequest>(oldSet)),
            RenderCore::TerrainPreparationRequestStatus::Accepted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!firstStarted && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        ASSERT_TRUE(firstStarted);

        const std::vector newSet{ request("terrain:new", 1, true) };
        ASSERT_EQ(service.request(std::span<const RenderCore::TerrainPreparationRequest>(newSet)),
            RenderCore::TerrainPreparationRequestStatus::Accepted);
        releaseFirst = true;

        auto ready = waitForReady(service);
        ASSERT_TRUE(ready);
        ASSERT_EQ(ready->chunks.size(), 1u);
        EXPECT_EQ(ready->chunks.front().identity, "terrain:new");
        EXPECT_EQ(ready->generation, service.latestRequestedGeneration());
    }
}
