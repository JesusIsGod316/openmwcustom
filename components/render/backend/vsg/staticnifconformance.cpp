#include "staticnifconformance.hpp"

#include <components/nif/niffile.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <vsg/io/Logger.h>
#include <vsg/utils/SharedObjects.h>

#include <memory>
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
        auto decodeReport = std::make_shared<StaticTextureDecodeReport>();
        StaticTextureResolver textureResolver = makeStaticTextureResolver(
            [&vfs](std::string_view sourceIdentity) -> Files::IStreamPtr {
                return vfs.find(VFS::Path::Normalized(sourceIdentity));
            },
            effectiveShared, decodeReport);
        result.realization
            = realizeStaticAssetConformant(world, result.model, result.plan, textureResolver, effectiveShared);
        result.textureDecode = std::move(*decodeReport);
        if (!result.realization.valid())
            return result;

        // A valid VSG node graph is not sufficient for static semantic
        // conformance. These counters mean the neutral source requested a
        // material/texture behavior that the current backend did not realize.
        // Keep the result at the Realize stage so callers, JSON reports and the
        // corpus runner cannot mistake a drawable-but-incomplete graph for a
        // CP3B-complete asset.
        if (result.realization.stats.unsupportedTextureBindings != 0u)
        {
            result.realization.diagnostics.emplace_back(
                "Static VSG realization left one or more texture bindings unsupported; compatibility realization is required");
            return result;
        }
        if (result.realization.stats.runtimeContextEffects != 0u)
        {
            result.realization.diagnostics.emplace_back(
                "Static VSG realization left one or more promoted material semantics unresolved; compatibility realization is required");
            return result;
        }

        result.stage = StaticNifConformanceStage::Complete;
        return result;
    }
}
