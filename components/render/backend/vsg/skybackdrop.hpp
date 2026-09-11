#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_SKYBACKDROP_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_SKYBACKDROP_H

#include <components/rendercore/framerenderstate.hpp>

#include <vsg/core/Array.h>

namespace vsg
{
    class Node;
    class Switch;
}

namespace RenderVsg
{
    // Persistent CP4D atmosphere foundation. The graph and pipeline are built
    // once; only the small per-frame colour block and Switch mask change.
    class SkyBackdrop
    {
    public:
        [[nodiscard]] static SkyBackdrop create();

        [[nodiscard]] explicit operator bool() const noexcept { return mRoot && mParameters; }
        [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const noexcept;
        void update(const RenderCore::FrameEnvironmentState& environment, const RenderCore::FrameView& view) noexcept;

    private:
        vsg::ref_ptr<vsg::Switch> mRoot;
        vsg::ref_ptr<vsg::vec4Array> mParameters;
    };
}

#endif
