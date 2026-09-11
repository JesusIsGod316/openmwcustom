#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_WATERSURFACE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_WATERSURFACE_H

#include <components/rendercore/framerenderstate.hpp>

#include <vsg/core/Array.h>
#include <vsg/core/ref_ptr.h>

namespace vsg
{
    class ImageView;
    class MatrixTransform;
    class Node;
    class Switch;
}

namespace RenderVsg
{
    // CP4E's persistent water surface. It samples the resident reflection and
    // refraction targets when available and retains a deterministic fallback
    // for independently disabled effects.
    class WaterSurface
    {
    public:
        [[nodiscard]] static WaterSurface create(
            vsg::ref_ptr<vsg::ImageView> reflection, vsg::ref_ptr<vsg::ImageView> refraction);

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const noexcept;
        void update(const RenderCore::FrameEnvironmentState& environment,
            const RenderCore::FrameView& mainView, double simulationTime) noexcept;

    private:
        vsg::ref_ptr<vsg::Switch> mRoot;
        vsg::ref_ptr<vsg::MatrixTransform> mPlacement;
        vsg::ref_ptr<vsg::vec4Array> mParameters;
        bool mHasReflection = false;
        bool mHasRefraction = false;
    };
}

#endif
