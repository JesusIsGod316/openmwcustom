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
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderWorld, InstanceCommitMaintainsDerivedChunkMembership)
    {
        RenderCore::RenderWorld world;
        const BasicWorldHandles handles = populateBasicWorld(world);
        ASSERT_TRUE(handles.instance.valid());

        const RenderCore::ChunkRecord* chunk = world.get(handles.chunk);
        ASSERT_NE(chunk, nullptr);
        ASSERT_EQ(chunk->members.size(), 1u);
        EXPECT_EQ(chunk->members.front(), handles.instance);

        const RenderCore::InstanceRecord* instance = world.get(handles.instance);
        ASSERT_NE(instance, nullptr);
        ASSERT_TRUE(instance->chunk);
        EXPECT_EQ(*instance->chunk, handles.chunk);
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderWorld, ChunkCreateRejectsCallerAuthoredIndividualMembership)
    {
        RenderCore::RenderWorld world;
        const auto chunk = world.reserveChunk();
        ASSERT_TRUE(chunk);

        RenderCore::ChunkRecord invalid;
        invalid.producerIdentity = "cell:invalid";
        invalid.members.push_back(RenderCore::InstanceHandle::fromParts(0u, 1u));
        const auto revisionBefore = world.revision();

        EXPECT_FALSE(world.commit(*chunk, std::move(invalid)));
        EXPECT_EQ(world.revision(), revisionBefore);
        EXPECT_EQ(world.get(*chunk), nullptr);
        EXPECT_TRUE(world.cancel(*chunk));
    }

    TEST(RenderWorld, ReparentMovesDerivedMembershipExactlyOnce)
    {
        RenderCore::RenderWorld world;
        const BasicWorldHandles handles = populateBasicWorld(world);
        ASSERT_TRUE(handles.instance.valid());

        const auto secondChunk = world.reserveChunk();
        ASSERT_TRUE(secondChunk);
        ASSERT_TRUE(world.commit(*secondChunk, RenderCore::ChunkRecord{ .producerIdentity = "cell:second" }));
        const auto revisionBefore = world.revision();

        ASSERT_TRUE(world.reparentInstance(handles.instance, *secondChunk));
        EXPECT_GT(world.revision(), revisionBefore);

        const RenderCore::ChunkRecord* oldChunk = world.get(handles.chunk);
        const RenderCore::ChunkRecord* newChunk = world.get(*secondChunk);
        const RenderCore::InstanceRecord* instance = world.get(handles.instance);
        ASSERT_NE(oldChunk, nullptr);
        ASSERT_NE(newChunk, nullptr);
        ASSERT_NE(instance, nullptr);
        EXPECT_TRUE(oldChunk->members.empty());
        ASSERT_EQ(newChunk->members.size(), 1u);
        EXPECT_EQ(newChunk->members.front(), handles.instance);
        ASSERT_TRUE(instance->chunk);
        EXPECT_EQ(*instance->chunk, *secondChunk);
        EXPECT_TRUE(world.valid());

        const auto noOpRevision = world.revision();
        EXPECT_TRUE(world.reparentInstance(handles.instance, *secondChunk));
        EXPECT_EQ(world.revision(), noOpRevision);
        EXPECT_EQ(world.get(*secondChunk)->members.size(), 1u);
    }

    TEST(RenderWorld, GenericUpdatesCannotBypassChunkOwnership)
    {
        RenderCore::RenderWorld world;
        const BasicWorldHandles handles = populateBasicWorld(world);
        ASSERT_TRUE(handles.instance.valid());

        const auto secondChunk = world.reserveChunk();
        ASSERT_TRUE(secondChunk);
        ASSERT_TRUE(world.commit(*secondChunk, RenderCore::ChunkRecord{ .producerIdentity = "cell:second" }));

        RenderCore::InstanceRecord moved = *world.get(handles.instance);
        moved.chunk = *secondChunk;
        const auto revisionBeforeInstance = world.revision();
        EXPECT_FALSE(world.update(handles.instance, std::move(moved)));
        EXPECT_EQ(world.revision(), revisionBeforeInstance);
        EXPECT_EQ(*world.get(handles.instance)->chunk, handles.chunk);

        RenderCore::ChunkRecord editedMembers = *world.get(handles.chunk);
        editedMembers.revision = RenderCore::ResourceRevision{ 2 };
        editedMembers.members.clear();
        const auto revisionBeforeChunk = world.revision();
        EXPECT_FALSE(world.update(handles.chunk, std::move(editedMembers)));
        EXPECT_EQ(world.revision(), revisionBeforeChunk);
        ASSERT_EQ(world.get(handles.chunk)->members.size(), 1u);
        EXPECT_EQ(world.get(handles.chunk)->members.front(), handles.instance);
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderWorld, InstanceRetireRemovesDerivedMembershipBeforeChunkRetire)
    {
        RenderCore::RenderWorld world;
        const BasicWorldHandles handles = populateBasicWorld(world);
        ASSERT_TRUE(handles.instance.valid());

        const auto revisionBeforeRejectedRetire = world.revision();
        EXPECT_FALSE(world.retire(handles.chunk));
        EXPECT_EQ(world.revision(), revisionBeforeRejectedRetire);
        EXPECT_NE(world.get(handles.chunk), nullptr);

        ASSERT_TRUE(world.retire(handles.instance));
        ASSERT_NE(world.get(handles.chunk), nullptr);
        EXPECT_TRUE(world.get(handles.chunk)->members.empty());

        EXPECT_TRUE(world.retire(handles.chunk));
        EXPECT_EQ(world.get(handles.chunk), nullptr);
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderWorld, ReferencedLogicalResourcesCannotRetireIndependently)
    {
        RenderCore::RenderWorld world;

        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto skeleton = world.reserveSkeleton();
        const auto instance = world.reserveInstance();
        ASSERT_TRUE(mesh && material && skeleton && instance);
        ASSERT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/a.nif" }));
        ASSERT_TRUE(world.commit(*material, RenderCore::MaterialRecord{ .sourceIdentity = "materials/a" }));
        ASSERT_TRUE(world.commit(*skeleton, RenderCore::SkeletonRecord{ .sourceIdentity = "skeletons/a" }));

        RenderCore::InstanceRecord record;
        record.mesh = *mesh;
        record.materials.push_back(*material);
        record.skeleton = *skeleton;
        ASSERT_TRUE(world.commit(*instance, std::move(record)));

        const auto revisionBefore = world.revision();
        EXPECT_FALSE(world.retire(*mesh));
        EXPECT_FALSE(world.retire(*material));
        EXPECT_FALSE(world.retire(*skeleton));
        EXPECT_EQ(world.revision(), revisionBefore);
        EXPECT_NE(world.get(*mesh), nullptr);
        EXPECT_NE(world.get(*material), nullptr);
        EXPECT_NE(world.get(*skeleton), nullptr);

        ASSERT_TRUE(world.retire(*instance));
        EXPECT_TRUE(world.retire(*mesh));
        EXPECT_TRUE(world.retire(*material));
        EXPECT_TRUE(world.retire(*skeleton));
        EXPECT_TRUE(world.valid());
    }

    TEST(RenderWorld, VersionedResourceUpdatePreservesLogicalHandle)
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        ASSERT_TRUE(mesh);
        ASSERT_TRUE(world.commit(*mesh, RenderCore::MeshRecord{ .sourceIdentity = "meshes/a.nif", .surfaceCount = 1 }));

        const auto revisionBefore = world.revision();
        RenderCore::MeshRecord sameRevision = *world.get(*mesh);
        sameRevision.surfaceCount = 2;
        EXPECT_FALSE(world.update(*mesh, sameRevision));
        EXPECT_EQ(world.revision(), revisionBefore);
        EXPECT_EQ(world.get(*mesh)->surfaceCount, 1u);

        RenderCore::MeshRecord nextRevision = *world.get(*mesh);
        nextRevision.revision = RenderCore::ResourceRevision{ 2 };
        nextRevision.surfaceCount = 2;
        EXPECT_TRUE(world.update(*mesh, std::move(nextRevision)));
        EXPECT_GT(world.revision(), revisionBefore);
        EXPECT_EQ(world.get(*mesh)->surfaceCount, 2u);
        EXPECT_EQ(world.get(*mesh)->revision, RenderCore::ResourceRevision{ 2 });
    }

    TEST(RenderWorld, StaleGenerationCannotReparentReplacement)
    {
        RenderCore::RenderWorld world;
        const BasicWorldHandles first = populateBasicWorld(world);
        ASSERT_TRUE(first.instance.valid());
        ASSERT_TRUE(world.retire(first.instance));

        const auto replacement = world.reserveInstance();
        ASSERT_TRUE(replacement);
        RenderCore::InstanceRecord record;
        record.chunk = first.chunk;
        record.mesh = first.mesh;
        ASSERT_TRUE(world.commit(*replacement, std::move(record)));

        const auto secondChunk = world.reserveChunk();
        ASSERT_TRUE(secondChunk);
        ASSERT_TRUE(world.commit(*secondChunk, RenderCore::ChunkRecord{ .producerIdentity = "cell:second" }));
        const auto revisionBefore = world.revision();

        EXPECT_FALSE(world.reparentInstance(first.instance, *secondChunk));
        EXPECT_EQ(world.revision(), revisionBefore);
        ASSERT_EQ(world.get(first.chunk)->members.size(), 1u);
        EXPECT_EQ(world.get(first.chunk)->members.front(), *replacement);
        EXPECT_TRUE(world.get(*secondChunk)->members.empty());
        EXPECT_TRUE(world.valid());
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
        EXPECT_TRUE(world.valid());

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
        ASSERT_NE(first.get(a.chunk), nullptr);
        ASSERT_NE(second.get(b.chunk), nullptr);
        EXPECT_EQ(first.get(a.chunk)->members, second.get(b.chunk)->members);
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
