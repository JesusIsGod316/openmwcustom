#include "nifassetservice.hpp"

#include <memory>
#include <string>
#include <utility>

namespace RenderNative
{
    NifAssetResolveResult NifAssetService::resolve(
        VFS::Path::NormalizedView path, NifRender::TranslatorOptions options)
    {
        NifAssetResolveResult result;
        if (path.empty())
        {
            result.status = NifAssetResolveStatus::InvalidSource;
            result.diagnostic = "native NIF asset service received an empty source path";
            return result;
        }

        const std::string identity(path.value());
        const std::optional<RenderCore::ModelHandle> existing = mModels.find(identity);
        const auto metadata = mMetadata.find(identity);
        if (existing && metadata != mMetadata.end())
        {
            result.status = NifAssetResolveStatus::Reused;
            result.model = *existing;
            result.skeleton = mModels.findSkeleton(identity);
            result.namedVisualCapabilities = metadata->second.namedVisualCapabilities;
            result.controllers = metadata->second.controllers;
            return result;
        }

        NifSemanticCompileResult compiled = mCompiler.compile(path, options);
        if (!compiled.compiled())
        {
            result.status = NifAssetResolveStatus::CompileFailed;
            result.diagnostic = std::move(compiled.diagnostic);
            return result;
        }

        const NifRender::StaticModelCacheResult published = mModels.publish(compiled.bundle);
        if (!published.available())
        {
            result.status = NifAssetResolveStatus::PublishFailed;
            result.namedVisualCapabilities = compiled.namedVisualCapabilities;
            result.diagnostic = "native NIF asset publication failed with cache status "
                + std::to_string(static_cast<unsigned int>(published.status))
                + " and publish status " + std::to_string(static_cast<unsigned int>(published.publishStatus));
            if (!compiled.diagnostic.empty())
                result.diagnostic += ": " + compiled.diagnostic;
            return result;
        }

        auto controllers = std::make_shared<const NifControllerProgram>(std::move(compiled.controllers));
        mMetadata.insert_or_assign(identity, SourceMetadata{ compiled.namedVisualCapabilities, controllers });
        result.status = published.status == NifRender::StaticModelCacheStatus::Published
            ? NifAssetResolveStatus::Published
            : NifAssetResolveStatus::Reused;
        result.model = published.model;
        result.skeleton = published.skeleton;
        result.namedVisualCapabilities = compiled.namedVisualCapabilities;
        result.controllers = std::move(controllers);
        result.diagnostic = std::move(compiled.diagnostic);
        return result;
    }
}
