#include <components/rendercore/framerenderstate.hpp>
#include <components/rendercore/records.hpp>
#include <components/rendercore/renderworld.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <memory>

namespace
{
    TEST(RenderCoreSemanticAssets, MaterialOwnsLogicalTextureDependency)
    {
        RenderCore::RenderWorld world;
        const auto texture = world.reserveTexture();
        const auto material = world.reserveMaterial();
        ASSERT_TRUE(texture && material);
        ASSERT_TRUE(world.commit(*texture, RenderCore::TextureRecord{
            .sourceIdentity = "textures/stone.dds", .contentIdentity = "test:texture-stone" }));

        RenderCore::MaterialRecord materialRecord;
        materialRecord.sourceIdentity = "material:stone";
        materialRecord.textures.push_back(RenderCore::TextureBinding{ .role = RenderCore::TextureRole::Diffuse, .texture = *texture });
        ASSERT_TRUE(world.commit(*material, std::move(materialRecord)));

        EXPECT_FALSE(world.retire(*texture));
        EXPECT_TRUE(world.retire(*material));
        EXPECT_TRUE(world.retire(*texture));
    }

    TEST(RenderCoreSemanticAssets, OneImageSupportsDistinctBindingInterpretations)
    {
        RenderCore::RenderWorld world;
        const auto texture = world.reserveTexture();
        const auto colorMaterial = world.reserveMaterial();
        const auto dataMaterial = world.reserveMaterial();
        ASSERT_TRUE(texture && colorMaterial && dataMaterial);
        ASSERT_TRUE(world.commit(*texture, RenderCore::TextureRecord{
            .sourceIdentity = "textures/shared.dds", .contentIdentity = "test:texture-shared" }));

        RenderCore::MaterialRecord color;
        color.sourceIdentity = "material:color";
        color.textures.push_back(RenderCore::TextureBinding{
            .role = RenderCore::TextureRole::Diffuse,
            .texture = *texture,
            .colorSpace = RenderCore::TextureColorSpace::Srgb,
            .formatClass = RenderCore::TextureFormatClass::Color });
        ASSERT_TRUE(world.commit(*colorMaterial, color));

        RenderCore::MaterialRecord data;
        data.sourceIdentity = "material:data";
        data.textures.push_back(RenderCore::TextureBinding{
            .role = RenderCore::TextureRole::Normal,
            .texture = *texture,
            .colorSpace = RenderCore::TextureColorSpace::Data,
            .formatClass = RenderCore::TextureFormatClass::Normal });
        ASSERT_TRUE(world.commit(*dataMaterial, data));

        EXPECT_EQ(world.get(*colorMaterial)->textures.front().texture, world.get(*dataMaterial)->textures.front().texture);
        EXPECT_NE(world.get(*colorMaterial)->textures.front().colorSpace,
            world.get(*dataMaterial)->textures.front().colorSpace);
    }

    TEST(RenderCoreSemanticAssets, TexturePublicationRequiresContentIdentity)
    {
        RenderCore::RenderWorld world;
        const auto texture = world.reserveTexture();
        ASSERT_TRUE(texture);

        EXPECT_FALSE(world.commit(*texture,
            RenderCore::TextureRecord{ .sourceIdentity = "textures/source-only.dds" }));
        EXPECT_EQ(world.get(*texture), nullptr);
        EXPECT_TRUE(world.cancel(*texture));

        const auto resolved = world.reserveTexture();
        ASSERT_TRUE(resolved);
        EXPECT_TRUE(world.commit(*resolved, RenderCore::TextureRecord{
            .sourceIdentity = "textures/resolved.dds", .contentIdentity = "openmw128:resolved" }));
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderCoreSemanticAssets, MeshPayloadRejectsMismatchedVertexStreams)
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        ASSERT_TRUE(mesh);

        auto payload = std::make_shared<RenderCore::MeshPayload>();
        payload->positions.resize(3);
        payload->normals.resize(2);
        payload->indices = { 0, 1, 2 };
        payload->surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3 });

        RenderCore::MeshRecord record;
        record.sourceIdentity = "meshes/bad.nif";
        record.surfaceCount = 1;
        record.payload = std::move(payload);
        EXPECT_FALSE(world.commit(*mesh, std::move(record)));
        EXPECT_TRUE(world.cancel(*mesh));
    }

    TEST(RenderCoreSemanticAssets, MeshPayloadRejectsOutOfRangeIndex)
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        ASSERT_TRUE(mesh);

        auto payload = std::make_shared<RenderCore::MeshPayload>();
        payload->positions.resize(3);
        payload->indices = { 0, 1, 3 };
        payload->surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3 });

        RenderCore::MeshRecord record;
        record.sourceIdentity = "meshes/bad-index.nif";
        record.surfaceCount = 1;
        record.payload = std::move(payload);
        EXPECT_FALSE(world.commit(*mesh, std::move(record)));
        EXPECT_TRUE(world.cancel(*mesh));
    }

    TEST(RenderCoreSemanticAssets, MeshPayloadRejectsNonFiniteVertexData)
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        ASSERT_TRUE(mesh);

        auto payload = std::make_shared<RenderCore::MeshPayload>();
        payload->positions = {
            { 0.0f, 0.0f, 0.0f },
            { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
        };
        payload->indices = { 0, 1, 2 };
        payload->surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3 });

        RenderCore::MeshRecord record;
        record.sourceIdentity = "meshes/nonfinite.nif";
        record.surfaceCount = 1;
        record.payload = std::move(payload);
        EXPECT_FALSE(world.commit(*mesh, std::move(record)));
        EXPECT_TRUE(world.cancel(*mesh));
    }

    TEST(RenderCoreSemanticAssets, SkeletonRequiresParentsBeforeChildren)
    {
        RenderCore::RenderWorld world;
        const auto skeleton = world.reserveSkeleton();
        ASSERT_TRUE(skeleton);

        auto payload = std::make_shared<RenderCore::SkeletonPayload>();
        payload->bones.push_back(RenderCore::BoneRecord{ .name = "root", .parent = -1 });
        payload->bones.push_back(RenderCore::BoneRecord{ .name = "child", .parent = 2 });

        RenderCore::SkeletonRecord record;
        record.sourceIdentity = "skeleton:bad";
        record.payload = std::move(payload);
        EXPECT_FALSE(world.commit(*skeleton, std::move(record)));
        EXPECT_TRUE(world.cancel(*skeleton));
    }

    TEST(RenderCoreSemanticAssets, SkeletonRejectsNonFiniteBindData)
    {
        RenderCore::RenderWorld world;
        const auto skeleton = world.reserveSkeleton();
        ASSERT_TRUE(skeleton);

        auto payload = std::make_shared<RenderCore::SkeletonPayload>();
        RenderCore::BoneRecord root;
        root.name = "root";
        root.parent = -1;
        root.inverseBind[0][0] = std::numeric_limits<float>::infinity();
        payload->bones.push_back(root);

        RenderCore::SkeletonRecord record;
        record.sourceIdentity = "skeleton:nonfinite";
        record.payload = std::move(payload);
        EXPECT_FALSE(world.commit(*skeleton, std::move(record)));
        EXPECT_TRUE(world.cancel(*skeleton));
    }

    TEST(RenderCoreSemanticAssets, AttachmentCycleFailsClosed)
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        ASSERT_TRUE(mesh);
        ASSERT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/a.nif" }));

        const auto parent = world.reserveInstance();
        ASSERT_TRUE(parent);
        RenderCore::InstanceRecord parentRecord;
        parentRecord.mesh = *mesh;
        ASSERT_TRUE(world.commit(*parent, parentRecord));

        const auto child = world.reserveInstance();
        ASSERT_TRUE(child);
        RenderCore::InstanceRecord childRecord;
        childRecord.mesh = *mesh;
        childRecord.attachment = RenderCore::AttachmentBinding{ .parent = *parent };
        ASSERT_TRUE(world.commit(*child, childRecord));

        parentRecord.attachment = RenderCore::AttachmentBinding{ .parent = *child };
        EXPECT_FALSE(world.update(*parent, parentRecord));
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderCoreSemanticAssets, DynamicMaterialOverridesAreFrameLocalAndUnambiguous)
    {
        RenderCore::FrameRenderStateDesc desc;
        desc.renderExtent = { 1920, 1080 };
        desc.outputExtent = { 1920, 1080 };
        RenderCore::DynamicMaterialState material;
        material.material = RenderCore::MaterialHandle::fromParts(4u, 1u);
        material.alpha = 0.5f;
        material.textureTransforms.push_back(RenderCore::DynamicTextureTransformState{ .bindingIndex = 0 });
        desc.dynamicMaterials.push_back(material);
        EXPECT_TRUE(RenderCore::FrameRenderState(desc).valid());

        desc.dynamicMaterials.push_back(material);
        EXPECT_FALSE(RenderCore::FrameRenderState(desc).valid());
    }

    TEST(RenderCoreSemanticAssets, DynamicMaterialRejectsNonFiniteControllerValue)
    {
        RenderCore::FrameRenderStateDesc desc;
        desc.renderExtent = { 1280, 720 };
        desc.outputExtent = { 1280, 720 };
        RenderCore::DynamicMaterialState material;
        material.material = RenderCore::MaterialHandle::fromParts(1u, 1u);
        material.emissiveMultiplier = std::numeric_limits<float>::quiet_NaN();
        desc.dynamicMaterials.push_back(material);
        EXPECT_FALSE(RenderCore::FrameRenderState(std::move(desc)).valid());
    }
}
