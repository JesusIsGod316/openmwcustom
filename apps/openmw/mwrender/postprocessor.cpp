#include "postprocessor.hpp"

#include <SDL3/SDL_opengl_glext.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <typeinfo>

#include <osg/Texture1D>
#include <osg/Texture2D>
#include <osg/Texture2DArray>
#include <osg/Texture2DMultisample>
#include <osg/Texture3D>

#include <osgUtil/GLObjectsVisitor>
#include <osgUtil/IncrementalCompileOperation>

#include <components/debug/v36gpuprofiler.hpp>
#include <components/rendercore/ownedrenderstage.hpp>
#include <components/sceneutil/drawphasetrace.hpp>
#include <components/files/conversion.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/multiview.hpp>
#include <components/stereo/stereomanager.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwgui/postprocessorhud.hpp"

#include "camera.hpp"
#include "temporalmotion.hpp"
#include "temporaldynamic.hpp"
#include <osg/Version>
#include <osgUtil/SceneView>
#include <osgViewer/Renderer>
#include "distortion.hpp"
#include "depthclear.hpp"
#include "pingpongcull.hpp"
#include "renderbin.hpp"
#include "renderingmanager.hpp"
#include "sky.hpp"
#include "transparentpass.hpp"
#include "vismask.hpp"

namespace
{
    struct ResizedCallback : osg::GraphicsContext::ResizedCallback
    {
        ResizedCallback(MWRender::PostProcessor* postProcessor)
            : mPostProcessor(postProcessor)
        {
        }

        void resizedImplementation(osg::GraphicsContext* gc, int x, int y, int width, int height) override
        {
            gc->resizedImplementation(x, y, width, height);

            mPostProcessor->setRenderTargetSize(width, height);
            mPostProcessor->resize();
        }

        MWRender::PostProcessor* mPostProcessor;
    };

    class HUDCullCallback : public SceneUtil::NodeCallback<HUDCullCallback, osg::Camera*, osgUtil::CullVisitor*>
    {
    public:
        explicit HUDCullCallback(MWRender::PostProcessor* processor) : mPostProcessor(processor) {}
        void operator()(osg::Camera* camera, osgUtil::CullVisitor* cv)
        {
            osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;
            auto& sm = Stereo::Manager::instance();
            auto* fullViewport = camera->getViewport();
            if (mPostProcessor->temporalOwnershipAvailable())
            {
                // RenderStage and inherited StateGraph must both retain a
                // cull-owned viewport. A resize can otherwise mutate the
                // Camera's shared viewport during a delayed static draw.
                osg::ref_ptr<osg::Viewport> viewport = new osg::Viewport(*fullViewport);
                cv->getCurrentRenderStage()->setViewport(viewport);
                stateset->setAttributeAndModes(viewport);
            }
            if (sm.getEye(cv) == Stereo::Eye::Left)
                stateset->setAttributeAndModes(
                    new osg::Viewport(0, 0, fullViewport->width() / 2, fullViewport->height()));
            if (sm.getEye(cv) == Stereo::Eye::Right)
                stateset->setAttributeAndModes(
                    new osg::Viewport(fullViewport->width() / 2, 0, fullViewport->width() / 2, fullViewport->height()));

            cv->pushStateSet(stateset);
            traverse(camera, cv);
            cv->popStateSet();
            if (mPostProcessor->temporalOwnershipAvailable())
                RenderCore::snapshotRenderStageCamera(*cv->getCurrentRenderStage(),camera);
        }
    private:
        MWRender::PostProcessor* mPostProcessor;
    };

    enum class Usage
    {
        RENDER_BUFFER,
        TEXTURE,
    };

    static osg::FrameBufferAttachment createFrameBufferAttachmentFromTemplate(
        Usage usage, int width, int height, osg::Texture* textureTemplate, int samples)
    {
        if (usage == Usage::RENDER_BUFFER && !Stereo::getMultiview())
        {
            osg::ref_ptr<osg::RenderBuffer> attachment
                = new osg::RenderBuffer(width, height, textureTemplate->getInternalFormat(), samples);
            return osg::FrameBufferAttachment(attachment);
        }

        auto texture = Stereo::createMultiviewCompatibleTexture(width, height, samples);
        texture->setSourceFormat(textureTemplate->getSourceFormat());
        texture->setSourceType(textureTemplate->getSourceType());
        texture->setInternalFormat(textureTemplate->getInternalFormat());
        texture->setFilter(osg::Texture2D::MIN_FILTER, textureTemplate->getFilter(osg::Texture2D::MIN_FILTER));
        texture->setFilter(osg::Texture2D::MAG_FILTER, textureTemplate->getFilter(osg::Texture2D::MAG_FILTER));
        texture->setWrap(osg::Texture::WRAP_S, textureTemplate->getWrap(osg::Texture2D::WRAP_S));
        texture->setWrap(osg::Texture::WRAP_T, textureTemplate->getWrap(osg::Texture2D::WRAP_T));

        return Stereo::createMultiviewCompatibleAttachment(texture);
    }

    constexpr float DistortionRatio = 0.25;
}

namespace MWRender
{
    PostProcessor::PostProcessor(
        RenderingManager& rendering, osgViewer::Viewer* viewer, osg::Group* rootNode, const VFS::Manager* vfs)
        : osg::Group()
        , mRootNode(rootNode)
        , mHUDCamera(new osg::Camera)
        , mRendering(rendering)
        , mViewer(viewer)
        , mVFS(vfs)
        , mUsePostProcessing(Settings::postProcessing().mEnabled)
        , mSamples(Settings::video().mAntialiasing)
        , mPingPongCull(new PingPongCull(this))
        , mDistortionCallback(new DistortionCallback([](osg::RenderInfo& info) -> osg::ref_ptr<osg::FrameBufferObject> {
            auto* camera = info.getCurrentCamera();
            auto* processor = camera ? dynamic_cast<PostProcessor*>(camera->getUserData()) : nullptr;
            return processor ? processor->getPrimaryFbo(info.getState()->getFrameStamp()->getFrameNumber() % 2) : nullptr;
        }))
    {
        auto& shaderManager = mRendering.getResourceSystem()->getSceneManager()->getShaderManager();

        std::shared_ptr<LuminanceCalculator> luminanceCalculator = std::make_shared<LuminanceCalculator>(shaderManager);

        for (auto& canvas : mCanvases)
            canvas = new PingPongCanvas(shaderManager, luminanceCalculator);

        if (TemporalMotion::enabled() && !Stereo::getStereo() && viewer->getCamera()->getGraphicsContext())
        {
            try
            {
                auto program = shaderManager.getProgram("temporal_camera_motion");
                auto debugProgram = TemporalMotion::debugView() ? shaderManager.getProgram("temporal_motion_view") : nullptr;
                if (program)
                {
                    auto dynamicProgram = SceneUtil::temporalDynamicMotionEnabled()
                        ? shaderManager.getProgram("temporal_dynamic_motion") : nullptr;
                    mTemporalMotion = std::make_shared<TemporalMotion>(program, dynamicProgram);
                    for (auto& canvas : mCanvases) canvas->setTemporalMotion(mTemporalMotion, debugProgram);
                    Log(Debug::Info) << "Phase 9 camera/static motion capture enabled; dense dynamic motion and DLSS are not integrated";
                }
                else
                    Log(Debug::Warning) << "Phase 9 motion shader unavailable; retaining normal rendering";
            }
            catch (const std::exception& error)
            {
                for (auto& canvas : mCanvases) canvas->setTemporalMotion(nullptr, nullptr);
                mTemporalMotion.reset();
                Log(Debug::Warning) << "Phase 9 motion setup failed; retaining normal rendering: " << error.what();
            }
        }

        mHUDCamera->setReferenceFrame(osg::Camera::ABSOLUTE_RF);
        mHUDCamera->setRenderOrder(osg::Camera::POST_RENDER);
        mHUDCamera->setClearColor(osg::Vec4(0.45f, 0.45f, 0.14f, 1.f));
        mHUDCamera->setClearMask(0);
        mHUDCamera->setProjectionMatrix(osg::Matrix::ortho2D(0, 1, 0, 1));
        mHUDCamera->setAllowEventFocus(false);
        mHUDCamera->setViewport(0, 0, mWidth, mHeight);
        mHUDCamera->setNodeMask(Mask_RenderToTexture);
        mHUDCamera->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        mHUDCamera->addChild(mCanvases[0]);
        mHUDCamera->addChild(mCanvases[1]);
        mHUDCamera->setCullCallback(new HUDCullCallback(this));
        if (Settings::cells().mV36AsyncGpuProfiler)
            Debug::V36GpuProfiler::attachCamera(*mHUDCamera, "postprocess_hud_composite");
        mViewer->getCamera()->addCullCallback(mPingPongCull);

        // resolves the multisampled depth buffer and optionally draws an additional depth postpass
        mTransparentDepthPostPass
            = new TransparentDepthBinCallback(mRendering.getResourceSystem()->getSceneManager()->getShaderManager(),
                Settings::postProcessing().mTransparentPostpass);
        osgUtil::RenderBin::getRenderBinPrototype("DepthSortedBin")->setDrawCallback(mTransparentDepthPostPass);

        osg::ref_ptr<osgUtil::RenderBin> distortionRenderBin
            = new osgUtil::RenderBin(osgUtil::RenderBin::SORT_BACK_TO_FRONT);
        // This is silly to have to do, but if nothing is drawn then the drawcallback is never called and the distortion
        // texture will never be cleared
        osg::ref_ptr<osg::Node> dummyNodeToClear = new osg::Node;
        dummyNodeToClear->setCullingActive(false);
        dummyNodeToClear->getOrCreateStateSet()->setRenderBinDetails(RenderBin_Distortion, "Distortion");
        rootNode->addChild(dummyNodeToClear);
        distortionRenderBin->setDrawCallback(mDistortionCallback);
        distortionRenderBin->getStateSet()->setDefine("DISTORTION", "1", osg::StateAttribute::ON);

        // Give the renderbin access to the opaque depth sampler so it can write its occlusion
        // Distorted geometry is drawn with ALWAYS depth function and depths writes disbled.
        const int unitSoftEffect
            = shaderManager.reserveGlobalTextureUnits(Shader::ShaderManager::Slot::OpaqueDepthTexture);
        distortionRenderBin->getStateSet()->addUniform(new osg::Uniform("opaqueDepthTex", unitSoftEffect));

        osgUtil::RenderBin::addRenderBinPrototype("Distortion", distortionRenderBin);

        auto defines = shaderManager.getGlobalDefines();
        defines["distorionRTRatio"] = std::to_string(DistortionRatio);
        shaderManager.setGlobalDefines(defines);

        createObjectsForFrame(0);
        createObjectsForFrame(1);

        populateTechniqueFiles();

        auto distortion = loadTechnique("internal_distortion");
        distortion->setInternal(true);
        distortion->setLocked(true);
        mInternalTechniques.push_back(std::move(distortion));

        osg::GraphicsContext* gc = viewer->getCamera()->getGraphicsContext();
        if (gc == nullptr)
        {
            // The explicit VSG/Vulkan route intentionally retains an OSG viewer for gameplay update and semantic
            // extraction without ever creating an OpenGL graphics context. Keep the legacy postprocessor object alive
            // for existing CPU-side state consumers, but do not initialize or attach its OpenGL presentation path.
            mWidth = std::max(1, static_cast<int>(Settings::video().mResolutionX));
            mHeight = std::max(1, static_cast<int>(Settings::video().mResolutionY));
            mGLSLVersion = 0;
            mUBO = false;
            mNormalsSupported = false;
            mUsePostProcessing = false;
            mStateUpdater = new Fx::StateUpdater(false);
            Log(Debug::Info) << "V4 Vulkan headless OSG route: legacy OpenGL post-processing presentation disabled";
            return;
        }
        osg::GLExtensions* ext = gc->getState()->get<osg::GLExtensions>();

        mWidth = gc->getTraits()->width;
        mHeight = gc->getTraits()->height;

        if (!ext->glDisablei && ext->glDisableIndexedEXT)
            ext->glDisablei = ext->glDisableIndexedEXT;

#ifdef ANDROID
        ext->glDisablei = nullptr;
#endif

        if (ext->glDisablei)
            mNormalsSupported = true;
        else
            Log(Debug::Error) << "'glDisablei' unsupported, pass normals will not be available to shaders.";

        mGLSLVersion = static_cast<int>(ext->glslLanguageVersion * 100);
        mUBO = ext->isUniformBufferObjectSupported && mGLSLVersion >= 330;
        mStateUpdater = new Fx::StateUpdater(mUBO);

        addChild(mHUDCamera);
        addChild(mRootNode);

        mViewer->setSceneData(this);
        mViewer->getCamera()->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
        mViewer->getCamera()->getGraphicsContext()->setResizedCallback(new ResizedCallback(this));
        mViewer->getCamera()->setUserData(this);

        setCullCallback(mStateUpdater);

        if (mUsePostProcessing)
            enable();
    }

    PostProcessor::~PostProcessor()
    {
        if (auto* bin = osgUtil::RenderBin::getRenderBinPrototype("DepthSortedBin"))
            bin->setDrawCallback(nullptr);
    }

    void PostProcessor::resize()
    {
        ++mTemporalResourceEpoch;
        mHUDCamera->resize(mWidth, mHeight);
        mViewer->getCamera()->resize(mWidth, mHeight);
        if (Stereo::getStereo())
            Stereo::Manager::instance().screenResolutionChanged();

        size_t frameId = frame() % 2;

        createObjectsForFrame(frameId);

        mRendering.updateProjectionMatrix();
        mRendering.setScreenRes(renderWidth(), renderHeight());

        dirtyTechniques(true);

        mDirty = true;
        mDirtyFrameId = !frameId;
    }

    void PostProcessor::populateTechniqueFiles()
    {
        for (const auto& path : mVFS->getRecursiveDirectoryIterator(Fx::Technique::sSubdir))
        {
            std::string_view fileExt = Misc::getFileExtension(path);
            if (path.parent().parent().empty() && fileExt == Fx::Technique::sExt)
            {
                mTechniqueFiles.emplace(path);
            }
        }
    }

    void PostProcessor::enable()
    {
        mReload = true;
        mUsePostProcessing = true;
    }

    void PostProcessor::disable()
    {
        mUsePostProcessing = false;
        mRendering.getSkyManager()->setSunglare(true);
    }

    void PostProcessor::traverse(osg::NodeVisitor& nv)
    {
        unsigned frameId = nv.getTraversalNumber() % 2;

        if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
            cull(frameId, static_cast<osgUtil::CullVisitor*>(&nv));
        else if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
            update(frameId);

        osg::Group::traverse(nv);
    }

    void PostProcessor::captureTemporalCamera(osgUtil::CullVisitor* cv)
    {
        if (!mTemporalMotion || !cv || Stereo::getStereo())
        {
            mTemporalOwnershipAvailable = false;
            for (auto& canvas : mCanvases)
            {
                canvas->setTargetGenerationSingleContext(false);
                canvas->setTemporalOwnershipAvailable(false, Stereo::getStereo() ? "stereo" : "no_temporal_input");
            }
            return;
        }
        bool owned = false;
        std::string fallback = "not_requested";
        auto* renderer = dynamic_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer());
        const auto threading = mViewer->getThreadingModel();
        osgViewer::ViewerBase::Contexts contexts;
        mViewer->getContexts(contexts, false);
        auto* primaryContext = mViewer->getCamera()->getGraphicsContext();
        const bool generationDrawContext = std::string_view(osgGetVersion()) == "3.6.5"
            && renderer && (typeid(*renderer) == typeid(osgViewer::Renderer)
                || typeid(*renderer) == typeid(SceneUtil::DrawPhaseTrace::Renderer))
            && (threading == osgViewer::ViewerBase::DrawThreadPerContext
                || threading == osgViewer::ViewerBase::SingleThreaded)
            && primaryContext && primaryContext->valid() && primaryContext->getState()
            && contexts.size() == 1 && contexts.front() == primaryContext;
        if (TemporalMotion::ownershipEnabled() && generationDrawContext
            && (typeid(*cv) == typeid(osgUtil::CullVisitor)
                || typeid(*cv) == typeid(SceneUtil::DrawPhaseTrace::CullVisitor)))
        {
            // This is the actual SceneView-local visitor acquired through
            // Renderer::availableQueue, not a frame-parity approximation.
            for (unsigned i = 0; i < 2; ++i)
                owned |= renderer->getSceneView(i) && renderer->getSceneView(i)->getCullVisitor() == cv;
            if (owned && mTemporalOwnerRenderer.get() != renderer)
            {
                ++mTemporalResourceEpoch;
                mTemporalCanvasOwner = PingPongCanvas::createTemporalOwner();
                mTemporalOwnerRenderer = renderer;
                for (auto& canvas : mCanvases) canvas->setTemporalOwner(mTemporalCanvasOwner);
            }
        }
        // The generation's shared initialization marker is draw-owned only
        // under this one-context qualification. A later unsupported Fx binding
        // can use the DYNAMIC source without clearing already-written history.
        osg::ref_ptr<osg::StateSet> fxState;
        if (owned)
        {
            // Qualify the exact updater binding types BEFORE admitting a
            // STATIC canvas. Late world light values are finalized below.
            fxState = mStateUpdater->ownedFrame(cv);
            owned = fxState.valid();
            fallback = owned ? "active" : "unsupported_fx_state";
        }
        else if (TemporalMotion::ownershipEnabled())
        {
            if (std::string_view(osgGetVersion()) != "3.6.5") fallback = "unsupported_osg";
            else if (!renderer || (typeid(*renderer) != typeid(osgViewer::Renderer)
                    && typeid(*renderer) != typeid(SceneUtil::DrawPhaseTrace::Renderer)))
                fallback = "unsupported_renderer";
            else if (typeid(*cv) != typeid(osgUtil::CullVisitor)
                && typeid(*cv) != typeid(SceneUtil::DrawPhaseTrace::CullVisitor)) fallback = "unsupported_visitor";
            else if (threading != osgViewer::ViewerBase::DrawThreadPerContext
                && threading != osgViewer::ViewerBase::SingleThreaded) fallback = "unsupported_threading";
            else if (contexts.size() != 1) fallback = "multiple_contexts";
            else fallback = "unknown_sceneview";
        }
        for (auto& canvas : mCanvases)
        {
            canvas->setTargetGenerationSingleContext(generationDrawContext);
            canvas->setOwnedFxState(fxState);
            canvas->setTemporalOwnershipAvailable(owned, fallback);
        }
        mTemporalOwnershipAvailable = owned;
        TemporalCamera camera;
        camera.projection = cv->getProjectionMatrix();
        camera.view = cv->getCurrentCamera()->getViewMatrix();
        camera.frame = cv->getTraversalNumber();
        camera.cameraEpoch = mRendering.getCamera()->temporalEpoch();
        camera.worldEpoch = mTemporalWorldEpoch;
        camera.resourceEpoch = mTemporalResourceEpoch;
        camera.renderWidth = renderWidth();
        camera.renderHeight = renderHeight();
        camera.outputWidth = outputWidth();
        camera.outputHeight = outputHeight();
        camera.zeroToOne = SceneUtil::AutoDepth::isReversed();
        camera.clearDepth = cv->getCurrentCamera()->getClearDepth();
        if (SceneUtil::temporalDynamicMotionEnabled())
        {
            auto& dynamic = mTemporalDynamicFrames[camera.frame % 2];
            dynamic = std::make_shared<TemporalDynamicFrame>();
            camera.dynamic = dynamic;
        }
        mCanvases[camera.frame % 2]->setTemporalCamera(std::move(camera));
    }

    void PostProcessor::captureTemporalDynamic(osgUtil::CullVisitor* cv)
    {
        if (!mTemporalMotion || !cv || Stereo::getStereo()) return;
        const unsigned frameId = cv->getTraversalNumber() % 2;
        if (mTemporalOwnershipAvailable && cv->getCurrentRenderStage())
        {
            // HUD is culled before the world. Only now have all point lights
            // been collected. Freeze the complete cv-local uniform/UBO data
            // into the exact acquired leaf before Renderer publishes draw.
            auto fxState = mStateUpdater->ownedFrame(cv);
            PingPongCanvas::finalizeTemporalOwner(mTemporalCanvasOwner, cv, fxState);
            auto freezeBin = [&](auto&& self, osgUtil::RenderBin& bin) -> void {
                if (const auto* callback = bin.getDrawCallback())
                {
                    if (typeid(*callback) == typeid(TransparentDepthBinCallback))
                        bin.setDrawCallback(mTransparentDepthPostPass->ownedFrame(frameId));
                    else if (typeid(*callback) == typeid(DistortionCallback))
                        bin.setDrawCallback(mDistortionCallback->ownedFrame(mFbos[frameId][FBO_Distortion],
                            mFbos[frameId][FBO_Primary], getPrimaryFbo(frameId)));
                    else if (typeid(*callback) == typeid(DepthClearCallback))
                        bin.setDrawCallback(static_cast<const DepthClearCallback*>(callback)->ownedFrame(
                            mFbos[frameId][FBO_FirstPerson],mFbos[frameId][FBO_OpaqueDepth],getPrimaryFbo(frameId)));
                }
                for (const auto& child : bin.getRenderBinList()) self(self,*child.second);
            };
            freezeBin(freezeBin,*cv->getCurrentRenderStage());
            RenderCore::snapshotRenderStageCamera(*cv->getCurrentRenderStage(),nullptr,true);
        }
        if (!SceneUtil::temporalDynamicMotionEnabled()) return;
        auto& dynamic = mTemporalDynamicFrames[cv->getTraversalNumber() % 2];
        // All HUD snapshot readers retain this per-submission frame, and the
        // Renderer does not publish drawQueue until the whole cull returns.
        // Capture exact already-visible/deformed RenderLeaves after traversal.
        if (dynamic && cv->getCurrentRenderStage()) dynamic->capture(*cv->getCurrentRenderStage());
    }

    void PostProcessor::cull(unsigned frameId, osgUtil::CullVisitor* cv)
    {
        if (const auto& fbo = getFbo(FBO_Intercept, frameId))
        {
            osgUtil::RenderStage* rs = cv->getRenderStage();
            if (rs && rs->getMultisampleResolveFramebufferObject())
                rs->setMultisampleResolveFramebufferObject(fbo);
        }

        mCanvases[frameId]->setPostProcessing(mUsePostProcessing);
        mCanvases[frameId]->setTextureNormals(mNormals ? getTexture(Tex_Normal, frameId) : nullptr);
        mCanvases[frameId]->setMask(mUnderwater, mExteriorFlag);
        mCanvases[frameId]->setCalculateAvgLum(mHDR);

        mCanvases[frameId]->setTextureScene(getTexture(Tex_Scene, frameId));
        mCanvases[frameId]->setTextureDepth(getTexture(Tex_OpaqueDepth, frameId));
        mCanvases[frameId]->setTextureDistortion(getTexture(Tex_Distortion, frameId));

        mTransparentDepthPostPass->mFbo[frameId] = mFbos[frameId][FBO_Primary];
        mTransparentDepthPostPass->mMsaaFbo[frameId] = mFbos[frameId][FBO_Multisample];
        mTransparentDepthPostPass->mOpaqueFbo[frameId] = mFbos[frameId][FBO_OpaqueDepth];

        mDistortionCallback->setFBO(mFbos[frameId][FBO_Distortion], frameId);
        mDistortionCallback->setOriginalFBO(mFbos[frameId][FBO_Primary], frameId);

        size_t frame = cv->getTraversalNumber();

        // V3.18: PostFX/shader resolution follows the internal 3D render target,
        // while the HUD camera and final presentation stay at native output size.
        mStateUpdater->setResolution(
            osg::Vec2f(static_cast<float>(renderWidth()), static_cast<float>(renderHeight())));

        // per-frame data
        if (frame != mLastFrameNumber)
        {
            mLastFrameNumber = frame;
            auto stamp = cv->getFrameStamp();

            mStateUpdater->setSimulationTime(static_cast<float>(stamp->getSimulationTime()));
            mStateUpdater->setDeltaSimulationTime(static_cast<float>(stamp->getSimulationTime() - mLastSimulationTime));
            // Use a signed int because 'uint' type is not supported in GLSL 120 without extensions
            mStateUpdater->setFrameNumber(static_cast<int>(stamp->getFrameNumber()));
            mLastSimulationTime = stamp->getSimulationTime();

            for (const auto& dispatchNode : mCanvases[frameId]->getPasses())
            {
                for (auto& uniform : dispatchNode.mHandle->getUniformMap())
                {
                    if (uniform->getType().has_value() && !uniform->mSamplerType)
                        if (auto* u = dispatchNode.mRootStateSet->getUniform(uniform->mName))
                            uniform->setUniform(u);
                }
            }
        }
    }

    void PostProcessor::updateLiveReload()
    {
        if (!mEnableLiveReload && !mTriggerShaderReload)
            return;

        mTriggerShaderReload = false; // Done only once

        for (auto& technique : mTechniques)
        {
            if (technique->getStatus() == Fx::Technique::Status::File_Not_exists)
                continue;

            const auto lastWriteTime = mVFS->getLastModified(technique->getFileName());
            const bool isDirty = technique->setLastModificationTime(lastWriteTime);

            if (!isDirty)
                continue;

            // TODO: Temporary workaround to avoid conflicts with external programs saving the file, especially
            // problematic on Windows.
            //       If we move to a file watcher using native APIs this should be removed.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            if (technique->compile())
                Log(Debug::Info) << "Reloaded technique : " << technique->getFileName();

            mReload = technique->isValid();
        }
    }

    void PostProcessor::reloadIfRequired()
    {
        if (!mReload)
            return;

        mReload = false;

        loadChain();
        resize();
    }

    void PostProcessor::update(size_t frameId)
    {
        while (!mQueuedTemplates.empty())
        {
            mTemplates.push_back(std::move(mQueuedTemplates.back()));

            mQueuedTemplates.pop_back();
        }

        updateLiveReload();

        reloadIfRequired();

        mCanvases[frameId]->setNodeMask(~0u);
        mCanvases[!frameId]->setNodeMask(0);

        if (mDirty && mDirtyFrameId == frameId)
        {
            createObjectsForFrame(frameId);

            mDirty = false;
            mCanvases[frameId]->setPasses(Fx::DispatchArray(mTemplateData), mTemplateTargetGeneration);
        }

        if ((mNormalsSupported && mNormals != mPrevNormals) || (mPassLights != mPrevPassLights))
        {
            mPrevNormals = mNormals;
            mPrevPassLights = mPassLights;

            mViewer->stopThreading();

            if (mNormalsSupported)
            {
                auto& shaderManager
                    = MWBase::Environment::get().getResourceSystem()->getSceneManager()->getShaderManager();
                auto defines = shaderManager.getGlobalDefines();
                defines["disableNormals"] = mNormals ? "0" : "1";
                shaderManager.setGlobalDefines(defines);
            }

            mRendering.getLightRoot()->setCollectPPLights(mPassLights);
            mStateUpdater->bindPointLights(mPassLights ? mRendering.getLightRoot()->getPPLightsBuffer() : nullptr);
            mStateUpdater->reset();

            mViewer->startThreading();

            createObjectsForFrame(frameId);

            mDirty = true;
            mDirtyFrameId = !frameId;
        }
    }

    void PostProcessor::createObjectsForFrame(size_t frameId)
    {
        ++mTemporalResourceEpoch;
        auto& textures = mTextures[frameId];

        int width = renderWidth();
        int height = renderHeight();

        for (osg::ref_ptr<osg::Texture>& texture : textures)
        {
            if (!texture || (TemporalMotion::ownershipEnabled() && !Stereo::getStereo()))
            {
                if (Stereo::getMultiview())
                    texture = new osg::Texture2DArray;
                else
                    texture = new osg::Texture2D;
            }
            Stereo::setMultiviewCompatibleTextureSize(texture, width, height);
            texture->setSourceFormat(GL_RGBA);
            texture->setSourceType(GL_UNSIGNED_BYTE);
            texture->setInternalFormat(GL_RGBA);
            texture->setFilter(osg::Texture2D::MIN_FILTER, osg::Texture::LINEAR);
            texture->setFilter(osg::Texture2D::MAG_FILTER, osg::Texture::LINEAR);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setResizeNonPowerOfTwoHint(false);
            Stereo::setMultiviewCompatibleTextureSize(texture, width, height);
            texture->dirtyTextureObject();
        }

        // f16 normals: u8 isn't quite accurate enough even for opaque objects, but now we also
        //  need to blend terrain normals, and it's additive, so now u8 rounding losses would pile up.
        textures[Tex_Normal]->setSourceFormat(GL_RGB);
        textures[Tex_Normal]->setSourceType(GL_HALF_FLOAT);
        textures[Tex_Normal]->setInternalFormat(GL_RGB16F);

        textures[Tex_Distortion]->setSourceFormat(GL_RGB);
        textures[Tex_Distortion]->setInternalFormat(GL_RGB);

        Stereo::setMultiviewCompatibleTextureSize(textures[Tex_Distortion], static_cast<int>(width * DistortionRatio),
            static_cast<int>(height * DistortionRatio));
        textures[Tex_Distortion]->dirtyTextureObject();

        auto setupDepth = [](osg::Texture* tex) {
            tex->setSourceFormat(GL_DEPTH_STENCIL_EXT);
            tex->setSourceType(SceneUtil::AutoDepth::depthSourceType());
            tex->setInternalFormat(SceneUtil::AutoDepth::depthInternalFormat());
        };

        setupDepth(textures[Tex_Depth]);
        setupDepth(textures[Tex_OpaqueDepth]);
        textures[Tex_OpaqueDepth]->setName("opaqueTexMap");

        auto& fbos = mFbos[frameId];

        fbos[FBO_Primary] = new osg::FrameBufferObject;
        fbos[FBO_Primary]->setAttachment(
            osg::Camera::COLOR_BUFFER0, Stereo::createMultiviewCompatibleAttachment(textures[Tex_Scene]));
        if (mNormals && mNormalsSupported)
            fbos[FBO_Primary]->setAttachment(
                osg::Camera::COLOR_BUFFER1, Stereo::createMultiviewCompatibleAttachment(textures[Tex_Normal]));
        fbos[FBO_Primary]->setAttachment(
            osg::Camera::PACKED_DEPTH_STENCIL_BUFFER, Stereo::createMultiviewCompatibleAttachment(textures[Tex_Depth]));

        fbos[FBO_FirstPerson] = new osg::FrameBufferObject;

        auto fpDepthRb = createFrameBufferAttachmentFromTemplate(
            Usage::RENDER_BUFFER, width, height, textures[Tex_Depth], mSamples);
        fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER,
            osg::FrameBufferAttachment(fpDepthRb));

        if (mSamples > 1)
        {
            fbos[FBO_Multisample] = new osg::FrameBufferObject;
            fbos[FBO_Intercept] = new osg::FrameBufferObject;
            auto colorRB = createFrameBufferAttachmentFromTemplate(
                Usage::RENDER_BUFFER, width, height, textures[Tex_Scene], mSamples);
            if (mNormals && mNormalsSupported)
            {
                auto normalRB = createFrameBufferAttachmentFromTemplate(
                    Usage::RENDER_BUFFER, width, height, textures[Tex_Normal], mSamples);
                fbos[FBO_Multisample]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER1, normalRB);
                fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER1, normalRB);
                fbos[FBO_Intercept]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER1,
                    Stereo::createMultiviewCompatibleAttachment(textures[Tex_Normal]));
            }
            auto depthRB = createFrameBufferAttachmentFromTemplate(
                Usage::RENDER_BUFFER, width, height, textures[Tex_Depth], mSamples);
            fbos[FBO_Multisample]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, colorRB);
            fbos[FBO_Multisample]->setAttachment(
                osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER, depthRB);
            fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0, colorRB);

            fbos[FBO_Intercept]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0,
                Stereo::createMultiviewCompatibleAttachment(textures[Tex_Scene]));
        }
        else
        {
            fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0,
                Stereo::createMultiviewCompatibleAttachment(textures[Tex_Scene]));
            if (mNormals && mNormalsSupported)
                fbos[FBO_FirstPerson]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER1,
                    Stereo::createMultiviewCompatibleAttachment(textures[Tex_Normal]));
        }

        fbos[FBO_OpaqueDepth] = new osg::FrameBufferObject;
        fbos[FBO_OpaqueDepth]->setAttachment(osg::FrameBufferObject::BufferComponent::PACKED_DEPTH_STENCIL_BUFFER,
            Stereo::createMultiviewCompatibleAttachment(textures[Tex_OpaqueDepth]));

        fbos[FBO_Distortion] = new osg::FrameBufferObject;
        fbos[FBO_Distortion]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0,
            Stereo::createMultiviewCompatibleAttachment(textures[Tex_Distortion]));

#ifdef __APPLE__
        if (textures[Tex_OpaqueDepth])
            fbos[FBO_OpaqueDepth]->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER,
                osg::FrameBufferAttachment(new osg::RenderBuffer(textures[Tex_OpaqueDepth]->getTextureWidth(),
                    textures[Tex_OpaqueDepth]->getTextureHeight(), textures[Tex_Scene]->getInternalFormat())));
#endif

        mCanvases[frameId]->dirty();
    }

    void PostProcessor::dirtyTechniques(bool dirtyAttachments)
    {
        size_t frameId = frame() % 2;

        mDirty = true;
        mDirtyFrameId = !frameId;

        mTemplateData = {};

        bool sunglare = true;
        mHDR = false;
        mNormals = false;
        mPassLights = false;

        std::vector<Fx::Types::RenderTarget> attachmentsToDirty;
        osgViewer::ViewerBase::Contexts contexts;
        mViewer->getContexts(contexts, false);
        auto* primaryContext = mViewer->getCamera()->getGraphicsContext();
        // Initial chain setup precedes the final threading-model selection.
        // Require an assigned single native context here; draw qualification
        // independently checks its serialized Renderer/threading contract.
        const bool ownedTargets = TemporalMotion::ownershipEnabled() && !Stereo::getStereo()
            && primaryContext && primaryContext->valid() && primaryContext->getState()
            && contexts.size() == 1 && contexts.front() == primaryContext;
        mTemplateTargetGeneration = ownedTargets ? std::make_shared<PostFxTargetGeneration>() : nullptr;

        for (const auto& technique : mTechniques)
        {
            if (!technique->isValid())
                continue;

            if (technique->getGLSLVersion() > mGLSLVersion)
            {
                Log(Debug::Warning) << "Technique " << technique->getName() << " requires GLSL version "
                                    << technique->getGLSLVersion() << " which is unsupported by your hardware.";
                continue;
            }

            Fx::DispatchNode node;

            if (ownedTargets)
            {
                // A delayed acquired SceneView may still sample any old
                // target. Replace generation objects before resizing or
                // dirtying; never modify those retained by an unfinished draw.
                for (auto& [name, target] : technique->getRenderTargetsMap())
                    target.mTarget = new osg::Texture2D(*target.mTarget, osg::CopyOp::SHALLOW_COPY);
            }

            node.mFlags = technique->getFlags();

            if (technique->getHDR())
                mHDR = true;

            if (technique->getNormals())
                mNormals = true;

            if (technique->getLights())
                mPassLights = true;

            if (node.mFlags & Fx::Technique::Flag_Disable_SunGlare)
                sunglare = false;

            // required default samplers available to every shader pass
            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerLastShader", Unit_LastShader));
            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerLastPass", Unit_LastPass));
            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerDepth", Unit_Depth));
            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerDistortion", Unit_Distortion));

            if (mNormals)
                node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerNormals", Unit_Normals));

            if (technique->getHDR())
                node.mRootStateSet->addUniform(new osg::Uniform("omw_EyeAdaptation", Unit_EyeAdaptation));

            node.mRootStateSet->addUniform(new osg::Uniform("omw_SamplerDistortion", Unit_Distortion));

            int texUnit = Unit_NextFree;

            // user-defined samplers
            for (const osg::Texture* texture : technique->getTextures())
            {
                if (const auto* tex1D = dynamic_cast<const osg::Texture1D*>(texture))
                    node.mRootStateSet->setTextureAttribute(texUnit, new osg::Texture1D(*tex1D));
                else if (const auto* tex2D = dynamic_cast<const osg::Texture2D*>(texture))
                    node.mRootStateSet->setTextureAttribute(texUnit, new osg::Texture2D(*tex2D));
                else if (const auto* tex3D = dynamic_cast<const osg::Texture3D*>(texture))
                    node.mRootStateSet->setTextureAttribute(texUnit, new osg::Texture3D(*tex3D));

                node.mRootStateSet->addUniform(new osg::Uniform(texture->getName().c_str(), texUnit++));
            }

            // user-defined uniforms
            for (auto& uniform : technique->getUniformMap())
            {
                if (uniform->mSamplerType)
                    continue;

                if (auto type = uniform->getType())
                    uniform->setUniform(node.mRootStateSet->getOrCreateUniform(
                        uniform->mName, *type, static_cast<unsigned>(uniform->getNumElements())));
            }

            for (const auto& pass : technique->getPasses())
            {
                int subTexUnit = texUnit;
                Fx::DispatchNode::SubPass subPass;

                pass->prepareStateSet(subPass.mStateSet, technique->getName());

                node.mHandle = technique;

                if (!pass->getTarget().empty())
                {
                    // FIXME: https://gitlab.com/OpenMW/openmw/-/work_items/9034
                    std::string target = pass->getTarget();
                    auto& renderTarget = technique->getRenderTargetsMap()[target];
                    subPass.mSize = renderTarget.mSize;
                    subPass.mRenderTexture = renderTarget.mTarget;
                    subPass.mMipMap = renderTarget.mMipMap;

                    const auto [w, h] = renderTarget.mSize.get(renderWidth(), renderHeight());
                    subPass.mStateSet->setAttributeAndModes(new osg::Viewport(0, 0, w, h));

                    if (subPass.mMipMap)
                    {
                        subPass.mRenderTexture->setNumMipmapLevels(osg::Image::computeNumberOfMipmapLevels(w, h));
                    }
                    else
                    {
                        subPass.mRenderTexture->setNumMipmapLevels(0);
                    }
                    subPass.mRenderTexture->setTextureSize(w, h);
                    subPass.mRenderTexture->dirtyTextureObject();

                    subPass.mRenderTarget = new osg::FrameBufferObject;
                    subPass.mRenderTarget->setAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0,
                        osg::FrameBufferAttachment(subPass.mRenderTexture));

                    if (std::find_if(attachmentsToDirty.cbegin(), attachmentsToDirty.cend(),
                            [renderTarget](const auto& rt) { return renderTarget.mTarget == rt.mTarget; })
                        == attachmentsToDirty.cend())
                    {
                        attachmentsToDirty.push_back(Fx::Types::RenderTarget(renderTarget));
                    }
                }

                for (const auto& name : pass->getRenderTargets())
                {
                    if (name.empty())
                    {
                        continue;
                    }

                    auto& renderTarget = technique->getRenderTargetsMap()[name];
                    subPass.mStateSet->setTextureAttribute(subTexUnit, renderTarget.mTarget);
                    subPass.mStateSet->addUniform(new osg::Uniform(name.c_str(), subTexUnit));

                    if (std::find_if(attachmentsToDirty.cbegin(), attachmentsToDirty.cend(),
                            [renderTarget](const auto& rt) { return renderTarget.mTarget == rt.mTarget; })
                        == attachmentsToDirty.cend())
                    {
                        attachmentsToDirty.push_back(Fx::Types::RenderTarget(renderTarget));
                    }
                    subTexUnit++;
                }

                node.mPasses.emplace_back(std::move(subPass));
            }

            node.compile();

            mTemplateData.emplace_back(std::move(node));
        }

        if (mTemplateTargetGeneration) mTemplateTargetGeneration->attachments = attachmentsToDirty;
        mCanvases[frameId]->setPasses(Fx::DispatchArray(mTemplateData), mTemplateTargetGeneration);

        if (static_cast<bool>(Settings::cells().mV314PostfxCompileWarmup))
        {
            osgUtil::IncrementalCompileOperation* const ico = mRendering.getIncrementalCompileOperation();
            if (ico)
            {
                osg::ref_ptr<osg::Group> compileRoot = new osg::Group;
                for (const auto& dispatch : mTemplateData)
                {
                    osg::ref_ptr<osg::Group> techniqueRoot = new osg::Group;
                    techniqueRoot->setStateSet(dispatch.mRootStateSet);
                    compileRoot->addChild(techniqueRoot);
                    for (const auto& subPass : dispatch.mPasses)
                    {
                        osg::ref_ptr<osg::Group> passNode = new osg::Group;
                        passNode->setStateSet(subPass.mStateSet);
                        techniqueRoot->addChild(passNode);
                    }
                }

                auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(compileRoot);
                const auto compileMode = static_cast<osgUtil::GLObjectsVisitor::Mode>(
                    osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES);
                compileSet->buildCompileMap(ico->getContextSet(), compileMode);
                ico->add(compileSet, false);
                Log(Debug::Info) << "V3.14 queued active PostFX chain for ICO compile warmup";
            }
        }

        if (auto hud = MWBase::Environment::get().getWindowManager()->getPostProcessorHud())
            hud->updateTechniques();

        if (mUsePostProcessing)
            mRendering.getSkyManager()->setSunglare(sunglare);

        if (dirtyAttachments)
            mCanvases[frameId]->setDirtyAttachments(attachmentsToDirty);
    }

    PostProcessor::Status PostProcessor::enableTechnique(
        std::shared_ptr<Fx::Technique> technique, std::optional<int> location)
    {
        if (technique->getLocked() || (location.has_value() && location.value() < 0))
            return Status_Error;

        disableTechnique(technique, false);

        size_t pos = std::min(location.value_or(mTechniques.size()) + mInternalTechniques.size(), mTechniques.size());

        mTechniques.insert(mTechniques.begin() + pos, technique);
        dirtyTechniques(Settings::ShaderManager::get().getMode() == Settings::ShaderManager::Mode::Debug);

        return Status_Toggled;
    }

    PostProcessor::Status PostProcessor::disableTechnique(std::shared_ptr<Fx::Technique> technique, bool dirty)
    {
        if (technique->getLocked())
            return Status_Error;

        auto it = std::find(mTechniques.begin(), mTechniques.end(), technique);
        if (it == std::end(mTechniques))
            return Status_Unchanged;

        mTechniques.erase(it);
        if (dirty)
            dirtyTechniques();

        return Status_Toggled;
    }

    bool PostProcessor::isTechniqueEnabled(const std::shared_ptr<Fx::Technique>& technique) const
    {
        if (auto it = std::find(mTechniques.begin(), mTechniques.end(), technique); it == mTechniques.end())
            return false;

        return technique->isValid();
    }

    std::shared_ptr<Fx::Technique> PostProcessor::loadTechnique(std::string_view name, bool loadNextFrame)
    {
        VFS::Path::Normalized path = Fx::Technique::makeFileName(name);
        return loadTechnique(VFS::Path::NormalizedView(path), loadNextFrame);
    }

    std::shared_ptr<Fx::Technique> PostProcessor::loadTechnique(VFS::Path::NormalizedView path, bool loadNextFrame)
    {
        for (const auto& technique : mTemplates)
            if (technique->getFileName() == path)
                return technique;

        for (const auto& technique : mQueuedTemplates)
            if (technique->getFileName() == path)
                return technique;

        std::string name;
        if (mTechniqueFiles.contains(path))
            name = mVFS->getStem(path);
        else
            name = path.stem();

        auto technique = std::make_shared<Fx::Technique>(*mVFS, *mRendering.getResourceSystem()->getImageManager(),
            path, std::move(name), renderWidth(), renderHeight(), mUBO, mNormalsSupported);

        technique->compile();

        if (technique->getStatus() != Fx::Technique::Status::File_Not_exists)
            technique->setLastModificationTime(mVFS->getLastModified(path));

        if (loadNextFrame)
        {
            mQueuedTemplates.push_back(technique);
            return technique;
        }

        mTemplates.push_back(std::move(technique));

        return mTemplates.back();
    }

    PostProcessor::TechniqueList PostProcessor::getChain()
    {
        return mTechniques;
    }

    void PostProcessor::loadChain()
    {
        mTechniques.clear();

        for (const auto& technique : mInternalTechniques)
        {
            mTechniques.push_back(technique);
        }

        for (const std::string& techniqueName : Settings::postProcessing().mChain.get())
        {
            if (techniqueName.empty())
                continue;

            mTechniques.push_back(loadTechnique(techniqueName));
        }

        dirtyTechniques();
    }

    void PostProcessor::saveChain()
    {
        std::vector<std::string> chain;

        for (const auto& technique : mTechniques)
        {
            if (technique->getDynamic() || technique->getInternal())
                continue;
            chain.push_back(technique->getName());
        }

        Settings::postProcessing().mChain.set(chain);
    }

    void PostProcessor::toggleMode()
    {
        for (auto& technique : mTemplates)
        {
            if (technique->getStatus() == Fx::Technique::Status::File_Not_exists)
                continue;
            technique->compile();
        }

        dirtyTechniques(true);
    }

    void PostProcessor::disableDynamicShaders()
    {
        auto erased = std::erase_if(mTechniques, [](const auto& technique) { return technique->getDynamic(); });

        if (erased)
            dirtyTechniques();
    }

    int PostProcessor::renderWidth() const
    {
        if (Stereo::getStereo())
            return Stereo::Manager::instance().eyeResolution().x();
        const float scale = Settings::video().mRenderScale;
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(mWidth) * scale)));
    }

    int PostProcessor::renderHeight() const
    {
        if (Stereo::getStereo())
            return Stereo::Manager::instance().eyeResolution().y();
        const float scale = Settings::video().mRenderScale;
        return std::max(1, static_cast<int>(std::lround(static_cast<double>(mHeight) * scale)));
    }

    bool PostProcessor::renderScalingActive() const
    {
        return !Stereo::getStereo() && renderWidth() != outputWidth() && renderHeight() != outputHeight();
    }

    void PostProcessor::triggerShaderReload()
    {
        mTriggerShaderReload = true;
    }
}
