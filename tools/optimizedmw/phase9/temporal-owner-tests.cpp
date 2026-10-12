#include <apps/openmw/mwrender/temporalmotion.hpp>
#include <apps/openmw/mwrender/depthclear.hpp>
#include <components/rendercore/sceneviewowner.hpp>
#include <components/rendercore/ownedrenderstage.hpp>
#include <osg/Geode>
#include <osg/GLExtensions>
#include <osg/Image>
#include <osg/Shader>
#include <osgUtil/SceneView>
#include <osgViewer/Renderer>
#include <osgViewer/Viewer>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void requireAttachment(osg::State& state, osg::Texture2D& texture)
{
    using Query = void (GL_APIENTRY *)(GLenum,GLenum,GLenum,GLint*);
    auto query = reinterpret_cast<Query>(osg::getGLExtensionFuncPtr("glGetFramebufferAttachmentParameteriv"));
    require(query != nullptr,"framebuffer attachment query missing");
    GLint name = 0;
    query(GL_FRAMEBUFFER_EXT,GL_COLOR_ATTACHMENT0_EXT,0x8CD1,&name);
    const auto* object = texture.getTextureObject(state.getContextID());
    require(object && static_cast<GLuint>(name) == object->id(),
        "deferred draw consumed a later/implicit framebuffer attachment generation");
}
std::string source(const char* path)
{
    std::ifstream input(path); require(input.good(), "production shader missing");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
struct Results
{
    std::mutex mutex;
    std::condition_variable changed;
    bool delay = false, entered = false, release = false;
    unsigned completed = 0, culled = 0;
    std::string error;
    void wait(unsigned count)
    {
        std::unique_lock lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return completed >= count || !error.empty(); }),
            "draw did not consume its SceneView submission");
        require(error.empty(), error.c_str());
    }
};
struct Submission : osg::Geometry
{
    MWRender::TemporalCamera camera;
    osg::ref_ptr<osg::Texture2D> depth;
    std::shared_ptr<MWRender::TemporalMotion> motion;
    std::shared_ptr<Results> results;
    bool expectedSubmission = true;
    float expectedX = 0;
    osg::ref_ptr<osg::Texture2D> expectedAttachment;
    unsigned expectedViewport = 0;
    osg::ref_ptr<osgUtil::RenderStage> stage;
    osg::ref_ptr<osg::FrameBufferObject> expectedFramebuffer, expectedResolve;
    Submission()
    {
        setDataVariance(osg::Object::STATIC); setCullingActive(false);
        setUseDisplayList(false); setUseVertexBufferObjects(true);
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({-1,-1,0}); vertices->push_back({3,-1,0}); vertices->push_back({-1,3,0});
        setVertexArray(vertices); addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    }
    void drawImplementation(osg::RenderInfo& info) const override
    {
        try
        {
            {
                std::unique_lock lock(results->mutex);
                if (results->delay && !results->entered)
                {
                    results->entered = true; results->changed.notify_all();
                    require(results->changed.wait_for(lock, std::chrono::seconds(5), [&] { return results->release; }),
                        "fixture delayed draw was not released");
                }
            }
            requireAttachment(*info.getState(),*expectedAttachment);
            GLint viewport[4]{}; glGetIntegerv(GL_VIEWPORT,viewport);
            require(viewport[2] == static_cast<GLint>(expectedViewport)
                && viewport[3] == static_cast<GLint>(expectedViewport),
                "draw consumed the live SceneView viewport instead of the cull-owned override");
            if (expectedFramebuffer)
                require(stage->getFrameBufferObject() == expectedFramebuffer
                    && stage->getMultisampleResolveFramebufferObject() == expectedResolve,
                    "camera setup replaced explicitly supplied framebuffer/resolve resources");
            osg::Texture2D* flow = motion->render(info, camera, depth, *this);
            require((flow != nullptr) == expectedSubmission, "repeat/stale draw submission mismatch");
            const auto context = info.getState()->getContextID();
            if (flow)
            {
                require(!motion->consumerFrame(context, camera.frame + 1), "stale consumer accepted a different frame");
                require(motion->consumerFrame(context, camera.frame).has_value(), "current consumer missing");
                info.getState()->applyTextureAttribute(0, flow);
                std::vector<float> pixels(camera.renderWidth * camera.renderHeight * 2);
                glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_FLOAT, pixels.data());
                for (std::size_t i = 0; i < pixels.size(); i += 2)
                    require(std::abs(pixels[i] - expectedX) < .003f && std::abs(pixels[i+1]) < .003f,
                        "delayed draw consumed another cull's camera or depth");
            }
            else require(!motion->consumerFrame(context, camera.frame), "failed draw retained a consumer");
            require(glGetError() == GL_NO_ERROR, "owned temporal pixels caused GL errors");
        }
        catch (const std::exception& e)
        {
            std::lock_guard lock(results->mutex); results->error = e.what();
        }
        { std::lock_guard lock(results->mutex); ++results->completed; }
        results->changed.notify_all();
    }
};
struct Source : osg::Drawable
{
    RenderCore::SceneViewOwner<Submission> owner;
    osgViewer::Viewer* viewer = nullptr;
    MWRender::TemporalCamera camera;
    osg::ref_ptr<osg::Texture2D> depth;
    std::shared_ptr<MWRender::TemporalMotion> motion;
    std::shared_ptr<Results> results;
    bool expectedSubmission = true;
    float expectedX = 0;
    bool manualFramebuffer = false;
    osg::ref_ptr<osg::Texture2D> color;
    void accept(osg::NodeVisitor& nv) override
    {
        if (nv.getVisitorType() != osg::NodeVisitor::CULL_VISITOR) { osg::Drawable::accept(nv); return; }
        auto* cv = static_cast<osgUtil::CullVisitor*>(&nv);
        auto* renderer = dynamic_cast<osgViewer::Renderer*>(viewer->getCamera()->getRenderer());
        require(renderer && (renderer->getSceneView(0)->getCullVisitor() == cv
            || renderer->getSceneView(1)->getCullVisitor() == cv), "fixture did not use a real acquired SceneView");
        auto capture = [&](Submission& value) {
            value.camera = camera; value.camera.frame = cv->getTraversalNumber();
            value.camera.projection = cv->getProjectionMatrix();
            value.depth = depth; value.motion = motion; value.results = results;
            value.expectedSubmission = expectedSubmission; value.expectedX = expectedX;
            value.expectedAttachment = color;
            value.expectedViewport = static_cast<unsigned>(viewer->getCamera()->getViewport()->width());
            value.stage = cv->getCurrentRenderStage();
            osg::ref_ptr<osg::Viewport> viewport = new osg::Viewport(*viewer->getCamera()->getViewport());
            value.getOrCreateStateSet()->setAttribute(viewport);
            value.stage->setViewport(viewport);
            if (manualFramebuffer)
            {
                value.expectedFramebuffer = new osg::FrameBufferObject;
                value.expectedFramebuffer->setAttachment(osg::Camera::COLOR_BUFFER0,osg::FrameBufferAttachment(color));
                value.expectedResolve = new osg::FrameBufferObject;
                osg::ref_ptr<osg::Texture2D> resolveColor = new osg::Texture2D;
                resolveColor->setTextureSize(value.expectedViewport,value.expectedViewport);
                resolveColor->setInternalFormat(GL_RGBA8);
                value.expectedResolve->setAttachment(osg::Camera::COLOR_BUFFER0,osg::FrameBufferAttachment(resolveColor));
                value.stage->setFrameBufferObject(value.expectedFramebuffer);
                value.stage->setMultisampleResolveFramebufferObject(value.expectedResolve);
            }
        };
        auto* submission = owner.acquire(cv, [&] { auto* value = new Submission; capture(*value); return value; }, capture);
        require(submission, "real SceneView exceeded the exact two-slot bound");
        submission->accept(nv);
        RenderCore::snapshotRenderStageCamera(*cv->getCurrentRenderStage(),viewer->getCamera(),manualFramebuffer);
        { std::lock_guard lock(results->mutex); ++results->culled; }
        results->changed.notify_all();
    }
};
struct DelayBeforeSetup : osg::Camera::DrawCallback
{
    std::shared_ptr<Results> results;
    explicit DelayBeforeSetup(std::shared_ptr<Results> value) : results(std::move(value)) {}
    void operator()(osg::RenderInfo&) const override
    {
        std::unique_lock lock(results->mutex);
        if (results->delay && !results->entered)
        {
            results->entered = true; results->changed.notify_all();
            if (!results->changed.wait_for(lock,std::chrono::seconds(5),[&]{return results->release;}))
            { results->error = "draw-before-setup delay was not released"; results->changed.notify_all(); }
        }
    }
};
osg::ref_ptr<osg::Texture2D> depthTexture(unsigned size)
{
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->allocateImage(size,size,1,GL_RED,GL_FLOAT);
    auto* values = reinterpret_cast<float*>(image->data());
    std::fill(values, values + size*size, .75f);
    osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
    texture->setTextureSize(size,size); texture->setInternalFormat(GL_R32F);
    texture->setResizeNonPowerOfTwoHint(false);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
    return texture;
}
void runFixture(bool threaded,bool manualFramebuffer)
{
    osgViewer::Viewer viewer;
    viewer.setThreadingModel(threaded ? osgViewer::Viewer::DrawThreadPerContext : osgViewer::Viewer::SingleThreaded);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32; traits->height = 32; traits->doubleBuffer = false; traits->windowDecoration = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(), "real GL context missing");
    viewer.getCamera()->setGraphicsContext(context); viewer.getCamera()->setViewport(0,0,32,32);
    viewer.getCamera()->setProjectionMatrix(osg::Matrix::identity()); viewer.getCamera()->setViewMatrix(osg::Matrix::identity());
    viewer.getCamera()->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    viewer.getCamera()->setCullingMode(osg::CullSettings::NO_CULLING);
    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX,source(P9_VERTEX)));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT,source(P9_FRAGMENT)));
    auto results = std::make_shared<Results>(); results->delay = threaded;
    osg::ref_ptr<osg::Texture2D> output = new osg::Texture2D;
    output->setTextureSize(32,32); output->setInternalFormat(GL_RGBA8);
    viewer.getCamera()->setRenderTargetImplementation(osg::Camera::FRAME_BUFFER_OBJECT);
    if (!manualFramebuffer) viewer.getCamera()->attach(osg::Camera::COLOR_BUFFER0,output);
    viewer.getCamera()->setInitialDrawCallback(new DelayBeforeSetup(results));
    osg::ref_ptr<Source> input = new Source;
    input->viewer = &viewer; input->results = results; input->motion = std::make_shared<MWRender::TemporalMotion>(program);
    input->manualFramebuffer = manualFramebuffer; input->color = output;
    input->depth = depthTexture(16); input->camera.renderWidth = input->camera.renderHeight = 16;
    input->camera.outputWidth = input->camera.outputHeight = 32;
    input->setCullingActive(false);
    osg::ref_ptr<osg::Geode> root = new osg::Geode; root->addDrawable(input);
    viewer.setSceneData(root); viewer.realize(); require(viewer.isRealized(), "viewer did not realize");
    viewer.frame();
    if (threaded)
    {
        std::unique_lock lock(results->mutex);
        require(results->changed.wait_for(lock,std::chrono::seconds(5),[&]{return results->entered;}), "delayed draw did not start");
    }
    else results->wait(1);
    // Deliberately use another identical parity/ID: a parity owner would
    // overwrite the retained camera while the previous draw is still blocked.
    const unsigned firstFrame = viewer.getFrameStamp()->getFrameNumber();
    viewer.getFrameStamp()->setFrameNumber(firstFrame - 1);
    input->camera.view = osg::Matrixd::translate(-.125,0,0); input->expectedSubmission = false;
    // Replace camera attachments and the depth generation while the old
    // initial draw callback is blocked BEFORE deferred FBO setup.
    input->depth = depthTexture(8); input->camera.renderWidth = input->camera.renderHeight = 8;
    ++input->camera.resourceEpoch;
    osg::ref_ptr<osg::Texture2D> replacement = new osg::Texture2D;
    replacement->setTextureSize(16,16); replacement->setInternalFormat(GL_RGBA8);
    input->color = replacement;
    if (!manualFramebuffer) viewer.getCamera()->attach(osg::Camera::COLOR_BUFFER0,replacement);
    viewer.getCamera()->getViewport()->setViewport(0,0,16,16);
    std::thread releaser;
    if (threaded) releaser = std::thread([&] {
        std::unique_lock lock(results->mutex);
        results->changed.wait_for(lock,std::chrono::seconds(5),[&]{return results->culled >= 2;});
        results->release = true; results->changed.notify_all();
    });
    viewer.frame();
    if (releaser.joinable()) releaser.join();
    { std::lock_guard lock(results->mutex); results->release = true; }
    results->changed.notify_all(); results->wait(2);
    input->depth = depthTexture(16); input->camera.renderWidth = input->camera.renderHeight = 16;
    ++input->camera.resourceEpoch;
    input->expectedSubmission = true; viewer.getFrameStamp()->setFrameNumber(firstFrame + 2);
    viewer.frame(); results->wait(3); // skipped draw ID resets history
    input->camera.view = osg::Matrixd::translate(-.25,0,0); input->expectedX = 1;
    viewer.frame(); results->wait(4);
    input->depth = depthTexture(8); input->camera.renderWidth = input->camera.renderHeight = 8;
    ++input->camera.resourceEpoch; input->expectedX = 0;
    viewer.frame(); results->wait(5);
    viewer.stopThreading(); viewer.startThreading(); ++input->camera.resourceEpoch;
    viewer.frame(); results->wait(6);
    viewer.stopThreading();
    // Break the fixture-only stage -> Submission -> stage cycle.
    for (unsigned i = 0; i < 2; ++i)
    {
        auto* renderer = static_cast<osgViewer::Renderer*>(viewer.getCamera()->getRenderer());
        renderer->getSceneView(i)->getRenderStage()->reset();
    }
}
int main(int argc, char** argv) try
{
    const bool threaded = argc > 1 && std::string(argv[1]) == "--threaded";
    runFixture(threaded,false);
    runFixture(threaded,true);
    std::cout << "PASS: acquired SceneView ownership, delay before setup, actual attachment/viewport, manual FBO/resolve, repeated/skipped IDs, resize/restart and real motion pixels\n";
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
