#ifndef OPENMW_COMPONENTS_RENDER_NATIVE_NIFSEMANTICCOMPILER_H
#define OPENMW_COMPONENTS_RENDER_NATIVE_NIFSEMANTICCOMPILER_H

#include <components/nifrender/niftranslator.hpp>
#include <components/vfs/pathutil.hpp>

#include <cstdint>
#include <string>

namespace Nif
{
    class FileView;
}

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
    enum class NifSemanticCompileStatus : std::uint8_t
    {
        Compiled,
        InvalidSource,
        MissingSource,
        ParseFailed,
        TranslationFailed,
    };

    struct NifSemanticCompileResult
    {
        NifSemanticCompileStatus status = NifSemanticCompileStatus::InvalidSource;
        NifRender::TranslationBundle bundle;
        // Source-authored named visual capabilities are extracted from the same
        // parsed NIF so callers do not need a second parse or an OSG node tree.
        std::uint64_t namedVisualCapabilities = 0;
        std::string diagnostic;

        [[nodiscard]] bool compiled() const noexcept
        {
            return status == NifSemanticCompileStatus::Compiled;
        }

        // Translation diagnostics remain authoritative. A syntactically parsed
        // NIF can compile into a bundle that is deliberately fail-closed because
        // it contains unsupported semantics.
        [[nodiscard]] bool semanticReady() const noexcept
        {
            return compiled() && bundle.valid() && !bundle.hasErrors();
        }
    };

    // VulkanMW Phase 1 source-side compiler.
    //
    // Authored NIF data is parsed from the winning OpenMW VFS entry and compiled
    // directly into the existing backend-neutral NifRender/RenderCore semantic
    // bundle. No osg::Node, StateSet, Drawable, Geometry, NodeVisitor, SceneUtil
    // object, VSG object, or Vulkan object participates in this boundary.
    //
    // The parsed-file overload exists so callers that also need source metadata
    // can parse exactly once and share the same FileView without constructing an
    // OSG scene. The path overload is the normal one-shot native asset entry.
    class NifSemanticCompiler final
    {
    public:
        explicit NifSemanticCompiler(
            const VFS::Manager& vfs, NifRender::TextureIdentityCache* textureIdentities = nullptr) noexcept
            : mVfs(vfs)
            , mTextureIdentities(textureIdentities)
        {
        }

        [[nodiscard]] NifSemanticCompileResult compile(
            VFS::Path::NormalizedView path, NifRender::TranslatorOptions options = {}) const;

        [[nodiscard]] NifSemanticCompileResult compile(
            Nif::FileView file, NifRender::TranslatorOptions options = {}) const;

    private:
        const VFS::Manager& mVfs;
        NifRender::TextureIdentityCache* mTextureIdentities = nullptr;
    };
}

#endif
