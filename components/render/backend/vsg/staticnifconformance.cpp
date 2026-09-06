#include "staticnifconformance.hpp"

#include <components/nif/niffile.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <utility>

namespace RenderVsg
{
    StaticNifConformanceResult realizeStaticNif(const Nif::FileView& file, const VFS::Manager& vfs,
        RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher, StaticPlanOptions planOptions,
        vsg::ref_ptr<vsg::SharedObjects> sharedObjects)
    {
        StaticNifConformanceResult result;

        NifRender::TranslationBundle bundle = NifRender::translateStaticNif(file, vfs);
        result.translationSummary = bundle.summary();
        result.translationDiagnostics = bundle.diagnostics;
        if (!bundle.valid() || bundle.hasErrors())
            return result;

        result.stage = StaticNifConformanceStage::Publish;
        NifRender::TranslationPublishResult published
            = NifRender::publishTranslation(world, publisher, bundle, RenderCore::InitialUpdateSequence);
        result.publishStatus = published.status;
        if (!published.applied())
            return result;
        result.model = published.binding.model;

        result.stage = StaticNifConformanceStage::Plan;
        const std::optional<StaticAssetPlan> plan = buildStaticAssetPlan(world, result.model, planOptions);
        if (!plan)
            return result;
        result.plan = *plan;

        result.stage = StaticNifConformanceStage::Realize;
        vsg::ref_ptr<vsg::SharedObjects> effectiveShared
            = sharedObjects ? std::move(sharedObjects) : vsg::SharedObjects::create();
        StaticTextureResolver textureResolver = makeStaticTextureResolver(
            [&vfs](std::string_view sourceIdentity) -> Files::IStreamPtr {
                return vfs.find(VFS::Path::Normalized(sourceIdentity));
            },
            effectiveShared);
        result.realization
            = realizeStaticAssetConformant(world, result.model, result.plan, textureResolver, effectiveShared);
        if (!result.realization.valid())
            return result;

        result.stage = StaticNifConformanceStage::Complete;
        return result;
    }
}
