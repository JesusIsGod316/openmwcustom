#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICTEXTUREDECODE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_STATICTEXTUREDECODE_H

#include "staticassetrealizer.hpp"

#include <components/files/istreamptr.hpp>

#include <vsg/core/ref_ptr.h>

#include <functional>
#include <memory>
#include <string_view>

namespace vsg
{
    class Data;
    class SharedObjects;
}

namespace vsgXchange
{
    class images;
}

namespace RenderVsg
{
    inline constexpr std::string_view OpenMwWarningTextureSourceIdentity = "builtin:openmw-warning-image";
    inline constexpr std::string_view OpenMwWarningTextureContentIdentity = "builtin:openmw-warning-image-v1";

    using StaticTextureStreamOpener = std::function<Files::IStreamPtr(std::string_view)>;

    // Backend-private texture decoder for CP3B3. It consumes the winning source
    // stream supplied by the caller, never a filesystem path or OSG ImageManager,
    // and realizes color-space variants from TextureRealizationKey. This keeps the
    // logical TextureHandle content-only while letting sRGB/data views diverge at
    // the Vulkan image realization boundary.
    class StaticTextureDecoder
    {
    public:
        explicit StaticTextureDecoder(vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {});
        ~StaticTextureDecoder();

        [[nodiscard]] vsg::ref_ptr<vsg::Data> decode(const RenderCore::TextureRecord& record,
            const RenderCore::TextureRealizationKey& key, const StaticTextureStreamOpener& opener) const;

    private:
        vsg::ref_ptr<vsg::SharedObjects> mSharedObjects;
        vsg::ref_ptr<vsgXchange::images> mImages;
    };

    // Convenience adapter matching StaticAssetRealizer's resolver seam. The
    // opener is intentionally generic so the standalone CP3B3 tests can use
    // synthetic streams while the real NIF conformance tool can bind VFS::Manager.
    [[nodiscard]] StaticTextureResolver makeStaticTextureResolver(
        StaticTextureStreamOpener opener, vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {});
}

#endif
