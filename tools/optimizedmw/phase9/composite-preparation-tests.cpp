#include <components/resource/openmwcompileoperation.hpp>
#include <components/debug/compileattribution.hpp>
#include <components/resource/v321classifiedcompileset.hpp>
#include <components/resource/preparedterraintexture.hpp>
#include <components/resource/statictextureprogram.hpp>
#include <components/sceneutil/glcalltrace.hpp>
#include <components/terrain/compositemaprenderer.hpp>
#include <osg/BlendFunc>
#include <osg/ContextData>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Uniform>
#include <osgViewer/Viewer>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <osg/observer_ptr>

namespace
{
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    struct BatchEnvironment
    {
        std::string previous;
        bool existed;
        explicit BatchEnvironment(const char* value)
            : previous(std::getenv("OPENMW_HITCH_COMPOSITE_BATCH") ? std::getenv("OPENMW_HITCH_COMPOSITE_BATCH") : "")
            , existed(std::getenv("OPENMW_HITCH_COMPOSITE_BATCH") != nullptr)
        { set(value); }
        static void set(const char* value)
        {
#ifdef _WIN32
            _putenv_s("OPENMW_HITCH_COMPOSITE_BATCH", value ? value : "");
#else
            if (value) setenv("OPENMW_HITCH_COMPOSITE_BATCH", value, 1);
            else unsetenv("OPENMW_HITCH_COMPOSITE_BATCH");
#endif
        }
        ~BatchEnvironment() { set(existed ? previous.c_str() : nullptr); }
    };
    struct EarlyEnvironment
    {
        std::string previous;
        bool existed;
        explicit EarlyEnvironment(const char* value)
            : previous(std::getenv("OPENMW_HITCH_COMPOSITE_EARLY") ? std::getenv("OPENMW_HITCH_COMPOSITE_EARLY") : "")
            , existed(std::getenv("OPENMW_HITCH_COMPOSITE_EARLY") != nullptr) { set(value); }
        static void set(const char* value)
        {
#ifdef _WIN32
            _putenv_s("OPENMW_HITCH_COMPOSITE_EARLY", value ? value : "");
#else
            if (value) setenv("OPENMW_HITCH_COMPOSITE_EARLY", value, 1);
            else unsetenv("OPENMW_HITCH_COMPOSITE_EARLY");
#endif
        }
        ~EarlyEnvironment() { set(existed ? previous.c_str() : nullptr); }
    };
    struct StepEnvironment
    {
        std::string previous = std::getenv("OPENMW_HITCH_COMPOSITE_STEP") ? std::getenv("OPENMW_HITCH_COMPOSITE_STEP") : "";
        bool existed = std::getenv("OPENMW_HITCH_COMPOSITE_STEP") != nullptr;
        explicit StepEnvironment(const char* value) { set(value); }
        static void set(const char* value)
        {
#ifdef _WIN32
            _putenv_s("OPENMW_HITCH_COMPOSITE_STEP", value ? value : "");
#else
            if (value) setenv("OPENMW_HITCH_COMPOSITE_STEP", value, 1);
            else unsetenv("OPENMW_HITCH_COMPOSITE_STEP");
#endif
        }
        ~StepEnvironment() { set(existed ? previous.c_str() : nullptr); }
    };
    struct NamedEnvironment
    {
        std::string name, previous;
        bool existed;
        NamedEnvironment(const char* key, const char* value) : name(key),
            previous(std::getenv(key) ? std::getenv(key) : ""), existed(std::getenv(key) != nullptr)
        { set(value); }
        void set(const char* value) const
        {
#ifdef _WIN32
            _putenv_s(name.c_str(), value ? value : "");
#else
            if (value) setenv(name.c_str(), value, 1); else unsetenv(name.c_str());
#endif
        }
        ~NamedEnvironment() { set(existed ? previous.c_str() : nullptr); }
    };
    class CountingProgram final : public osg::Program
    {
    public:
        mutable unsigned applications = 0;
        void apply(osg::State& state) const override { ++applications; osg::Program::apply(state); }
    };
    class AdmissionRetiringTexture final : public Resource::PreparedTerrainTexture
    {
    public:
        explicit AdmissionRetiringTexture(osg::Image* image) : PreparedTerrainTexture(image) {}
        bool preparationAdmissionEligible(const osg::State&) const noexcept override { return admitted; }
        void apply(osg::State& state) const override
        {
            ++applications;
            PreparedTerrainTexture::apply(state);
        }
        bool admitted = true;
        mutable unsigned applications = 0;
    };
    class ControlledGeometry final : public osg::Geometry
    {
    public:
        mutable bool fail = false;
        bool delay = false, changeViewport = false;
        ControlledGeometry(const osg::Geometry& original) : osg::Geometry(original) {}
        void drawImplementation(osg::RenderInfo& info) const override
        {
            if (fail) throw std::runtime_error("fixture draw failure");
            osg::Geometry::drawImplementation(info);
            if (changeViewport) glViewport(1, 1, 1, 1);
            if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
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
            map->mBatchingEligible = true;
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
        const auto width = image.getTextureWidth(), height = image.getTextureHeight();
        require(width > 0 && height > 0 && width <= 4096 && height <= 4096, "fixture readback dimensions invalid");
        std::vector<unsigned char> pixels(std::size_t(width) * height * 4);
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

    struct CachedPressure
    {
        Resource::OpenGlPressureSample value;
        CachedPressure()
        {
            value.generation = 1;
            value.memory.physicalValid = value.memory.commitValid = true;
            value.memory.physicalTotal = 32ull * 1024 * 1024 * 1024;
            value.memory.physicalAvailable = value.memory.commitAvailable = 20ull * 1024 * 1024 * 1024;
            value.decision.state = Resource::OpenGlPressure::Normal;
            value.decision.admissionsPerSample = 4;
        }
        static Resource::OpenGlPressureSample sample(void* owner) { return static_cast<CachedPressure*>(owner)->value; }
        Resource::SpeculativeScope::Context context() { return {nullptr, &sample, this}; }
        void transition(Resource::OpenGlPressure valueState)
        { ++value.generation; value.decision.state = valueState; }
    };
    class OrderedGeometry final : public osg::Geometry
    {
    public:
        std::vector<unsigned>* order;
        unsigned layer;
        OrderedGeometry(const osg::Geometry& original, std::vector<unsigned>& observed, unsigned value)
            : osg::Geometry(original), order(&observed), layer(value) {}
        void drawImplementation(osg::RenderInfo& info) const override
        { osg::Geometry::drawImplementation(info); order->push_back(layer); }
    };
    osg::ref_ptr<Resource::PreparedTerrainTexture> largeCompressedTexture(unsigned index)
    {
        // A real 4096-square, complete DXT5 mip chain: 21.333 MiB each.
        constexpr GLenum dxt5 = 0x83F3;
        osg::Image::MipmapDataType offsets;
        std::size_t bytes = 0;
        for (unsigned size = 4096; size; size /= 2)
        {
            if (bytes) offsets.push_back(static_cast<unsigned>(bytes));
            bytes += std::size_t(std::max(1u, (size + 3) / 4)) * std::max(1u, (size + 3) / 4) * 16;
        }
        auto* data = new unsigned char[bytes];
        std::fill_n(data, bytes, 0);
        for (std::size_t block = 0; block < bytes; block += 16)
        { data[block] = data[block + 1] = 255; data[block + 8] = data[block + 10] = 0xe0;
            data[block + 9] = data[block + 11] = 0x07; }
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->setImage(4096, 4096, 1, dxt5, dxt5, GL_UNSIGNED_BYTE, data, osg::Image::USE_NEW_DELETE, 1);
        image->setMipmapLevels(offsets);
        image->setFileName("fixture/step-green-" + std::to_string(index) + ".dds");
        require(image->getTotalDataSize() == bytes && bytes == 22369648, "compressed mip-chain estimate differs");
        osg::ref_ptr<Resource::PreparedTerrainTexture> result = new Resource::PreparedTerrainTexture(image);
        result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        result->setResizeNonPowerOfTwoHint(false);
        return result;
    }
    void opaqueLayer(MapFixture& fixture, Resource::PreparedTerrainTexture* source,
        std::vector<unsigned>& order, unsigned layer)
    {
        osg::ref_ptr<osg::Geometry> original = osg::createTexturedQuadGeometry(
            osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
        original->setUseDisplayList(false); original->setUseVertexBufferObjects(false);
        auto* pass = original->getOrCreateStateSet();
        pass->setAttributeAndModes(fixture.program, osg::StateAttribute::ON);
        pass->setTextureAttribute(0, source); pass->setTextureAttribute(1, texture({0, 0, 0, 255}));
        pass->addUniform(new osg::Uniform("diffuseMap", 0)); pass->addUniform(new osg::Uniform("blendMap", 1));
        pass->setMode(GL_BLEND, osg::StateAttribute::OFF); pass->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        fixture.map->mDrawables.push_back(new OrderedGeometry(*original, order, layer));
    }
    void stepFixtures(osg::GraphicsContext& context, osg::State& state, osg::RenderInfo& info,
        osg::FrameStamp& stamp, Resource::OpenMWIncrementalCompileOperation& ico, const char* tracePath)
    {
        StepEnvironment stepEnvironment("1");
        CachedPressure pressure;
        unsigned frame = 1000;
        auto makeRenderer = [&] {
            osg::ref_ptr<Terrain::CompositeMapRenderer> result = new Terrain::CompositeMapRenderer;
            result->configurePreparation(&ico, pressure.context());
            result->setMinimumTimeAvailableForCompile(.1); result->setCooperativeBackgroundCompile(true);
            return result;
        };
        auto next = [&](Terrain::CompositeMapRenderer& renderer, bool runIco = true) {
            stamp.setFrameNumber(frame++); context.clear(); renderer.drawImplementation(info);
            if (runIco) ico(&context);
            require(renderer.preparationStats().pendingBytes <= 32u * 1024u * 1024u, "STEP debit exceeded 32 MiB");
        };
        {
            auto renderer = makeRenderer();
            MapFixture large;
            large.map->mDrawables.clear();
            std::vector<unsigned> order;
            for (unsigned i = 0; i < 20; ++i) opaqueLayer(large, largeCompressedTexture(i), order, i);
            require(20ull * 22369648 > 426ull * 1024 * 1024, "large map does not exercise former whole-map cap");
            renderer->addCompositeMap(large.map);
            std::size_t previous = 0;
            for (unsigned attempts = 0; attempts < 240 && !large.map->mDrawables.empty(); ++attempts)
            {
                next(*renderer);
                require(large.map->mCompiled <= previous + 1, "STEP drew more than one prepared layer per window");
                previous = large.map->mCompiled;
            }
            require(large.map->mDrawables.empty() && large.map->mCompiled == 20, "large map did not finish incremental bake");
            require(order.size() == 20, "STEP lost or repeated layers");
            for (unsigned i = 0; i < order.size(); ++i) require(order[i] == i, "STEP changed original layer order");
            require(renderer->preparationStats().dependencyWaves >= 21, "large map did not use bounded layer windows");
            const auto value = pixel(*large.map->mTexture, state);
            require(value[0] < 2 && value[1] > 253 && value[2] < 2 && value[3] == 255, "large DXT5 layer pixels differ");
            require(renderer->preparationStats().pendingBytes == 0, "complete large map retained preparation credit");
            renderer->releaseGLObjects(&state);
        }
        {
            auto renderer = makeRenderer();
            MapFixture partial;
            std::vector<unsigned> order;
            for (unsigned i = 0; i < partial.map->mDrawables.size(); ++i)
                partial.map->mDrawables[i] = new OrderedGeometry(*partial.map->mDrawables[i]->asGeometry(), order, i);
            renderer->addCompositeMap(partial.map);
            for (unsigned i = 0; i < 30 && partial.map->mCompiled == 0; ++i) next(*renderer);
            require(partial.map->mCompiled == 1 && order == std::vector<unsigned>{0}, "partial first layer not observed");
            const auto before = renderer->preparationStats().invalidated;
            partial.red->getImage()->data()[0] = 128; partial.red->getImage()->dirty();
            renderer->setImmediate(partial.map);
            next(*renderer);
            require(partial.map->mDrawables.empty() && order == std::vector<unsigned>({0, 0, 1}),
                "required stale consumed layer did not restart at layer zero");
            require(renderer->preparationStats().invalidated > before, "stale consumed revision was not invalidated");
            MapFixture control;
            control.red->getImage()->data()[0] = 128; control.red->getImage()->dirty();
            StepEnvironment controlEnvironment("0");
            osg::ref_ptr<Terrain::CompositeMapRenderer> old = new Terrain::CompositeMapRenderer;
            old->compile(*control.map, info);
            require(pixel(*control.map->mTexture, state) == pixel(*partial.map->mTexture, state),
                "required stale restart final pixel differs from fresh original bake");
            old->releaseGLObjects(&state); renderer->releaseGLObjects(&state);
        }
        {
            auto renderer = makeRenderer();
            MapFixture pending;
            renderer->addCompositeMap(pending.map);
            next(*renderer, false);
            require(pending.map->mPreparation && renderer->preparationStats().pendingBytes > 0, "STEP queue not admitted");
            pending.consumer = nullptr;
            next(*renderer, false);
            if (!pending.map->mDrawables.empty() || !renderer->preparationStats().pendingBytes)
                std::cerr << "orphan evidence: layers=" << pending.map->mDrawables.size()
                    << " compiled=" << pending.map->mCompiled << " debit=" << renderer->preparationStats().pendingBytes
                    << " target_refs=" << pending.map->mTexture->referenceCount()
                    << " descriptors=" << renderer->preparationStats().scheduled << '\n';
            require(pending.map->mDrawables.empty() && renderer->preparationStats().pendingBytes > 0,
                "cancellation refunded ownership before queued operation destruction");
            ico(&context);
            require(ico.getToCompile().empty() && renderer->preparationStats().pendingBytes == 0,
                "unsubmitted canceled STEP wave failed to release debit after queue retirement");
            renderer->releaseGLObjects(&state);
        }
        {
            auto renderer = makeRenderer();
            MapFixture denied;
            pressure.transition(Resource::OpenGlPressure::Critical);
            renderer->addCompositeMap(denied.map); next(*renderer);
            require(denied.map->mCompiled == 0 && !denied.map->mPreparation && renderer->preparationStats().pressureYields,
                "critical cached pressure admitted new optional STEP work");
            renderer->setImmediate(denied.map); next(*renderer);
            require(denied.map->mDrawables.empty(), "critical pressure delayed required original bake");
            pressure.transition(Resource::OpenGlPressure::Normal);
            MapFixture canceled;
            renderer->addCompositeMap(canceled.map); next(*renderer);
            pressure.value.generation = 0; next(*renderer);
            require(!canceled.map->mPreparation && canceled.map->mCompiled == 0,
                "missing pressure did not cancel existing optional wave");
            renderer->setImmediate(canceled.map); next(*renderer);
            require(canceled.map->mDrawables.empty(), "missing pressure delayed required fallback");
            pressure.value.generation = 1;
            renderer->releaseGLObjects(&state);
        }
        {
            auto renderer = makeRenderer();
            MapFixture submitted;
            renderer->addCompositeMap(submitted.map);
            for (unsigned i = 0; i < 30 && !submitted.red->getTextureObject(state.getContextID()); ++i) next(*renderer);
            require(submitted.map->mCompiled == 0 && submitted.red->getTextureObject(state.getContextID()),
                "source submission did not precede actual layer use");
            pressure.transition(Resource::OpenGlPressure::Critical);
            next(*renderer);
            require(!submitted.map->mPreparation && ico.getToCompile().empty()
                && renderer->preparationStats().pendingBytes > 0,
                "canceled already-submitted GL source/target falsely refunded unused debt");
            pressure.transition(Resource::OpenGlPressure::Normal);
            renderer->setImmediate(submitted.map); next(*renderer);
            require(submitted.map->mDrawables.empty() && renderer->preparationStats().pendingBytes == 0,
                "exact required layer use failed to retire canceled submitted-identity debt");
            renderer->releaseGLObjects(&state);
        }
        for (unsigned mutation = 0; mutation < 3; ++mutation)
        {
            auto renderer = makeRenderer();
            MapFixture partial;
            std::vector<unsigned> order;
            for (unsigned i = 0; i < partial.map->mDrawables.size(); ++i)
                partial.map->mDrawables[i] = new OrderedGeometry(*partial.map->mDrawables[i]->asGeometry(), order, i);
            renderer->addCompositeMap(partial.map);
            for (unsigned i = 0; i < 30 && partial.map->mCompiled == 0; ++i) next(*renderer);
            require(partial.map->mCompiled == 1, "program/target/release fixture failed to consume first layer");
            osg::observer_ptr<osg::Texture::TextureObject> oldTargetObject
                = partial.map->mTexture->getTextureObject(state.getContextID());
            const auto oldTargetIdentity = reinterpret_cast<std::uintptr_t>(oldTargetObject.get());
            require(oldTargetIdentity, "mutation fixture has no submitted target identity");
            MapFixture replacement;
            if (mutation == 0)
                partial.map->mDrawables[0]->getStateSet()->setAttributeAndModes(replacement.program, osg::StateAttribute::ON);
            else if (mutation == 1)
            { partial.map->mTexture = target(); partial.consumer = partial.map->mTexture; }
            else renderer->releaseGLObjects(&state);
            renderer->setImmediate(partial.map); next(*renderer);
            if (!partial.map->mDrawables.empty() || order != std::vector<unsigned>({0, 0, 1}))
            {
                std::cerr << "mutation evidence: kind=" << mutation << " layers=" << partial.map->mDrawables.size()
                    << " compiled=" << partial.map->mCompiled << " target_refs=" << partial.map->mTexture->referenceCount()
                    << " order=";
                for (const auto value : order) std::cerr << value << ',';
                std::cerr << '\n';
            }
            require(partial.map->mDrawables.empty() && order == std::vector<unsigned>({0, 0, 1}),
                "program/target/context-release mutation failed to restart consumed layers");
            const auto value = pixel(*partial.map->mTexture, state);
            require(value[0] >= 63 && value[0] <= 65 && value[1] >= 190 && value[1] <= 192,
                "program/target/context-release required restart changed final original pixels");
            if (mutation == 0)
                require(renderer->preparationStats().pendingBytes == 0,
                    "required program restart retained debit after exact source/target use");
            else
            {
                // FBO release recursively releases its attached texture too,
                // but OSG may reuse that exact pooled object for the new bake.
                const auto* currentTargetObject = partial.map->mTexture->getTextureObject(state.getContextID());
                require(currentTargetObject, "required restart has no current target object");
                osg::ref_ptr<osg::Texture::TextureObject> oldOwner;
                const bool oldLive = oldTargetObject.lock(oldOwner);
                const bool reused = oldLive && oldOwner.get() == currentTargetObject;
                std::cout << "mutation debt: kind=" << mutation << " branch="
                    << (reused ? "exact_live_reuse" : oldLive ? "distinct_old_live" : "old_expired")
                    << " old_address=" << oldTargetIdentity << " current_address="
                    << reinterpret_cast<std::uintptr_t>(currentTargetObject)
                    << " pending=" << renderer->preparationStats().pendingBytes << '\n';
                if (reused)
                    require(renderer->preparationStats().pendingBytes == 0,
                        "exact required use of reused live target did not consume its debt");
                else
                {
                    if (oldLive)
                        require(renderer->preparationStats().pendingBytes > 0,
                            "unused pooled target falsely refunded its conservative debit");
                    oldOwner = nullptr;
                    // Fixture-only cleanup drains already-orphaned objects.
                    // An expired weak owner is different even if the allocator
                    // has recycled its raw address for the current live one.
                    osg::flushAllDeletedGLObjects(state.getContextID());
                    require(!oldTargetObject.valid(), "OSG orphan cleanup retained the retired target owner");
                    if (ico.progressRepairEnabled())
                    {
                        next(*renderer, false); // no new map and no ICO admission
                        require(renderer->preparationStats().pendingBytes == 0,
                            "empty-queue maintenance failed to prune genuinely expired target debt");
                    }
                }
            }
            renderer->releaseGLObjects(&state);
        }
        {
            auto renderer = makeRenderer();
            MapFixture retired;
            osg::ref_ptr<AdmissionRetiringTexture> source = new AdmissionRetiringTexture(retired.red->getImage());
            source->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            source->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            source->setResizeNonPowerOfTwoHint(false);
            retired.red = source;
            retired.map->mDrawables[0]->getStateSet()->setTextureAttribute(0, source);
            const auto* imageIdentity = source->getImage();
            const unsigned imageRevision = imageIdentity->getModifiedCount();
            renderer->addCompositeMap(retired.map);
            for (unsigned i = 0; i < 12 && renderer->preparationStats().dependencyWaves < 2; ++i)
            {
                next(*renderer, false);
                if (renderer->preparationStats().dependencyWaves < 2) ico(&context);
            }
            require(retired.map->mPreparation && renderer->preparationStats().dependencyWaves >= 2
                && !ico.getToCompile().empty() && source->applications == 0,
                "retired-admission fixture did not queue a source wave before apply");
            source->admitted = false;
            require(source->getImage() == imageIdentity && imageIdentity->getModifiedCount() == imageRevision,
                "pure eligibility retirement changed outer image identity/revision");
            ico(&context); ico(&context);
            require(source->applications == 0 && ico.getToCompile().empty() && retired.map->mCompiled == 0,
                "retired optional source applied a fallback or failed to cancel its queued wave");
            renderer->setImmediate(retired.map);
            next(*renderer);
            require(retired.map->mDrawables.empty() && !retired.map->mPreparation && source->applications > 0,
                "eligibility retirement delayed required fallback or retained cancelled preparation");
            MapFixture control;
            StepEnvironment controlEnvironment("0");
            osg::ref_ptr<Terrain::CompositeMapRenderer> original = new Terrain::CompositeMapRenderer;
            original->compile(*control.map, info);
            require(pixel(*retired.map->mTexture, state) == pixel(*control.map->mTexture, state),
                "required fallback after pure admission retirement changed original pixels");
            require(source->getImage() == imageIdentity && imageIdentity->getModifiedCount() == imageRevision,
                "eligibility-only cancellation modified source image metadata");
            std::cout << "PASS: eligibility-only retirement cancels queued source without apply; required original pixels preserved\n";
            original->releaseGLObjects(&state); renderer->releaseGLObjects(&state);
        }
        {
            auto renderer = makeRenderer();
            std::array<std::unique_ptr<MapFixture>, 9> bounded;
            for (auto& fixture : bounded)
            { fixture = std::make_unique<MapFixture>(); renderer->addCompositeMap(fixture->map); }
            next(*renderer, false);
            unsigned admitted = 0;
            for (const auto& fixture : bounded) if (fixture->map->mPreparation) ++admitted;
            require(admitted == 8, "STEP descriptor admission exceeded eight map limit");
            for (auto& fixture : bounded) fixture->consumer = nullptr;
            next(*renderer, false); ico(&context);
            require(renderer->preparationStats().pendingBytes == 0, "bounded canceled STEP descriptors retained ownership");
            renderer->releaseGLObjects(&state);
        }
        {
            auto renderer = makeRenderer();
            MapFixture oversize;
            oversize.map->mDrawables.clear();
            std::vector<unsigned> order;
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(3072, 3072, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            std::fill_n(image->data(), image->getTotalDataSize(), 255);
            osg::ref_ptr<Resource::PreparedTerrainTexture> source = new Resource::PreparedTerrainTexture(image);
            source->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
            source->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
            opaqueLayer(oversize, source, order, 0);
            renderer->addCompositeMap(oversize.map); next(*renderer);
            require(oversize.map->mDrawables.empty() && !oversize.map->mPreparation
                && renderer->preparationStats().scheduled == 0 && order == std::vector<unsigned>{0},
                "single 36 MiB layer did not decline optional STEP and retain complete original bake");
            const auto value = pixel(*oversize.map->mTexture, state);
            require(value == std::array<unsigned char, 4>{255, 255, 255, 255}, "oversize fallback pixel changed");
            renderer->releaseGLObjects(&state);
        }
#ifdef P9_COMPILE_TRACE_TEST_HOOKS
        if (ico.progressRepairEnabled())
        {
            auto renderer = makeRenderer();
            MapFixture submitted;
            renderer->addCompositeMap(submitted.map);
            for (unsigned i = 0; i < 30 && !submitted.red->getTextureObject(state.getContextID()); ++i) next(*renderer);
            require(submitted.map->mCompiled == 0 && submitted.red->getTextureObject(state.getContextID()),
                "debt failure fixture did not submit an unused source");
            {
                NamedEnvironment failure("P9_COMPOSITE_DEBT_FAILURE", "1");
                pressure.transition(Resource::OpenGlPressure::Critical);
                next(*renderer);
            }
            require(!submitted.map->mPreparation && ico.getToCompile().empty()
                && renderer->preparationStats().pendingBytes > 0,
                "debt ledger allocation failure falsely refunded submitted ownership");
            pressure.transition(Resource::OpenGlPressure::Normal);
            const auto scheduled = renderer->preparationStats().scheduled;
            MapFixture rejected;
            renderer->addCompositeMap(rejected.map);
            next(*renderer);
            require(!rejected.map->mPreparation && renderer->preparationStats().scheduled == scheduled,
                "debt ledger failure did not disable subsequent speculation");
            renderer->setImmediate(submitted.map); renderer->setImmediate(rejected.map);
            next(*renderer);
            require(submitted.map->mDrawables.empty() && rejected.map->mDrawables.empty(),
                "failed debt ledger blocked required complete rendering");
            require(pixel(*submitted.map->mTexture, state) == pixel(*rejected.map->mTexture, state),
                "required rendering after debt ledger failure changed pixels");
            // The failed entry cannot be proven consumed; conservative credit
            // remains stranded for this renderer instead of being guessed free.
            require(renderer->preparationStats().pendingBytes > 0,
                "failed ledger advertised unprovable unused ownership as free");
            renderer->releaseGLObjects(&state);
        }
#endif
        // Real selected-extension mipmap call with the actual producer apply
        // owner; a neighboring cached attribute must never supply its identity.
        auto& capture = SceneUtil::GLCallTrace::Capture::instance();
        capture.enable(tracePath);
        require(SceneUtil::GLCallTrace::install(state.getContextID(), *state.get<osg::GLExtensions>()), "GL hooks unavailable");
        osg::ref_ptr<osg::Image> mipImage = new osg::Image;
        mipImage->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        std::fill_n(mipImage->data(), mipImage->getTotalDataSize(), 255);
        osg::ref_ptr<Resource::PreparedTerrainTexture> mip = new Resource::PreparedTerrainTexture(mipImage);
        mip->setPreparationMemoEnabled(false);
        mip->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        state.setActiveTextureUnit(1);
        auto neighbor = texture({255, 0, 0, 255});
        state.applyTextureAttribute(1, neighbor);
        mip->apply(state); mip->allocateMipmapLevels(); mip->apply(state);
        const auto call = capture.breadcrumb(state.getContextID()).row;
        require(call.api == SceneUtil::GLCallTrace::GenerateMipmap && call.texture == reinterpret_cast<std::uintptr_t>(mip.get())
            && call.image == reinterpret_cast<std::uintptr_t>(mipImage.get()) && call.textureUnit == 1,
            "actual mipmap owner was unknown or inferred from neighboring cached texture");
        mip->rememberPreparation(state);
        require(!mip->preparationUnchanged(state), "diagnostic-only wrapper accidentally enabled precompile reuse");
        state.haveAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE);
        mip->releaseGLObjects(&state); neighbor->releaseGLObjects(&state);
        require(glGetError() == GL_NO_ERROR, "GL error in STEP/owner fixtures");
        std::cout << "PASS: STEP 20 large mipmapped layers, 32MiB/eight-map debt, exact order/pixels, stale consumed-source/program/target/context-release required restart, submitted/unsubmitted cancellation lifetime, pressure fallback, oversized complete layer, and actual mipmap owner\n";
    }

    void progressFixtures(osg::GraphicsContext& context, osg::State& state, osg::RenderInfo& info,
        osg::FrameStamp& stamp, Resource::OpenMWIncrementalCompileOperation& ico)
    {
        StepEnvironment stepEnvironment("1");
        EarlyEnvironment earlyEnvironment("1");
        CachedPressure pressure;
        osg::ref_ptr<Terrain::CompositeMapRenderer> renderer = new Terrain::CompositeMapRenderer;
        renderer->configurePreparation(&ico, pressure.context());
        renderer->setMinimumTimeAvailableForCompile(.002);
        renderer->setCooperativeBackgroundCompile(true);
        require(renderer->producerPreparationEnabled() && ico.progressRepairEnabled(), "progress producer gate is not active");
        require(ico.compositeMaxQueueAge() == 120 && ico.compositeBudgetMs() == 2,
            "progress fixture does not use the representative 2ms/120-frame configuration");

        // The actual private producer builds 171 retained maps without GL. Only
        // eight descriptors may own optional resource preparation at once.
        constexpr unsigned MapCount = 171;
        std::array<std::unique_ptr<MapFixture>, MapCount> maps;
        std::array<std::vector<unsigned>, MapCount> orders;
        for (unsigned i = 0; i < MapCount; ++i)
        {
            maps[i] = std::make_unique<MapFixture>();
            maps[i]->map->mTexture->setTextureSize(512, 512); // Real cold 1 MiB target.
            for (unsigned layer = 0; layer < 2; ++layer)
                maps[i]->map->mDrawables[layer] = new OrderedGeometry(
                    *maps[i]->map->mDrawables[layer]->asGeometry(), orders[i], layer);
        }
        std::thread producer([&] {
            for (const auto& fixture : maps) renderer->prepareAtCreation(*fixture->map);
        });
        producer.join();
        auto stats = renderer->preparationStats();
        unsigned admitted = 0;
        for (const auto& fixture : maps)
        {
            if (fixture->map->mPreparation) ++admitted;
            require(!fixture->map->mTexture->getTextureObject(state.getContextID()),
                "producer descriptor creation executed GL");
        }
        require(admitted == 8 && stats.scheduled == 8 && stats.descriptorBuilds == 8
            && stats.capacityDeclines == MapCount - 8, "producer did not bound descriptor admission to eight maps");
        const auto declines = stats.capacityDeclines;
        std::thread deniedProducer([&] {
            for (unsigned repeat = 0; repeat < 3; ++repeat)
                for (unsigned i = 8; i < MapCount; ++i) renderer->prepareAtCreation(*maps[i]->map);
        });
        deniedProducer.join();
        stats = renderer->preparationStats();
        require(stats.descriptorBuilds == 8 && stats.capacityDeclines == declines + 3 * (MapCount - 8),
            "full descriptor pool rescanned denied producer graphs");
        require(stats.pendingBytes > 0 && stats.pendingBytes <= 32u * 1024u * 1024u,
            "producer preparation debit is absent or exceeds 32MiB");
        for (const auto& fixture : maps) renderer->addCompositeMap(fixture->map);
        require(renderer->getCompileSetSize() == MapCount, "retained maps were lost at queue publication");

        unsigned frame = 5000;
        auto ownerDraw = [&] {
            const auto before = renderer->preparationStats().polls;
            renderer->drawImplementation(info);
            require(renderer->preparationStats().polls - before <= 16
                && ico.compositeAdmission()->compositeMaintenanceVisits() <= 16,
                "owner frame exceeded sixteen map visits");
            require(renderer->preparationStats().pendingBytes <= 32u * 1024u * 1024u,
                "progress queue exceeded 32MiB ownership cap");
        };
        auto next = [&](bool runIco = true) {
            stamp.setFrameNumber(frame++); context.clear(); ownerDraw();
            if (runIco) ico(&context);
        };
        const auto wakeBefore = maps[0]->map->mProgressWake.load(std::memory_order_acquire);
        bool coldTargetBootstrapped = false;
        for (unsigned attempts = 0; attempts < 32 && !coldTargetBootstrapped; ++attempts)
        {
            next();
            bool submitted = false;
            for (unsigned i = 0; i < 8; ++i)
                submitted = submitted || maps[i]->map->mTexture->getTextureObject(state.getContextID());
            coldTargetBootstrapped = submitted && ico.compositeAdmission()->bootstrapUsed();
        }
        require(coldTargetBootstrapped, "cold 1MiB target waited for 120-frame age instead of bounded bootstrap");
        require(ico.compositeAdmission()->chargedMs() > 0, "cold target/bootstrap work was not actually charged");
        const auto pollsBeforeRepeats = renderer->preparationStats().polls;
        const auto visitsBeforeRepeats = ico.compositeAdmission()->compositeMaintenanceVisits();
        for (unsigned repeat = 0; repeat < 3; ++repeat) ownerDraw();
        require(renderer->preparationStats().polls - pollsBeforeRepeats <= 16 - visitsBeforeRepeats,
            "repeated traversal replenished the owner-frame maintenance allowance");

        unsigned promoted = MapCount;
        for (unsigned attempts = 0; attempts < 48 && promoted == MapCount; ++attempts)
        {
            for (unsigned i = 8; i < MapCount; ++i)
                if (maps[i]->map->mProgressWait.reason() == Terrain::CompositeProgressWait::Reason::Capacity)
                { promoted = i; break; }
            if (promoted == MapCount) next(false);
        }
        require(promoted != MapCount && !maps[promoted]->map->mPreparation,
            "no denied map retained canonical capacity-parked queue membership");
        renderer->setImmediate(maps[promoted]->map);
        next(false);
        require(maps[promoted]->map->mDrawables.empty() && orders[promoted] == std::vector<unsigned>({0, 1})
            && renderer->preparationStats().requiredFallbacks > 0,
            "required promotion of a parked map lost original complete layer order");

        auto complete = [&] {
            return std::all_of(maps.begin(), maps.end(), [](const auto& fixture) { return fixture->map->mDrawables.empty(); });
        };
        unsigned attempts = 0;
        for (; attempts < 900 && !complete(); ++attempts) next();
        if (!complete())
        {
            stats = renderer->preparationStats();
            std::cerr << "progress evidence: scheduled=" << stats.scheduled << " completed=" << stats.completedMaps
                << " layers=" << stats.consumedLayers << " visits=" << stats.polls << " pending=" << stats.pendingBytes << '\n';
        }
        require(complete(), "bounded progress/fair cursor left retained maps unfinished after 900 owner frames");
        for (unsigned i = 0; i < MapCount; ++i)
            require(maps[i]->map->mCompiled == 2 && orders[i] == std::vector<unsigned>({0, 1}),
                "progress preparation lost, repeated, or reordered a terrain layer");
        next();
        stats = renderer->preparationStats();
        require(stats.scheduled >= MapCount - 1 && stats.descriptorBuilds == stats.scheduled
            && stats.completedMaps >= MapCount - 1 && stats.consumedLayers >= 2 * (MapCount - 1),
            "weak completion/capacity wake did not admit and complete the waiting maps");
        require(maps[0]->map->mProgressWake.load(std::memory_order_acquire) > wakeBefore,
            "actual ICO completion did not publish a weak map wake");
        require(stats.pendingBytes == 0 && ico.getToCompile().empty(),
            "completed progress queue retained optional preparation debit");
        {
            MapFixture control;
            StepEnvironment oldStep("0");
            NamedEnvironment oldProgress("OPENMW_HITCH_COMPOSITE_PROGRESS", "0");
            osg::ref_ptr<Terrain::CompositeMapRenderer> original = new Terrain::CompositeMapRenderer;
            original->compile(*control.map, info);
            const auto expected = pixel(*control.map->mTexture, state);
            for (const unsigned index : {0u, promoted, MapCount - 1})
                require(pixel(*maps[index]->map->mTexture, state) == expected,
                    "progress/required terrain pixels differ from original control");
            original->releaseGLObjects(&state);
        }
        // Cancellation releases an unsubmitted wave only after the queued ICO
        // owner is destroyed. There is no GL object whose use can be guessed.
        MapFixture cancelled;
        renderer->prepareAtCreation(*cancelled.map);
        renderer->addCompositeMap(cancelled.map);
        require(renderer->preparationStats().pendingBytes > 0, "cancellation fixture was not admitted");
        cancelled.consumer = nullptr;
        next(false);
        require(cancelled.map->mDrawables.empty() && renderer->preparationStats().pendingBytes > 0,
            "cancelled progress wave refunded before queued ownership retired");
        ico(&context);
        require(renderer->preparationStats().pendingBytes == 0 && ico.getToCompile().empty(),
            "cancelled unsubmitted progress wave failed to release all debit");
        renderer->releaseGLObjects(&state);
        require(glGetError() == GL_NO_ERROR, "GL error in actual progress queue fixtures");
        std::cout << "PASS: 171 retained maps/eight descriptors, cheap producer refusal, <=16 shared owner-frame visits, cold 1MiB target bootstrap at 2ms/120 age, weak completion and capacity wakes, fairness, parked required promotion, original two-layer order/pixels, and cancellation debit\n";
    }
}

int main(int argc, char** argv) try
{
    BatchEnvironment originalEnvironment("0");
    EarlyEnvironment originalEarly("0");
    StepEnvironment originalStep("0");
    const std::string_view mode = argc > 1 ? argv[1] : "";
    const bool progress = mode == "--progress" || mode == "--step-progress";
    NamedEnvironment originalProgress("OPENMW_HITCH_COMPOSITE_PROGRESS", progress ? "1" : "0");
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
    Terrain::CompositeProgressWait wait;
    osg::ref_ptr<Terrain::CompositeMap> wakeFixture = new Terrain::CompositeMap;
    const auto precheckWake = wakeFixture->mProgressWake.load(std::memory_order_acquire);
    wakeFixture->mProgressWake.fetch_add(1, std::memory_order_release);
    wait.park(Terrain::CompositeProgressWait::Reason::Dependency, 100, precheckWake, 1, 1);
    require(wait.eligible(100, wakeFixture->mProgressWake.load(std::memory_order_acquire), 1, 1),
        "completion between readiness check and park was swallowed");
    wait.park(Terrain::CompositeProgressWait::Reason::Capacity, 100, 2, 1, 1);
    require(!wait.eligible(101, 2, 1, 1) && wait.eligible(101, 2, 2, 1)
        && wait.eligible(101, 2, 1, 2), "capacity/pressure generation wakes were lost");
    wait.park(Terrain::CompositeProgressWait::Reason::FrameBudget, 100, 2, 1, 1);
    require(!wait.eligible(100, 2, 1, 1) && wait.eligible(101, 2, 1, 1),
        "CPU budget parking did not wake on the next owner frame");

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
    if (mode == "--progress")
    {
        config.mTargetFrameRate = 60;
        config.mMaxBudgetMs = 2;
        config.mMaxObjectsPerFrame = 12;
        config.mMaxQueueAgeFrames = 120;
    }
    osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> ico = new Resource::OpenMWIncrementalCompileOperation(config);
    osgUtil::IncrementalCompileOperation::Contexts contexts{context.get()};
    ico->assignContexts(contexts);
    if (mode == "--progress")
    {
        progressFixtures(*context, state, info, *stamp, *ico);
        ico->removeContexts(contexts);
        context->releaseContext(); context->close(true);
        return 0;
    }
    if (mode == "--step" || mode == "--step-batch" || mode == "--step-progress")
    {
        BatchEnvironment stepBatch(std::string_view(argv[1]) == "--step-batch" ? "1" : "0");
        stepFixtures(*context, state, info, *stamp, *ico, argc > 2 ? argv[2] : "composite-step-fixture");
        ico->removeContexts(contexts);
        context->releaseContext(); context->close(true);
        return 0;
    }
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

    {
        osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> staticICO = new Resource::OpenMWIncrementalCompileOperation(config);
        staticICO->assignContexts(contexts);
        auto slot = std::make_shared<Resource::StaticTextureProgramAdmission>();
        Resource::SpeculativeBudget budget;
        const auto imageLease = [&](osg::Image* image) {
            osg::ref_ptr<Resource::GenericObjectCache<std::string>> cache = new Resource::GenericObjectCache<std::string>;
            cache->setSpeculativeBudget(&budget);
            cache->addEntryToObjectCache(std::string("normal"), image, 0.0, 1);
            return cache->getLeaseFromObjectCache(std::string("normal"), image, 1);
        }; // Local cache disappears; only the real copied publication lease remains.
        MapFixture material;
        require(!Resource::prepareStaticTextureProgram(staticICO, material.red, material.program, slot, {}),
            "uncharged static image admitted optional retention");
        auto preparation = Resource::prepareStaticTextureProgram(staticICO, material.red, material.program, slot,
            imageLease(material.red->getImage()));
        require(preparation && slot->active.load() && staticICO->getToCompile().size() == 1,
            "owned static material not admitted to existing ICO");
        require(!Resource::prepareStaticTextureProgram(staticICO, material.green, material.program, slot,
            imageLease(material.green->getImage())),
            "static material admitted a second retained descriptor");
        for (unsigned i = 0; i < 4; ++i) { stamp->setFrameNumber(frame++); (*staticICO)(context); }
        require(!preparation->lifetime->pending() && !preparation->lifetime->cancelled()
            && material.red->getTextureObject(state.getContextID()) && material.program->getPCP(state)->isLinked(),
            "owned static texture/program did not compile on original context");
        require(preparation->pass->getTextureAttributeList().size() == 1
            && preparation->pass->getTextureAttribute(0, osg::StateAttribute::TEXTURE) == material.red.get(),
            "static material retained dynamic/unrelated texture state");
        preparation.reset();
        require(!slot->active.load() && budget.stats().knownOwnerBytes == 0,
            "static material descriptor/charge retirement did not complete together");
        MapFixture changed;
        preparation = Resource::prepareStaticTextureProgram(staticICO, changed.red, changed.program, slot,
            imageLease(changed.red->getImage()));
        changed.red->getImage()->dirty();
        for (unsigned i = 0; i < 3; ++i) { stamp->setFrameNumber(frame++); (*staticICO)(context); }
        require(preparation->lifetime->cancelled() && !changed.red->getTextureObject(state.getContextID()),
            "stale static image generation was submitted");
        preparation.reset();
        MapFixture cancelledStatic;
        preparation = Resource::prepareStaticTextureProgram(staticICO, cancelledStatic.red, cancelledStatic.program, slot,
            imageLease(cancelledStatic.red->getImage()));
        preparation->cancel();
        preparation.reset();
        require(slot->active.load() && budget.stats().knownOwnerBytes == cancelledStatic.red->getImage()->getTotalDataSize(),
            "queued operations lost descriptor/image charge before cancellation drain");
        for (unsigned i = 0; i < 3; ++i) { stamp->setFrameNumber(frame++); (*staticICO)(context); }
        require(!slot->active.load() && budget.stats().knownOwnerBytes == 0
            && !cancelledStatic.red->getTextureObject(state.getContextID()),
            "cancelled static material submitted or retained its queue owner");
        osg::ref_ptr<osg::Image> mipImage = new osg::Image;
        constexpr unsigned baseBytes = 32u * 1024u * 1024u;
        mipImage->setImage(4096, 2048, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE,
            new unsigned char[baseBytes + baseBytes / 4], osg::Image::USE_NEW_DELETE);
        mipImage->setMipmapLevels(osg::Image::MipmapDataType{baseBytes});
        osg::ref_ptr<osg::Texture2D> mipTexture = new osg::Texture2D(mipImage);
        require(mipImage->getImageSizeInBytes() == baseBytes && mipImage->getTotalSizeInBytesIncludingMipmaps() > baseBytes
            && !Resource::prepareStaticTextureProgram(staticICO, mipTexture, material.program, slot, imageLease(mipImage)),
            "full mipchain exceeded source cap while base image fit");
    }

    {
        EarlyEnvironment enabled("1");
        osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> earlyICO = new Resource::OpenMWIncrementalCompileOperation(config);
        earlyICO->assignContexts(contexts);
        osg::ref_ptr<Terrain::CompositeMapRenderer> early = new Terrain::CompositeMapRenderer;
        early->configurePreparation(earlyICO);
        early->setMinimumTimeAvailableForCompile(.1);
        require(early->producerPreparationEnabled() && !renderer->producerPreparationEnabled(), "early gate not frozen");
        EarlyEnvironment::set("true");
        osg::ref_ptr<Terrain::CompositeMapRenderer> invalidEarly = new Terrain::CompositeMapRenderer;
        invalidEarly->configurePreparation(earlyICO);
        require(!invalidEarly->producerPreparationEnabled(), "nonexact early gate accepted");
        MapFixture producer;
        std::thread worker([&] { early->prepareAtCreation(*producer.map); });
        worker.join();
        require(producer.map->mPreparation && producer.map->mCompiled == 0
            && earlyICO->getToCompile().size() == 1, "producer did not stage before renderer's first draw");
        for (unsigned i = 0; i < 12; ++i)
        {
            stamp->setFrameNumber(frame++);
            (*earlyICO)(context);
        }
        early->addCompositeMap(producer.map);
        early->setImmediate(producer.map);
        early->drawImplementation(info);
        require(producer.map->mDrawables.empty() && producer.map->mCompiled == 2
            && early->preparationStats().requiredFallbacks == 0, "early submitted dependencies did not satisfy required demand");
        require(pixel(*producer.map->mTexture, state)[0] >= 63, "early preparation changed completed pixels");

        std::array<std::unique_ptr<MapFixture>, 9> producers;
        std::array<std::thread, 9> workers;
        for (unsigned i = 0; i < producers.size(); ++i)
        {
            producers[i] = std::make_unique<MapFixture>();
            workers[i] = std::thread([&, i] { early->prepareAtCreation(*producers[i]->map); });
        }
        for (auto& thread : workers) thread.join();
        unsigned accepted = 0;
        for (auto& item : producers) if (item->map->mPreparation) ++accepted;
        require(accepted == 8 && early->preparationStats().pendingBytes <= 32u * 1024u * 1024u,
            "concurrent producer admission exceeded eight-map/byte bound");
        for (auto& item : producers) { early->addCompositeMap(item->map); item->consumer = nullptr; }
        stamp->setFrameNumber(frame++);
        early->drawImplementation(info);
        (*earlyICO)(context);
        require(early->preparationStats().pendingBytes == 0, "producer cancellation retained byte admission");
        early->releaseGLObjects(&state);
    }

    // Exercise the production batch and its caller/array lifetime boundary,
    // not a copied policy. The two-mode constructors freeze startup settings.
    {
        BatchEnvironment enabled("1");
        osg::ref_ptr<Terrain::CompositeMapRenderer> batch = new Terrain::CompositeMapRenderer;
        require(batch->compositeBatchEnabled() && !control->compositeBatchEnabled(), "batch gate not frozen at construction");
        BatchEnvironment::set("true");
        osg::ref_ptr<Terrain::CompositeMapRenderer> invalid = new Terrain::CompositeMapRenderer;
        require(!invalid->compositeBatchEnabled() && batch->compositeBatchEnabled(), "batch gate accepted a nonexact value or changed hot");
        osg::ref_ptr<CountingProgram> callerProgram = new CountingProgram;
        osg::ref_ptr<osg::StateSet> caller = new osg::StateSet;
        caller->setAttributeAndModes(callerProgram, osg::StateAttribute::ON);
        state.pushStateSet(caller);
        state.apply();
        const auto assertRestored = [&] {
            GLint program = -1, fbo = -1;
            glGetIntegerv(GL_CURRENT_PROGRAM, &program);
            glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &fbo);
            require(state.getLastAppliedAttribute(osg::StateAttribute::PROGRAM) == callerProgram.get()
                && state.getLastAppliedProgramObject() == nullptr && program == 0 && state.getStateSetStackSize() == 1,
                "batch released a producer before restoring caller Program/PCP");
            require(static_cast<GLuint>(fbo) == context->getDefaultFboId(), "batch left its FBO bound");
            require(glIsEnabled(GL_VERTEX_ARRAY) == GL_FALSE, "batch left temporary client arrays enabled");
            const auto* arrays = state.getCurrentVertexArrayState();
            const auto retiredArray = [](const osg::VertexArrayState::ArrayDispatch* dispatch) {
                return !dispatch || (!dispatch->array && !dispatch->active && dispatch->modifiedCount == 0xffffffff);
            };
            require(arrays && retiredArray(arrays->_vertexArray) && retiredArray(arrays->_normalArray)
                && retiredArray(arrays->_colorArray)
                && std::all_of(arrays->_texCoordArrays.begin(), arrays->_texCoordArrays.end(),
                    [&](const auto& dispatch) { return retiredArray(dispatch.get()); }),
                "batch retained raw client-array pointers after geometry retirement");
        };
        MapFixture controlPixels;
        callerProgram->applications = 0;
        control->compile(*controlPixels.map, info);
        require(callerProgram->applications == 2, "control caller restoration count changed");
        assertRestored();
        const auto expected = pixel(*controlPixels.map->mTexture, state);
        state.apply();
        MapFixture batchPixels;
        osg::observer_ptr<osg::Program> retired = batchPixels.program;
        batchPixels.program = nullptr; // StateSets are now the only program owners.
        callerProgram->applications = 0;
        batch->compile(*batchPixels.map, info);
        require(callerProgram->applications == 1, "batch did not coalesce caller restoration");
        assertRestored();
        require(!retired.valid(), "completed batch retained its source program");
        require(pixel(*batchPixels.map->mTexture, state) == expected, "batch changed multipass blend/depth/order pixels");
        state.apply();

        MapFixture unowned;
        unowned.map->mBatchingEligible = false;
        callerProgram->applications = 0;
        batch->compile(*unowned.map, info);
        require(callerProgram->applications == 2, "unowned composite producer entered batch");
        assertRestored();
        require(pixel(*unowned.map->mTexture, state) == expected, "unowned fallback changed pixels");
        state.apply();

        MapFixture viewportChange;
        auto* viewportGeometry = new ControlledGeometry(*viewportChange.map->mDrawables[0]->asGeometry());
        viewportGeometry->changeViewport = true;
        viewportChange.map->mDrawables[0] = viewportGeometry;
        batch->compile(*viewportChange.map, info);
        assertRestored();
        require(pixel(*viewportChange.map->mTexture, state) == expected, "batch failed to override changed viewport before each layer");
        state.apply();

        // A cooperative yield preserves unfinished passes and retires the
        // completed ones only after caller restoration. Re-entry is identical.
        batch->setCooperativeBackgroundCompile(true);
        batch->setTargetFrameRate(1e9f);
        batch->setMinimumTimeAvailableForCompile(.005);
        MapFixture yielding;
        auto* delayed = new ControlledGeometry(*yielding.map->mDrawables[0]->asGeometry());
        delayed->delay = true;
        yielding.map->mDrawables[0] = delayed;
        batch->addCompositeMap(yielding.map);
        callerProgram->applications = 0;
        batch->drawImplementation(info);
        require(yielding.map->mCompiled == 1 && yielding.map->mDrawables[0] == nullptr
            && yielding.map->mDrawables[1] && batch->backgroundYields() > 0, "batch ignored deadline or retired unfinished pass");
        assertRestored();
        batch->setImmediate(yielding.map);
        batch->drawImplementation(info);
        require(yielding.map->mDrawables.empty() && yielding.map->mCompiled == 2, "required batch did not finish after yield");
        assertRestored();
        require(pixel(*yielding.map->mTexture, state) == expected, "yielded batch changed final pixels");
        state.apply();

        MapFixture failed;
        auto* throwing = new ControlledGeometry(*failed.map->mDrawables[1]->asGeometry());
        throwing->fail = true;
        failed.map->mDrawables[1] = throwing;
        bool threw = false;
        try { batch->compile(*failed.map, info); }
        catch (const std::runtime_error&) { threw = true; }
        require(threw && failed.map->mCompiled == 1, "batch draw exception falsely committed its pass");
        assertRestored();
        throwing->fail = false;
        batch->compile(*failed.map, info);
        require(failed.map->mDrawables.empty() && pixel(*failed.map->mTexture, state) == expected,
            "batch exception recovery lost or replayed a completed pass");
        assertRestored();
        state.popStateSet(); state.apply(); state.disableAllVertexArrays();
        batch->releaseGLObjects(&state);
    }

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
    // Two ordinary OSG texture operations: the front op is destroyed by
    // CompileSet::compile(). Metadata must survive without dereferencing it.
    osg::ref_ptr<Resource::V321ClassifiedCompileSet> stock = new Resource::V321ClassifiedCompileSet(
        nullptr, Resource::V321CompileClass::GenericModel, Resource::V321CompileUrgency::NearFuture);
    std::array<osg::ref_ptr<osg::Texture2D>, 2> stockTextures;
    for (unsigned i = 0; i < stockTextures.size(); ++i)
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        std::fill_n(image->data(), image->getTotalDataSize(), static_cast<unsigned char>(i * 70));
        image->setFileName("attribution,image\"" + std::to_string(i) + ".dds");
        stockTextures[i] = new osg::Texture2D(image);
        stockTextures[i]->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        stock->_compileMap[context].add(new osgUtil::IncrementalCompileOperation::CompileTextureOp(stockTextures[i]));
    }
    ++stock->_numberCompileListsToCompile;
    ico->add(stock, false);
#ifdef P9_COMPILE_TRACE_TEST_HOOKS
    if (const auto* value = std::getenv("P9_COMPILE_METADATA_FAILURE"); value && std::string_view(value) == "1")
        Debug::CompileAttribution::metadataAllocationFaults = 1;
#endif
    next(); next();
    require(ico->getToCompile().empty(), "ordinary stock texture operations did not complete");
    for (auto& texture : stockTextures)
    {
        require(texture->getTextureObject(state.getContextID()) != nullptr, "stock texture did not upload");
        texture->releaseGLObjects(&state);
    }

    // Observation generation and op age remain distinct from CompileSet age;
    // a retired set address must never inherit the former occupant's history.
    Debug::CompileAttribution::ObservedSets observed;
    const auto first = observed.observe(stock.get(), stockTextures[0].get(), 5, false);
    require(first.opFirst == 5 && first.demandFirst == Debug::CompileAttribution::UnknownFrame,
        "near-future work was mislabeled as observed demand");
    observed.progressed(stock.get(), 6);
    const auto advanced = observed.observe(stock.get(), stockTextures[1].get(), 7, true);
    require(advanced.generation == first.generation && advanced.opFirst == 7
        && advanced.demandFirst == 7 && advanced.lastProgress == 6, "front op/demand/progress history conflated");
    observed.forget(stock.get());
    require(observed.observe(stock.get(), stockTextures[0].get(), 8, false).generation != first.generation,
        "recycled set address inherited its old generation");
    observed.prune([](const void*) { return false; });
    for (std::size_t i = 1; i <= Debug::CompileAttribution::ObservedSets::MaximumSets; ++i)
        observed.observe(reinterpret_cast<const void*>(i), nullptr, 9, false);
    require(observed.observe(reinterpret_cast<const void*>(Debug::CompileAttribution::ObservedSets::MaximumSets + 1),
        nullptr, 10, true).status == "unknown_capacity", "bounded observation overflow invented history");
    observed.prune([](const void*) { return false; });
    require(observed.observe(stock.get(), stockTextures[0].get(), 11, true).status == "observed"
        && observed.capacityFailures() == 1, "later observed history hid prior registry saturation");
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
    if (const auto* value = std::getenv("P9_COMPILE_NOFRAME"); value && std::string_view(value) == "1")
    {
        state.setFrameStamp(nullptr);
        (*ico)(context);
    }
    context->releaseContext(); context->close(true);
    std::cout << "PASS: shared admission, real preparation, image/program/release/resize/replacement invalidation, layer pixels/control, required and in-flight promotion, cancellation, bounded speculation/program-only dependencies, unsupported-context fallback and zero-headroom progress\n";
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
