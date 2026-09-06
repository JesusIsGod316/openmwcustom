#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VFSTEXTURERESOLVER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VFSTEXTURERESOLVER_H

#include "statictexturedecode.hpp"

#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <utility>

namespace RenderVsg
{
    // Concrete CP3B3 adapter from OpenMW's already-selected VFS view to the
    // backend-private stream decoder. VFS remains outside RenderCore; no OSG
    // ImageManager/cache participates in modern resource ownership.
    [[nodiscard]] inline StaticTextureResolver makeVfsStaticTextureResolver(
        const VFS::Manager& vfs, vsg::ref_ptr<vsg::SharedObjects> sharedObjects = {})
    {
        return makeStaticTextureResolver(
            [&vfs](std::string_view sourceIdentity) {
                return vfs.find(VFS::Path::toNormalized(sourceIdentity));
            },
            std::move(sharedObjects));
    }
}

#endif
