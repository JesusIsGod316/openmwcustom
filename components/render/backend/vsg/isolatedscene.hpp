#ifndef OPENMW_RENDER_VSG_ISOLATEDSCENE_H
#define OPENMW_RENDER_VSG_ISOLATEDSCENE_H

#include "immediateeffectrealizer.hpp"
#include "framecamera.hpp"
#include <components/rendercore/framerenderstate.hpp>

namespace RenderVsg
{
    // Realize an already evaluated preview snapshot. This has no access to the
    // gameplay scene, actor cache, water, or simulation clock.
    [[nodiscard]] inline vsg::ref_ptr<vsg::Group> realizeIsolatedScene(
        const RenderCore::IsolatedSceneSnapshot& scene, const StaticTextureResolver& textures,
        vsg::ref_ptr<vsg::SharedObjects> shared, std::string& diagnostic)
    {
        auto root = vsg::Group::create();
        for (const auto& draw : scene.draws)
        {
            auto realized = realizeImmediateEffectDraw(draw, textures, shared);
            if (!realized.valid())
            {
                diagnostic = "isolated preview " + draw.identity + ": " + realized.diagnostic;
                return {};
            }
            auto placement = vsg::MatrixTransform::create(toVsgMatrix(draw.worldTransform));
            placement->addChild(realized.root);
            root->addChild(placement);
        }
        return root;
    }
}
#endif
