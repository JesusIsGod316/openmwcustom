#include <array>
#include <cmath>
#include <components/resource/compositecpumips.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <osg/BlendFunc>
#include <osg/FrameBufferObject>
#include <osg/Geometry>
#include <osg/Program>
#include <osg/Shader>
#include <osg/Uniform>
#include <osgViewer/Viewer>
#include <stdexcept>
#include <vector>

namespace Mips = Resource::CompositeCpuMips;
namespace
{
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    osg::ref_ptr<osg::Image> image(int width, int height, unsigned char value = 0)
    {
        osg::ref_ptr<osg::Image> result = new osg::Image;
        result->allocateImage(width, height, 1, GL_ALPHA, GL_UNSIGNED_BYTE, 1);
        std::fill_n(result->data(), result->getTotalDataSize(), value);
        return result;
    }
    void cpuTests()
    {
        Mips::Capabilities caps{ nullptr, 0, 4096, true };
        for (const auto dimensions :
            { std::pair{ 130, 130 }, { 65, 65 }, { 34, 34 }, { 64, 64 }, { 1, 17 }, { 17, 1 }, { 3, 3 } })
        {
            for (const unsigned char value : std::array<unsigned char, 3>{ 0, 73, 255 })
            {
                auto source = image(dimensions.first, dimensions.second, value);
                const auto revision = source->getModifiedCount();
                auto prepared = Mips::prepare(source, caps, true, true);
                require(prepared.source == source && prepared.mipmapped, "qualified CPU image declined");
                require(source->getModifiedCount() == revision && !source->isMipmap(),
                    "CPU preparation changed source publication");
                int width = source->s(), height = source->t();
                unsigned total = 0;
                for (unsigned level = 0; level < prepared.mipmapped->getNumMipmapLevels(); ++level)
                {
                    require(prepared.mipmapped->getMipmapOffset(level) == total,
                        "mip offset missing base or padded wrongly");
                    const auto* data = prepared.mipmapped->getMipmapData(level);
                    require(
                        std::all_of(data, data + width * height, [=](unsigned char sample) { return sample == value; }),
                        "constant alpha was not preserved");
                    total += unsigned(width * height);
                    width = std::max(1, width / 2);
                    height = std::max(1, height / 2);
                }
                require(total == prepared.mipmapped->getTotalDataSize(), "complete payload estimate incorrect");
                if (dimensions == std::pair{ 130, 130 })
                    require(total == 22490, "130x130 NPOT chain incomplete");
                require(std::memcmp(source->data(), prepared.mipmapped->data(), source->getTotalDataSize()) == 0,
                    "base level changed");
            }
        }
        auto impulse = image(3, 3);
        impulse->data(2, 2)[0] = 255;
        auto reduced = Mips::prepare(impulse, caps, true, true);
        require(reduced.mipmapped->getMipmapData(1)[0] == 28, "odd last row/column dropped from area reduction");
        auto checker = image(4, 4);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                checker->data(x, y)[0] = (x + y) % 2 ? 255 : 0;
        auto checked = Mips::prepare(checker, caps, true, true);
        require(checked.mipmapped->getMipmapData(1)[0] == 128 && checked.mipmapped->getMipmapData(2)[0] == 128,
            "linear checkerboard reduction or rounding changed");
        auto a = image(65, 65), b = image(65, 65);
        for (unsigned i = 0; i < a->getTotalDataSize(); ++i)
        {
            a->data()[i] = static_cast<unsigned char>(i % 256);
            b->data()[i] = 255 - a->data()[i];
        }
        auto pa = Mips::prepare(a, caps, true, true), pb = Mips::prepare(b, caps, true, true);
        for (unsigned i = 0; i < pa.mipmapped->getTotalDataSize(); ++i)
            require(std::abs(int(pa.mipmapped->data()[i]) + int(pb.mipmapped->data()[i]) - 255) <= 1,
                "independent complementary weights drifted or were renormalized");
        auto original = image(130, 130, 73);
        require(!Mips::prepare(original, caps, false, true).mipmapped, "direct terrain changed");
        require(!Mips::prepare(original, caps, true, false).mipmapped, "ESM4 opacity changed");
        auto noNpot = caps;
        noNpot.npotMipSupported = false;
        require(!Mips::prepare(original, noNpot, true, true).mipmapped, "unsupported NPOT accepted");
        auto small = caps;
        small.maxDimension = 64;
        require(!Mips::prepare(original, small, true, true).mipmapped, "rescale-required image accepted");
        require(!Mips::prepare(image(1, 1), caps, true, true).mipmapped, "1x1 supplied-mip identity claimed");
        auto dynamic = image(4, 4);
        dynamic->setDataVariance(osg::Object::DYNAMIC);
        require(!Mips::prepare(dynamic, caps, true, true).mipmapped, "dynamic image accepted");
        auto rgba = image(4, 4);
        rgba->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        require(!Mips::prepare(rgba, caps, true, true).mipmapped, "arbitrary color image accepted");
        auto packed = image(3, 3);
        packed->allocateImage(3, 3, 1, GL_ALPHA, GL_UNSIGNED_BYTE, 4);
        require(!Mips::prepare(packed, caps, true, true).mipmapped, "unsupported unpack layout accepted");
        require(!Mips::prepare(image(2048, 2048), caps, true, true).mipmapped, "optional allocation bound ignored");
        const auto before = Mips::stats();
        Mips::failNextAllocation.store(true);
        require(!Mips::prepare(original, caps, true, true).mipmapped, "allocation failure published partial chain");
        require(original->data()[0] == 73 && !original->isMipmap()
                && Mips::stats().allocationFailures == before.allocationFailures + 1,
            "allocation decline damaged original or lost failure count");
        std::cout << "PASS CPU: complete/odd/thin/POT chains, exact base, linear weights, source purity, unsupported "
                     "and allocation fallback\n";
    }

    unsigned generationCalls = 0;
    decltype(osg::GLExtensions::glGenerateMipmap) originalGenerate = nullptr;
    void GL_APIENTRY countedGenerate(GLenum target)
    {
        ++generationCalls;
        originalGenerate(target);
    }
    osg::ref_ptr<osg::GraphicsContext> context()
    {
        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        traits->readDISPLAY();
        traits->setUndefinedScreenDetailsToDefaultScreen();
        traits->width = 128;
        traits->height = 128;
        traits->windowDecoration = false;
        traits->doubleBuffer = false;
        auto result = osg::GraphicsContext::createGraphicsContext(traits);
        require(result && result->realize() && result->makeCurrent(), "native GL context unavailable");
        return result;
    }
    void configure(osg::Texture2D& texture)
    {
        texture.setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture.setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture.setResizeNonPowerOfTwoHint(false);
        texture.setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR);
        texture.setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
    }
    void verifyLevels(osg::Image& expected)
    {
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        int width = expected.s(), height = expected.t();
        for (unsigned level = 0; level < expected.getNumMipmapLevels(); ++level)
        {
            GLint actualWidth = 0, actualHeight = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, GLint(level), GL_TEXTURE_WIDTH, &actualWidth);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, GLint(level), GL_TEXTURE_HEIGHT, &actualHeight);
            require(actualWidth == width && actualHeight == height, "installed OSG mip dimensions incomplete");
            std::vector<unsigned char> bytes(std::size_t(width) * height);
            glGetTexImage(GL_TEXTURE_2D, GLint(level), GL_ALPHA, GL_UNSIGNED_BYTE, bytes.data());
            require(std::equal(bytes.begin(), bytes.end(), expected.getMipmapData(level)),
                "installed OSG supplied alpha payload changed");
            width = std::max(1, width / 2);
            height = std::max(1, height / 2);
        }
        require(glGetError() == GL_NO_ERROR, "GL error reading supplied mip chain");
    }
    std::string shader(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        require(bool(file), "real terrain shader unavailable");
        std::string result((std::istreambuf_iterator<char>(file)), {});
        // These two exact compatibility shaders have no includes and only the
        // blendMap template define. Replace every occurrence as ShaderManager
        // does; a remaining template/include is a hard fixture failure.
        for (auto marker = result.find("@blendMap"); marker != std::string::npos; marker = result.find("@blendMap"))
            result.replace(marker, 9, "1");
        require(result.find('@') == std::string::npos && result.find("#include") == std::string::npos,
            "unprocessed real terrain shader template/include");
        return result;
    }
    std::vector<unsigned char> render(osg::State& state, const std::filesystem::path& shaders,
        const std::array<osg::ref_ptr<osg::Texture2D>, 3>& masks, int size)
    {
        osg::ref_ptr<osg::Texture2D> target = new osg::Texture2D;
        target->setTextureSize(size, size);
        target->setInternalFormat(GL_RGBA8);
        target->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        target->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        osg::ref_ptr<osg::FrameBufferObject> fbo = new osg::FrameBufferObject;
        fbo->setAttachment(osg::Camera::COLOR_BUFFER, osg::FrameBufferAttachment(target));
        fbo->apply(state);
        state.haveAppliedTextureAttribute(state.getActiveTextureUnit(), osg::StateAttribute::TEXTURE);
        require(
            state.get<osg::GLExtensions>()->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT,
            "composite FBO incomplete");
        glViewport(0, 0, size, size);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        state.applyModelViewMatrix(new osg::RefMatrix(osg::Matrix::identity()));
        state.applyProjectionMatrix(new osg::RefMatrix(osg::Matrix::identity()));
        osg::ref_ptr<osg::Program> program = new osg::Program;
        program->addShader(new osg::Shader(osg::Shader::VERTEX, shader(shaders / "terrain_composite.vert")));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT, shader(shaders / "terrain_composite.frag")));
        osg::ref_ptr<osg::StateSet> caller = new osg::StateSet;
        caller->setAttributeAndModes(new osg::Program, osg::StateAttribute::ON);
        caller->setMode(GL_BLEND, osg::StateAttribute::OFF);
        caller->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        caller->setMode(GL_CULL_FACE, osg::StateAttribute::OFF);
        // Keep all source textures, uniforms and client arrays alive through
        // caller/cache retirement, even if the allocator reuses addresses.
        std::array<osg::ref_ptr<osg::Geometry>, 3> quads;
        state.pushStateSet(caller);
        state.apply();
        osg::RenderInfo info(&state, nullptr);
        for (unsigned layer = 0; layer < 3; ++layer)
        {
            osg::ref_ptr<osg::Image> color = new osg::Image;
            color->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            std::fill_n(color->data(), 4, 0);
            color->data()[layer] = 255;
            color->data()[3] = 255;
            osg::ref_ptr<osg::Texture2D> diffuse = new osg::Texture2D(color);
            diffuse->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            diffuse->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            // A constant diffuse oracle must not fade at GL_CLAMP's black
            // border when the single texel is magnified over the whole quad.
            diffuse->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
            diffuse->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
            osg::ref_ptr<osg::Geometry> quad
                = osg::createTexturedQuadGeometry(osg::Vec3(-1, -1, 0), osg::Vec3(2, 0, 0), osg::Vec3(0, 2, 0));
            quad->setUseDisplayList(false);
            quad->setUseVertexBufferObjects(false);
            quads[layer] = quad;
            auto* pass = quad->getOrCreateStateSet();
            pass->setAttributeAndModes(program, osg::StateAttribute::ON);
            pass->setTextureAttribute(0, diffuse);
            pass->setTextureAttribute(1, masks[layer]);
            pass->addUniform(new osg::Uniform("diffuseMap", 0));
            pass->addUniform(new osg::Uniform("blendMap", 1));
            pass->addUniform(new osg::Uniform("texMat0", osg::Matrixf::identity()));
            osg::Matrixf matrix;
            matrix.preMultTranslate(osg::Vec3f(.5f, .5f, 0.f));
            matrix.preMultScale(osg::Vec3f(64.f / 65.f, 64.f / 65.f, 1.f));
            matrix.preMultTranslate(osg::Vec3f(-.5f, -.5f, 0.f));
            matrix.preMultTranslate(osg::Vec3f(1.f / 64.f / 4.f, -1.f / 64.f / 4.f, 0));
            pass->addUniform(new osg::Uniform("texMat1", matrix));
            pass->setMode(GL_BLEND, osg::StateAttribute::ON);
            pass->setAttributeAndModes(new osg::BlendFunc(GL_SRC_ALPHA, layer == 0 ? GL_ZERO : GL_ONE));
            state.apply(pass);
            const auto* compiled = program->getPCP(state);
            require(compiled && compiled->isLinked(), "real terrain shader program failed to link");
            require(state.getLastAppliedProgramObject() == compiled, "real terrain program was not bound");
            auto* ext = state.get<osg::GLExtensions>();
            GLint sampler0 = -1, sampler1 = -1;
            ext->glGetUniformiv(
                compiled->getHandle(), ext->glGetUniformLocation(compiled->getHandle(), "diffuseMap"), &sampler0);
            ext->glGetUniformiv(
                compiled->getHandle(), ext->glGetUniformLocation(compiled->getHandle(), "blendMap"), &sampler1);
            require(sampler0 == 0 && sampler1 == 1, "real shader sampler uniforms differ from production units");
            GLint src = 0, dst = 0;
            glGetIntegerv(GL_BLEND_SRC, &src);
            glGetIntegerv(GL_BLEND_DST, &dst);
            require(glIsEnabled(GL_BLEND) && src == GL_SRC_ALPHA && dst == (layer == 0 ? GL_ZERO : GL_ONE),
                "real shader additive blend state differs from production");
            quad->draw(info);
            // OSG keeps raw last-applied Program/uniform/client-array pointers.
            // Restore the caller while every source owner is still alive.
            state.apply();
            state.disableAllVertexArrays();
            if (const auto* owned = dynamic_cast<const Resource::PreparedTerrainTexture*>(masks[layer].get()))
                owned->recordProducerDrawUse(state, layer != 1);
        }
        std::vector<unsigned char> pixels(std::size_t(size) * size * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        state.get<osg::GLExtensions>()->glBindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
        state.popStateSet();
        state.apply();
        state.disableAllVertexArrays();
        for (unsigned unit : { 0u, 1u })
            state.haveAppliedTextureAttribute(unit, osg::StateAttribute::TEXTURE);
        require(glGetError() == GL_NO_ERROR, "GL error in real composite shader fixture");
        return pixels;
    }
    void glTests(const std::filesystem::path& shaders)
    {
        osgViewer::Viewer anchor;
        auto gc = context();
        auto& state = *gc->getState();
        Mips::publishCapabilities(*gc);
        const auto caps = Mips::startupCapabilities();
        require(caps.npotMipSupported && caps.maxDimension >= 130, "current GL cannot qualify candidate");
        auto* extensions = state.get<osg::GLExtensions>();
        originalGenerate = extensions->glGenerateMipmap;
        require(originalGenerate != nullptr, "native mip generation unavailable");
        extensions->glGenerateMipmap = &countedGenerate;
        auto source = image(130, 130, 73);
        auto prepared = Mips::prepare(source, caps, true, true);
        osg::ref_ptr<Mips::Texture> texture = new Mips::Texture(prepared);
        configure(*texture);
        const auto beforeApply = Mips::stats();
        state.setActiveTextureUnit(1);
        state.applyTextureAttribute(1, texture);
        require(generationCalls == 0, "eligible supplied chain invoked generation");
        verifyLevels(*prepared.mipmapped);
        require(texture->preparationAdmissionEligible(state), "eligible supplied image declined optional admission");
        require(Mips::stats().suppliedApplies == beforeApply.suppliedApplies + 1
                && Mips::stats().uploadSubmissions == beforeApply.uploadSubmissions + 1
                && Mips::stats().firstProducerUses == beforeApply.firstProducerUses,
            "accepted upload missing, or apply/precompile falsely counted as first producer use");
        require(Resource::retainedTerrainTextureBytes(*texture) == 39390,
            "original+supplied source ownership was undercharged");
        texture->rememberPreparation(state);
        require(texture->preparationUnchanged(state), "eligible supplied preparation not recognized");
        texture->dirtyTextureObject();
        state.haveAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE);
        state.applyTextureAttribute(1, texture);
        verifyLevels(*prepared.mipmapped);
        require(generationCalls == 0, "reused supplied upload invoked generation");

        // Quality bounds were fixed before execution: max8/255, RGB RMS2/255.
        // They are an offscreen guard, not universal GPU bit identity or in-game acceptance.
        std::array<osg::ref_ptr<osg::Texture2D>, 3> control, candidate;
        for (unsigned layer = 0; layer < 3; ++layer)
        {
            auto mask = image(130, 130);
            for (int y = 0; y < 130; ++y)
                for (int x = 0; x < 130; ++x)
                {
                    const int sx = x / 2, sy = y / 2;
                    const unsigned owner = unsigned((sx / 9 + sy / 13) % 3);
                    mask->data(x, y)[0] = owner == layer ? 255 : 0;
                }
            control[layer] = new Resource::PreparedTerrainTexture(mask);
            configure(*control[layer]);
            candidate[layer] = new Mips::Texture(Mips::prepare(mask, caps, true, true));
            configure(*candidate[layer]);
        }
        const auto beforeProducer = Mips::stats();
        for (int size : { 128, 64, 32, 16 })
        {
            const auto a = render(state, shaders, control, size);
            require(a == render(state, shaders, control, size),
                "control composite oracle varied with cache/lifetime reuse");
            const auto b = render(state, shaders, candidate, size);
            unsigned maximum = 0;
            double sum = 0;
            std::size_t count = 0;
            for (std::size_t i = 0; i < a.size(); ++i)
                if (i % 4 != 3)
                {
                    const auto delta = std::abs(int(a[i]) - int(b[i]));
                    maximum = std::max(maximum, unsigned(delta));
                    sum += delta * delta;
                    ++count;
                }
            const double rms = std::sqrt(sum / double(count));
            std::cout << "composite size=" << size << " max=" << maximum << " RGB_RMS=" << rms << " bound=8/2\n";
            require(
                maximum <= 8 && rms <= 2., "real composite NPOT seam/coverage difference exceeded predeclared bound");
            for (std::size_t pixel = 0; pixel < a.size(); pixel += 4)
            {
                const unsigned controlCoverage = unsigned(a[pixel]) + a[pixel + 1] + a[pixel + 2];
                const unsigned candidateCoverage = unsigned(b[pixel]) + b[pixel + 1] + b[pixel + 2];
                require(controlCoverage >= 252 && controlCoverage <= 258 && candidateCoverage >= 252
                        && candidateCoverage <= 258,
                    "real shader masks lost alpha coverage or bypassed layered blending at an edge/pixel");
            }
        }
        const auto afterProducer = Mips::stats();
        require(afterProducer.firstProducerUses == beforeProducer.firstProducerUses + 3
                && afterProducer.requiredFirstUses == beforeProducer.requiredFirstUses + 2
                && afterProducer.optionalFirstUses == beforeProducer.optionalFirstUses + 1
                && afterProducer.producerDrawUses == beforeProducer.producerDrawUses + 12,
            "real producer draws did not distinguish repeated use from wrapper/context first use");
        state.setActiveTextureUnit(1);
        const auto beforeRelease = generationCalls;
        texture->releaseGLObjects(&state);
        state.haveAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE);
        require(!texture->preparationAdmissionEligible(state), "released supplied image admitted optional retry");
        state.applyTextureAttribute(1, texture);
        require(generationCalls > beforeRelease && !texture->preparationUnchanged(state),
            "context retirement did not use original-image control");
        require(state.getLastAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE) == texture->controlTexture(),
            "fallback left misleading cached texture owner");
        GLint bound = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        require(unsigned(bound) == texture->controlTexture()->getTextureObject(state.getContextID())->id(),
            "fallback binding disagrees with actual owner");
        const auto beforeFallbackDraw = Mips::stats();
        require(beforeFallbackDraw.controlFirstUses == beforeApply.controlFirstUses,
            "fallback apply/precompile falsely counted as producer use");
        const std::array<osg::ref_ptr<osg::Texture2D>, 3> fallback{ texture, control[1], control[2] };
        render(state, shaders, fallback, 32);
        render(state, shaders, fallback, 32);
        require(Mips::stats().controlProducerUses == beforeFallbackDraw.controlProducerUses + 2
                && Mips::stats().controlFirstUses == beforeFallbackDraw.controlFirstUses + 1
                && Mips::stats().firstProducerUses == beforeFallbackDraw.firstProducerUses,
            "fallback producer use claimed supplied chain or duplicated first use");
        osg::ref_ptr<Mips::Texture> retiredClone = new Mips::Texture(*texture);
        retiredClone->apply(state);
        require(!retiredClone->preparationUnchanged(state), "clone revived retired context proof");
        osg::ref_ptr<Mips::Texture> generationRequest = new Mips::Texture(prepared);
        configure(*generationRequest);
        generationRequest->allocateMipmapLevels();
        const auto beforeRequest = generationCalls;
        require(generationRequest->preparationAdmissionEligible(state)
                && !generationRequest->preparationDispatchEligible(state) && generationCalls == beforeRequest,
            "late mip generation request did not decline draw dispatch before control GL");
        generationRequest->apply(state);
        require(generationCalls > beforeRequest && !generationRequest->preparationUnchanged(state),
            "explicit generation dirtiness bypassed original control");
        require(!generationRequest->preparationAdmissionEligible(state),
            "generation control fallback admitted optional retry");
        osg::ref_ptr<Mips::Texture> changed = new Mips::Texture(prepared);
        configure(*changed);
        prepared.mipmapped->data()[0] = 199;
        prepared.mipmapped->dirty();
        const auto beforeDirtyUse = Mips::stats();
        changed->recordProducerDrawUse(state, true);
        require(Mips::stats().unprovenProducerUses == beforeDirtyUse.unprovenProducerUses + 1,
            "dirty/unapplied owner created a producer-use proof");
        changed->apply(state);
        require(!changed->preparationAdmissionEligible(state), "dirty supplied image admitted optional retry");
        std::vector<unsigned char> changedBase(16900);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_ALPHA, GL_UNSIGNED_BYTE, changedBase.data());
        require(
            changedBase[0] == 199 && !changed->preparationUnchanged(state), "dirty base revision used stale mip data");

        // The retained original can change without changing the outer supplied
        // Image identity/revision that a queued dependency descriptor records.
        // Reject its pure optional admission before any fallback GL is invoked.
        auto dirtyOriginal = image(130, 130, 73);
        auto originalOnly = Mips::prepare(dirtyOriginal, caps, true, true);
        osg::ref_ptr<Mips::Texture> originalChanged = new Mips::Texture(originalOnly);
        configure(*originalChanged);
        state.haveAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE);
        const auto beforeOriginalUpload = generationCalls;
        state.applyTextureAttribute(1, originalChanged);
        originalChanged->rememberPreparation(state);
        require(originalChanged->preparationAdmissionEligible(state) && originalChanged->preparationUnchanged(state)
                && generationCalls == beforeOriginalUpload,
            "original-only dirty fixture was not initially admitted/prepared without generation");
        const auto suppliedRevision = originalOnly.mipmapped->getModifiedCount();
        const auto originalRevision = dirtyOriginal->getModifiedCount();
        for (unsigned i = 0; i < dirtyOriginal->getTotalDataSize(); ++i)
            dirtyOriginal->data()[i] = static_cast<unsigned char>((i * 17u + 41u) % 256u);
        dirtyOriginal->dirty();
        require(dirtyOriginal->getModifiedCount() != originalRevision
                && originalOnly.mipmapped->getModifiedCount() == suppliedRevision
                && originalChanged->getImage() == originalOnly.mipmapped.get(),
            "original-only mutation changed the queued supplied Image identity/revision");
        require(!originalChanged->preparationAdmissionEligible(state) && !originalChanged->preparationUnchanged(state),
            "original-only revision change admitted speculative fallback or reused a false memo");
        state.haveAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE);
        state.applyTextureAttribute(1, originalChanged); // exact required/demand fallback
        std::vector<unsigned char> originalChangedBase(dirtyOriginal->getTotalDataSize());
        glGetTexImage(GL_TEXTURE_2D, 0, GL_ALPHA, GL_UNSIGNED_BYTE, originalChangedBase.data());
        require(std::equal(originalChangedBase.begin(), originalChangedBase.end(), dirtyOriginal->data()),
            "original-only mutation fallback pixels differ from current original base");
        const auto* exactControl = originalChanged->controlTexture();
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
        require(exactControl && exactControl->getImage() == dirtyOriginal.get()
                && state.getLastAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE) == exactControl
                && exactControl->getTextureObject(state.getContextID())
                && unsigned(bound) == exactControl->getTextureObject(state.getContextID())->id(),
            "original-only mutation fallback cache/binding/source identity differs from exact control");
        originalChanged->rememberPreparation(state);
        require(!originalChanged->preparationUnchanged(state) && !originalChanged->preparationAdmissionEligible(state)
                && generationCalls > beforeOriginalUpload
                && originalOnly.mipmapped->getModifiedCount() == suppliedRevision,
            "original-only control fallback created a false supplied preparation/generation claim");
        std::cout << "PASS original-only revision: unchanged supplied Image rejects admission; exact current "
                     "base/cache/binding; no false memo\n";
        auto other = context();
        auto& otherState = *other->getState();
        otherState.setActiveTextureUnit(1);
        osg::ref_ptr<Mips::Texture> replacement
            = new Mips::Texture(Mips::prepare(image(130, 130, 73), caps, true, true));
        configure(*replacement);
        require(!replacement->preparationAdmissionEligible(otherState),
            "replacement context admitted optional supplied image");
        replacement->apply(otherState);
        require(!replacement->preparationUnchanged(otherState), "replacement context reused startup proof");
        require(
            otherState.getLastAppliedTextureAttribute(1, osg::StateAttribute::TEXTURE) == replacement->controlTexture(),
            "replacement did not preserve original control cache identity");
        replacement->releaseGLObjects(&otherState);
        other->releaseContext();
        other->close(true);
        gc->makeCurrent();
        texture->releaseGLObjects(&state);
        retiredClone->releaseGLObjects(&state);
        generationRequest->releaseGLObjects(&state);
        changed->releaseGLObjects(&state);
        originalChanged->releaseGLObjects(&state);
        for (auto& object : control)
            object->releaseGLObjects(&state);
        for (auto& object : candidate)
            object->releaseGLObjects(&state);
        require(glGetError() == GL_NO_ERROR, "GL error in retirement/replacement/dirtied fallbacks");
        extensions->glGenerateMipmap = originalGenerate;
        gc->releaseContext();
        gc->close(true);
        std::cout << "PASS GL: complete installed uploads, generation suppression, real shader quality, charged "
                     "originals, exact fallback cache/binding and retired/replaced contexts\n";
    }
}
int main(int argc, char** argv)
try
{
    if (argc > 1 && std::string_view(argv[1]) == "--gl")
    {
        require(argc == 3, "--gl requires real shader directory");
        glTests(argv[2]);
    }
    else
        cpuTests();
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
