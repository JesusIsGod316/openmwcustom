#include <apps/openmw/mwrender/pingpongcanvas.hpp>
#include <components/fx/stateupdater.hpp>
#include <components/rendercore/ownedrenderstage.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/stereo/stereomanager.hpp>
#include <osg/BufferIndexBinding>
#include <osg/Geode>
#include <osg/GLExtensions>
#include <osg/GLObjects>
#include <osg/Image>
#include <osgUtil/SceneView>
#include <osgViewer/Renderer>
#include <osgViewer/Viewer>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <map>

namespace
{
    struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    std::string read(const std::filesystem::path& path)
    {
        std::ifstream input(path); require(input.good(), "fixture production shader missing");
        return {std::istreambuf_iterator<char>(input), {}};
    }
    void write(const std::filesystem::path& path, const std::string& value)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path); output << value; require(output.good(), "fixture shader write failed");
    }
    osg::ref_ptr<osg::Texture2D> solid(unsigned size, const osg::Vec4f& color, bool depth = false)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(size, size, 1, depth ? GL_RED : GL_RGBA, GL_FLOAT);
        // Fill exactly the allocated component count, including GL_RED.
        // Image::setColor's supported formats vary across pinned OSG builds.
        auto* values = reinterpret_cast<float*>(image->data());
        for (unsigned i = 0; i < size * size; ++i)
            if (depth) values[i] = color.r();
            else for (unsigned channel = 0; channel < 4; ++channel) values[i * 4 + channel] = color[channel];
        osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D(image);
        result->setTextureSize(size, size);
        result->setInternalFormat(depth ? GL_R32F : GL_RGBA8);
        result->setResizeNonPowerOfTwoHint(false);
        result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        result->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        result->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        return result;
    }
    osg::ref_ptr<osg::Program> program(const std::string& fragment, bool globals = false, bool useUBO = true)
    {
        osg::ref_ptr<osg::Program> result = new osg::Program;
        result->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 330 compatibility\nout vec2 uv;void main(){gl_Position=vec4(gl_Vertex.xy,0,1);uv=gl_Vertex.xy*.5+.5;}\n"));
        result->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 330 compatibility\nin vec2 uv;uniform sampler2D scene,history,lastPass;uniform float userGain,passGain;\n"
            + (globals ? Fx::StateUpdater::getStructDefinition()
                + (useUBO ? "layout(std140) uniform _data { _omw_data omw; };" : "uniform _omw_data omw;")
                + "uniform int omw_PointLightsCount;"
                  "uniform vec4 omw_PointLights[120];\n" : std::string{})
            + "void main(){" + fragment + "}\n"));
        if (globals && useUBO) result->addBindUniformBlock("_data", static_cast<unsigned>(Resource::SceneManager::UBOBinding::PostProcessor));
        return result;
    }
    struct Results
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool delay = false, entered = false, release = false;
        unsigned culled = 0, completed = 0;
        std::vector<osg::Vec4f> expected;
        std::vector<unsigned> outputSize;
        std::vector<osg::ref_ptr<osg::Program>> resolvePrograms;
        std::vector<osg::Vec4f> lightDiffuse;
        std::string error;
        void wait(unsigned count)
        {
            std::unique_lock lock(mutex);
            require(changed.wait_for(lock, std::chrono::seconds(8), [&] { return completed >= count || !error.empty(); }),
                "production canvas draw did not finish");
            require(error.empty(), error.c_str());
        }
    };
    struct Delay : osg::Camera::DrawCallback
    {
        std::shared_ptr<Results> results;
        explicit Delay(std::shared_ptr<Results> value) : results(std::move(value)) {}
        void operator()(osg::RenderInfo&) const override
        {
            std::unique_lock lock(results->mutex);
            if (results->delay && !results->entered)
            {
                results->entered = true; results->changed.notify_all();
                if (!results->changed.wait_for(lock, std::chrono::seconds(8), [&] { return results->release; }))
                    results->error = "delayed production canvas was not released";
            }
        }
    };
    struct Pixels : osg::Camera::DrawCallback
    {
        std::shared_ptr<Results> results;
        explicit Pixels(std::shared_ptr<Results> value) : results(std::move(value)) {}
        void operator()(osg::RenderInfo& info) const override
        {
            try
            {
                osg::Vec4f expected; unsigned size = 0;
                {
                    std::lock_guard lock(results->mutex);
                    expected = results->expected.at(results->completed);
                    size = results->outputSize.at(results->completed);
                }
                info.getState()->get<osg::GLExtensions>()->glBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
                glReadBuffer(GL_FRONT);
                std::vector<unsigned char> pixels(size * size * 4);
                glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                for (std::size_t i = 0; i < pixels.size(); ++i)
                    if (std::abs(int(pixels[i]) - int(std::lround(expected[i % 4] * 255))) > 3)
                    {
                        std::cerr << "draw=" << results->completed << " channel=" << i % 4
                            << " actual=" << int(pixels[i]) << " expected=" << expected[i % 4] * 255 << '\n';
                        auto* ext = info.getState()->get<osg::GLExtensions>();
                        const auto handle = results->resolvePrograms.at(results->completed)->getPCP(*info.getState())->getHandle();
                        float diffuse[4]{};
                        ext->glGetUniformfv(handle, ext->glGetUniformLocation(handle, "omw_PointLights[1]"), diffuse);
                        std::cerr << "GPU light=" << diffuse[0] << ',' << diffuse[1] << ',' << diffuse[2]
                            << " expected=" << results->lightDiffuse.at(results->completed).x() << ','
                            << results->lightDiffuse.at(results->completed).y() << ','
                            << results->lightDiffuse.at(results->completed).z() << '\n';
                        throw std::runtime_error("production PostFX consumed another cull's uniforms, target or lights");
                    }
                require(glGetError() == GL_NO_ERROR, "production PostFX generated GL errors");
            }
            catch (const std::exception& error)
            {
                std::lock_guard lock(results->mutex); results->error = error.what();
            }
            { std::lock_guard lock(results->mutex); ++results->completed; }
            results->changed.notify_all();
        }
    };
    struct Inspect : osg::Drawable::DrawCallback
    {
        void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
        {
            require(glGetError() == GL_NO_ERROR, "PostFX inherited state generated GL errors");
            const auto depth = info.getState()->getStateSetStack().size();
            drawable->drawImplementation(info);
            require(glGetError() == GL_NO_ERROR, "production canvas draw generated GL errors");
            require(info.getState()->getStateSetStack().size() == depth,
                "production owned PostFX failed to restore the caller's state stack");
        }
    };
    MWRender::PingPongCanvas* leaf(osgUtil::RenderBin& bin)
    {
        for (auto* graph : bin.getStateGraphList())
            for (const auto& value : graph->_leaves)
                if (auto* canvas = dynamic_cast<MWRender::PingPongCanvas*>(value->_drawable.get())) return canvas;
        for (auto& [order, child] : bin.getRenderBinList())
            if (auto* canvas = leaf(*child)) return canvas;
        return nullptr;
    }
    struct Root : osg::Group
    {
        osgViewer::Viewer* viewer = nullptr;
        osg::ref_ptr<MWRender::PingPongCanvas> canvas;
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        osg::ref_ptr<Fx::StateUpdater> fx = new Fx::StateUpdater(true);
        std::shared_ptr<SceneUtil::PPLightBuffer> lights = std::make_shared<SceneUtil::PPLightBuffer>();
        std::shared_ptr<MWRender::TemporalCanvasOwner> owner;
        std::shared_ptr<Results> results;
        osg::ref_ptr<osg::Texture2D> scene, depth, history;
        float gain = .5f, fog = 2.f;
        osg::Vec4f diffuse = {1, 2, 3, 1};
        unsigned size = 32;
        bool expectedOwned = true;
        bool useUBO = true;
        osg::Vec4f expected;
        std::map<const osgUtil::CullVisitor*, osg::ref_ptr<osg::BufferObject>> slotBuffers;
        void traverse(osg::NodeVisitor& nv) override
        {
            if (nv.getVisitorType() != osg::NodeVisitor::CULL_VISITOR) { osg::Group::traverse(nv); return; }
            auto* cv = static_cast<osgUtil::CullVisitor*>(&nv);
            auto* renderer = dynamic_cast<osgViewer::Renderer*>(viewer->getCamera()->getRenderer());
            require(renderer && (renderer->getSceneView(0)->getCullVisitor() == cv
                || renderer->getSceneView(1)->getCullVisitor() == cv), "fixture used an unacquired visitor");
            MWRender::TemporalCamera camera;
            camera.frame = cv->getTraversalNumber(); camera.renderWidth = camera.renderHeight = size;
            camera.outputWidth = camera.outputHeight = 32; camera.projection = cv->getProjectionMatrix();
            canvas->setTemporalCamera(camera);
            canvas->setTextureScene(scene); canvas->setTextureDepth(depth);
            auto early = fx->ownedFrame(cv);
            require(early.valid(), "known production Fx bindings rejected before leaf creation");
            canvas->setOwnedFxState(early);
            canvas->setTemporalOwner(owner); canvas->setTemporalOwnershipAvailable(expectedOwned, "fixture_fallback");
            canvas->setMask(false, true);
            osg::ref_ptr<osg::Viewport> viewport = new osg::Viewport(*viewer->getCamera()->getViewport());
            osg::ref_ptr<osg::StateSet> hudState = new osg::StateSet;
            hudState->setAttribute(viewport);
            cv->pushStateSet(hudState); // Actual HUD keeps Fx globals in ancestor stacks.
            geode->accept(nv); // HUD cull happens BEFORE world point-light fill.
            cv->popStateSet();
            auto* acquired = leaf(*cv->getCurrentRenderStage());
            require(acquired && (acquired != canvas.get()) == expectedOwned, "production canvas ownership gate did not select its control path");
            require(acquired->getDataVariance() == (expectedOwned ? osg::Object::STATIC : osg::Object::DYNAMIC),
                "canvas data variance contradicts effective ownership");
            // Real per-parity point-light arrays are updated after the HUD leaf.
            lights->clear(cv->getTraversalNumber());
            osg::ref_ptr<SceneUtil::Light> light = new SceneUtil::Light; light->setDiffuse(diffuse);
            lights->setLight(cv->getTraversalNumber(), light, 10.f);
            lights->updateCount(cv->getTraversalNumber());
            auto complete = fx->ownedFrame(cv);
            if (expectedOwned)
            {
                MWRender::PingPongCanvas::finalizeTemporalOwner(owner, cv, complete);
                osg::Vec4f capturedDiffuse;
                int capturedCount = 0;
                require(acquired->getStateSet()->getUniform("omw_PointLights")->getElement(1, capturedDiffuse)
                    && acquired->getStateSet()->getUniform("omw_PointLightsCount")->get(capturedCount)
                    && capturedDiffuse == diffuse && capturedCount == 1,
                    "finalized owned leaf did not retain the current world's light data");
                require(acquired->getStateSet()->getUniform("omw_PointLights") != early->getUniform("omw_PointLights"),
                    "owned light uniform reused source array storage");
                if (!useUBO)
                    require(acquired->getStateSet()->getUniform("omw.fogNear") != early->getUniform("omw.fogNear"),
                        "owned scalar globals reused source uniforms");
                else
                {
                    auto* liveBinding = early->getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                        static_cast<unsigned>(Resource::SceneManager::UBOBinding::PostProcessor));
                    auto* ownedBinding = acquired->getStateSet()->getAttribute(osg::StateAttribute::UNIFORMBUFFERBINDING,
                        static_cast<unsigned>(Resource::SceneManager::UBOBinding::PostProcessor));
                    require(liveBinding != ownedBinding, "acquired Fx data reused an earlier buffer binding");
                    auto* binding = dynamic_cast<osg::UniformBufferBinding*>(ownedBinding);
                    auto* original = dynamic_cast<osg::UniformBufferBinding*>(liveBinding);
                    require(binding && original && binding->getBufferData() != original->getBufferData()
                        && binding->getBufferData()->getBufferObject() != original->getBufferData()->getBufferObject(),
                        "owned Fx UBO has shared CPU or GPU storage");
                    const auto previous = slotBuffers.find(cv);
                    if (previous != slotBuffers.end())
                        require(previous->second == binding->getBufferData()->getBufferObject(),
                            "retired acquired slot allocated another private GPU UBO");
                    else
                    {
                        for (const auto& [otherVisitor, buffer] : slotBuffers)
                            require(buffer != binding->getBufferData()->getBufferObject(), "two pending SceneViews shared one UBO");
                        slotBuffers.emplace(cv, binding->getBufferData()->getBufferObject());
                    }
                }
            }
            cv->getCurrentRenderStage()->setViewport(viewport);
            // OSG can insert the live camera viewport into SceneView local
            // state after cull; retain the leaf override as production HUD does.
            acquired->getOrCreateStateSet()->setAttribute(viewport);
            RenderCore::snapshotRenderStageCamera(*cv->getCurrentRenderStage(), viewer->getCamera());
            {
                std::lock_guard lock(results->mutex);
                results->expected.push_back(expected); results->outputSize.push_back(32); ++results->culled;
                results->resolvePrograms.push_back(static_cast<osg::Program*>(
                    acquired->getPasses()[0].mPasses[2].mStateSet->getAttribute(osg::StateAttribute::PROGRAM)));
                results->lightDiffuse.push_back(diffuse);
            }
            results->changed.notify_all();
        }
    };
    void chain(Root& root)
    {
        root.scene = solid(root.size, {.2f, .3f, .4f, 1});
        root.depth = solid(root.size, {.75f, 0, 0, 1}, true);
        root.history = new osg::Texture2D;
        root.history->setTextureSize(root.size, root.size); root.history->setInternalFormat(GL_RGBA8);
        root.history->setSourceFormat(GL_RGBA); root.history->setSourceType(GL_UNSIGNED_BYTE);
        root.history->setResizeNonPowerOfTwoHint(false);
        root.history->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        root.history->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        Fx::DispatchNode node;
        node.mRootStateSet->addUniform(new osg::Uniform("scene", 0));
        node.mRootStateSet->addUniform(new osg::Uniform("lastPass", 1));
        node.mRootStateSet->addUniform(new osg::Uniform("history", 6));
        node.mRootStateSet->addUniform(new osg::Uniform("userGain", root.gain));
        node.mRootStateSet->setTextureAttribute(6, root.history);
        Fx::DispatchNode::SubPass first;
        first.mStateSet->setAttribute(program("gl_FragColor=texture2D(scene,uv)+texture2D(history,uv)*.1;"));
        node.mPasses.push_back(first);
        Fx::DispatchNode::SubPass writeHistory;
        writeHistory.mStateSet->setAttribute(program("gl_FragColor=texture2D(lastPass,uv)*userGain*passGain;"));
        writeHistory.mStateSet->addUniform(new osg::Uniform("passGain", 1.f));
        writeHistory.mStateSet->setAttribute(new osg::Viewport(0, 0, root.size, root.size));
        writeHistory.mRenderTexture = root.history;
        writeHistory.mRenderTarget = new osg::FrameBufferObject;
        writeHistory.mRenderTarget->setAttachment(osg::Camera::COLOR_BUFFER0, osg::FrameBufferAttachment(root.history));
        node.mPasses.push_back(writeHistory);
        Fx::DispatchNode::SubPass resolve;
        resolve.mResolve = true;
        resolve.mStateSet->setAttribute(program("gl_FragColor=vec4(texture2D(history,uv).rgb+vec3(omw.fogNear*.01)"
            "+omw_PointLights[1].rgb*.01+vec3(float(omw_PointLightsCount)*.01),1);", true, root.useUBO));
        node.mPasses.push_back(resolve);
        // A second disabled technique proves mask capture and preserves order.
        Fx::DispatchNode underwater;
        underwater.mFlags = Fx::Technique::Flag_Disable_Abovewater;
        underwater.mPasses.push_back(resolve);
        auto generation = std::make_shared<MWRender::PostFxTargetGeneration>();
        Fx::Types::RenderTarget target; target.mTarget = root.history; target.mClearColor = {0, 0, 0, 0};
        generation->attachments.push_back(target);
        root.canvas->setPasses(Fx::DispatchArray{node, underwater}, generation);
    }
    void run(bool threaded, bool useUBO)
    {
        // Stereo::Manager retains its Viewer through osg::ref_ptr. Supply a
        // heap-owned Viewer so exception unwinding cannot delete stack memory.
        osg::ref_ptr<osgViewer::Viewer> retainedViewer = new osgViewer::Viewer;
        osgViewer::Viewer& viewer = *retainedViewer;
        viewer.setThreadingModel(threaded ? osgViewer::Viewer::DrawThreadPerContext : osgViewer::Viewer::SingleThreaded);
        Stereo::Manager stereo(&viewer, false, .1f, 1000.f);
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = traits->height = 32; traits->doubleBuffer = false; traits->windowDecoration = false;
        osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
        if (!context || !context->realize() || !context->makeCurrent())
            throw Unsupported("real native PostFX context missing");
        auto* extensions = context->getState()->get<osg::GLExtensions>();
        if (!extensions || !extensions->isFrameBufferObjectSupported || !extensions->glCheckFramebufferStatus
            || !extensions->glBindBufferRange || !extensions->glIsBuffer || osg::getGLVersionNumber() < 3.3f)
            throw Unsupported("OpenGL 3.3 framebuffer and UBO contract unavailable");
        context->releaseContext();
        viewer.getCamera()->setGraphicsContext(context); viewer.getCamera()->setViewport(0, 0, 32, 32);
        viewer.getCamera()->setProjectionMatrix(osg::Matrix::identity()); viewer.getCamera()->setViewMatrix(osg::Matrix::identity());
        viewer.getCamera()->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
        viewer.getCamera()->setCullingMode(osg::CullSettings::NO_CULLING);
        Shader::ShaderManager shaders;
        const auto path = std::filesystem::current_path() / "postfx-owner-shaders";
        // Supply simple fixture presentation shaders; exercise the production
        // canvas, temporal shader, exposure shaders and GPU dispatch unchanged.
        write(path / "compatibility/fullscreen_tri.vert", "#version 120\nvarying vec2 uv;void main(){gl_Position=vec4(gl_Vertex.xy,0,1);uv=gl_Vertex.xy*.5+.5;}\n");
        write(path / "compatibility/fullscreen_tri.frag", "#version 120\nvarying vec2 uv;uniform sampler2D lastShader;void main(){gl_FragColor=texture2D(lastShader,uv);}\n");
        write(path / "compatibility/multiview_resolve.vert", read(path / "compatibility/fullscreen_tri.vert"));
        write(path / "compatibility/multiview_resolve.frag", read(path / "compatibility/fullscreen_tri.frag"));
        for (const auto* relative : {"luminance/luminance.frag", "luminance/resolve.frag", "lib/luminance/constants.glsl"})
            write(path / (std::string(relative).starts_with("lib/") ? "" : "compatibility") / relative,
                read(std::filesystem::path(P9_SOURCE) / "files/shaders"
                    / (std::string(relative).starts_with("lib/") ? "" : "compatibility") / relative));
        shaders.setShaderPath(path);
        auto luminance = std::make_shared<MWRender::LuminanceCalculator>(shaders);
        osg::ref_ptr<Root> root = new Root;
        root->useUBO = useUBO;
        root->fx = new Fx::StateUpdater(useUBO);
        root->viewer = &viewer; root->results = std::make_shared<Results>(); root->results->delay = threaded;
        root->owner = MWRender::PingPongCanvas::createTemporalOwner();
        root->canvas = new MWRender::PingPongCanvas(shaders, luminance);
        root->canvas->setDrawCallback(new Inspect);
        root->canvas->setCullingActive(false); root->canvas->setPostProcessing(true); root->canvas->setCalculateAvgLum(true);
        root->canvas->setTargetGenerationSingleContext(true);
        osg::ref_ptr<osg::Program> temporal = new osg::Program;
        temporal->addShader(new osg::Shader(osg::Shader::VERTEX,
            read(std::filesystem::path(P9_SOURCE) / "files/shaders/compatibility/temporal_camera_motion.vert")));
        temporal->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            read(std::filesystem::path(P9_SOURCE) / "files/shaders/compatibility/temporal_camera_motion.frag")));
        root->canvas->setTemporalMotion(std::make_shared<MWRender::TemporalMotion>(temporal), nullptr);
        root->geode->addDrawable(root->canvas);
        // SceneView also checks the scene root's bound before traversing it.
        // Keep the manually ordered HUD traversal present in that bound.
        root->addChild(root->geode);
        root->getOrCreateStateSet()->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        root->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        root->fx->bindPointLights(root->lights); root->fx->setFogRange(root->fog, 100.f);
        root->setCullCallback(root->fx); chain(*root);
        root->expected = {.1f + .04f, .15f + .05f, .2f + .06f, 1};
        viewer.getCamera()->setInitialDrawCallback(new Delay(root->results));
        viewer.getCamera()->setFinalDrawCallback(new Pixels(root->results));
        viewer.setSceneData(root); viewer.realize(); require(viewer.isRealized(), "PostFX viewer did not realize");
        viewer.frame();
        if (threaded)
        {
            std::unique_lock lock(root->results->mutex);
            require(root->results->changed.wait_for(lock, std::chrono::seconds(8), [&] { return root->results->entered; }),
                "draw did not enter the pre-setup delay");
        }
        else root->results->wait(1);
        const unsigned firstFrame = viewer.getFrameStamp()->getFrameNumber();
        // Mutate both actual source uniform arrays in place before replacing
        // the chain. A Uniform object clone with shared arrays is insufficient.
        root->canvas->getPasses()[0].mRootStateSet->getUniform("userGain")->set(.1f);
        root->canvas->getPasses()[0].mPasses[1].mStateSet->getUniform("passGain")->set(.1f);
        viewer.getFrameStamp()->setFrameNumber(firstFrame - 1); // same parity and same actual ID
        auto retainedHistory = root->history;
        // A rebuild/resize replaces all render targets and user/pass state
        // while the previous real production draw is unfinished.
        root->size = 16; root->gain = .75f; root->fog = 4.f; root->diffuse = {3, 1, 2, 1};
        root->fx->setFogRange(root->fog, 100.f); chain(*root);
        root->expected = {.15f + .08f, .225f + .06f, .3f + .07f, 1};
        std::thread release;
        if (threaded) release = std::thread([&] {
            std::unique_lock lock(root->results->mutex);
            root->results->changed.wait_for(lock, std::chrono::seconds(8), [&] { return root->results->culled >= 2; });
            root->results->release = true; root->results->changed.notify_all();
        });
        viewer.frame(); if (release.joinable()) release.join(); root->results->wait(2);
        require(retainedHistory->getTextureWidth() == 32, "rebuild resized a retained old-generation target");
        // Same generation on the other acquired SceneView: target history must
        // persist and must not be cleared a second time by that slot.
        viewer.getFrameStamp()->setFrameNumber(firstFrame + 3);
        root->expected = {(.2f + .15f * .1f) * .75f + .08f,
            (.3f + .225f * .1f) * .75f + .06f, (.4f + .3f * .1f) * .75f + .07f, 1};
        viewer.frame(); root->results->wait(3);
        // This acquired visitor now has the opposite frame parity from its
        // first cull. The live updater may still contain old parity uniforms;
        // ownership must capture the current world's provider array instead.
        root->diffuse = {2, 4, 1, 1};
        root->expected = {(.2f + .16125f * .1f) * .75f + .07f,
            (.3f + .241875f * .1f) * .75f + .09f, (.4f + .3225f * .1f) * .75f + .06f, 1};
        viewer.frame(); root->results->wait(4);
        // A DYNAMIC fallback using this SAME generation must preserve the
        // already-written history even if the source's dirty list is pending.
        root->expectedOwned = false;
        root->fx->reset(); // Keep the retained legacy parity path's normal setup.
        root->expected = {(.2f + .16209375f * .1f) * .75f + .07f,
            (.3f + .243140625f * .1f) * .75f + .09f, (.4f + .3241875f * .1f) * .75f + .06f, 1};
        viewer.frame(); root->results->wait(5);
        viewer.stopThreading();
        require(context->makeCurrent(), "serialized context release could not acquire GL context");
        std::vector<GLuint> oldNames;
        for (const auto& [visitor, buffer] : root->slotBuffers)
        {
            auto* object = buffer->getGLBufferObject(context->getState()->getContextID());
            require(object && extensions->glIsBuffer(object->getGLObjectID()), "owned UBO was not realized before release");
            oldNames.push_back(object->getGLObjectID());
        }
        root->canvas->releaseGLObjects(context->getState());
        for (const auto& [visitor, buffer] : root->slotBuffers)
            require(!buffer->getGLBufferObject(context->getState()->getContextID()), "release retained a slot's stale GL UBO");
        osg::flushAllDeletedGLObjects(context->getState()->getContextID());
        for (auto name : oldNames) require(!extensions->glIsBuffer(name), "released private GL UBO name was not deleted");
        // Production releases these objects during serialized context teardown.
        // State::reset does not invalidate OSG 3.6.5's global non-VAO vertex
        // array cache, so release/reset and continued draws on that same State
        // would not model GraphicsContext::close. Destroy its native context
        // and acquire a new State, including reuse of the released context ID.
        const auto releasedContextId = context->getState()->getContextID();
        context->close(true);
        viewer.getCamera()->setGraphicsContext(nullptr);
        context = osg::GraphicsContext::createGraphicsContext(traits);
        require(context && context->realize() && context->makeCurrent(), "released native context did not recreate");
        require(context->getState()->getContextID() == releasedContextId, "fixture did not reuse the released context ID");
        extensions = context->getState()->get<osg::GLExtensions>();
        require(extensions && extensions->glIsBuffer, "recreated context has no UBO support");
        viewer.getCamera()->setGraphicsContext(context);
        context->releaseContext();
        // Reuse the same source generation. Its history must clear once after
        // teardown, including when the first new-context draw is DYNAMIC.
        root->fx->reset();
        root->expected = {.15f + .07f, .225f + .09f, .3f + .06f, 1};
        viewer.startThreading(); viewer.frame(); root->results->wait(6); viewer.stopThreading();
        root->expectedOwned = true;
        root->expected = {(.2f + .15f * .1f) * .75f + .07f,
            (.3f + .225f * .1f) * .75f + .09f, (.4f + .3f * .1f) * .75f + .06f, 1};
        viewer.startThreading(); viewer.frame(); root->results->wait(7); viewer.stopThreading();
        require(context->makeCurrent(), "serialized recreated UBO check could not acquire context");
        bool recreated = false;
        for (const auto& [visitor, buffer] : root->slotBuffers)
            if (auto* object = buffer->getGLBufferObject(context->getState()->getContextID()))
            {
                require(extensions->glIsBuffer(object->getGLObjectID()), "recreated private UBO has an invalid GL name");
                recreated = true;
            }
        require(recreated || !useUBO, "context recreation did not recreate a slot's UBO");
        context->releaseContext();
        // Qualify captured presentation settings separately from GL release.
        Settings::video().mUpscaler.set("nis"); Settings::video().mUpscalerSharpness.set(.8f);
        root->expected = {(.2f + .16125f * .1f) * .75f + .07f,
            (.3f + .241875f * .1f) * .75f + .09f, (.4f + .3225f * .1f) * .75f + .06f, 1};
        viewer.startThreading(); viewer.frame(); root->results->wait(8); viewer.stopThreading();
        for (unsigned i = 0; i < 2; ++i)
            static_cast<osgViewer::Renderer*>(viewer.getCamera()->getRenderer())->getSceneView(i)->getRenderStage()->reset();
    }
}
int main(int argc, char** argv) try
{
    Settings::SettingsFileParser parser;
    parser.loadSettingsFile(std::filesystem::path(P9_SOURCE) / "files/settings-default.cfg", Settings::Manager::mDefaultSettings);
    Settings::StaticValues::initDefaults(); Settings::Manager::mUserSettings = Settings::Manager::mDefaultSettings;
    Settings::StaticValues::init();
    bool threaded = false, useUBO = true;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--threaded") threaded = true;
        else if (std::string(argv[i]) == "--no-ubo") useUBO = false;
        else throw std::runtime_error("unknown PostFX fixture argument");
    run(threaded, useUBO);
    std::cout << "PASS: production canvas PostFX pixels, acquired SceneView ownership, repeated/skipped IDs, retained resize/rebuild generations, once-only target clear across DYNAMIC fallback, user/pass uniforms, late world lights, "
        << (useUBO ? "private UBOs" : "private scalar/array globals")
        << ", actual luminance passes, NIS presentation, serialized context teardown and context-ID reuse\n";
}
catch (const Unsupported& error) { std::cout << "SKIP: " << error.what() << '\n'; return 77; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
