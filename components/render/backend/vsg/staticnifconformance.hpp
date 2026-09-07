#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICNIFCONFORMANCE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICNIFCONFORMANCE_H

#include "staticassetconformance.hpp"
#include "statictexturedecode.hpp"

#include <components/nifrender/translationbundle.hpp>
#include <components/nifrender/translationpublish.hpp>

#include <cstdint>
#include <vector>

namespace Nif
{
    class FileView;
}

namespace VFS
{
    class Manager;
}

namespace RenderVsg
{
    enum class StaticNifConformanceStage : std::uint8_t
    {
        Translate,
        Publish,
        Plan,
        Realize,
        Complete,
    };

    struct StaticNifConformanceResult
    {
        StaticNifConformanceStage stage = StaticNifConformanceStage::Translate;
        NifRender::TranslationSummary translationSummary;
        std::vector<NifRender::TranslationDiagnostic> translationDiagnostics;
        NifRender::TranslationPublishStatus publishStatus = NifRender::TranslationPublishStatus::InvalidBundle;
        RenderCore::ModelHandle model;
        StaticAssetPlan plan;
        StaticTextureDecodeReport textureDecode;
        StaticRealizationResult realization;

        [[nodiscard]] bool complete() const noexcept
        {
            return stage == StaticNifConformanceStage::Complete && realization.valid()
                && realization.stats.unsupportedTextureBindings == 0u
                && realization.stats.runtimeContextEffects == 0u
                && realization.stats.modernPbrDraws == 0u
                && realization.stats.legacyCompatibilityDraws == realization.stats.drawCount;
        }
    };

    // Full CP3B3 static source-to-backend seam. One already-parsed FileView is
    // translated with current OpenMW/V3.25 semantics, atomically published into
    // the neutral RenderWorld, planned, and realized through the conformant VSG
    // routing path. Texture bytes are reopened from the same winning VFS. No OSG
    // ImageManager/cache, source pointer, or NIF record identity crosses the
    // RenderCore boundary. OpenMW-compatible warning-image substitutions remain
    // visible in textureDecode rather than being hidden by successful realization.
    // A drawable graph is not "complete" while any promoted static semantic or
    // texture binding remains unrealized. Current CP3B NIF conformance also requires
    // every realized draw to remain in the legacy compatibility shader family;
    // ModernPbr is reserved for an explicit CP4+ producer contract.
    [[nodiscard]] StaticNifConformanceResult realizeStaticNif(const Nif::FileView& file, const VFS::Manager& vfs,
        RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher,
        StaticPlanOptions planOptions = {}, vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {});
}

#endif