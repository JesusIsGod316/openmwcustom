#ifndef OPENMW_SCENEUTIL_TEXTUREOWNERBOUNDARY_H
#define OPENMW_SCENEUTIL_TEXTUREOWNERBOUNDARY_H

#include <components/resource/assetpolicy.hpp>
#include <components/resource/texturestreaming.hpp>
#include <osg/Version>
#include <osg/RenderInfo>
#include <osgUtil/RenderStage>
#include <osgViewer/Renderer>
#include <osgViewer/Viewer>
#include <cstring>
#include <stdexcept>

namespace SceneUtil::TextureOwnerBoundary
{
    class RootStage final : public osgUtil::RenderStage
    {
    public:
        explicit RootStage(const osgUtil::RenderStage& stage) : osgUtil::RenderStage(stage) {}
        void drawPreRenderStages(osg::RenderInfo& info,osgUtil::RenderLeaf*& previous) override
        {
            // OSG 3.6.5 SceneView::draw sets the exact scene-view FrameStamp
            // before this call, and all PRE_RENDER cameras run inside the base
            // implementation. A GraphicsContext queued operation runs too late.
            if(auto* state=info.getState()) Resource::TextureStreaming::frameBoundary(*state);
            osgUtil::RenderStage::drawPreRenderStages(info,previous);
        }
        // Deliberately inherit RenderStage::clone/cloneType. CullVisitor clones
        // the root stage for nested cameras: those remain ordinary RenderStages
        // and cannot add a mid-frame quality decision or replace camera hooks.
    };
    inline unsigned installBeforeRealize(osgViewer::Viewer& viewer)
    {
        if(!Resource::AssetPolicy::enabled(Resource::AssetPolicy::TextureStreaming)) return 0;
        if(viewer.isRealized() || viewer.areThreadsRunning() || std::strcmp(osgGetVersion(),"3.6.5")!=0)
            throw std::runtime_error("native texture owner boundary requires fresh OSG 3.6.5 mono scene views");
        auto* camera=viewer.getCamera();
        auto* renderer=camera ? dynamic_cast<osgViewer::Renderer*>(camera->getRenderer()) : nullptr;
        if(!renderer) throw std::runtime_error("native texture owner boundary requires the camera Renderer");
        // Validate both before making the reversible fresh-stage replacements.
        for(unsigned i=0;i<2;++i)
        {
            const auto* view=renderer->getSceneView(i);
            const auto* stage=view ? view->getRenderStage() : nullptr;
            if(!stage || typeid(*stage)!=typeid(osgUtil::RenderStage))
                throw std::runtime_error("native texture owner boundary requires fresh stock root RenderStages");
        }
        for(unsigned i=0;i<2;++i)
        {
            auto* view=renderer->getSceneView(i);
            view->setRenderStage(new RootStage(*view->getRenderStage()));
        }
        return 2;
    }
}
#endif
