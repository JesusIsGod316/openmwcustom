#include <components/render/backend/vsg/residencyledger.hpp>
#include <components/rendercore/framerenderstate.hpp>
#include <components/rendercore/renderworld.hpp>

#include <cstdint>
#include <iostream>
#include <limits>

namespace
{
    bool require(bool condition, const char* message)
    {
        if (!condition)
            std::cerr << "FAIL: " << message << '\n';
        return condition;
    }

    bool testFrameRenderState()
    {
        RenderCore::FrameRenderStateDesc desc;
        desc.frameId = RenderCore::FrameId{ 7 };
        desc.worldEpoch = RenderCore::WorldEpoch{ 2 };
        desc.renderWorldRevision = RenderCore::RenderWorldRevision{ 13 };
        desc.historyEpoch = RenderCore::HistoryEpoch{ 3 };
        desc.renderExtent = { 1280, 720 };
        desc.outputExtent = { 1920, 1080 };
        desc.historyValid = true;

        RenderCore::FrameView mainView;
        mainView.viewIndex = 0;
        mainView.kind = RenderCore::ViewKind::Main;
        mainView.extent = desc.renderExtent;
        mainView.temporal = true;
        mainView.historyValid = true;
        mainView.historyEpoch = desc.historyEpoch;
        desc.views.push_back(mainView);

        RenderCore::DynamicTransformState dynamic;
        dynamic.instance = RenderCore::InstanceHandle::fromParts(4u, 9u);
        dynamic.historyValid = true;
        desc.dynamicTransforms.push_back(dynamic);

        const RenderCore::FrameRenderState frame(std::move(desc));
        return require(frame.valid(), "valid immutable frame snapshot rejected")
            && require(frame.frameId() == RenderCore::FrameId{ 7 }, "frame identity changed")
            && require(frame.renderExtent() == RenderCore::Extent2D{ 1280, 720 }, "render extent changed")
            && require(frame.outputExtent() == RenderCore::Extent2D{ 1920, 1080 }, "output extent changed")
            && require(frame.views().size() == 1, "main view missing")
            && require(frame.dynamicTransforms().size() == 1, "dynamic transform missing");
    }

    bool testDuplicateViewRejection()
    {
        RenderCore::FrameRenderStateDesc desc;
        desc.renderExtent = { 640, 480 };
        desc.outputExtent = desc.renderExtent;

        RenderCore::FrameView first;
        first.viewIndex = 2;
        first.extent = desc.renderExtent;
        RenderCore::FrameView duplicate = first;
        duplicate.kind = RenderCore::ViewKind::Shadow;
        desc.views = { first, duplicate };

        const RenderCore::FrameRenderState frame(std::move(desc));
        return require(!frame.valid(), "duplicate frame-local view index was accepted");
    }

    bool testNonFiniteFrameRejection()
    {
        RenderCore::FrameRenderStateDesc desc;
        desc.renderExtent = { 640, 480 };
        desc.outputExtent = desc.renderExtent;

        RenderCore::FrameView view;
        view.extent = desc.renderExtent;
        view.lodScale = std::numeric_limits<float>::quiet_NaN();
        desc.views.push_back(view);

        const RenderCore::FrameRenderState frame(std::move(desc));
        return require(!frame.valid(), "non-finite frame/view state was accepted");
    }

    bool populateChunkedInstance(RenderCore::RenderWorld& world, RenderCore::MeshHandle& mesh,
        RenderCore::ChunkHandle& chunk, RenderCore::InstanceHandle& instance)
    {
        const auto reservedMesh = world.reserveMesh();
        const auto reservedChunk = world.reserveChunk();
        const auto reservedInstance = world.reserveInstance();
        if (!require(reservedMesh.has_value(), "mesh reservation failed")
            || !require(reservedChunk.has_value(), "chunk reservation failed")
            || !require(reservedInstance.has_value(), "instance reservation failed"))
            return false;

        RenderCore::MeshRecord meshRecord;
        meshRecord.sourceIdentity = "meshes/chunked.nif";
        RenderCore::ChunkRecord chunkRecord;
        chunkRecord.producerIdentity = "cell:first";
        if (!require(world.commit(*reservedMesh, meshRecord), "mesh commit failed")
            || !require(world.commit(*reservedChunk, chunkRecord), "chunk commit failed"))
            return false;

        RenderCore::InstanceRecord instanceRecord;
        instanceRecord.mesh = *reservedMesh;
        instanceRecord.chunk = *reservedChunk;
        if (!require(world.commit(*reservedInstance, instanceRecord), "instance commit failed"))
            return false;

        mesh = *reservedMesh;
        chunk = *reservedChunk;
        instance = *reservedInstance;
        return require(world.valid(), "basic chunked world invalid after atomic instance publication");
    }

    bool testRenderWorldDerivedMembership()
    {
        RenderCore::RenderWorld world;
        RenderCore::MeshHandle mesh;
        RenderCore::ChunkHandle firstChunk;
        RenderCore::InstanceHandle instance;
        if (!populateChunkedInstance(world, mesh, firstChunk, instance))
            return false;

        const RenderCore::ChunkRecord* first = world.get(firstChunk);
        if (!require(first && first->members.size() == 1 && first->members.front() == instance,
                "instance publication did not derive chunk membership"))
            return false;

        const auto invalidChunk = world.reserveChunk();
        if (!require(invalidChunk.has_value(), "invalid chunk reservation failed"))
            return false;
        RenderCore::ChunkRecord callerAuthored;
        callerAuthored.members.push_back(instance);
        if (!require(!world.commit(*invalidChunk, callerAuthored), "caller-authored chunk membership was accepted"))
            return false;
        if (!require(world.cancel(*invalidChunk), "failed chunk reservation was not cancellable"))
            return false;

        const auto secondChunk = world.reserveChunk();
        if (!require(secondChunk.has_value(), "second chunk reservation failed"))
            return false;
        RenderCore::ChunkRecord secondRecord;
        secondRecord.producerIdentity = "cell:second";
        if (!require(world.commit(*secondChunk, secondRecord), "second chunk commit failed"))
            return false;

        const auto revisionBeforeReparent = world.revision();
        if (!require(world.reparentInstance(instance, *secondChunk), "instance reparent failed")
            || !require(world.revision() > revisionBeforeReparent, "reparent did not publish a world revision")
            || !require(world.get(firstChunk) && world.get(firstChunk)->members.empty(),
                "old chunk retained reparented member")
            || !require(world.get(*secondChunk) && world.get(*secondChunk)->members.size() == 1
                    && world.get(*secondChunk)->members.front() == instance,
                "new chunk did not receive reparented member")
            || !require(world.valid(), "world invalid after atomic reparent"))
            return false;

        const auto noOpRevision = world.revision();
        if (!require(world.reparentInstance(instance, *secondChunk), "same-chunk reparent rejected")
            || !require(world.revision() == noOpRevision, "same-chunk reparent changed revision")
            || !require(world.get(*secondChunk)->members.size() == 1, "same-chunk reparent duplicated membership"))
            return false;

        RenderCore::InstanceRecord illegalGenericMove = *world.get(instance);
        illegalGenericMove.chunk = firstChunk;
        if (!require(!world.update(instance, illegalGenericMove), "generic instance update changed semantic chunk ownership"))
            return false;

        RenderCore::ChunkRecord illegalMemberUpdate = *world.get(*secondChunk);
        illegalMemberUpdate.revision = RenderCore::ResourceRevision{ 2 };
        illegalMemberUpdate.members.clear();
        if (!require(!world.update(*secondChunk, illegalMemberUpdate), "generic chunk update changed derived membership"))
            return false;

        if (!require(!world.retire(*secondChunk), "chunk with a live member retired")
            || !require(world.retire(instance), "instance retirement failed")
            || !require(world.get(*secondChunk) && world.get(*secondChunk)->members.empty(),
                "instance retirement did not remove derived membership")
            || !require(world.retire(*secondChunk), "empty second chunk retirement failed")
            || !require(world.retire(firstChunk), "empty first chunk retirement failed")
            || !require(world.retire(mesh), "unreferenced mesh retirement failed")
            || !require(world.valid(), "world invalid after relationship retirement"))
            return false;

        return true;
    }

    bool testRenderWorldResourceRevisionAndStaleGeneration()
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto instance = world.reserveInstance();
        if (!require(mesh.has_value(), "revision mesh reservation failed")
            || !require(material.has_value(), "material reservation failed")
            || !require(instance.has_value(), "revision instance reservation failed"))
            return false;

        RenderCore::MeshRecord meshRecord;
        meshRecord.sourceIdentity = "meshes/test.nif";
        RenderCore::MaterialRecord materialRecord;
        materialRecord.sourceIdentity = "materials/test";
        if (!require(world.commit(*mesh, meshRecord), "revision mesh commit failed")
            || !require(world.commit(*material, materialRecord), "material commit failed"))
            return false;

        RenderCore::InstanceRecord instanceRecord;
        instanceRecord.mesh = *mesh;
        instanceRecord.materials.push_back(*material);
        if (!require(world.commit(*instance, instanceRecord), "revision instance commit failed")
            || !require(!world.retire(*mesh), "referenced mesh retirement was accepted")
            || !require(!world.retire(*material), "referenced material retirement was accepted"))
            return false;

        RenderCore::MeshRecord staleUpdate = *world.get(*mesh);
        if (!require(!world.update(*mesh, staleUpdate), "same-revision mesh update was accepted"))
            return false;

        RenderCore::MeshRecord nextUpdate = *world.get(*mesh);
        nextUpdate.revision = RenderCore::ResourceRevision{ 2 };
        nextUpdate.surfaceCount = 3;
        if (!require(world.update(*mesh, nextUpdate), "higher-revision mesh update failed")
            || !require(world.get(*mesh) && world.get(*mesh)->surfaceCount == 3, "mesh update payload missing"))
            return false;

        const RenderCore::InstanceHandle staleInstance = *instance;
        if (!require(world.retire(*instance), "revision instance retirement failed")
            || !require(world.retire(*material), "material retirement after unlink failed")
            || !require(world.retire(*mesh), "mesh retirement after unlink failed"))
            return false;

        const auto replacementMesh = world.reserveMesh();
        if (!require(replacementMesh.has_value(), "replacement mesh reservation failed")
            || !require(replacementMesh->slot() == mesh->slot(), "lowest mesh slot was not reused")
            || !require(replacementMesh->generation() != mesh->generation(), "mesh generation did not advance"))
            return false;

        RenderCore::MeshRecord replacementRecord;
        replacementRecord.sourceIdentity = "meshes/replacement.nif";
        if (!require(world.commit(*replacementMesh, replacementRecord), "replacement mesh commit failed")
            || !require(world.get(*mesh) == nullptr, "stale mesh handle aliased replacement"))
            return false;

        const auto replacementInstance = world.reserveInstance();
        if (!require(replacementInstance.has_value(), "replacement instance reservation failed")
            || !require(replacementInstance->slot() == staleInstance.slot(), "lowest instance slot was not reused")
            || !require(replacementInstance->generation() != staleInstance.generation(),
                "instance generation did not advance"))
            return false;

        RenderCore::InstanceRecord replacementInstanceRecord;
        replacementInstanceRecord.mesh = *replacementMesh;
        if (!require(world.commit(*replacementInstance, replacementInstanceRecord), "replacement instance commit failed"))
            return false;

        const auto chunk = world.reserveChunk();
        if (!require(chunk.has_value(), "stale-reparent chunk reservation failed"))
            return false;
        RenderCore::ChunkRecord chunkRecord;
        if (!require(world.commit(*chunk, chunkRecord), "stale-reparent chunk commit failed"))
            return false;

        const auto revisionBeforeStale = world.revision();
        return require(!world.reparentInstance(staleInstance, *chunk), "stale instance reparented replacement")
            && require(world.revision() == revisionBeforeStale, "stale reparent changed world revision")
            && require(world.valid(), "world invalid after stale-generation rejection");
    }

    bool testResidencyRetirement()
    {
        using RenderVsg::ResidencyCategory;
        RenderVsg::ResidencyLedger ledger;

        constexpr std::uint64_t mib = 1024u * 1024u;
        constexpr std::uint64_t bytes = 4u * mib;
        if (!require(ledger.addLogicalLive(ResidencyCategory::MeshVertexIndex, bytes), "logical mesh accounting failed")
            || !require(ledger.beginUpload(ResidencyCategory::MeshVertexIndex, bytes), "pending upload accounting failed")
            || !require(ledger.commitUpload(ResidencyCategory::MeshVertexIndex, bytes, false), "resident upload commit failed"))
            return false;

        auto snapshot = ledger.snapshot();
        if (!require(snapshot.total.logicalLiveBytes == bytes, "logical-live total mismatch")
            || !require(snapshot.total.residentBytes == bytes, "resident total mismatch")
            || !require(snapshot.total.evictableBytes == bytes, "evictable total mismatch")
            || !require(snapshot.total.pendingUploadBytes == 0, "completed upload remained pending"))
            return false;

        constexpr std::uint64_t firstRetire = 3u * mib;
        if (!require(ledger.queueRetire(
                         ResidencyCategory::MeshVertexIndex, firstRetire, false, RenderCore::FrameId{ 12 }),
                "frame-delayed retirement queue failed")
            || !require(!ledger.queueRetire(
                            ResidencyCategory::MeshVertexIndex, 2u * mib, false, RenderCore::FrameId{ 12 }),
                "overcommitted retirement was accepted"))
            return false;

        snapshot = ledger.snapshot();
        if (!require(snapshot.total.pendingRetireBytes == firstRetire,
                "rejected retirement mutated pending-retire accounting")
            || !require(snapshot.total.residentBytes == bytes, "queued retirement released bytes too early")
            || !require(ledger.collectRetired(RenderCore::FrameId{ 11 }) == 0, "resource retired before last-use frame")
            || !require(ledger.pendingRetirementCount() == 1, "retirement ticket disappeared early")
            || !require(ledger.collectRetired(RenderCore::FrameId{ 12 }) == firstRetire,
                "resource did not retire at completion frame"))
            return false;

        snapshot = ledger.snapshot();
        if (!require(snapshot.total.residentBytes == mib, "partial retirement resident total mismatch")
            || !require(snapshot.total.pendingRetireBytes == 0, "completed retirement remained pending")
            || !require(snapshot.total.evictableBytes == mib, "partial retirement evictable total mismatch"))
            return false;

        if (!require(ledger.queueRetire(
                         ResidencyCategory::MeshVertexIndex, mib, false, RenderCore::FrameId{ 13 }),
                "final retirement queue failed")
            || !require(ledger.collectRetired(RenderCore::FrameId{ 13 }) == mib, "final retirement collection failed"))
            return false;

        snapshot = ledger.snapshot();
        if (!require(snapshot.total.residentBytes == 0, "retired bytes remained resident")
            || !require(snapshot.total.pendingRetireBytes == 0, "retired bytes remained pending")
            || !require(snapshot.total.evictableBytes == 0, "retired evictable bytes remained charged")
            || !require(snapshot.total.logicalLiveBytes == bytes,
                "backend retirement incorrectly invalidated logical resource lifetime"))
            return false;

        return require(ledger.removeLogicalLive(ResidencyCategory::MeshVertexIndex, bytes), "logical retirement failed")
            && require(ledger.snapshot().total.logicalLiveBytes == 0, "logical retirement total mismatch");
    }
}

int main()
{
    const bool pass = testFrameRenderState() && testDuplicateViewRejection() && testNonFiniteFrameRejection()
        && testRenderWorldDerivedMembership() && testRenderWorldResourceRevisionAndStaleGeneration()
        && testResidencyRetirement();
    std::cout << (pass ? "V4 CP2 foundation contract tests: PASS\n" : "V4 CP2 foundation contract tests: FAIL\n");
    return pass ? 0 : 1;
}
