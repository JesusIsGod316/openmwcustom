#include <components/rendercore/records.hpp>
#include <components/rendercore/renderworld.hpp>
#include <components/rendercore/updatebatch.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <memory>

namespace
{
    std::shared_ptr<const RenderCore::ModelPayload> makeSimpleModel(
        RenderCore::MeshHandle mesh, RenderCore::MaterialHandle material)
    {
        auto payload = std::make_shared<RenderCore::ModelPayload>();

        RenderCore::ModelNodeRecord root;
        root.name = "root";
        root.sourceRecordId = 0u;
        root.kind = RenderCore::ModelNodeKind::Transform;
        payload->nodes.push_back(root);

        RenderCore::ModelNodeRecord lod;
        lod.name = "lod";
        lod.sourceRecordId = 1u;
        lod.parent = RenderCore::ModelNodeIndex{ 0u };
        lod.kind = RenderCore::ModelNodeKind::Lod;
        RenderCore::ModelLodSemantic lodSemantic;
        lodSemantic.ranges.push_back(RenderCore::ModelLodRange{
            .child = RenderCore::ModelNodeIndex{ 2u }, .minimumDistance = 0.0f, .maximumDistance = 4096.0f });
        lod.lod = std::move(lodSemantic);
        payload->nodes.push_back(std::move(lod));

        RenderCore::ModelNodeRecord geometry;
        geometry.name = "geometry";
        geometry.sourceRecordId = 2u;
        geometry.parent = RenderCore::ModelNodeIndex{ 1u };
        geometry.kind = RenderCore::ModelNodeKind::Geometry;
        geometry.mesh = mesh;
        geometry.materials.push_back(material);
        payload->nodes.push_back(std::move(geometry));

        payload->roots.push_back(RenderCore::ModelNodeIndex{ 0u });
        return payload;
    }

    TEST(RenderCoreModelAssets, TangentBasisIsPairedAndFinite)
    {
        RenderCore::MeshPayload payload;
        payload.positions.resize(3);
        payload.indices = { 0, 1, 2 };
        payload.surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3 });

        payload.tangents.resize(3);
        EXPECT_FALSE(RenderCore::validMeshPayload(payload));

        payload.bitangents.resize(3);
        EXPECT_TRUE(RenderCore::validMeshPayload(payload));

        payload.bitangents[1].x = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(RenderCore::validMeshPayload(payload));
    }

    TEST(RenderCoreModelAssets, MaterialPreservesExactStaticStateWithoutConflatingBlendAndTest)
    {
        RenderCore::RenderWorld world;
        const auto texture = world.reserveTexture();
        const auto material = world.reserveMaterial();
        ASSERT_TRUE(texture && material);
        ASSERT_TRUE(world.commit(*texture, RenderCore::TextureRecord{ .sourceIdentity = "textures/state.dds" }));

        RenderCore::MaterialRecord record;
        record.sourceIdentity = "material:state";
        record.alphaMode = RenderCore::AlphaMode::Blend;
        record.alphaBlendEnabled = true;
        record.alphaTestEnabled = true;
        record.alphaCompare = RenderCore::CompareOp::GreaterEqual;
        record.transparentSort = RenderCore::TransparentSortPolicy::Unsorted;
        record.textureApply = RenderCore::TextureApplyMode::Highlight2;
        record.frontFace = RenderCore::FrontFaceWinding::Clockwise;
        record.wireframe = true;
        record.stencil.enabled = true;
        record.stencil.compare = RenderCore::CompareOp::NotEqual;
        record.stencil.reference = 7u;
        record.stencil.fail = RenderCore::StencilOp::Replace;
        record.stencil.depthFail = RenderCore::StencilOp::Increment;
        record.stencil.pass = RenderCore::StencilOp::Invert;

        RenderCore::TextureBinding binding;
        binding.texture = *texture;
        binding.transform.offset = { 0.25f, -0.5f };
        binding.transform.scale = { 2.0f, 0.5f };
        binding.transform.center = { 0.1f, 0.9f };
        binding.transform.rotation = 0.75f;
        binding.transform.convention = RenderCore::TextureTransformConvention::Maya;
        record.textures.push_back(binding);

        ASSERT_TRUE(world.commit(*material, record));
        const RenderCore::MaterialRecord* published = world.get(*material);
        ASSERT_NE(published, nullptr);
        EXPECT_TRUE(published->alphaBlendEnabled);
        EXPECT_TRUE(published->alphaTestEnabled);
        EXPECT_EQ(published->alphaCompare, RenderCore::CompareOp::GreaterEqual);
        EXPECT_EQ(published->transparentSort, RenderCore::TransparentSortPolicy::Unsorted);
        EXPECT_EQ(published->textureApply, RenderCore::TextureApplyMode::Highlight2);
        EXPECT_EQ(published->frontFace, RenderCore::FrontFaceWinding::Clockwise);
        EXPECT_TRUE(published->wireframe);
        EXPECT_TRUE(published->stencil.enabled);
        EXPECT_EQ(published->textures.front().transform.convention, RenderCore::TextureTransformConvention::Maya);
    }

    TEST(RenderCoreModelAssets, MaterialRejectsNonFiniteUvSemantic)
    {
        RenderCore::RenderWorld world;
        const auto texture = world.reserveTexture();
        const auto material = world.reserveMaterial();
        ASSERT_TRUE(texture && material);
        ASSERT_TRUE(world.commit(*texture, RenderCore::TextureRecord{ .sourceIdentity = "textures/state.dds" }));

        RenderCore::MaterialRecord record;
        RenderCore::TextureBinding binding;
        binding.texture = *texture;
        binding.transform.center.x = std::numeric_limits<float>::infinity();
        record.textures.push_back(binding);
        EXPECT_FALSE(world.commit(*material, record));
        EXPECT_TRUE(world.cancel(*material));
    }

    TEST(RenderCoreModelAssets, ModelGraphRequiresDeterministicParentBeforeChildOrdering)
    {
        RenderCore::ModelPayload payload;
        RenderCore::ModelNodeRecord invalidRoot;
        invalidRoot.parent = RenderCore::ModelNodeIndex{ 1u };
        payload.nodes.push_back(invalidRoot);
        payload.nodes.push_back(RenderCore::ModelNodeRecord{});
        payload.roots.push_back(RenderCore::ModelNodeIndex{ 1u });
        EXPECT_FALSE(RenderCore::validModelPayloadStructure(payload));
    }

    TEST(RenderCoreModelAssets, LodAndCollisionSemanticsFailClosed)
    {
        RenderCore::ModelPayload payload;
        RenderCore::ModelNodeRecord lod;
        lod.kind = RenderCore::ModelNodeKind::Lod;
        lod.lod = RenderCore::ModelLodSemantic{};
        payload.nodes.push_back(lod);

        RenderCore::ModelNodeRecord child;
        child.parent = RenderCore::ModelNodeIndex{ 0u };
        child.flags = RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::CollisionOnly);
        payload.nodes.push_back(child);
        payload.roots.push_back(RenderCore::ModelNodeIndex{ 0u });

        EXPECT_FALSE(RenderCore::validModelPayloadStructure(payload));

        payload.nodes[1].flags |= RenderCore::modelNodeFlag(RenderCore::ModelNodeFlag::Collision);
        auto& lodSemantic = *payload.nodes[0].lod;
        lodSemantic.ranges.push_back(RenderCore::ModelLodRange{ .child = RenderCore::ModelNodeIndex{ 1u } });
        EXPECT_TRUE(RenderCore::validModelPayloadStructure(payload));
    }

    TEST(RenderCoreModelAssets, CompoundModelOwnsDependenciesAndInstanceUsesOneAssetPath)
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto model = world.reserveModel();
        const auto instance = world.reserveInstance();
        ASSERT_TRUE(mesh && material && model && instance);
        ASSERT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/model.nif" }));
        ASSERT_TRUE(world.commit(*material, RenderCore::MaterialRecord{ .sourceIdentity = "material:model" }));
        ASSERT_TRUE(world.commit(*model,
            RenderCore::ModelRecord{ .sourceIdentity = "meshes/model.nif", .payload = makeSimpleModel(*mesh, *material) }));

        RenderCore::InstanceRecord both;
        both.mesh = *mesh;
        both.model = *model;
        EXPECT_FALSE(world.commit(*instance, both));

        RenderCore::InstanceRecord compound;
        compound.model = *model;
        ASSERT_TRUE(world.commit(*instance, compound));
        EXPECT_FALSE(world.retire(*model));
        EXPECT_FALSE(world.retire(*mesh));
        EXPECT_FALSE(world.retire(*material));
        EXPECT_TRUE(world.valid());

        EXPECT_TRUE(world.retire(*instance));
        EXPECT_TRUE(world.retire(*model));
        EXPECT_TRUE(world.retire(*mesh));
        EXPECT_TRUE(world.retire(*material));
    }

    TEST(RenderCoreModelAssets, ModelAndDependenciesPublishAtomicallyInOneBatch)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto model = world.reserveModel();
        const auto instance = world.reserveInstance();
        ASSERT_TRUE(mesh && material && model && instance);

        RenderCore::InstanceRecord instanceRecord;
        instanceRecord.model = *model;

        RenderCore::RenderWorldUpdateBatch batch(
            world.epoch(), RenderCore::InitialUpdateSequence, "test:model-asset-create");
        ASSERT_TRUE(batch.add(RenderCore::CreateMesh{
            *mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/model.nif" } }));
        ASSERT_TRUE(batch.add(RenderCore::CreateMaterial{
            *material, RenderCore::MaterialRecord{ .sourceIdentity = "material:model" } }));
        ASSERT_TRUE(batch.add(RenderCore::CreateModel{ *model,
            RenderCore::ModelRecord{ .sourceIdentity = "meshes/model.nif", .payload = makeSimpleModel(*mesh, *material) } }));
        ASSERT_TRUE(batch.add(RenderCore::CreateInstance{ *instance, instanceRecord }));
        ASSERT_TRUE(batch.seal());

        EXPECT_EQ(publisher.apply(batch), RenderCore::PublishStatus::Applied);
        EXPECT_NE(world.get(*mesh), nullptr);
        EXPECT_NE(world.get(*material), nullptr);
        EXPECT_NE(world.get(*model), nullptr);
        EXPECT_NE(world.get(*instance), nullptr);
        EXPECT_TRUE(world.valid());
    }
}
