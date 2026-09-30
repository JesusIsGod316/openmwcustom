#include <components/resource/openmwcompileoperation.hpp>
#include <components/resource/preparedterraintexture.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <osg/BlendFunc>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Uniform>
#include <osgViewer/Viewer>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    osg::ref_ptr<Resource::PreparedTerrainTexture> texture(std::array<unsigned char, 4> pixel)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        std::copy(pixel.begin(), pixel.end(), image->data());
        osg::ref_ptr<Resource::PreparedTerrainTexture> result = new Resource::PreparedTerrainTexture(image);
        result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        result->setResizeNonPowerOfTwoHint(false);
        return result;
    }
    osg::ref_ptr<Resource::PreparedTerrainTexture> target()
    {
        osg::ref_ptr<Resource::PreparedTerrainTexture> result = new Resource::PreparedTerrainTexture;
        result->setPreparationRenderTarget(true);
        result->setTextureSize(32, 32);
        result->setInternalFormat(GL_RGBA8);
        result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        return result;
    }
    struct MapFixture
    {
        osg::ref_ptr<Terrain::CompositeMap> map = new Terrain::CompositeMap;
        osg::ref_ptr<osg::Texture2D> consumer;
        osg::ref_ptr<osg::Program> program = new osg::Program;
        osg::ref_ptr<Resource::PreparedTerrainTexture> red = texture({255, 0, 0, 255});
        osg::ref_ptr<Resource::PreparedTerrainTexture> green = texture({0, 255, 0, 255});
        MapFixture()
        {
            map->mTexture = target();
            consumer = map->mTexture;
            program->addShader(new osg::Shader(osg::Shader::VERTEX,
                "#version 120\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;}\n"));
            program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 120\nuniform sampler2D diffuseMap;uniform sampler2D blendMap;"
                "void main(){vec4 c=texture2D(diffuseMap,gl_TexCoord[0].xy);"
                "gl_FragColor=vec4(c.rgb,texture2D(blendMap,gl_TexCoord[0].xy).a);}\n"));
            for (unsigned i = 0; i < 2; ++i)
            {
                osg::ref_ptr<osg::Geometry> quad = osg::createTexturedQuadGeometry(
                    osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
                quad->setUseDisplayList(false);
                quad->setUseVertexBufferObjects(false);
                auto* pass = quad->getOrCreateStateSet();
                pass->setAttributeAndModes(program, osg::StateAttribute::ON);
                pass->setTextureAttribute(0, i == 0 ? red.get() : green.get());
                pass->setTextureAttribute(1, texture({0, 0, 0, i == 0 ? (unsigned char)64 : (unsigned char)191}));
                pass->addUniform(new osg::Uniform("diffuseMap", 0));
                pass->addUniform(new osg::Uniform("blendMap", 1));
                pass->setMode(GL_BLEND, osg::StateAttribute::ON);
                pass->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, i == 0 ? GL_ZERO : GL_ONE));
                pass->setAttributeAndModes(new osg::Depth(i == 0 ? osg::Depth::LEQUAL : osg::Depth::EQUAL));
                map->mDrawables.push_back(quad);
            }
        }
    };
    std::array<unsigned char, 4> pixel(osg::Texture2D& image, osg::State& state)
    {
        image.apply(state);
        std::array<unsigned char, 32 * 32 * 4> pixels{};
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        return {pixels[0], pixels[1], pixels[2], pixels[3]};
    }
    class PromoteGeometry final : public osg::Geometry
    {
    public:
        Terrain::CompositeMapRenderer* renderer;
        Terrain::CompositeMap* map;
        PromoteGeometry(const osg::Geometry& original, Terrain::CompositeMapRenderer* r, Terrain::CompositeMap* m)
            : osg::Geometry(original), renderer(r), map(m) {}
        void drawImplementation(osg::RenderInfo& info) const override
        {
            std::thread cull([&] { renderer->setImmediate(map); });
            cull.join();
            osg::Geometry::drawImplementation(info);
        }
    };
}

int main() try
{
    Resource::P9DiscretionaryAdmission admission;
    const int contextIdentity = 0, replacementIdentity = 1;
    admission.begin(&contextIdentity, 10, 2);
    admission.charge(1.5);
    admission.begin(&contextIdentity, 10, 5);
    require(admission.remainingMs() == .5, "repeated frame replenished shared budget");
    require(!admission.fits(.6), "combined preparation+bake exceeds budget");
    require(admission.takeProgress(120, 120) && !admission.takeProgress(200, 120), "progress not bounded to one per frame");
    admission.begin(&replacementIdentity, 10, 1);
    require(admission.remainingMs() == 1, "context replacement inherited prior charges");

    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32; traits->height = 32; traits->windowDecoration = false; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(), "native GL context unavailable");
    auto& state = *context->getState();
    osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
    state.setFrameStamp(stamp);
    osg::RenderInfo info(&state, nullptr);
    Resource::OpenMWCompileSchedulerConfig config;
    config.mMode = 1;
    config.mCompositePreparation = true;
    config.mTargetFrameRate = 1;
    config.mMaxBudgetMs = 100;
    config.mHeadroomRatio = 1;
    config.mMaxObjectsPerFrame = 1;
    config.mDeleteBudgetMs = 0;
    config.mMaxQueueAgeFrames = 3;
    osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> ico = new Resource::OpenMWIncrementalCompileOperation(config);
    osgUtil::IncrementalCompileOperation::Contexts contexts{context.get()};
    ico->assignContexts(contexts);
    osg::ref_ptr<Terrain::CompositeMapRenderer> renderer = new Terrain::CompositeMapRenderer;
    renderer->configurePreparation(ico);
    renderer->setMinimumTimeAvailableForCompile(.1);
    renderer->setCooperativeBackgroundCompile(true);
    unsigned frame = 1;
    auto next = [&] {
        stamp->setFrameNumber(frame++);
        context->clear();
        renderer->drawImplementation(info);
        (*ico)(context);
    };

    MapFixture candidate;
    renderer->addCompositeMap(candidate.map);
    next();
    require(candidate.map->mCompiled == 0 && candidate.map->mPreparation, "producer dependencies not staged before bake");
    require(renderer->preparationStats().scheduled == 1 && renderer->preparationStats().pendingBytes > 0,
        "pending byte accounting missing");
    // Six pass resources (including two actual variants) plus destination/FBO.
    for (unsigned i = 0; i < 7; ++i) next();
    require(candidate.map->mCompiled == 0, "map baked before complete context dependency submission");
    candidate.red->getImage()->data()[0] = 128;
    candidate.red->getImage()->dirty();
    next();
    require(renderer->preparationStats().invalidated == 1 && candidate.map->mCompiled == 0,
        "stale image revision preparation survived");
    for (unsigned i = 0; i < 7; ++i) next();
    candidate.program->dirtyProgram();
    next();
    require(renderer->preparationStats().invalidated >= 2, "program relink revision preparation survived");
    for (unsigned i = 0; i < 7; ++i) next();
    candidate.map->mTexture->releaseGLObjects(&state);
    next();
    require(renderer->preparationStats().invalidated >= 3, "released destination preparation survived");
    for (unsigned i = 0; i < 7; ++i) next();
    candidate.map->mTexture->setTextureSize(16, 16);
    next();
    require(renderer->preparationStats().invalidated >= 4, "resized destination preparation survived");
    for (unsigned i = 0; i < 7; ++i) next();
    candidate.map->mTexture = target();
    candidate.consumer = candidate.map->mTexture;
    next();
    require(renderer->preparationStats().invalidated >= 5, "replaced destination generation preparation survived");
    for (unsigned i = 0; i < 7; ++i) next();
    candidate.map->mDrawables[0]->setStateSet(new osg::StateSet(*candidate.map->mDrawables[0]->getStateSet()));
    next();
    require(renderer->preparationStats().invalidated >= 6, "replaced producer pass preparation survived");
    for (unsigned i = 0; i < 10 && !candidate.map->mDrawables.empty(); ++i) next();
    require(candidate.map->mDrawables.empty() && candidate.map->mCompiled == 2, "prepared map never finished");
    const auto preparedPixel = pixel(*candidate.map->mTexture, state);
    require(preparedPixel[0] >= 31 && preparedPixel[0] <= 33 && preparedPixel[1] >= 190 && preparedPixel[1] <= 192,
        "first/subsequent layer weighting or order changed");
    require(renderer->preparationStats().pendingBytes == 0, "finished map retained pending resource bytes");
    require(!candidate.map->mPreparation, "finished map retained hidden source preparation residency");

    MapFixture legacy;
    legacy.red->getImage()->data()[0] = 128;
    legacy.red->getImage()->dirty();
    osg::ref_ptr<Terrain::CompositeMapRenderer> control = new Terrain::CompositeMapRenderer;
    control->compile(*legacy.map, info);
    require(pixel(*legacy.map->mTexture, state) == preparedPixel, "prepared output differs from same quality control");

    MapFixture required;
    renderer->addCompositeMap(required.map);
    next();
    renderer->setImmediate(required.map);
    next();
    require(required.map->mDrawables.empty() && required.map->mCompiled == 2,
        "required map waited for speculative ICO preparation");
    require(renderer->preparationStats().requiredFallbacks > 0, "required fallback not attributed");
    const auto requiredPixel = pixel(*required.map->mTexture, state);
    require(requiredPixel[0] >= 63 && requiredPixel[1] >= 190, "required fallback lost a terrain layer");

    MapFixture inFlight;
    inFlight.map->mDrawables[0] = new PromoteGeometry(*inFlight.map->mDrawables[0]->asGeometry(), renderer, inFlight.map);
    renderer->addCompositeMap(inFlight.map);
    for (unsigned i = 0; i < 12 && !inFlight.map->mDrawables.empty(); ++i) next();
    require(inFlight.map->mRequired.load() && inFlight.map->mDrawables.empty(), "in-flight cull demand was lost");

    MapFixture cancelled;
    renderer->addCompositeMap(cancelled.map);
    next();
    cancelled.consumer = nullptr;
    next();
    require(cancelled.map->mDrawables.empty() && renderer->preparationStats().cancelled > 0,
        "abandoned speculative dependency retained or submitted");

    std::array<std::unique_ptr<MapFixture>, 9> bounded;
    for (auto& fixture : bounded)
    {
        fixture = std::make_unique<MapFixture>();
        renderer->addCompositeMap(fixture->map);
    }
    stamp->setFrameNumber(frame++);
    context->clear();
    renderer->drawImplementation(info); // submit requests, but do not run ICO yet
    unsigned preparedMaps = 0;
    for (const auto& fixture : bounded) if (fixture->map->mPreparation) ++preparedMaps;
    require(preparedMaps == 8 && renderer->preparationStats().pendingBytes <= 32u * 1024u * 1024u,
        "speculative map/resource preparation bound ignored");
    for (auto& fixture : bounded) fixture->consumer = nullptr;
    next();
    require(renderer->preparationStats().pendingBytes == 0 && ico->getToCompile().empty(),
        "cancelled bounded preparation retained queue slots/byte admission");

    // Texture counting must not be the only bound: program-only producer
    // passes also occupy retained dependency descriptors and queued ICO ops.
    MapFixture programOnly;
    programOnly.map->mDrawables.clear();
    programOnly.program = new osg::Program;
    programOnly.program->addShader(new osg::Shader(osg::Shader::VERTEX,
        "#version 120\nvoid main(){gl_Position=gl_Vertex;}\n"));
    programOnly.program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
        "#version 120\nvoid main(){gl_FragColor=vec4(0.25,0.5,0.75,1.0);}\n"));
    for (unsigned i = 0; i < 513; ++i)
    {
        osg::ref_ptr<osg::Geometry> quad = osg::createTexturedQuadGeometry(
            osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
        quad->setUseDisplayList(false);
        quad->setUseVertexBufferObjects(false);
        auto* pass = quad->getOrCreateStateSet();
        pass->setAttributeAndModes(programOnly.program, osg::StateAttribute::ON);
        pass->setMode(GL_BLEND, osg::StateAttribute::OFF);
        pass->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        programOnly.map->mDrawables.push_back(quad);
    }
    const auto beforeProgramBound = renderer->preparationStats();
    renderer->addCompositeMap(programOnly.map);
    next();
    require(!programOnly.map->mPreparation && ico->getToCompile().empty()
        && renderer->preparationStats().scheduled == beforeProgramBound.scheduled,
        "program-only producer exceeded retained dependency bound/queued preparation");
    for (unsigned i = 0; i < 16 && !programOnly.map->mDrawables.empty(); ++i) next();
    require(programOnly.map->mDrawables.empty() && programOnly.map->mCompiled == 513,
        "oversized program-only producer did not finish complete original bake fallback");
    const auto programFallbackPixel = pixel(*programOnly.map->mTexture, state);
    require(programFallbackPixel[0] >= 63 && programFallbackPixel[0] <= 65
        && programFallbackPixel[1] >= 127 && programFallbackPixel[1] <= 129
        && programFallbackPixel[2] >= 190 && programFallbackPixel[2] <= 192
        && programFallbackPixel[3] == 255,
        "oversized program-only producer lost complete fallback content");
    require(!programOnly.map->mPreparation && ico->getToCompile().empty()
        && renderer->preparationStats().pendingBytes == 0,
        "oversized program-only producer retained hidden speculation after fallback");

    osg::ref_ptr<osg::GraphicsContext::Traits> secondTraits = new osg::GraphicsContext::Traits(*traits);
    osg::ref_ptr<osg::GraphicsContext> secondContext = osg::GraphicsContext::createGraphicsContext(secondTraits);
    require(secondContext && secondContext->realize() && context->makeCurrent(), "second native context unavailable");
    osgUtil::IncrementalCompileOperation::Contexts extraContext{secondContext.get()};
    ico->assignContexts(extraContext);
    const auto singleContextStats = renderer->preparationStats();
    MapFixture unsupportedContext;
    renderer->addCompositeMap(unsupportedContext.map);
    renderer->drawImplementation(info);
    require(unsupportedContext.map->mDrawables.empty() && !unsupportedContext.map->mPreparation
        && renderer->preparationStats().scheduled == singleContextStats.scheduled
        && renderer->preparationStats().chargedMs == singleContextStats.chargedMs,
        "multiple contexts entered shared mutable admission/preparation instead of old bake");
    require(pixel(*unsupportedContext.map->mTexture, state) == requiredPixel, "unsupported-context exact fallback pixels differ");
    ico->removeContexts(extraContext);
    secondContext->close(true);
    require(context->makeCurrent(), "primary context restore failed");
    config.mMaxBudgetMs = 0;
    config.mMaxQueueAgeFrames = 2;
    osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> starvedIco = new Resource::OpenMWIncrementalCompileOperation(config);
    starvedIco->assignContexts(contexts);
    osg::ref_ptr<Terrain::CompositeMapRenderer> starvedRenderer = new Terrain::CompositeMapRenderer;
    starvedRenderer->configurePreparation(starvedIco);
    starvedRenderer->setMinimumTimeAvailableForCompile(.1);
    MapFixture starved;
    starvedRenderer->addCompositeMap(starved.map);
    for (unsigned i = 0; i < 24 && !starved.map->mDrawables.empty(); ++i)
    {
        stamp->setFrameNumber(frame++);
        context->clear();
        starvedRenderer->drawImplementation(info);
        (*starvedIco)(context);
    }
    require(starved.map->mDrawables.empty() && starvedRenderer->preparationStats().ageProgress > 0,
        "zero discretionary headroom starved composite preparation/bake indefinitely");
    require(pixel(*starved.map->mTexture, state) == requiredPixel, "queue-age progress changed final pixels");
    starvedRenderer->releaseGLObjects(&state);
    starvedIco->removeContexts(contexts);
    require(glGetError() == GL_NO_ERROR, "GL error during composite preparation regression");
    renderer->releaseGLObjects(&state);
    control->releaseGLObjects(&state);
    ico->removeContexts(contexts);
    context->releaseContext(); context->close(true);
    std::cout << "PASS: shared admission, real preparation, image/program/release/resize/replacement invalidation, layer pixels/control, required and in-flight promotion, cancellation, bounded speculation/program-only dependencies, unsupported-context fallback and zero-headroom progress\n";
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
