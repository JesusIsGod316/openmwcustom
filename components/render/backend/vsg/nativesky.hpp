#ifndef OPENMW_RENDER_VSG_NATIVESKY_H
#define OPENMW_RENDER_VSG_NATIVESKY_H

#include "frameresourcepool.hpp"
#include "staticassetrealizer.hpp"
#include <components/rendercore/framerenderstate.hpp>
#include <vsg/core/Array.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <functional>

namespace RenderVsg
{
    // Per-view camera-relative native sky. Only completed ring versions are
    // mutable. Geometry and texture images are shared; no pixel readback,
    // OpenGL context, extra worker, or GPU-idle barrier is introduced.
    class NativeSky
    {
    public:
        NativeSky();
        using Compile = std::function<bool(vsg::ref_ptr<vsg::Node>)>;
        bool prepare(const RenderCore::NativeSkySnapshot* snapshot,
            const RenderCore::FrameEnvironmentState& environment, const RenderCore::FrameView& view,
            RenderCore::FrameId frame, std::optional<RenderCore::FrameId> completed,
            const StaticTextureResolver& resolver, vsg::ref_ptr<vsg::SharedObjects> shared,
            const Compile& compile, std::string& diagnostic, bool visible);
        [[nodiscard]] bool markSubmitted(RenderCore::FrameId frame) noexcept { return mResidents.markSubmitted(frame); }
        [[nodiscard]] vsg::ref_ptr<vsg::Node> node() const noexcept { return mRoot; }
        [[nodiscard]] std::size_t residentVersions() const noexcept { return mResidents.size(); }
    private:
        struct Resident
        {
            RenderCore::SkyDrawSnapshot contract;
            vsg::ref_ptr<vsg::MatrixTransform> placement;
            vsg::ref_ptr<vsg::vec4Array> parameters;
        };
        static bool sameResources(const RenderCore::SkyDrawSnapshot&, const RenderCore::SkyDrawSnapshot&) noexcept;
        static Resident realize(const RenderCore::SkyDrawSnapshot&, const StaticTextureResolver&,
            vsg::ref_ptr<vsg::SharedObjects>, std::string&);
        vsg::ref_ptr<vsg::Group> mRoot;
        FrameResourcePool<Resident> mResidents{3};
    };
}
#endif
