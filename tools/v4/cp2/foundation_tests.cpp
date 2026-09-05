#include <components/render/backend/vsg/residencyledger.hpp>
#include <components/rendercore/framerenderstate.hpp>

#include <cstdint>
#include <iostream>

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

    bool testResidencyRetirement()
    {
        using RenderVsg::ResidencyCategory;
        RenderVsg::ResidencyLedger ledger;

        constexpr std::uint64_t bytes = 4u * 1024u * 1024u;
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

        if (!require(ledger.queueRetire(
                         ResidencyCategory::MeshVertexIndex, bytes, false, RenderCore::FrameId{ 12 }),
                "frame-delayed retirement queue failed")
            || !require(ledger.collectRetired(RenderCore::FrameId{ 11 }) == 0, "resource retired before last-use frame")
            || !require(ledger.pendingRetirementCount() == 1, "retirement ticket disappeared early")
            || !require(ledger.collectRetired(RenderCore::FrameId{ 12 }) == bytes, "resource did not retire at completion frame"))
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
    const bool pass = testFrameRenderState() && testDuplicateViewRejection() && testResidencyRetirement();
    std::cout << (pass ? "V4 CP2 foundation contract tests: PASS\n" : "V4 CP2 foundation contract tests: FAIL\n");
    return pass ? 0 : 1;
}
