#include <components/render/backend/vsg/staticworldplan.hpp>
#include <components/rendercore/namedvisualsemantics.hpp>
#include <components/rendercore/records.hpp>
#include <components/rendercore/renderworld.hpp>
#include <components/rendercore/staticpopulationproducer.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace
{
    struct PublishedSwitch
    {
        RenderCore::ModelHandle model;
        RenderCore::InstanceHandle instance;
    };

    PublishedSwitch publishSwitch(RenderCore::RenderWorld& world, std::string_view name, std::size_t childCount)
    {
        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto model = world.reserveModel();
        const auto instance = world.reserveInstance();
        EXPECT_TRUE(mesh && material && model && instance);
        if (!mesh || !material || !model || !instance)
            return {};

        auto meshPayload = std::make_shared<RenderCore::MeshPayload>();
        meshPayload->positions = { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f } };
        meshPayload->indices = { 0u, 1u, 2u };
        meshPayload->surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3u });
        EXPECT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{
            .sourceIdentity = "test:switch-mesh", .surfaceCount = 1u, .payload = std::move(meshPayload) }));
        EXPECT_TRUE(world.commit(
            *material, RenderCore::MaterialRecord{ .sourceIdentity = "test:switch-material" }));

        auto payload = std::make_shared<RenderCore::ModelPayload>();
        RenderCore::ModelNodeRecord root;
        root.name = std::string(name);
        root.sourceRecordId = 0u;
        root.kind = RenderCore::ModelNodeKind::Switch;
        if (childCount != 0)
            root.activeSwitchChild = RenderCore::ModelNodeIndex{ 1u };
        payload->nodes.push_back(std::move(root));

        for (std::size_t i = 0; i < childCount; ++i)
        {
            RenderCore::ModelNodeRecord geometry;
            geometry.name = "switch-child-" + std::to_string(i);
            geometry.sourceRecordId = static_cast<std::uint32_t>(i + 1u);
            geometry.parent = RenderCore::ModelNodeIndex{ 0u };
            geometry.kind = RenderCore::ModelNodeKind::Geometry;
            geometry.mesh = *mesh;
            geometry.materials.push_back(*material);
            payload->nodes.push_back(std::move(geometry));
        }
        payload->roots.push_back(RenderCore::ModelNodeIndex{ 0u });

        EXPECT_TRUE(world.commit(*model,
            RenderCore::ModelRecord{ .sourceIdentity = "test:switch-model", .payload = std::move(payload) }));
        RenderCore::InstanceRecord placed;
        placed.model = *model;
        EXPECT_TRUE(world.commit(*instance, placed));
        return { *model, *instance };
    }

    TEST(RenderCoreNamedVisualSemantics, NightDayNodeNameAloneDoesNotEnableOverride)
    {
        RenderCore::RenderWorld world;
        const PublishedSwitch scene = publishSwitch(world, "NightDaySwitch", 3u);
        ASSERT_TRUE(scene.model.valid() && scene.instance.valid());

        RenderVsg::StaticPlanOptions options;
        options.dayNightSwitchesEnabled = true;
        options.nightDaySwitchState = RenderCore::NightDaySwitchState::ExteriorNight;
        const auto plan = RenderVsg::buildStaticInstancePlan(world, scene.instance, options);
        ASSERT_TRUE(plan);
        EXPECT_FALSE(plan->options.dayNightSwitchesEnabled);
        ASSERT_EQ(plan->asset.draws.size(), 1u);
        EXPECT_EQ(plan->asset.draws.front().node, RenderCore::ModelNodeIndex{ 1u });
    }

    TEST(RenderCoreNamedVisualSemantics, NightDayCapabilityUsesLegacyChildIndicesAndFallback)
    {
        RenderCore::RenderWorld world;
        const PublishedSwitch scene = publishSwitch(world, "NightDaySwitch", 3u);
        ASSERT_TRUE(scene.model.valid() && scene.instance.valid());

        RenderCore::InstanceRecord placed = *world.get(scene.instance);
        placed.semanticFlags |= RenderCore::NightDaySwitchCapabilitySemanticFlag;
        ASSERT_TRUE(world.update(scene.instance, placed));

        RenderVsg::StaticPlanOptions options;
        options.dayNightSwitchesEnabled = true;
        options.nightDaySwitchState = RenderCore::NightDaySwitchState::ExteriorNight;
        auto plan = RenderVsg::buildStaticInstancePlan(world, scene.instance, options);
        ASSERT_TRUE(plan);
        EXPECT_TRUE(plan->options.dayNightSwitchesEnabled);
        ASSERT_EQ(plan->asset.draws.size(), 1u);
        EXPECT_EQ(plan->asset.draws.front().node, RenderCore::ModelNodeIndex{ 2u });

        options.nightDaySwitchState = RenderCore::NightDaySwitchState::InteriorDay;
        plan = RenderVsg::buildStaticInstancePlan(world, scene.instance, options);
        ASSERT_TRUE(plan);
        ASSERT_EQ(plan->asset.draws.size(), 1u);
        EXPECT_EQ(plan->asset.draws.front().node, RenderCore::ModelNodeIndex{ 3u });

        RenderCore::RenderWorld shortWorld;
        const PublishedSwitch shortScene = publishSwitch(shortWorld, "NightDaySwitch", 2u);
        ASSERT_TRUE(shortScene.instance.valid());
        RenderCore::InstanceRecord shortPlaced = *shortWorld.get(shortScene.instance);
        shortPlaced.semanticFlags |= RenderCore::NightDaySwitchCapabilitySemanticFlag;
        ASSERT_TRUE(shortWorld.update(shortScene.instance, shortPlaced));
        plan = RenderVsg::buildStaticInstancePlan(shortWorld, shortScene.instance, options);
        ASSERT_TRUE(plan);
        ASSERT_EQ(plan->asset.draws.size(), 1u);
        EXPECT_EQ(plan->asset.draws.front().node, RenderCore::ModelNodeIndex{ 1u });
    }

    TEST(RenderCoreNamedVisualSemantics, HerbalismRequiresCapabilityAndHarvestedState)
    {
        RenderCore::RenderWorld world;
        const PublishedSwitch scene = publishSwitch(world, "HerbalismSwitch", 2u);
        ASSERT_TRUE(scene.instance.valid());

        RenderCore::InstanceRecord placed = *world.get(scene.instance);
        placed.semanticFlags = RenderCore::HerbalismHarvestedSemanticFlag;
        ASSERT_TRUE(world.update(scene.instance, placed));

        auto plan = RenderVsg::buildStaticInstancePlan(world, scene.instance);
        ASSERT_TRUE(plan);
        EXPECT_FALSE(plan->options.herbalismHarvested);
        ASSERT_EQ(plan->asset.draws.size(), 1u);
        EXPECT_EQ(plan->asset.draws.front().node, RenderCore::ModelNodeIndex{ 1u });

        placed = *world.get(scene.instance);
        placed.semanticFlags |= RenderCore::HerbalismSwitchCapabilitySemanticFlag;
        ASSERT_TRUE(world.update(scene.instance, placed));
        plan = RenderVsg::buildStaticInstancePlan(world, scene.instance);
        ASSERT_TRUE(plan);
        EXPECT_TRUE(plan->options.herbalismHarvested);
        ASSERT_EQ(plan->asset.draws.size(), 1u);
        EXPECT_EQ(plan->asset.draws.front().node, RenderCore::ModelNodeIndex{ 2u });
    }

    TEST(RenderCoreNamedVisualSemantics, PopulationRejectsMixedCapabilitiesAndHarvestedPlacements)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::StaticPopulationProducer producer(world, publisher);
        const PublishedSwitch scene = publishSwitch(world, "NightDaySwitch", 3u);
        ASSERT_TRUE(scene.model.valid());
        ASSERT_EQ(producer.addCell({ .identity = "cell:0,0", .worldspaceIdentity = "world" }),
            RenderCore::StaticPopulationPublishStatus::Applied);

        RenderCore::StaticPopulationInstanceSource first;
        first.identity = "ref:first";
        first.cellIdentity = "cell:0,0";
        first.model = scene.model;
        first.semanticFlags |= RenderCore::NightDaySwitchCapabilitySemanticFlag;
        ASSERT_EQ(producer.upsert(first), RenderCore::StaticPopulationPublishStatus::Applied);

        RenderCore::StaticPopulationInstanceSource second = first;
        second.identity = "ref:second";
        second.semanticFlags &= ~RenderCore::NightDaySwitchCapabilitySemanticFlag;
        ASSERT_EQ(producer.upsert(second), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.flush(), RenderCore::StaticPopulationPublishStatus::Applied);

        RenderCore::ChunkHandle chunk;
        const RenderCore::ChunkRecord* chunkRecord = nullptr;
        world.forEachChunk([&](RenderCore::ChunkHandle handle, const RenderCore::ChunkRecord& record) {
            chunk = handle;
            chunkRecord = &record;
        });
        ASSERT_TRUE(chunk.valid());
        ASSERT_NE(chunkRecord, nullptr);
        ASSERT_NE(chunkRecord->population, nullptr);
        ASSERT_EQ(chunkRecord->population->groups.size(), 1u);

        RenderVsg::StaticPlanOptions options;
        options.dayNightSwitchesEnabled = true;
        EXPECT_FALSE(RenderVsg::buildStaticPopulationPlan(
            world, chunk, chunkRecord->population->groups.front(), options));

        RenderCore::PopulationInstanceRecord harvested = chunkRecord->population->groups.front().instances.front();
        harvested.semanticFlags |= RenderCore::HerbalismSwitchCapabilitySemanticFlag
            | RenderCore::HerbalismHarvestedSemanticFlag;
        RenderCore::ModelPopulationRecord harvestedGroup = chunkRecord->population->groups.front();
        harvestedGroup.instances.assign(2u, harvested);
        EXPECT_FALSE(RenderVsg::buildStaticPopulationPlan(world, chunk, harvestedGroup, options));
    }

    TEST(RenderCoreNamedVisualSemantics, InvalidNightDayStateFailsClosed)
    {
        RenderCore::RenderWorld world;
        const PublishedSwitch scene = publishSwitch(world, "NightDaySwitch", 3u);
        ASSERT_TRUE(scene.instance.valid());
        RenderCore::InstanceRecord placed = *world.get(scene.instance);
        placed.semanticFlags |= RenderCore::NightDaySwitchCapabilitySemanticFlag;
        ASSERT_TRUE(world.update(scene.instance, placed));

        RenderVsg::StaticPlanOptions options;
        options.dayNightSwitchesEnabled = true;
        options.nightDaySwitchState = static_cast<RenderCore::NightDaySwitchState>(3u);
        EXPECT_FALSE(RenderVsg::buildStaticInstancePlan(world, scene.instance, options));
    }
}
