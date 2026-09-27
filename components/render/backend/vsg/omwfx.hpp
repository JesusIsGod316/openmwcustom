#ifndef OPENMW_RENDER_VSG_OMWFX_HPP
#define OPENMW_RENDER_VSG_OMWFX_HPP

#include <components/fx/nativeplan.hpp>
#include <components/rendercore/framerenderstate.hpp>
#include <vsg/all.h>
#include "viewcompilemanager.hpp"

namespace RenderVsg
{
    class OmwFxRuntime
    {
    public:
        OmwFxRuntime(vsg::Viewer& viewer, vsg::Device* device, vsg::ref_ptr<vsg::ImageView> scene,
            vsg::ref_ptr<vsg::ImageView> depth, RenderCore::Extent2D extent, const Fx::NativeFrame& frame,
            vsg::ref_ptr<vsg::ubyteArray> lights);
        void update(const Fx::NativeFrame& frame, const RenderCore::FrameView& view,
            double simulationTime, double delta);

        vsg::ref_ptr<vsg::Group> commands;
        vsg::ref_ptr<vsg::ImageView> output;
        std::shared_ptr<const Fx::NativeChain> chain;
        bool interior = false, underwater = false;
        RenderCore::Extent2D extent;

    private:
        vsg::ref_ptr<vsg::ubyteArray> mState;
        vsg::ref_ptr<vsg::ubyteArray> mLights;
        std::vector<vsg::ref_ptr<vsg::ubyteArray>> mParameters;
        vsg::ref_ptr<vsg::vec4Value> mExposure;
        std::vector<std::unique_ptr<ViewCompileManager::Registration>> mRegistrations;
    };
}
#endif
