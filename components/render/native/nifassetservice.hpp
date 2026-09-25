#ifndef OPENMW_COMPONENTS_RENDER_NATIVE_NIFASSETSERVICE_H
#define OPENMW_COMPONENTS_RENDER_NATIVE_NIFASSETSERVICE_H

#include "nifsemanticcompiler.hpp"

#include <components/nifrender/staticmodelcache.hpp>

#include <cstdint>
#include <map>
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
        std::string diagnostic;

        [[nodiscard]] bool available() const noexcept
        {
            return status == NifAssetResolveStatus::Published || status == NifAssetResolveStatus::Reused;
        }
    };

    // Session-side canonical native model service.
    //
    // This is the Phase 1 boundary Phase 2 should consume: normalized winning
    // VFS identity -> direct semantic compile -> stable RenderWorld publication.
    // Repeated world references reuse the same published model handle and source
    // metadata; no OSG rendering node is created or inspected.
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
        NifSemanticCompiler mCompiler;
        NifRender::StaticModelCache& mModels;
        std::map<std::string, std::uint64_t, std::less<>> mMetadata;
    };
}

#endif
