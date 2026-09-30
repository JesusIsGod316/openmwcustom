#include <components/resource/cachemaintenance.hpp>
#include <components/resource/openmwcompileoperation.hpp>
#include <components/resource/preparedterraintexture.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/v321classifiedcompileset.hpp>
#include <components/terrain/compositemaprenderer.hpp>

#include <osg/FrameStamp>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/Group>
#include <osg/Program>
#include <osg/RenderInfo>
#include <osg/Shader>
#include <osg/Uniform>
#include <osgViewer/Viewer>

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
    struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };

    // These are the entry points used by the fixture's GLSL 1.20 program,
    // OSG shader linking/reflection, client arrays and texture-only FBO.
    constexpr auto sRequiredGraphicsFunctions = std::tuple{
        &osg::GLExtensions::glCreateShader, &osg::GLExtensions::glShaderSource,
        &osg::GLExtensions::glCompileShader, &osg::GLExtensions::glGetShaderiv,
        &osg::GLExtensions::glGetShaderInfoLog, &osg::GLExtensions::glDeleteShader,
        &osg::GLExtensions::glCreateProgram, &osg::GLExtensions::glGetAttachedShaders,
        &osg::GLExtensions::glAttachShader, &osg::GLExtensions::glDetachShader,
        &osg::GLExtensions::glLinkProgram, &osg::GLExtensions::glGetProgramiv,
        &osg::GLExtensions::glGetProgramInfoLog, &osg::GLExtensions::glGetActiveUniform,
        &osg::GLExtensions::glGetUniformLocation, &osg::GLExtensions::glGetActiveAttrib,
        &osg::GLExtensions::glGetAttribLocation, &osg::GLExtensions::glUseProgram,
        &osg::GLExtensions::glUniform1iv, &osg::GLExtensions::glDeleteProgram,
        &osg::GLExtensions::glGenFramebuffers, &osg::GLExtensions::glDeleteFramebuffers,
        &osg::GLExtensions::glBindFramebuffer, &osg::GLExtensions::glFramebufferTexture2D,
        &osg::GLExtensions::glCheckFramebufferStatus,
        &osg::GLExtensions::glActiveTexture, &osg::GLExtensions::glClientActiveTexture};

    void requireCompositeGraphics(const osg::GLExtensions* extensions)
    {
        if (!extensions || extensions->glVersion < 2.1f || extensions->glslLanguageVersion < 1.2f
            || !extensions->isGlslSupported || !extensions->isFrameBufferObjectSupported
            || !std::apply([&](auto... member) { return (... && (extensions->*member != nullptr)); },
                sRequiredGraphicsFunctions))
            throw Unsupported("OpenGL 2.1 / GLSL 1.20 framebuffer and program contract unavailable; "
                              "the separate --cache-only test remains mandatory");
    }

    void verifyMissingCompositeGraphics(osg::GLExtensions& extensions)
    {
        // Exercise the actual gate with individual resolved capabilities absent.
        // Restore every field before applying state or making any GL call.
        auto rejected = [&](auto& capability) {
            const auto saved = std::exchange(capability, {});
            bool unsupported = false;
            try { requireCompositeGraphics(&extensions); }
            catch (const Unsupported&) { unsupported = true; }
            capability = saved;
            require(unsupported, "missing graphics capability was accepted before fixture state application");
        };
        rejected(extensions.glVersion);
        rejected(extensions.glslLanguageVersion);
        rejected(extensions.isGlslSupported);
        rejected(extensions.isFrameBufferObjectSupported);
        std::apply([&](auto... member) { (rejected(extensions.*member), ...); }, sRequiredGraphicsFunctions);
        requireCompositeGraphics(&extensions);
    }

    class CallerStateScope
    {
    public:
        CallerStateScope(osg::State& state, const osg::StateSet& caller) : mState(state)
        {
            mState.pushStateSet(&caller);
            mState.apply();
        }
        ~CallerStateScope() { if (mActive) restore(); }
        void restore()
        {
            mState.popStateSet();
            mState.apply();
            mState.disableAllVertexArrays();
            mActive = false;
        }
    private:
        osg::State& mState;
        bool mActive = true;
    };

    void cacheOnly()
    {
        bool missingGraphicsRejected = false;
        try { requireCompositeGraphics(nullptr); }
        catch (const Unsupported&) { missingGraphicsRejected = true; }
        require(missingGraphicsRejected, "missing graphics capability object bypassed the native fixture gate");
        osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> ico
            = new Resource::OpenMWIncrementalCompileOperation({});
        Resource::SceneManager sceneManager(nullptr, nullptr, nullptr, nullptr, 0);
        sceneManager.setIncrementalCompileOperation(ico);
        double time = 0;
        auto enqueue = [&](osgUtil::IncrementalCompileOperation::CompileSet* set) {
            std::lock_guard<OpenThreads::Mutex> lock(*ico->getToCompiledMutex());
            ico->getToCompile().push_back(set);
        };
        auto maintenance = [&](std::size_t releases = 128) {
            std::thread worker([&] {
                Resource::CacheMaintenanceBudget budget(4096, releases, std::chrono::seconds(1));
                Resource::CacheMaintenanceScope scope(budget);
                sceneManager.updateCache(++time);
            });
            worker.join();
        };
        auto queued = [&] {
            std::lock_guard<OpenThreads::Mutex> lock(*ico->getToCompiledMutex());
            return ico->getToCompile().size();
        };
        auto owner = std::make_shared<Resource::V321ResourceCompileLifetime>();
        osg::ref_ptr<Resource::V321ClassifiedCompileSet> resources = new Resource::V321ClassifiedCompileSet(
            owner, Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
        enqueue(resources);
        maintenance();
        require(queued() == 1, "actual cache maintenance discarded a pending resource-only producer");
        owner->cancel();
        maintenance(0);
        require(queued() == 1, "resource-only cancellation bypassed maintenance release budget");
        maintenance();
        require(queued() == 0, "actual cache maintenance failed to retire cancelled resource work");
        owner = std::make_shared<Resource::V321ResourceCompileLifetime>();
        resources = new Resource::V321ClassifiedCompileSet(
            owner, Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
        enqueue(resources);
        owner->complete();
        maintenance();
        require(queued() == 0, "completed resource producer retained queue ownership");
        owner = std::make_shared<Resource::V321ResourceCompileLifetime>();
        resources = new Resource::V321ClassifiedCompileSet(
            owner, Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
        enqueue(resources);
        owner.reset();
        maintenance();
        require(queued() == 0, "expired resource producer retained queue ownership");

        osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> unknown
            = new osgUtil::IncrementalCompileOperation::CompileSet(nullptr);
        enqueue(unknown);
        enqueue(nullptr);
        maintenance();
        require(queued() == 1, "unknown null-subgraph owner discarded or invalid queue entry retained");
        ico->remove(unknown);

        osg::ref_ptr<osg::Group> node = new osg::Group;
        resources = new Resource::V321ClassifiedCompileSet(node, Resource::V321CompileClass::Terrain);
        osg::ref_ptr<osg::Group> liveOwner = node;
        enqueue(resources);
        maintenance();
        require(queued() == 1, "ordinary Terrain scene compile lost external owner");
        liveOwner = nullptr;
        maintenance();
        require(queued() == 0, "ordinary Terrain scene reference pruning no longer retires unused work");
        sceneManager.setIncrementalCompileOperation(nullptr);
        std::cout << "PASS: actual SceneManager background maintenance without GL, explicit resource ownership, "
                     "cancel/completion/expired owner retirement, release budget, null safety and Terrain control\n";
    }

    struct MapFixture
    {
        osg::ref_ptr<Terrain::CompositeMap> map = new Terrain::CompositeMap;
        osg::ref_ptr<osg::Texture2D> consumer;

        MapFixture()
        {
            osg::ref_ptr<Resource::PreparedTerrainTexture> target = new Resource::PreparedTerrainTexture;
            target->setPreparationRenderTarget(true);
            target->setTextureSize(16, 16);
            target->setInternalFormat(GL_RGBA8);
            target->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            target->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            map->mTexture = target;
            consumer = target;

            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            const std::array<unsigned char, 4> color{ 64, 128, 192, 255 };
            std::copy(color.begin(), color.end(), image->data());
            osg::ref_ptr<Resource::PreparedTerrainTexture> source = new Resource::PreparedTerrainTexture(image);
            source->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            source->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            source->setResizeNonPowerOfTwoHint(false);
            osg::ref_ptr<osg::Program> program = new osg::Program;
            program->addShader(new osg::Shader(osg::Shader::VERTEX,
                "#version 120\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;}\n"));
            program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 120\nuniform sampler2D source;"
                "void main(){gl_FragColor=texture2D(source,gl_TexCoord[0].xy);}\n"));
            osg::ref_ptr<osg::Geometry> quad = osg::createTexturedQuadGeometry(
                osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
            quad->setUseDisplayList(false);
            quad->setUseVertexBufferObjects(false);
            auto* pass = quad->getOrCreateStateSet();
            pass->setAttributeAndModes(program, osg::StateAttribute::ON);
            pass->setTextureAttribute(0, source);
            pass->addUniform(new osg::Uniform("source", 0));
            pass->setMode(GL_BLEND, osg::StateAttribute::OFF);
            pass->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
            map->mDrawables.push_back(quad);
        }
    };

    std::array<unsigned char, 4> pixel(osg::Texture2D& texture, osg::State& state)
    {
        texture.apply(state);
        std::array<unsigned char, 16 * 16 * 4> pixels{};
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return { pixels[0], pixels[1], pixels[2], pixels[3] };
    }

    class CountOp final : public osgUtil::IncrementalCompileOperation::CompileOp
    {
    public:
        explicit CountOp(unsigned& count) : mCount(count) {}
        double estimatedTimeForCompile(osgUtil::IncrementalCompileOperation::CompileInfo&) const override { return 0; }
        bool compile(osgUtil::IncrementalCompileOperation::CompileInfo&) override { ++mCount; return true; }
    private:
        unsigned& mCount;
    };

    struct MergeHandled final : osgUtil::IncrementalCompileOperation::CompileCompletedCallback
    {
        bool compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet*) override { return true; }
    };
}

int main(int argc, char** argv) try
{
    if (argc == 2 && std::string_view(argv[1]) == "--cache-only")
    {
        cacheOnly();
        return 0;
    }
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY();
    traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 16; traits->height = 16; traits->windowDecoration = false; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    if (!context || !context->realize() || !context->makeCurrent())
        throw Unsupported("native GL context unavailable; the separate --cache-only test remains mandatory");
    auto& state = *context->getState();
    auto* extensions = state.get<osg::GLExtensions>();
    // Context creation can succeed on Windows GDI's OpenGL 1.1 software
    // renderer. Establish the required contract before caller state applies.
    requireCompositeGraphics(extensions);
    verifyMissingCompositeGraphics(*extensions);
    osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
    state.setFrameStamp(stamp);
    osg::RenderInfo info(&state, nullptr);
    // Real composite rendering runs inside caller state. The empty Program
    // deliberately owns no PCP, so a stale source PCP is observable without
    // depending on whether the allocator reuses its former Program address.
    osg::ref_ptr<osg::StateSet> caller = new osg::StateSet;
    osg::ref_ptr<osg::Program> callerProgram = new osg::Program;
    caller->setAttributeAndModes(callerProgram, osg::StateAttribute::ON);
    caller->setMode(GL_BLEND, osg::StateAttribute::ON);
    CallerStateScope callerScope(state, *caller);
    auto callerRestored = [&] {
        GLint program = -1;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        require(state.getStateSetStackSize() == 1
            && state.getLastAppliedAttribute(osg::StateAttribute::PROGRAM) == callerProgram.get()
            && state.getLastAppliedProgramObject() == nullptr && program == 0
            && state.getLastAppliedMode(GL_BLEND),
            "composite bake did not restore caller state before releasing its source Program/PCP");
        const auto* arrays = state.getCurrentVertexArrayState();
        auto retiredArray = [](const osg::VertexArrayState::ArrayDispatch* dispatch) {
            return !dispatch || (!dispatch->array && !dispatch->active && dispatch->modifiedCount == 0xffffffff);
        };
        require(arrays && retiredArray(arrays->_vertexArray) && retiredArray(arrays->_normalArray)
            && retiredArray(arrays->_colorArray)
            && std::all_of(arrays->_texCoordArrays.begin(), arrays->_texCoordArrays.end(),
                [&](const auto& dispatch) { return retiredArray(dispatch.get()); })
            && glIsEnabled(GL_VERTEX_ARRAY) == GL_FALSE
            && glIsEnabled(GL_TEXTURE_COORD_ARRAY) == GL_FALSE,
            "composite bake retained client-array dispatchers after releasing source geometry");
    };

    Resource::OpenMWCompileSchedulerConfig config;
    config.mMode = 1; config.mCompositePreparation = true; config.mTargetFrameRate = 1;
    config.mMaxBudgetMs = 100; config.mHeadroomRatio = 1; config.mMaxObjectsPerFrame = 1;
    config.mDeleteBudgetMs = 0; config.mMaxQueueAgeFrames = 2;
    osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> ico = new Resource::OpenMWIncrementalCompileOperation(config);
    osgUtil::IncrementalCompileOperation::Contexts contexts{ context.get() };
    ico->assignContexts(contexts);
    // No resource loading is needed. This is the actual production SceneManager
    // and its cache-pruning consumer, not a replacement queue helper.
    Resource::SceneManager sceneManager(nullptr, nullptr, nullptr, nullptr, 0);
    sceneManager.setIncrementalCompileOperation(ico);
    osg::ref_ptr<Terrain::CompositeMapRenderer> renderer = new Terrain::CompositeMapRenderer;
    renderer->configurePreparation(ico);
    renderer->setMinimumTimeAvailableForCompile(.1);
    unsigned frame = 1;
    double cacheTime = 0;
    auto maintenance = [&](bool budgeted = false, std::size_t releases = 128) {
        std::thread worker([&] {
            Resource::CacheMaintenanceBudget budget(4096, releases, std::chrono::seconds(1));
            if (budgeted)
            {
                Resource::CacheMaintenanceScope scope(budget);
                sceneManager.updateCache(++cacheTime);
            }
            else
                sceneManager.updateCache(++cacheTime);
        });
        worker.join();
    };
    auto queued = [&] {
        std::lock_guard<OpenThreads::Mutex> lock(*ico->getToCompiledMutex());
        return ico->getToCompile().size();
    };
    auto draw = [&] {
        stamp->setFrameNumber(frame++);
        context->clear();
        renderer->drawImplementation(info);
    };
    auto next = [&] { draw(); maintenance(); (*ico)(context); maintenance(true); };

    // Ordinary scene-reference pruning still applies to Terrain-class jobs.
    osg::ref_ptr<osg::Group> unusedNode = new osg::Group;
    osg::ref_ptr<Resource::V321ClassifiedCompileSet> unused = new Resource::V321ClassifiedCompileSet(
        unusedNode, Resource::V321CompileClass::Terrain);
    ico->add(unused, false);
    osg::ref_ptr<osg::Group> liveNode = new osg::Group;
    osg::ref_ptr<osg::Group> externalOwner = liveNode;
    osg::ref_ptr<Resource::V321ClassifiedCompileSet> live = new Resource::V321ClassifiedCompileSet(
        liveNode, Resource::V321CompileClass::Terrain);
    ico->add(live, false);
    maintenance();
    require(queued() == 1 && ico->getToCompile().front() == live, "ordinary Terrain scene pruning changed");
    externalOwner = nullptr;
    maintenance();
    require(queued() == 0, "scene compile not retired after its last external owner left");

    // An unclassified null-subgraph job belongs to its existing ICO completion
    // protocol. Do not crash or silently discard it based on a missing node.
    unsigned unknownCompiled = 0;
    osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> unknown
        = new osgUtil::IncrementalCompileOperation::CompileSet(nullptr);
    unknown->_compileMap[context].add(new CountOp(unknownCompiled));
    ++unknown->_numberCompileListsToCompile;
    unknown->_compileCompletedCallback = new MergeHandled;
    ico->add(unknown, false);
    maintenance(); maintenance(true);
    require(queued() == 1 && unknownCompiled == 0, "unknown resource-only job was discarded by scene pruning");
    next();
    require(queued() == 0 && unknownCompiled == 1, "unknown resource-only job failed its ordinary completion");

    // The reported crash occurred here: real preparation pending in the shared
    // ICO queue while a background cache-maintenance worker runs updateCache.
    MapFixture complete;
    renderer->addCompositeMap(complete.map);
    draw();
    require(queued() == 1 && complete.map->mPreparation && complete.map->mCompiled == 0,
        "actual composite producer did not queue pending resource dependencies");
    const auto* classified = dynamic_cast<const Resource::V321ClassifiedCompileSet*>(ico->getToCompile().front().get());
    require(classified && classified->resourceOnly() && classified->resourceCompilePending()
        && !classified->_subgraphToCompile, "composite producer has no explicit resource lifetime");
    maintenance(); maintenance(true);
    require(queued() == 1, "cache maintenance removed live composite preparation");
    (*ico)(context); // a partially submitted producer remains owned
    maintenance();
    require(queued() == 1 && complete.map->mCompiled == 0, "partial preparation lost producer ownership");
    for (unsigned i = 0; i < 12 && !complete.map->mDrawables.empty(); ++i) next();
    require(queued() == 0 && complete.map->mDrawables.empty() && complete.map->mCompiled == 1
        && !complete.map->mPreparation && renderer->preparationStats().pendingBytes == 0,
        "completed composite retained queued/source preparation resources");
    callerRestored();
    const auto expected = pixel(*complete.map->mTexture, state);
    require(expected == std::array<unsigned char, 4>{64, 128, 192, 255}, "prepared composite pixels changed");

    MapFixture required;
    renderer->addCompositeMap(required.map);
    draw(); maintenance();
    require(queued() == 1, "required fixture never began speculative preparation");
    renderer->setImmediate(required.map);
    draw(); maintenance();
    require(queued() == 0 && required.map->mDrawables.empty() && required.map->mCompiled == 1
        && renderer->preparationStats().pendingBytes == 0,
        "required fallback did not retire pending resource-only work");
    callerRestored();
    require(pixel(*required.map->mTexture, state) == expected, "required fallback lost complete terrain content");

    MapFixture abandoned;
    renderer->addCompositeMap(abandoned.map);
    draw(); maintenance();
    require(queued() == 1, "abandoned fixture never queued");
    abandoned.consumer = nullptr;
    draw(); maintenance();
    require(queued() == 0 && abandoned.map->mDrawables.empty()
        && renderer->preparationStats().pendingBytes == 0, "abandoned producer retained compile resources");

    MapFixture released;
    renderer->addCompositeMap(released.map);
    draw();
    require(queued() == 1, "release fixture never queued");
    renderer->releaseGLObjects(&state);
    maintenance(true, 0);
    require(queued() == 1, "producer retirement ignored shared maintenance release budget");
    maintenance(true);
    require(queued() == 0, "GL release left stale producer operations queued");
    // A subsequent draw can prepare the still demanded map again.
    for (unsigned i = 0; i < 12 && !released.map->mDrawables.empty(); ++i) next();
    require(released.map->mDrawables.empty() && queued() == 0
        && renderer->preparationStats().pendingBytes == 0,
        "GL release prevented demanded terrain from rebuilding");
    callerRestored();
    require(pixel(*released.map->mTexture, state) == expected, "release/rebuild changed terrain content");

    // The same temporary source retirement also occurs on the original bake.
    MapFixture legacy;
    osg::ref_ptr<Terrain::CompositeMapRenderer> legacyRenderer = new Terrain::CompositeMapRenderer;
    legacyRenderer->setMinimumTimeAvailableForCompile(.1);
    legacyRenderer->addCompositeMap(legacy.map, true);
    stamp->setFrameNumber(frame++);
    context->clear();
    legacyRenderer->drawImplementation(info);
    require(legacy.map->mDrawables.empty() && legacy.map->mCompiled == 1 && queued() == 0,
        "original composite bake did not complete its temporary source retirement");
    callerRestored();
    require(pixel(*legacy.map->mTexture, state) == expected, "original composite bake changed terrain content");
    legacyRenderer->releaseGLObjects(&state);

    MapFixture teardown;
    osg::ref_ptr<Terrain::CompositeMapRenderer> temporary = new Terrain::CompositeMapRenderer;
    temporary->configurePreparation(ico);
    temporary->setMinimumTimeAvailableForCompile(.1);
    temporary->addCompositeMap(teardown.map);
    stamp->setFrameNumber(frame++); context->clear(); temporary->drawImplementation(info);
    require(queued() == 1, "teardown fixture never queued");
    temporary = nullptr;
    maintenance();
    require(queued() == 0, "renderer destruction retained producer-owned compile work");

    // A producer that disappears without a queue-owned token also retires.
    auto orphanOwner = std::make_shared<Resource::V321ResourceCompileLifetime>();
    osg::ref_ptr<Resource::V321ClassifiedCompileSet> orphan = new Resource::V321ClassifiedCompileSet(
        orphanOwner, Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
    ico->add(orphan, false);
    orphanOwner.reset();
    maintenance();
    require(queued() == 0, "expired resource producer retained compile work");

    renderer->releaseGLObjects(&state);
    sceneManager.setIncrementalCompileOperation(nullptr);
    ico->removeContexts(contexts);
    callerScope.restore();
    require(glGetError() == GL_NO_ERROR, "GL error during actual cache/preparation integration");
    context->releaseContext(); context->close(true);
    std::cout << "PASS: real SceneManager maintenance with pending/partial/completed composite preparation, "
                 "required fallback, cancellation, release/rebuild, teardown, budget, ordinary Terrain pruning "
                 "and unknown null-subgraph completion, caller Program/PCP restoration and client-array retirement "
                 "on prepared and original bakes\n";
}
catch (const Unsupported& error)
{
    std::cout << "SKIP: " << error.what() << '\n';
    return 77;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
