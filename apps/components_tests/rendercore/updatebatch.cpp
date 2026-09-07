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
        ASSERT_TRUE(batch.add(RenderCore::CreateTexture{ *texture,
            RenderCore::TextureRecord{ .sourceIdentity = "textures/stone.dds", .contentIdentity = "test:updatebatch-stone" } }));
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
        EXPECT_EQ(fallback.reason,
            RenderCore::RenderBackendSelection::Reason::RequestedBackendUnavailableFallback);
        EXPECT_NE(fallback.missingVsgCompatibilityFacets, 0u);

        const auto strict = RenderCore::selectRenderBackend(
            { RenderCore::RenderBackendPreference::VsgVulkan, false }, capabilities);
        EXPECT_FALSE(strict.valid);
    }

    TEST(RenderCoreBackendSelection, ParsesStableAdditiveConfigurationValues)
    {
        EXPECT_EQ(RenderCore::parseRenderBackendPreference("auto"), RenderCore::RenderBackendPreference::Auto);
        EXPECT_EQ(RenderCore::parseRenderBackendPreference("opengl"),
            RenderCore::RenderBackendPreference::LegacyOpenGL);
        EXPECT_EQ(RenderCore::parseRenderBackendPreference("vulkan"),
            RenderCore::RenderBackendPreference::VsgVulkan);
        EXPECT_FALSE(RenderCore::parseRenderBackendPreference("Vulkan"));
        EXPECT_EQ(RenderCore::renderBackendKindName(RenderCore::RenderBackendKind::LegacyOpenGL), "OpenGL");
        EXPECT_EQ(RenderCore::renderBackendKindName(RenderCore::RenderBackendKind::VsgVulkan), "VSG/Vulkan");
        EXPECT_EQ(RenderCore::renderBackendPreferenceName(RenderCore::RenderBackendPreference::Auto), "auto");
        EXPECT_EQ(RenderCore::renderBackendPreferenceName(RenderCore::RenderBackendPreference::LegacyOpenGL),
            "opengl");
        EXPECT_EQ(RenderCore::renderBackendPreferenceName(RenderCore::RenderBackendPreference::VsgVulkan),
            "vulkan");
    }

    TEST(RenderCoreBackendSelection, AutoKeepsCompatibilityBackendUntilModernParityQualified)
    {
        RenderCore::RenderBackendCapabilities capabilities;
        capabilities.legacyOpenGL = true;
        capabilities.vsgVulkan = true;

        const auto beforeParity = RenderCore::selectRenderBackend({}, capabilities);
        ASSERT_TRUE(beforeParity.valid);
        EXPECT_EQ(beforeParity.backend, RenderCore::RenderBackendKind::LegacyOpenGL);
        EXPECT_EQ(beforeParity.reason,
            RenderCore::RenderBackendSelection::Reason::AutomaticCompatibilityControl);

        capabilities.vsgVulkanCompatibilityFacets
            = RenderCore::RequiredAutomaticVsgCompatibility
            & ~RenderCore::compatibilityFacet(RenderCore::RenderCompatibilityFacet::ShaderModSurface);
        const auto withoutShaderMods = RenderCore::selectRenderBackend({}, capabilities);
        ASSERT_TRUE(withoutShaderMods.valid);
        EXPECT_EQ(withoutShaderMods.backend, RenderCore::RenderBackendKind::LegacyOpenGL);

        capabilities.vsgVulkanCompatibilityFacets = RenderCore::RequiredAutomaticVsgCompatibility;
        const auto afterParity = RenderCore::selectRenderBackend({}, capabilities);
        ASSERT_TRUE(afterParity.valid);
        EXPECT_EQ(afterParity.backend, RenderCore::RenderBackendKind::VsgVulkan);
        EXPECT_EQ(afterParity.reason, RenderCore::RenderBackendSelection::Reason::AutomaticQualifiedVulkan);
        EXPECT_EQ(afterParity.missingVsgCompatibilityFacets, 0u);
    }

    TEST(RenderCoreBackendSelection, AutoNeverSelectsAnUnqualifiedVulkanOnlyBuild)
    {
        RenderCore::RenderBackendCapabilities capabilities;
        capabilities.legacyOpenGL = false;
        capabilities.vsgVulkan = true;

        const auto automatic = RenderCore::selectRenderBackend({}, capabilities);
        EXPECT_FALSE(automatic.valid);

        const auto explicitVulkan = RenderCore::selectRenderBackend(
            { RenderCore::RenderBackendPreference::VsgVulkan, false }, capabilities);
        ASSERT_TRUE(explicitVulkan.valid);
        EXPECT_EQ(explicitVulkan.backend, RenderCore::RenderBackendKind::VsgVulkan);

        const auto unsafeLegacyFallback = RenderCore::selectRenderBackend(
            { RenderCore::RenderBackendPreference::LegacyOpenGL, true }, capabilities);
        EXPECT_FALSE(unsafeLegacyFallback.valid);

        capabilities.vsgVulkanCompatibilityFacets = RenderCore::RequiredAutomaticVsgCompatibility;
        const auto qualifiedLegacyFallback = RenderCore::selectRenderBackend(
            { RenderCore::RenderBackendPreference::LegacyOpenGL, true }, capabilities);
        ASSERT_TRUE(qualifiedLegacyFallback.valid);
        EXPECT_TRUE(qualifiedLegacyFallback.fellBack);
        EXPECT_EQ(qualifiedLegacyFallback.backend, RenderCore::RenderBackendKind::VsgVulkan);
    }

    TEST(RenderCoreRenderer, FrameMustMatchPublishedWorldAndLiveReferences)
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        const auto instance = world.reserveInstance();
        const auto material = world.reserveMaterial();
        ASSERT_TRUE(mesh && instance && material);
        ASSERT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/a.nif" }));
        ASSERT_TRUE(world.commit(*material, RenderCore::MaterialRecord{ .sourceIdentity = "material:a" }));

        RenderCore::InstanceRecord instanceRecord;
        instanceRecord.mesh = *mesh;
        instanceRecord.materials.push_back(*material);
        ASSERT_TRUE(world.commit(*instance, instanceRecord));

        RenderCore::FrameRenderStateDesc desc;
        desc.worldEpoch = world.epoch();
        desc.renderWorldRevision = world.revision();
        desc.renderExtent = { 1280, 720 };
        desc.outputExtent = { 1280, 720 };
        desc.dynamicTransforms.push_back(RenderCore::DynamicTransformState{ .instance = *instance });
        desc.dynamicMaterials.push_back(RenderCore::DynamicMaterialState{ .material = *material });

        EXPECT_TRUE(RenderCore::frameCompatibleWithWorld(world, RenderCore::FrameRenderState(desc)));

        RenderCore::FrameRenderStateDesc stale = desc;
        stale.renderWorldRevision = RenderCore::InitialRenderWorldRevision;
        EXPECT_FALSE(RenderCore::frameCompatibleWithWorld(world, RenderCore::FrameRenderState(std::move(stale))));

        RenderCore::FrameRenderStateDesc deadHandle = desc;
        deadHandle.dynamicMaterials.front().material = RenderCore::MaterialHandle::fromParts(999u, 1u);
        EXPECT_FALSE(RenderCore::frameCompatibleWithWorld(world, RenderCore::FrameRenderState(std::move(deadHandle))));

        RenderCore::FrameRenderStateDesc badBinding = desc;
        badBinding.dynamicMaterials.front().textureTransforms.push_back(
            RenderCore::DynamicTextureTransformState{ .bindingIndex = 0 });
        EXPECT_TRUE(RenderCore::FrameRenderState(badBinding).valid());
        EXPECT_FALSE(RenderCore::frameCompatibleWithWorld(world, RenderCore::FrameRenderState(std::move(badBinding))));
    }
}
