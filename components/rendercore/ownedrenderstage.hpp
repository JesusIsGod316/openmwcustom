#ifndef OPENMW_RENDERCORE_OWNEDRENDERSTAGE_H
#define OPENMW_RENDERCORE_OWNEDRENDERSTAGE_H
#include <osg/Camera>
#include <osg/Viewport>
#include <osg/UserDataContainer>
#include <osgUtil/RenderStage>
namespace RenderCore
{
    class RenderStageCameraSnapshot : public osg::Object
    {
    public:
        RenderStageCameraSnapshot() = default;
        explicit RenderStageCameraSnapshot(osg::Camera* value) : camera(value) {}
        RenderStageCameraSnapshot(const RenderStageCameraSnapshot& source,const osg::CopyOp& copy)
            : osg::Object(source,copy), camera(source.camera) {}
        META_Object(RenderCore,RenderStageCameraSnapshot)
        osg::ref_ptr<osg::Camera> camera;
    };
    // Only the acquired SceneView's cull owner may freeze this stage. Deferred
    // GL setup reads its immutable camera/attachment/callback metadata.
    inline void snapshotRenderStageCamera(osgUtil::RenderStage& stage, const osg::Camera* source = nullptr,
        bool explicitFramebufferSetup = false)
    {
        const auto* camera = source ? source : stage.getCamera();
        if (!camera) return;
        const bool setup = stage.getCameraRequiresSetUp();
        osg::ref_ptr<osg::Camera> snapshot = new osg::Camera(*camera,osg::CopyOp::SHALLOW_COPY);
        // Metadata only: retaining renderer/context/children would create
        // cycles from SceneView -> stage -> camera -> renderer/SceneView.
        snapshot->setRenderer(nullptr);
        snapshot->setRenderingCache(nullptr);
        snapshot->setGraphicsContext(nullptr);
        snapshot->removeChildren(0,snapshot->getNumChildren());
        snapshot->setUserDataContainer(nullptr);
        if (stage.getViewport())
        {
            osg::ref_ptr<osg::Viewport> viewport = new osg::Viewport(*stage.getViewport());
            stage.setViewport(viewport);
            snapshot->setViewport(viewport);
        }
        stage.setCamera(snapshot);
        // RenderStage only observes its camera. Retain the metadata through
        // this stage's private container until its next acquired cull. The
        // stripped snapshot has no path back to its renderer or SceneView.
        osg::ref_ptr<osg::UserDataContainer> data = stage.getUserDataContainer()
            ? static_cast<osg::UserDataContainer*>(stage.getUserDataContainer()->clone(osg::CopyOp::SHALLOW_COPY))
            : new osg::DefaultUserDataContainer;
        for (unsigned i = data->getNumUserObjects(); i > 0; --i)
            if (dynamic_cast<RenderStageCameraSnapshot*>(data->getUserObject(i-1))) data->removeUserObject(i-1);
        data->addUserObject(new RenderStageCameraSnapshot(snapshot));
        stage.setUserDataContainer(data);
        // PostProcessor supplies the complete main-stage FBO/resolve pair.
        // Camera identity changes otherwise trigger implicit setup that would
        // replace that pair with attachments from the empty camera map.
        stage.setCameraRequiresSetUp(setup && !explicitFramebufferSetup);
        if (!setup || explicitFramebufferSetup)
            stage.setCameraAttachmentMapCount(snapshot->getAttachmentMapModifiedCount());
    }
}
#endif
