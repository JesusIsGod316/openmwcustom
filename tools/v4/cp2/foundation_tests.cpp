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

    bool testRenderWorldRevisionAndRetirement()
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto instance = world.reserveInstance();
        if (!require(mesh.has_value(), "mesh reservation failed")
            || !require(material.has_value(), "material reservation failed")
            || !require(instance.has_value(), "instance reservation failed"))
            return false;

        RenderCore::MeshRecord meshRecord;
        meshRecord.sourceIdentity = "meshes/test.nif";
        RenderCore::MaterialRecord materialRecord;
        materialRecord.sourceIdentity = "materials/test";
        if (!require(world.commit(*mesh, meshRecord), "mesh commit failed")
            || !require(world.commit(*material, materialRecord), "material commit failed"))
            return false;

        RenderCore::InstanceRecord instanceRecord;
        instanceRecord.mesh = *mesh;
        instanceRecord.materials.push_back(*material);
        if (!require(world.commit(*instance, instanceRecord), "instance commit failed")
            || !require(world.valid(), "valid RenderWorld rejected"))
            return false;

        if (!require(!world.retire(*mesh), "referenced mesh retirement was accepted")
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

        if (!require(world.retire(*instance), "unlinked instance retirement failed")
            || !require(world.retire(*material), "material retirement failed")
            || !require(world.retire(*mesh), "mesh retirement failed")
            || !require(world.get(*mesh) == nullptr, "retired mesh remained visible")
            || !require(world.valid(), "empty RenderWorld became invalid"))
            return false;

        const auto replacement = world.reserveMesh();
        if (!require(replacement.has_value(), "replacement mesh reservation failed")
            || !require(replacement->slot() == mesh->slot(), "lowest retired mesh slot was not reused")
            || !require(replacement->generation() != mesh->generation(), "replacement mesh reused stale generation"))
            return false;

        RenderCore::MeshRecord replacementRecord;
        replacementRecord.sourceIdentity = "meshes/replacement.nif";
        return require(world.commit(*replacement, replacementRecord), "replacement mesh commit failed")
            && require(world.get(*mesh) == nullptr, "stale mesh handle aliased replacement")
            && require(world.get(*replacement) != nullptr, "replacement mesh missing");
    }

    bool testRenderWorldChunkIntegrity()
    {
        RenderCore::RenderWorld world;
        const auto mesh = world.reserveMesh();
        const auto chunk = world.reserveChunk();
        if (!require(mesh.has_value(), "chunk test mesh reservation failed")
            || !require(chunk.has_value(), "chunk reservation failed"))
            return false;

        RenderCore::MeshRecord meshRecord;
        meshRecord.sourceIdentity = "meshes/chunk.nif";
        RenderCore::ChunkRecord chunkRecord;
        chunkRecord.producerIdentity = "cell:test";
        if (!require(world.commit(*mesh, meshRecord), "chunk test mesh commit failed")
            || !require(world.commit(*chunk, chunkRecord), "empty chunk commit failed"))
            return false;

        const auto instance = world.reserveInstance();
        if (!require(instance.has_value(), "chunk member reservation failed"))
            return false;

        RenderCore::InstanceRecord instanceRecord;
        instanceRecord.mesh = *mesh;
        instanceRecord.chunk = *chunk;
        if (!require(world.commit(*instance, instanceRecord), "chunk member commit failed"))
            return false;

        RenderCore::ChunkRecord populated = *world.get(*chunk);
        populated.revision = RenderCore::ResourceRevision{ 2 };
        populated.members.push_back(*instance);
        if (!require(world.update(*chunk, populated), "chunk membership update failed")
            || !require(world.valid(), "bidirectional chunk membership rejected")
            || !require(!world.retire(*instance), "chunk-owned instance retirement was accepted")
            || !require(!world.retire(*chunk), "referenced chunk retirement was accepted"))
            return false;

        RenderCore::InstanceRecord detached = *world.get(*instance);
        detached.chunk.reset();
        if (!require(world.update(*instance, detached), "instance chunk detach failed"))
            return false;

        RenderCore::ChunkRecord emptied = *world.get(*chunk);
        emptied.revision = RenderCore::ResourceRevision{ 3 };
        emptied.members.clear();
        if (!require(world.update(*chunk, emptied), "chunk member removal failed")
            || !require(world.valid(), "detached chunk state invalid"))
            return false;

        return require(world.retire(*instance), "detached instance retirement failed")
            && require(world.retire(*chunk), "detached chunk retirement failed")
            && require(world.retire(*mesh), "detached chunk mesh retirement failed");
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
        && testRenderWorldRevisionAndRetirement() && testRenderWorldChunkIntegrity() && testResidencyRetirement();
    std::cout << (pass ? "V4 CP2 foundation contract tests: PASS\n" : "V4 CP2 foundation contract tests: FAIL\n");
    return pass ? 0 : 1;
}
