#ifndef OPENMW_COMPONENTS_RENDER_NATIVE_NIFASSETSERVICE_H
#define OPENMW_COMPONENTS_RENDER_NATIVE_NIFASSETSERVICE_H

#include "nifsemanticcompiler.hpp"

#include <components/nifrender/staticmodelcache.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

namespace NifRender
{
    class TextureIdentityCache;
}

namespace VFS
{
    class Manager;
}

namespace RenderNative
{
    enum class NifAssetResolveStatus : std::uint8_t
    {
        Published,
        Reused,
        InvalidSource,
        CompileFailed,
        PublishFailed,
    };

    struct NifAssetResolveResult
    {
        NifAssetResolveStatus status = NifAssetResolveStatus::InvalidSource;
        RenderCore::ModelHandle model;
        std::optional<RenderCore::SkeletonHandle> skeleton;
        std::uint64_t namedVisualCapabilities = 0;
        std::shared_ptr<const NifControllerProgram> controllers;
        std::string diagnostic;

        [[nodiscard]] bool available() const noexcept
        {
            return status == NifAssetResolveStatus::Published || status == NifAssetResolveStatus::Reused;
        }
    };

    // Session-side canonical native model service.
    //
    // Winning VFS identity -> direct semantic/controller compile -> stable
    // RenderWorld publication. Repeated references reuse both model handles and
    // immutable native controller programs.
    class NifAssetService final
    {
    public:
        NifAssetService(const VFS::Manager& vfs, NifRender::TextureIdentityCache* textureIdentities,
            NifRender::StaticModelCache& models) noexcept
            : mCompiler(vfs, textureIdentities)
            , mModels(models)
        {
        }

        [[nodiscard]] NifAssetResolveResult resolve(
            VFS::Path::NormalizedView path, NifRender::TranslatorOptions options = {});

        void clearSourceMetadata() { mMetadata.clear(); }

    private:
        struct SourceMetadata
        {
            std::uint64_t namedVisualCapabilities = 0;
            std::shared_ptr<const NifControllerProgram> controllers;
        };

        NifSemanticCompiler mCompiler;
        NifRender::StaticModelCache& mModels;
        std::map<std::string, SourceMetadata, std::less<>> mMetadata;
    };
}

#endif
