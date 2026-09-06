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
        StaticRealizationResult realization;

        [[nodiscard]] bool complete() const noexcept
        {
            return stage == StaticNifConformanceStage::Complete && realization.valid();
        }
    };

    // Full CP3B3 static source-to-backend seam. One already-parsed FileView is
    // translated with current OpenMW/V3.25 semantics, atomically published into
    // the neutral RenderWorld, planned, and realized through the conformant VSG
    // routing path. Texture bytes are reopened from the same winning VFS. No OSG
    // ImageManager/cache, source pointer, or NIF record identity crosses the
    // RenderCore boundary.
    [[nodiscard]] StaticNifConformanceResult realizeStaticNif(const Nif::FileView& file, const VFS::Manager& vfs,
        RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher,
        StaticPlanOptions planOptions = {}, vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {});
}

#endif
