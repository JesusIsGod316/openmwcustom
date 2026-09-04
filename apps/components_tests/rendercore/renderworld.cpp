#include <components/rendercore/renderworld.hpp>

#include <gtest/gtest.h>

namespace
{
    struct BasicWorldHandles
    {
        RenderCore::MeshHandle mesh;
        RenderCore::ChunkHandle chunk;
        RenderCore::InstanceHandle instance;
    };

    BasicWorldHandles populateBasicWorld(RenderCore::RenderWorld& world)
    {
        const auto mesh = world.reserveMesh();
        EXPECT_TRUE(mesh);
        if (!mesh)
            return {};
        EXPECT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/example.nif", .surfaceCount = 1 }));

        const auto chunk = world.reserveChunk();
        EXPECT_TRUE(chunk);
        if (!chunk)
            return {};
        EXPECT_TRUE(world.commit(*chunk, RenderCore::ChunkRecord{ .producerIdentity = "cell:example" }));

        const auto instance = world.reserveInstance();
        EXPECT_TRUE(instance);
        if (!instance)
            return {};
        RenderCore::InstanceRecord record;
        record.chunk = *chunk;
        record.mesh = *mesh;
        record.transform.translation = { 100.0, 200.0, 300.0 };
        EXPECT_TRUE(world.commit(*instance, std::move(record)));

        return { *mesh, *chunk, *instance };
    }

    TEST(RenderWorld, ReservationsDoNotChangePublishedRevision)
    {
        RenderCore::RenderWorld world;
        const auto initialRevision = world.revision();
        const auto mesh = world.reserveMesh();
        ASSERT_TRUE(mesh);

        EXPECT_EQ(world.revision(), initialRevision);
        EXPECT_EQ(world.get(*mesh), nullptr);

        ASSERT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/a.nif" }));
        EXPECT_GT(world.revision(), initialRevision);
        ASSERT_NE(world.get(*mesh), nullptr);
        EXPECT_EQ(world.get(*mesh)->sourceIdentity, "meshes/a.nif");
    }

    TEST(RenderWorld, InstanceCommitRequiresLiveNeutralDependencies)
    {
        RenderCore::RenderWorld world;
        const auto instance = world.reserveInstance();
        ASSERT_TRUE(instance);

        RenderCore::InstanceRecord invalid;
        invalid.mesh = RenderCore::MeshHandle::fromParts(0u, 1u);
        EXPECT_FALSE(world.commit(*instance, invalid));
        EXPECT_EQ(world.get(*instance), nullptr);

        const auto mesh = world.reserveMesh();
        ASSERT_TRUE(mesh);
        ASSERT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/a.nif" }));

        invalid.mesh = *mesh;
        EXPECT_TRUE(world.commit(*instance, invalid));
        EXPECT_NE(world.get(*instance), nullptr);
    }

    TEST(RenderWorld, RetiredInstanceCannotAliasReplacement)
    {
        RenderCore::RenderWorld world;
        const BasicWorldHandles first = populateBasicWorld(world);
        ASSERT_TRUE(first.instance.valid());

        ASSERT_TRUE(world.retire(first.instance));
        EXPECT_EQ(world.get(first.instance), nullptr);

        const auto replacement = world.reserveInstance();
        ASSERT_TRUE(replacement);
        EXPECT_EQ(replacement->slot(), first.instance.slot());
        EXPECT_EQ(replacement->generation(), first.instance.generation() + 1u);

        RenderCore::InstanceRecord record;
        record.mesh = first.mesh;
        ASSERT_TRUE(world.commit(*replacement, record));
        EXPECT_EQ(world.get(first.instance), nullptr);
        EXPECT_NE(world.get(*replacement), nullptr);
    }

    TEST(RenderWorld, DestructiveResetAdvancesEpochAndInvalidatesOldState)
    {
        RenderCore::RenderWorld world;
        const BasicWorldHandles old = populateBasicWorld(world);
        ASSERT_TRUE(old.mesh.valid());
        const auto oldEpoch = world.epoch();
        const auto oldRevision = world.revision();

        ASSERT_TRUE(world.reset());
        EXPECT_GT(world.epoch(), oldEpoch);
        EXPECT_GT(world.revision(), oldRevision);
        EXPECT_EQ(world.get(old.mesh), nullptr);
        EXPECT_EQ(world.get(old.chunk), nullptr);
        EXPECT_EQ(world.get(old.instance), nullptr);

        const auto newMesh = world.reserveMesh();
        ASSERT_TRUE(newMesh);
        EXPECT_EQ(newMesh->slot(), old.mesh.slot());
        EXPECT_EQ(newMesh->generation(), old.mesh.generation() + 1u);
    }

    TEST(RenderWorld, IdenticalOrderedPopulationProducesIdenticalLogicalHandles)
    {
        RenderCore::RenderWorld first;
        RenderCore::RenderWorld second;

        const BasicWorldHandles a = populateBasicWorld(first);
        const BasicWorldHandles b = populateBasicWorld(second);
        ASSERT_TRUE(a.mesh.valid() && a.chunk.valid() && a.instance.valid());
        ASSERT_TRUE(b.mesh.valid() && b.chunk.valid() && b.instance.valid());

        EXPECT_EQ(a.mesh, b.mesh);
        EXPECT_EQ(a.chunk, b.chunk);
        EXPECT_EQ(a.instance, b.instance);
        EXPECT_EQ(first.epoch(), second.epoch());
        EXPECT_EQ(first.revision(), second.revision());
    }

    TEST(RenderWorld, InstanceDefaultsKeepOwnerSpecificVisibilityExpressible)
    {
        RenderCore::InstanceRecord ordinary;
        EXPECT_NE(ordinary.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::OrdinaryWorld), 0u);
        EXPECT_NE(ordinary.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster), 0u);

        RenderCore::InstanceRecord ownerHead;
        ownerHead.semanticFlags = RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::OwnerHead)
            | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster);
        EXPECT_EQ(ownerHead.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::OrdinaryWorld), 0u);
        EXPECT_NE(ownerHead.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::OwnerHead), 0u);
        EXPECT_NE(ownerHead.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster), 0u);
    }
}
