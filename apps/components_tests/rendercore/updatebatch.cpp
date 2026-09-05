#include <components/rendercore/renderer.hpp>
#include <components/rendercore/updatebatch.hpp>

#include <gtest/gtest.h>

namespace
{
    TEST(RenderCoreUpdateBatch, OrderedCreatePublishesAtomically)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        const auto texture = world.reserveTexture();
        const auto material = world.reserveMaterial();
        ASSERT_TRUE(texture && material);

        RenderCore::MaterialRecord materialRecord;
        materialRecord.sourceIdentity = "material:stone";
        materialRecord.textures.push_back(RenderCore::TextureBinding{ .texture = *texture });

        RenderCore::RenderWorldUpdateBatch batch(world.epoch(), RenderCore::InitialUpdateSequence, "test:asset-create");
        ASSERT_TRUE(batch.add(RenderCore::CreateTexture{ *texture, RenderCore::TextureRecord{ .sourceIdentity = "textures/stone.dds" } }));
        ASSERT_TRUE(batch.add(RenderCore::CreateMaterial{ *material, materialRecord }));
        ASSERT_TRUE(batch.seal());

        EXPECT_EQ(publisher.apply(batch), RenderCore::PublishStatus::Applied);
        EXPECT_NE(world.get(*texture), nullptr);
        EXPECT_NE(world.get(*material), nullptr);
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderCoreUpdateBatch, RejectedOperationLeavesPublishedWorldUntouched)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        const auto mesh = world.reserveMesh();
        const auto instance = world.reserveInstance();
        ASSERT_TRUE(mesh && instance);
        const auto revisionBefore = world.revision();

        RenderCore::InstanceRecord instanceRecord;
        instanceRecord.mesh = *mesh;
        RenderCore::RenderWorldUpdateBatch batch(world.epoch(), RenderCore::InitialUpdateSequence, "test:bad-order");
        ASSERT_TRUE(batch.add(RenderCore::CreateInstance{ *instance, instanceRecord }));
        ASSERT_TRUE(batch.add(RenderCore::CreateMesh{ *mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/a.nif" } }));
        ASSERT_TRUE(batch.seal());

        EXPECT_EQ(publisher.apply(batch), RenderCore::PublishStatus::OperationRejected);
        EXPECT_EQ(world.revision(), revisionBefore);
        EXPECT_EQ(world.get(*mesh), nullptr);
        EXPECT_EQ(world.get(*instance), nullptr);
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderCoreUpdateBatch, SequenceMustBeContiguous)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);

        RenderCore::RenderWorldUpdateBatch skipped(world.epoch(), RenderCore::UpdateSequence{ 2 }, "test:skip");
        ASSERT_TRUE(skipped.seal());
        EXPECT_EQ(publisher.apply(skipped), RenderCore::PublishStatus::OutOfOrder);

        RenderCore::RenderWorldUpdateBatch first(world.epoch(), RenderCore::InitialUpdateSequence, "test:first");
        ASSERT_TRUE(first.seal());
        EXPECT_EQ(publisher.apply(first), RenderCore::PublishStatus::Applied);

        RenderCore::RenderWorldUpdateBatch duplicate(world.epoch(), RenderCore::InitialUpdateSequence, "test:duplicate");
        ASSERT_TRUE(duplicate.seal());
        EXPECT_EQ(publisher.apply(duplicate), RenderCore::PublishStatus::OutOfOrder);
    }

    TEST(RenderCoreUpdateBatch, WorldResetRejectsOldEpochAndRestartsSequence)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        const RenderCore::WorldEpoch oldEpoch = world.epoch();

        RenderCore::RenderWorldUpdateBatch first(oldEpoch, RenderCore::InitialUpdateSequence);
        ASSERT_TRUE(first.seal());
        ASSERT_EQ(publisher.apply(first), RenderCore::PublishStatus::Applied);
        ASSERT_TRUE(world.reset());

        RenderCore::RenderWorldUpdateBatch stale(oldEpoch, RenderCore::UpdateSequence{ 2 });
        ASSERT_TRUE(stale.seal());
        EXPECT_EQ(publisher.apply(stale), RenderCore::PublishStatus::StaleEpoch);

        RenderCore::RenderWorldUpdateBatch fresh(world.epoch(), RenderCore::InitialUpdateSequence);
        ASSERT_TRUE(fresh.seal());
        EXPECT_EQ(publisher.apply(fresh), RenderCore::PublishStatus::Applied);
    }

    TEST(RenderCoreBackendSelection, VulkanCanFailClosedOrFallbackAtStartup)
    {
        RenderCore::RenderBackendCapabilities capabilities;
        capabilities.legacyOpenGL = true;
        capabilities.vsgVulkan = false;

        const auto fallback = RenderCore::selectRenderBackend(
            { RenderCore::RenderBackendPreference::VsgVulkan, true }, capabilities);
        EXPECT_TRUE(fallback.valid);
        EXPECT_TRUE(fallback.fellBack);
        EXPECT_EQ(fallback.backend, RenderCore::RenderBackendKind::LegacyOpenGL);

        const auto strict = RenderCore::selectRenderBackend(
            { RenderCore::RenderBackendPreference::VsgVulkan, false }, capabilities);
        EXPECT_FALSE(strict.valid);
    }
}
