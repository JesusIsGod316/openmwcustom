#include "diagnostic-gl-import.hpp"
#include <osg/Geometry>
#include <osg/Image>
#include <osg/GraphicsContext>
#include <osg/Program>
#include <osg/RenderInfo>
#include <osg/Shader>
#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Version>
#include <osg/observer_ptr>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

// Actual GL/OSG objects, controlled synthetic producer content. No game frame,
// asynchronous producer lease, native SDK, reconstruction output or evaluation.
namespace P9Dlss
{
    using namespace RenderCore::Temporal;
    unsigned checks = 0;
    const char* phase = "startup";
    void check(bool value, const char* message) { ++checks; require(value, message); }
    std::string jsonString(const std::string& value)
    {
        std::string result = "\"";
        for (const unsigned char byte : value)
        {
            check(byte >= 32, "Runtime evidence contains no control bytes");
            if (byte == '\\' || byte == '"') result += '\\';
            result += char(byte);
        }
        return result + '"';
    }
    std::string loadedModule(const wchar_t* name)
    {
        const HMODULE module = GetModuleHandleW(name); check(module != nullptr, "Actual linked runtime module loaded");
        std::array<wchar_t, 32768> path{};
        const DWORD count = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
        check(count > 0 && count < path.size(), "Observe complete actual runtime path");
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), int(count), nullptr, 0, nullptr, nullptr);
        check(bytes > 0, "Actual runtime path UTF8 length"); std::string result(bytes, '\0');
        check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(), int(count), result.data(), bytes, nullptr, nullptr)
            == bytes, "Actual runtime path UTF8 conversion"); return result;
    }
    template<class Value>
    std::vector<Value> pixels(const Texture& image, GLenum format, GLenum type, unsigned channels)
    {
        BorrowedGlState restore; restore.active(BorrowedGlState::Texture0);
        glBindTexture(GL_TEXTURE_2D, image.gl);
        std::vector<Value> result(std::size_t(image.width) * image.height * channels);
        glGetTexImage(GL_TEXTURE_2D, 0, format, type, result.data());
        glCheck("Read native diagnostic proof pixels"); return result;
    }
    struct Cache
    {
        const osg::Program::PerContextProgram* program;
        const osg::StateAttribute* texture;
        const osg::VertexArrayState* arrays;
        unsigned active, clientActive, stack;
        GLuint vao;
        bool blend, depth;
        explicit Cache(osg::State& state)
            : program(state.getLastAppliedProgramObject()),
              texture(state.getLastAppliedTextureAttribute(0, osg::StateAttribute::TEXTURE)),
              arrays(state.getCurrentVertexArrayState()), active(state.getActiveTextureUnit()),
              clientActive(state.getClientActiveTextureUnit()), stack(state.getStateSetStackSize()),
              vao(state.getCurrentVertexArrayObject()), blend(state.getLastAppliedModeValue(GL_BLEND)),
              depth(state.getLastAppliedModeValue(GL_DEPTH_TEST)) {}
        bool same(const Cache& other) const
        {
            return program == other.program && texture == other.texture && arrays == other.arrays
                && active == other.active && clientActive == other.clientActive && stack == other.stack
                && vao == other.vao && blend == other.blend && depth == other.depth;
        }
    };
    struct ObjectAudit
    {
        struct Event { DiagnosticGlObject type; GLuint name; bool created; };
        std::array<Event, 128> events{};
        unsigned count = 0;
        bool overflow = false;
        DiagnosticGlAudit callback()
        {
            return {this, [](void* owner, DiagnosticGlObject type, GLuint name, bool created) noexcept
            {
                auto& audit = *static_cast<ObjectAudit*>(owner);
                if (audit.count == audit.events.size()) { audit.overflow = true; return; }
                audit.events[audit.count++] = {type, name, created};
            }};
        }
        void allRetired(bool texturesOnly = false)
        {
            check(!overflow, "Private object observer has complete bounded coverage");
            for (unsigned i = 0; i < count; ++i) if (events[i].created
                && (!texturesOnly || events[i].type == DiagnosticGlObject::Texture))
            {
                int balance = 0;
                for (unsigned j = 0; j < count; ++j)
                    if (events[j].type == events[i].type && events[j].name == events[i].name)
                        balance += events[j].created ? 1 : -1;
                check(balance == 0, "Every observed private name was retired, including partial allocation");
                GLboolean alive = GL_FALSE;
                switch (events[i].type)
                {
                    case DiagnosticGlObject::Texture: alive = glIsTexture(events[i].name); break;
                    case DiagnosticGlObject::Framebuffer:
                        alive = function<GLboolean (APIENTRY*)(GLuint)>("glIsFramebuffer")(events[i].name); break;
                    case DiagnosticGlObject::Sampler:
                        alive = function<GLboolean (APIENTRY*)(GLuint)>("glIsSampler")(events[i].name); break;
                    case DiagnosticGlObject::Program:
                        alive = function<GLboolean (APIENTRY*)(GLuint)>("glIsProgram")(events[i].name); break;
                    case DiagnosticGlObject::Shader:
                        alive = function<GLboolean (APIENTRY*)(GLuint)>("glIsShader")(events[i].name); break;
                }
                check(!alive, "Retired private name is absent from the actual owner namespace");
            }
            glCheck("Inspect actual private name retirement");
        }
    };
    // A genuinely different hidden window/context, with no sound or display
    // switching. The root runner still assigns the unswitched private desktop.
    struct ReplacementContext
    {
        HWND window = nullptr;
        HDC dc = nullptr;
        HGLRC native = nullptr;
        ReplacementContext()
        {
            WNDCLASSW cls{}; cls.style = CS_OWNDC; cls.lpfnWndProc = DefWindowProcW;
            cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"OptimizedMW-Diagnostic-RecreatedContext";
            check(RegisterClassW(&cls) != 0, "Register second private native namespace window");
            window = CreateWindowW(cls.lpszClassName, L"Diagnostic recreated context", WS_POPUP,
                0, 0, 32, 32, nullptr, nullptr, cls.hInstance, nullptr);
            check(window && !IsWindowVisible(window), "Recreated context remains hidden");
            dc = GetDC(window); check(dc != nullptr, "Recreated context DC");
            PIXELFORMATDESCRIPTOR pfd{}; pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
            pfd.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW; pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
            const int pixelFormat = ChoosePixelFormat(dc, &pfd);
            check(pixelFormat && SetPixelFormat(dc, pixelFormat, &pfd), "Recreated context pixel format");
            native = wglCreateContext(dc); check(native != nullptr, "Create genuinely new native context");
        }
        void makeCurrent() { check(wglMakeCurrent(dc, native), "Current recreated native namespace"); }
        void close()
        {
            check(wglMakeCurrent(nullptr, nullptr), "Release recreated context");
            check(wglDeleteContext(native), "Destroy recreated context"); native = nullptr;
            ReleaseDC(window, dc); dc = nullptr;
            check(DestroyWindow(window), "Destroy recreated hidden window"); window = nullptr;
        }
    };
    struct Fixture
    {
        HiddenContext context;
        GpuTransfers gpu;
        osg::ref_ptr<osg::State> state = new osg::State;
        osg::ref_ptr<osg::Geometry> ordinary = new osg::Geometry;
        osg::ref_ptr<osg::Program> program = new osg::Program;
        osg::ref_ptr<osg::Texture2D> ordinaryTexture;
        osg::ref_ptr<osg::StateSet> ordinaryState = new osg::StateSet;
        DiagnosticTextureOwners sources;
        std::array<Texture, 3> names;
        Texture target;
        GLuint targetFbo = 0, sourceFbo = 0;
        unsigned contextId = osg::GraphicsContext::createNewContextID(), ordinaryDraws = 0;
        std::uint64_t sequence = 0, generation = 0;
        DiagnosticInputBorrowOwner owner;
        DiagnosticContextLifetime lifetime{contextId, 1};
        DiagnosticGlMirrors mirrors{lifetime};
        void (APIENTRY* clearDepthStencil)(GLenum, GLint, GLfloat, GLint)
            = function<decltype(clearDepthStencil)>("glClearBufferfi");

        Fixture()
        {
            check(std::string(osgGetVersion()) == "3.6.5", "Pinned OSG runtime");
            check(!IsWindowVisible(context.window), "Native fixture window is hidden");
            gpu.initialize();
            state->setContextID(contextId); state->initializeExtensionProcs();
            program->addShader(new osg::Shader(osg::Shader::VERTEX,
                "#version 130\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;}\n"));
            program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 130\nuniform sampler2D image;void main(){gl_FragColor=texture2D(image,gl_TexCoord[0].xy);}\n"));
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
            const std::array<unsigned char, 16> colors{255, 0, 0, 255, 0, 255, 0, 255,
                0, 0, 255, 255, 255, 255, 0, 255};
            std::copy(colors.begin(), colors.end(), image->data());
            ordinaryTexture = new osg::Texture2D(image);
            ordinaryTexture->setInternalFormat(GL_RGBA8);
            ordinaryTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            ordinaryTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            ordinaryTexture->setResizeNonPowerOfTwoHint(false);
            ordinaryState->setAttributeAndModes(program);
            ordinaryState->setTextureAttributeAndModes(0, ordinaryTexture);
            ordinaryState->addUniform(new osg::Uniform("image", 0));
            for (const auto capability : BorrowedGlState::capabilities)
                ordinaryState->setMode(capability, osg::StateAttribute::OFF);
            ordinary->setUseDisplayList(false); ordinary->setUseVertexBufferObjects(true);
            ordinary->setUseVertexArrayObject(false);
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            vertices->push_back({-1, -1, 0}); vertices->push_back({3, -1, 0}); vertices->push_back({-1, 3, 0});
            osg::ref_ptr<osg::Vec2Array> coords = new osg::Vec2Array;
            coords->push_back({0, 0}); coords->push_back({2, 0}); coords->push_back({0, 2});
            ordinary->setVertexArray(vertices); ordinary->setTexCoordArray(0, coords);
            ordinary->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
            target.width = 64; target.height = 48;
            glGenTextures(1, &target.gl); glBindTexture(GL_TEXTURE_2D, target.gl);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 48, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            gpu.gen(1, &targetFbo); gpu.gen(1, &sourceFbo);
            gpu.bind(GpuTransfers::Framebuffer, targetFbo);
            gpu.attach(GpuTransfers::Framebuffer, GpuTransfers::ColorAttachment, GL_TEXTURE_2D, target.gl, 0);
            glDrawBuffer(GpuTransfers::ColorAttachment); glReadBuffer(GpuTransfers::ColorAttachment);
            check(gpu.status(GpuTransfers::Framebuffer) == GpuTransfers::Complete, "Ordinary draw target complete");
            state->pushStateSet(ordinaryState);
        }
        std::vector<unsigned char> ordinaryPixels()
        {
            gpu.bind(GpuTransfers::Framebuffer, targetFbo); glViewport(0, 0, 64, 48);
            gpu.colorMaskIndexed(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            state->apply(); osg::RenderInfo info(state, nullptr);
            ordinary->osg::Geometry::drawImplementation(info); ++ordinaryDraws;
            auto result = pixels<unsigned char>(target, GL_RGBA, GL_UNSIGNED_BYTE, 4);
            const std::array<std::array<unsigned char, 4>, 4> expected{{{255, 0, 0, 255},
                {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}}};
            for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x)
                for (unsigned c = 0; c < 4; ++c)
                    check(result[(std::size_t(y) * 64 + x) * 4 + c] == expected[(x >= 32) + 2 * (y >= 24)][c],
                        "Following ordinary Geometry exact known pixels");
            return result;
        }
        DiagnosticSources make(unsigned width, unsigned height, DiagnosticFormat depth, bool reverse, bool floatColor)
        {
            check(!sources[0], "Source generation requires previous owners retired");
            ++generation; ++sequence;
            DiagnosticSources result;
            // Independent fixture submission/ticket/frame identities; none is
            // an actual TemporalMotion/game history publication.
            result.identity = {sequence * 3 + 5, sequence * 7 + 11, sequence, 7,
                reinterpret_cast<std::uintptr_t>(wglGetCurrentContext()), 1, generation, generation, contextId};
            result.colorStage = DiagnosticColorStage::SceneBeforePostFx;
            result.coverage = DiagnosticMotionCoverage::PartialOpaque;
            result.motionSpace = DiagnosticMotionSpace::CurrentToPreviousRenderPixels;
            result.depthRange = reverse ? DiagnosticDepthRange::ZeroToOne : DiagnosticDepthRange::NegativeOneToOne;
            result.depthInverted = reverse; result.producerSubmitted = true;
            const std::array<DiagnosticFormat, 3> formats{
                floatColor ? DiagnosticFormat::Rgba16Float : DiagnosticFormat::Rgba8, depth, DiagnosticFormat::Rg16Float};
            const std::array<GLenum, 3> internal{floatColor ? 0x881Au : GLenum(GL_RGBA8),
                depth == DiagnosticFormat::Depth24Stencil8 ? 0x88F0u
                    : depth == DiagnosticFormat::Depth32FloatStencil8 ? 0x8CADu : 0x8CACu, 0x822F};
            const bool packed = depth != DiagnosticFormat::Depth32Float;
            const std::array<GLenum, 3> external{GL_RGBA, packed ? 0x84F9u : GLenum(GL_DEPTH_COMPONENT), 0x8227};
            const std::array<GLenum, 3> types{GL_FLOAT, packed
                ? (depth == DiagnosticFormat::Depth24Stencil8 ? 0x84FAu : 0x8DADu) : GLenum(GL_FLOAT), GL_FLOAT};
            BorrowedGlState restore; restore.active(BorrowedGlState::Texture0);
            for (unsigned lane = 0; lane < sources.size(); ++lane)
            {
                auto& texture = sources[lane]; texture = new osg::Texture2D;
                texture->setTextureSize(width, height); texture->setInternalFormat(internal[lane]);
                texture->setSourceFormat(external[lane]); texture->setSourceType(types[lane]);
                texture->setResizeNonPowerOfTwoHint(false);
                texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
                texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
                texture->apply(*state);
                const auto* object = texture->getTextureObject(contextId);
                check(object && object->id(), "Actual source Texture2D has a realized per-context object");
                names[lane].gl = object->id(); names[lane].width = width; names[lane].height = height;
                result.images[lane] = {names[lane].gl, generation, sequence, 7,
                    result.identity.nativeContext, 1, width, height, formats[lane]};
                gpu.bind(GpuTransfers::Framebuffer, sourceFbo);
                gpu.attach(GpuTransfers::Framebuffer, GpuTransfers::ColorAttachment, GL_TEXTURE_2D, 0, 0);
                gpu.attach(GpuTransfers::Framebuffer, SourceTextures::DepthAttachment, GL_TEXTURE_2D, 0, 0);
                gpu.attach(GpuTransfers::Framebuffer, SourceTextures::DepthStencilAttachment, GL_TEXTURE_2D, 0, 0);
                gpu.attach(GpuTransfers::Framebuffer, lane == 1 ? (packed ? SourceTextures::DepthStencilAttachment
                    : SourceTextures::DepthAttachment) : GpuTransfers::ColorAttachment, GL_TEXTURE_2D, names[lane].gl, 0);
                glDrawBuffer(lane == 1 ? GL_NONE : GpuTransfers::ColorAttachment);
                glReadBuffer(lane == 1 ? GL_NONE : GpuTransfers::ColorAttachment);
                check(gpu.status(GpuTransfers::Framebuffer) == GpuTransfers::Complete, "Actual producer attachment complete");
                glViewport(0, 0, width, height); glDepthMask(GL_TRUE);
                glDisable(GL_DITHER); glDisable(0x8DB9); // Controlled linear producer, independent of inherited raster policy.
                gpu.colorMaskIndexed(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glEnable(GL_SCISSOR_TEST);
                for (unsigned q = 0; q < 4; ++q)
                {
                    glScissor(q & 1 ? width / 2 : 0, q & 2 ? height / 2 : 0,
                        q & 1 ? width - width / 2 : width / 2, q & 2 ? height - height / 2 : height / 2);
                    const float z = reverse ? 1.f - SourceTextures::depth(q) : SourceTextures::depth(q);
                    // Byte-aligned RGBA8 inputs avoid an independent oracle
                    // assuming one rounding choice at the 127.5 conversion tie.
                    const std::array<float, 4> value = lane == 0 ? (floatColor
                        ? std::array<float, 4>{q / 4.f, .5f, .75f, 1.f}
                        : std::array<float, 4>{float(q * 37 + 19) / 255, 83.f / 255, 167.f / 255, 1.f})
                        : lane == 1 ? std::array<float, 4>{z, 0, 0, 1} : std::array<float, 4>{-2.f, 1.f, 0, 1};
                    if (lane == 1 && packed) clearDepthStencil(0x84F9, 0, z, q + 17);
                    else gpu.clear(lane == 1 ? 0x1801 : GpuTransfers::Color, 0, value.data());
                }
                glDisable(GL_SCISSOR_TEST);
            }
            // Source sampling parameters are owned by the producer and must
            // remain unchanged. The importer's separate sampler overrides this.
            glBindTexture(GL_TEXTURE_2D, names[1].gl); glTexParameteri(GL_TEXTURE_2D, 0x884C, 0x884E);
            glCheck("Produce controlled actual source texture objects");
            check(owner.publish(result) == DiagnosticBorrowStatus::Observed, "Publish exact current diagnostic sources");
            return result;
        }
        DiagnosticImportStatus import(const DiagnosticSources& description, const DiagnosticTextureOwners& selected)
        {
            DiagnosticImportStatus result = DiagnosticImportStatus::MissingBorrow;
            check(owner.withBorrow({description.identity, description.images[0].width, description.images[0].height},
                [&](const DiagnosticInputBorrow& borrow)
                {
                    check(owner.borrowed() && borrow.sources(), "Borrow is live only on its synchronous callback");
                    check(!owner.retire(), "Producer cannot retire while exact draw inputs are borrowed");
                    result = mirrors.import(borrow, selected, *state);
                }) == DiagnosticBorrowStatus::Observed, "Typed borrow callback completes on owner");
            check(!owner.borrowed(), "No metadata borrow retained after import returns");
            return result;
        }
        void releaseSources()
        {
            check(owner.retire(), "Retire metadata after synchronous commands submitted");
            BorrowedGlState restore; gpu.bind(GpuTransfers::Framebuffer, sourceFbo);
            for (const GLenum attachment : {GpuTransfers::ColorAttachment, SourceTextures::DepthAttachment,
                SourceTextures::DepthStencilAttachment}) gpu.attach(GpuTransfers::Framebuffer, attachment, GL_TEXTURE_2D, 0, 0);
            for (auto& texture : sources)
            {
                osg::observer_ptr<osg::Texture2D> weak = texture;
                texture->releaseGLObjects(state); texture = nullptr;
                check(!weak.valid(), "Importer retained no source object outside its callback");
            }
            names = {};
        }
        void close()
        {
            check(!sources[0], "Every fixture source owner retired");
            glFinish(); // Verification/closure only, never part of DiagnosticGlMirrors::import.
            check(mirrors.retireCompleted(), "Actual completed copy retires without a timed wait");
            check(mirrors.bytes() == 0, "No live mirror payload after retirement");
            check(mirrors.close() && mirrors.close(), "Backend close is idempotent");
            state->popStateSet(); ordinary->releaseGLObjects(state); program->releaseGLObjects(state);
            ordinaryTexture->releaseGLObjects(state); state->releaseGLObjects();
            gpu.remove(1, &targetFbo); gpu.remove(1, &sourceFbo); glDeleteTextures(1, &target.gl); gpu.close();
            state = nullptr; osg::GraphicsContext::decrementContextIDUsageCount(contextId);
            check(lifetime.invalidate(), "Authoritative owner invalidates old namespace before context destruction");
            context.close();
        }
    };

    void backendFailureProof(Fixture& fixture)
    {
        const auto ordinary = fixture.ordinaryPixels(); const Cache cache(*fixture.state);
        for (const auto fault : {DiagnosticImportFault::BackendAfterFramebuffers, DiagnosticImportFault::BackendAfterSampler,
            DiagnosticImportFault::BackendAfterShader, DiagnosticImportFault::BackendAfterProgram})
        {
            ObjectAudit audit; BorrowedGlState before(false); bool failed = false;
            try { DiagnosticGlMirrors backend(fixture.lifetime, {}, fault, audit.callback()); }
            catch (const std::bad_alloc&) { failed = true; }
            check(failed, "Injected real-allocation-stage backend failure reached");
            BorrowedGlState after(false); check(before.same(after), "Backend initialization failure restores all borrowed state");
            audit.allRetired(); check(cache.same(Cache(*fixture.state)), "Failed initialization leaves OSG cache intact");
            check(fixture.ordinaryPixels() == ordinary, "Ordinary draw intact after every partial backend failure");
        }
    }
    void mirrorFailureProof(Fixture& fixture, const DiagnosticSources& description)
    {
        const auto ordinary = fixture.ordinaryPixels(); const Cache cache(*fixture.state);
        for (const auto fault : {DiagnosticImportFault::AfterFirstMirror, DiagnosticImportFault::AfterSecondMirror,
            DiagnosticImportFault::AfterThirdMirror, DiagnosticImportFault::AfterFirstCopy, DiagnosticImportFault::AfterFence})
        {
            phase = "mirror fault backend initialization";
            ObjectAudit audit; DiagnosticGlMirrors backend(fixture.lifetime, {}, fault, audit.callback());
            BorrowedGlState before(false); bool failed = false;
            try
            {
                phase = "mirror fault expected failure";
                fixture.owner.withBorrow({description.identity, description.images[0].width, description.images[0].height},
                    [&](const DiagnosticInputBorrow& borrow) { backend.import(borrow, fixture.sources, *fixture.state); });
            }
            catch (const std::bad_alloc&) { failed = true; }
            check(failed && !fixture.owner.borrowed(), "Partial copy/allocation failure releases typed synchronous borrow");
            phase = "mirror fault post-failure state observation";
            check(backend.bytes() == 0 && std::all_of(backend.images().begin(), backend.images().end(),
                [](const auto& image) { return image.gl == 0; }), "Failed generation leaves no mirror payload or names");
            BorrowedGlState after(false); check(before.same(after), "Failed partial mirror work restores raw GL state");
            audit.allRetired(true); check(cache.same(Cache(*fixture.state)), "Partial mirror failure leaves OSG cache intact");
            check(fixture.ordinaryPixels() == ordinary, "Ordinary draw intact after failed mirror submission");
            phase = "mirror fault retry";
            check(fixture.owner.withBorrow({description.identity, description.images[0].width, description.images[0].height},
                [&](const DiagnosticInputBorrow& borrow)
                { check(backend.import(borrow, fixture.sources, *fixture.state) == DiagnosticImportStatus::Imported,
                    "Failed generation can retry without stale success publication"); }) == DiagnosticBorrowStatus::Observed,
                "Recovered exact source borrow executes");
            phase = "mirror fault completed retirement";
            glFinish(); check(backend.retireCompleted() && backend.close() && backend.close(),
                "Recovered generation completes and closes idempotently");
            audit.allRetired();
        }
        phase = "mirror fault proof finished";
    }
}

int main(int argc, char** argv)
{
    using namespace P9Dlss;
    try
    {
        check(argc == 3 && std::string(argv[1]) == "--output", "Usage: p9-dlss-diagnostic-gl-import --output fresh-proof.jsonl");
        const auto path = std::filesystem::absolute(argv[2]); check(!std::filesystem::exists(path), "Preserve earlier evidence");
        std::ofstream evidence(path); check(evidence.good(), "Open fresh diagnostic GL evidence");
        auto* fixture = new Fixture;
        evidence << "{\"schema\":1,\"check\":\"diagnostic_native_runtime\",\"status\":\"pass\",\"window_hidden\":true,\"osg_version\":\"3.6.5\","
            "\"osg_runtime_path\":" << jsonString(loadedModule(L"osg.dll"))
            << ",\"openthreads_runtime_path\":" << jsonString(loadedModule(L"OpenThreads.dll"))
            << ",\"gl_vendor\":" << jsonString(reinterpret_cast<const char*>(glGetString(GL_VENDOR)))
            << ",\"gl_renderer\":" << jsonString(reinterpret_cast<const char*>(glGetString(GL_RENDERER)))
            << ",\"gl_version\":" << jsonString(reinterpret_cast<const char*>(glGetString(GL_VERSION)))
            << ",\"actual_game_frames\":0,\"actual_dlss_evaluations\":0}" << '\n';
        evidence.flush(); check(evidence.good(), "Flush actual runtime/module identities");
        backendFailureProof(*fixture);
        unsigned copies = 0;
        for (const DiagnosticFormat depth : {DiagnosticFormat::Depth24Stencil8, DiagnosticFormat::Depth32FloatStencil8,
            DiagnosticFormat::Depth32Float}) for (const bool reverse : {false, true}) for (const bool floatColor : {false, true})
        {
            const auto description = fixture->make(64, 48, depth, reverse, floatColor);
            const auto color = pixels<float>(fixture->names[0], GL_RGBA, GL_FLOAT, 4);
            const auto colorBytes = floatColor ? std::vector<unsigned char>{}
                : pixels<unsigned char>(fixture->names[0], GL_RGBA, GL_UNSIGNED_BYTE, 4);
            const auto rawDepth = pixels<float>(fixture->names[1], GL_DEPTH_COMPONENT, GL_FLOAT, 1);
            const auto motion = pixels<float>(fixture->names[2], 0x8227, GL_FLOAT, 2);
            for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x)
            {
                const unsigned q = (x >= 32) + 2 * (y >= 24); const auto position = std::size_t(y) * 64 + x;
                const bool exactColor = floatColor ? color[position * 4] == q / 4.f && color[position * 4 + 1] == .5f
                    && color[position * 4 + 2] == .75f && color[position * 4 + 3] == 1
                    : colorBytes[position * 4] == q * 37 + 19
                        && colorBytes[position * 4 + 1] == 83 && colorBytes[position * 4 + 2] == 167 && colorBytes[position * 4 + 3] == 255;
                if (!exactColor)
                    std::cerr << "Controlled color mismatch at " << x << ',' << y << " quadrant=" << q
                        << " float=" << floatColor << " actual_floats=" << color[position * 4] << ',' << color[position * 4 + 1]
                        << ',' << color[position * 4 + 2] << ',' << color[position * 4 + 3]
                        << " actual_bytes=" << (floatColor ? -1 : int(colorBytes[position * 4]))
                        << ',' << (floatColor ? -1 : int(colorBytes[position * 4 + 1]))
                        << ',' << (floatColor ? -1 : int(colorBytes[position * 4 + 2]))
                        << ',' << (floatColor ? -1 : int(colorBytes[position * 4 + 3])) << '\n';
                check(exactColor,
                    "Source scene color independently matches controlled producer values");
                check(motion[position * 2] == -2 && motion[position * 2 + 1] == 1,
                    "Source motion independently matches signed partial pixel vectors");
            }
            std::vector<float> expectedDepth = rawDepth;
            std::vector<std::uint32_t> packed24;
            struct Packed32 { float depth; std::uint32_t stencil; };
            std::vector<Packed32> packed32;
            if (depth == DiagnosticFormat::Depth24Stencil8)
            {
                packed24 = pixels<std::uint32_t>(fixture->names[1], 0x84F9, 0x84FA, 1);
                for (std::size_t i = 0; i < packed24.size(); ++i) expectedDepth[i] = float(double(packed24[i] >> 8) / 16777215.0);
            }
            else if (depth == DiagnosticFormat::Depth32FloatStencil8)
            {
                static_assert(sizeof(Packed32) == 8);
                packed32 = pixels<Packed32>(fixture->names[1], 0x84F9, 0x8DAD, 1);
                for (std::size_t i = 0; i < packed32.size(); ++i) expectedDepth[i] = packed32[i].depth;
            }
            for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x)
            {
                const unsigned q = (x >= 32) + 2 * (y >= 24); const auto position = std::size_t(y) * 64 + x;
                const float expected = reverse ? 1.f - SourceTextures::depth(q) : SourceTextures::depth(q);
                check(std::abs(expectedDepth[position] - expected) <= 1.f / 16777215,
                    "Depth producer matches independent depth convention and quantization bound");
                if (!packed24.empty()) check((packed24[position] & 255) == q + 17, "Packed D24 has independently expected nonzero stencil");
                if (!packed32.empty()) check((packed32[position].stencil & 255) == q + 17, "Packed D32F has independently expected nonzero stencil");
            }
            if (copies == 0) mirrorFailureProof(*fixture, description);
            if (copies == 0)
            {
                check(fixture->owner.withBorrow({description.identity, 64, 48}, [&](const DiagnosticInputBorrow& borrow)
                {
                    DiagnosticImportStatus status = DiagnosticImportStatus::Imported;
                    std::thread foreign([&] { status = fixture->mirrors.import(borrow, fixture->sources, *fixture->state); });
                    foreign.join(); check(status == DiagnosticImportStatus::WrongThread,
                        "A live producer borrow cannot authorize GL work on a foreign thread");
                }) == DiagnosticBorrowStatus::Observed, "Foreign-thread refusal stays inside synchronous source lifetime");
            }
            const auto ordinary = fixture->ordinaryPixels(); const Cache cache(*fixture->state);
            {
                ForeignGlSentinel foreign(fixture->program->getPCP(*fixture->state)->getHandle(), copies);
                BorrowedGlState before(false);
                bool called = false;
                auto staleRequest = DiagnosticBorrowRequest{description.identity, 64, 48};
                ++staleRequest.identity.ticket;
                check(fixture->owner.withBorrow(staleRequest, [&](const DiagnosticInputBorrow&) { called = true; })
                    == DiagnosticBorrowStatus::MismatchedRequest && !called,
                    "Wrong actual draw ticket refuses before entering any GL consumer");
                auto missing = fixture->sources; missing[2] = nullptr;
                check(fixture->import(description, missing) == DiagnosticImportStatus::MissingTexture,
                    "Missing actual producer object refuses before mirror allocation");
                check(fixture->mirrors.bytes() == 0, "Refused actual object allocates no mirror payload");
                auto unrelated = fixture->sources; unrelated[0] = fixture->ordinaryTexture;
                check(fixture->import(description, unrelated) == DiagnosticImportStatus::ResourceMismatch,
                    "Metadata cannot authorize an unrelated actual OSG texture");
                DiagnosticInputBorrowOwner forgedEpoch;
                auto forged = description; ++forged.identity.contextEpoch;
                for (auto& image : forged.images) ++image.contextEpoch;
                check(forgedEpoch.publish(forged) == DiagnosticBorrowStatus::Observed,
                    "Metadata-only publication does not itself certify a namespace epoch");
                check(forgedEpoch.withBorrow({forged.identity, 64, 48}, [&](const DiagnosticInputBorrow& borrow)
                    { check(fixture->mirrors.import(borrow, fixture->sources, *fixture->state) == DiagnosticImportStatus::WrongContext,
                        "Same context pointer cannot grant a forged recreated namespace epoch"); }) == DiagnosticBorrowStatus::Observed,
                    "Backend independently gates owner-issued lifetime");
                osg::ref_ptr<osg::State> wrongState = new osg::State;
                wrongState->setContextID(fixture->contextId + 100);
                check(fixture->owner.withBorrow({description.identity, 64, 48}, [&](const DiagnosticInputBorrow& borrow)
                    { check(fixture->mirrors.import(borrow, fixture->sources, *wrongState) == DiagnosticImportStatus::WrongContext,
                        "Current native context does not authorize a different OSG context"); }) == DiagnosticBorrowStatus::Observed,
                    "Wrong OSG identity checked within exact owner callback");
                {
                    DiagnosticGlMirrors bounded(fixture->lifetime, {64, 48, 64ull * 48 * (floatColor ? 16 : 12) - 1});
                    check(fixture->owner.withBorrow({description.identity, 64, 48}, [&](const DiagnosticInputBorrow& borrow)
                        { check(bounded.import(borrow, fixture->sources, *fixture->state) == DiagnosticImportStatus::OverBudget,
                            "Metadata allowance cannot bypass actual mirror payload ceiling"); }) == DiagnosticBorrowStatus::Observed,
                        "Actual backend refuses one byte above its independent ceiling");
                    check(bounded.bytes() == 0, "No payload allocated after budget refusal"); check(bounded.close(), "Empty backend closes");
                    check(fixture->owner.withBorrow({description.identity, 64, 48}, [&](const DiagnosticInputBorrow& borrow)
                        { check(bounded.import(borrow, fixture->sources, *fixture->state) == DiagnosticImportStatus::Closed,
                            "Closed private backend cannot import another draw"); }) == DiagnosticBorrowStatus::Observed,
                        "Closed backend refusal remains inside exact producer callback");
                }
                phase = "actual source format/extent mismatch fixture";
                // OSG realizes immutable texture storage here. Challenge the
                // metadata instead of illegally redefining that storage: the
                // importer must inspect the actual object and decline either.
                DiagnosticInputBorrowOwner falseFormat;
                auto wrongFormat = description;
                wrongFormat.images[0].format = floatColor ? DiagnosticFormat::Rgba8 : DiagnosticFormat::Rgba16Float;
                check(falseFormat.publish(wrongFormat) == DiagnosticBorrowStatus::Observed,
                    "Structurally valid metadata alone cannot certify the actual format");
                check(falseFormat.withBorrow({wrongFormat.identity, 64, 48}, [&](const DiagnosticInputBorrow& borrow)
                    { check(fixture->mirrors.import(borrow, fixture->sources, *fixture->state) == DiagnosticImportStatus::FormatMismatch,
                        "Actual source format overrides a mismatched declared format"); }) == DiagnosticBorrowStatus::Observed,
                    "Actual format rejection occurs inside exact synchronous scope");
                DiagnosticInputBorrowOwner falseExtent;
                auto wrongExtent = description; for (auto& image : wrongExtent.images) image.width = 65;
                check(falseExtent.publish(wrongExtent) == DiagnosticBorrowStatus::Observed,
                    "Structurally valid metadata alone cannot certify the actual extent");
                check(falseExtent.withBorrow({wrongExtent.identity, 65, 48}, [&](const DiagnosticInputBorrow& borrow)
                    { check(fixture->mirrors.import(borrow, fixture->sources, *fixture->state) == DiagnosticImportStatus::ExtentMismatch,
                        "Actual source extent overrides a mismatched declared extent"); }) == DiagnosticBorrowStatus::Observed,
                    "Actual extent rejection occurs inside exact synchronous scope");
                check(fixture->mirrors.bytes() == 0, "Bad actual-resource declarations allocate no mirror payload");
                before.active(BorrowedGlState::Texture0);
                glBindTexture(GL_TEXTURE_2D, fixture->names[0].gl);
                const GLint wrongSwizzle[] = {GL_BLUE, GL_GREEN, GL_RED, GL_ALPHA};
                glTexParameteriv(GL_TEXTURE_2D, 0x8E46, wrongSwizzle);
                check(fixture->import(description, fixture->sources) == DiagnosticImportStatus::UnsupportedSampling,
                    "Borrowed producer swizzle is refused rather than silently changing colors");
                const GLint identitySwizzle[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
                glTexParameteriv(GL_TEXTURE_2D, 0x8E46, identitySwizzle);
                if (depth != DiagnosticFormat::Depth32Float)
                {
                    glBindTexture(GL_TEXTURE_2D, fixture->names[1].gl); glTexParameteri(GL_TEXTURE_2D, 0x90EA, GL_STENCIL_INDEX);
                    check(fixture->import(description, fixture->sources) == DiagnosticImportStatus::UnsupportedSampling,
                        "Packed stencil sampling cannot masquerade as scene depth");
                    GLint mode = 0; glGetTexParameteriv(GL_TEXTURE_2D, 0x90EA, &mode);
                    check(mode == GL_STENCIL_INDEX, "Refused import does not mutate producer depth/stencil sampling policy");
                    glTexParameteri(GL_TEXTURE_2D, 0x90EA, GL_DEPTH_COMPONENT);
                }
                // Re-establish foreign state after the fixture's own deliberate
                // mutation. This does not mask any importer state change.
                before.active(GLenum(before.textureUnit));
                BorrowedGlState importBefore(false);
                auto genBuffer = function<void (APIENTRY*)(GLsizei, GLuint*)>("glGenBuffers");
                auto bindBuffer = function<void (APIENTRY*)(GLenum, GLuint)>("glBindBuffer");
                auto removeBuffer = function<void (APIENTRY*)(GLsizei, const GLuint*)>("glDeleteBuffers");
                GLuint unpack = 0; genBuffer(1, &unpack); bindBuffer(0x88EC, unpack);
                // Zero-sized nonzero unpack PBO is an adversarial sentinel:
                // allocation with nullptr would fail unless importer unbinds it.
                check(fixture->import(description, fixture->sources) == DiagnosticImportStatus::Imported,
                    "Exact typed draw borrow imports actual GL/OSG source objects");
                GLint actualUnpack = 0; glGetIntegerv(0x88EF, &actualUnpack);
                check(actualUnpack == GLint(unpack), "Foreign pixel-unpack PBO binding restored after mirror allocation");
                bindBuffer(0x88EC, 0); removeBuffer(1, &unpack);
                BorrowedGlState after(false); check(importBefore.same(after), "Foreign raw GL state restored without verifier restoration");
                check(cache.same(Cache(*fixture->state)), "Actual OSG caches unchanged across import");
                check(fixture->import(description, fixture->sources) == DiagnosticImportStatus::StaleSubmission,
                    "A completed draw submission cannot be silently imported twice");
                foreign.close();
            }
            const auto& mirrors = fixture->mirrors.images();
            check(pixels<float>(mirrors[0], GL_RGBA, GL_FLOAT, 4) == color, "Exact RGBA8/RGBA16F mirror pixels");
            check(pixels<float>(mirrors[1], 0x1903, GL_FLOAT, 1) == expectedDepth, "Exact independently expected packed-depth conversion");
            check(pixels<float>(mirrors[2], 0x8227, GL_FLOAT, 2) == motion, "Exact signed partial motion mirror pixels");
            check(pixels<float>(fixture->names[0], GL_RGBA, GL_FLOAT, 4) == color
                && pixels<float>(fixture->names[1], GL_DEPTH_COMPONENT, GL_FLOAT, 1) == rawDepth
                && pixels<float>(fixture->names[2], 0x8227, GL_FLOAT, 2) == motion, "Borrowed producer source pixels unchanged");
            if (!packed24.empty()) check(pixels<std::uint32_t>(fixture->names[1], 0x84F9, 0x84FA, 1) == packed24,
                "Both exact packed D24 depth and nonzero stencil unchanged");
            if (!packed32.empty())
            {
                const auto after = pixels<Packed32>(fixture->names[1], 0x84F9, 0x8DAD, 1);
                for (std::size_t i = 0; i < after.size(); ++i)
                    check(std::bit_cast<unsigned>(after[i].depth) == std::bit_cast<unsigned>(packed32[i].depth)
                        && after[i].stencil == packed32[i].stencil, "Both exact packed D32F depth and stencil unchanged");
            }
            {
                BorrowedGlState restore; restore.active(BorrowedGlState::Texture0);
                glBindTexture(GL_TEXTURE_2D, fixture->names[1].gl); GLint compare = 0;
                glGetTexParameteriv(GL_TEXTURE_2D, 0x884C, &compare); check(compare == 0x884E, "Producer comparison policy unchanged");
            }
            check(cache.same(Cache(*fixture->state)), "Ordinary cache remains intact before following draw");
            check(fixture->ordinaryPixels() == ordinary, "Following cached Geometry pixels equal control");
            const auto bytes = fixture->mirrors.bytes(); check(bytes == 64ull * 48 * (floatColor ? 16 : 12), "Exact bounded mirror payload charge");
            fixture->releaseSources();
            // Importer cannot leave a callback source reference, even while its
            // completed private destination survives independently.
            check(pixels<float>(mirrors[0], GL_RGBA, GL_FLOAT, 4) == color, "Private copy survives source owner retirement");
            glFinish(); check(fixture->mirrors.retireCompleted(), "GPU-completed one-generation retirement");
            ++copies;
            evidence << "{\"schema\":1,\"check\":\"diagnostic_actual_gl_import\",\"status\":\"pass\",\"copy\":" << copies
                << ",\"width\":64,\"height\":48,\"float_color\":" << (floatColor ? "true" : "false")
                << ",\"depth_format\":" << unsigned(depth) << ",\"reversed_depth\":" << (reverse ? "true" : "false")
                << ",\"mirror_bytes\":" << bytes << ",\"typed_draw_borrow\":true,\"actual_osg_objects\":true,\"exact_pixels\":true,"
                "\"raw_state_restored\":true,\"osg_cache_restored\":true,\"following_geometry_exact\":true,"
                "\"source_owners_retired\":true,\"one_mirror_generation\":true,\"actual_game_frames\":0,\"actual_dlss_evaluations\":0,"
                "\"dense_motion_proved\":false}" << '\n'; evidence.flush(); check(evidence.good(), "Flush actual per-generation evidence");
        }
        // A resize is refused even when the previous native copy completed,
        // until explicit completion-qualified retirement has released it.
        const auto first = fixture->make(64, 48, DiagnosticFormat::Depth32Float, false, false);
        check(fixture->import(first, fixture->sources) == DiagnosticImportStatus::Imported, "Initial bounded resize fixture copy");
        const auto reusedNames = fixture->mirrors.images(); const auto reusedGeneration = fixture->mirrors.generations();
        fixture->releaseSources();
        const auto sameSize = fixture->make(64, 48, DiagnosticFormat::Depth32Float, true, false);
        const auto sameSizeControl = fixture->ordinaryPixels();
        const auto changedDepth = pixels<float>(fixture->names[1], GL_DEPTH_COMPONENT, GL_FLOAT, 1);
        check(fixture->import(sameSize, fixture->sources) == DiagnosticImportStatus::Imported,
            "Fresh submitted frame reuses mirror storage on the same owner namespace");
        for (unsigned i = 0; i < reusedNames.size(); ++i)
            check(fixture->mirrors.images()[i].gl == reusedNames[i].gl, "Same-size frame retains exact private mirror names");
        check(fixture->mirrors.generations() == reusedGeneration
            && pixels<float>(fixture->mirrors.images()[1], 0x1903, GL_FLOAT, 1) == changedDepth,
            "Actual changed depth copied without a second mirror generation");
        check(fixture->ordinaryPixels() == sameSizeControl, "Ordinary pixels remain intact across same-size storage reuse");
        fixture->releaseSources();
        const auto larger = fixture->make(128, 64, DiagnosticFormat::Depth32Float, false, false);
        const auto oldBytes = fixture->mirrors.bytes(); const auto oldName = fixture->mirrors.images()[0].gl;
        check(fixture->import(larger, fixture->sources) == DiagnosticImportStatus::ResizeRequiresRetirement,
            "Resize cannot accumulate old and new mirror generations");
        check(fixture->mirrors.bytes() == oldBytes && fixture->mirrors.images()[0].gl == oldName,
            "Refused resize leaves exact old destination storage intact");
        glFinish(); check(fixture->mirrors.retireCompleted(), "Retire previous completed generation before resize");
        check(fixture->import(larger, fixture->sources) == DiagnosticImportStatus::Imported, "New extent imports after explicit retirement");
        check(fixture->mirrors.bytes() == 128ull * 64 * 12 && fixture->mirrors.peakBytes() == 128ull * 64 * 12,
            "Resize peak is one largest live generation, not a sum");
        // Keep an additional real mirror alive across actual destruction of
        // the old namespace. All normal resources close before owner teardown.
        auto* lostBackend = new DiagnosticGlMirrors(fixture->lifetime);
        check(fixture->owner.withBorrow({larger.identity, 128, 64}, [&](const DiagnosticInputBorrow& borrow)
            { check(lostBackend->import(borrow, fixture->sources, *fixture->state) == DiagnosticImportStatus::Imported,
                "Real copy submitted before namespace destruction"); }) == DiagnosticBorrowStatus::Observed,
            "Lost-namespace fixture uses actual exact producer owners");
        const auto lostNames = lostBackend->images(); const auto lostBytes = lostBackend->bytes();
        ReplacementContext replacement;
        check(replacement.native != fixture->context.context, "Recreation owns a genuinely separate native namespace");
        fixture->releaseSources(); fixture->close(); // Invalidates lifetime, destroys old HGLRC/window.
        replacement.makeCurrent();
        GLuint maximumName = 0; for (const auto& image : lostNames) maximumName = std::max(maximumName, image.gl);
        check(maximumName > 0 && maximumName <= 4096, "Bounded exact-name reuse fixture");
        std::vector<GLuint> reserved(std::size_t(maximumName) + 16); glGenTextures(GLsizei(reserved.size()), reserved.data());
        std::array<Texture, 3> collision;
        for (unsigned i = 0; i < collision.size(); ++i)
        {
            check(std::find(reserved.begin(), reserved.end(), lostNames[i].gl) != reserved.end(),
                "Recreated native namespace reserves the exact old texture name");
            collision[i].gl = lostNames[i].gl; collision[i].width = 8; collision[i].height = 8;
            glBindTexture(GL_TEXTURE_2D, collision[i].gl);
            std::vector<unsigned char> sentinel(8 * 8 * 4, static_cast<unsigned char>(37 + i));
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, sentinel.data());
        }
        glCheck("Create exact old-name sentinels in recreated native namespace");
        const auto sentinel = pixels<unsigned char>(collision[0], GL_RGBA, GL_UNSIGNED_BYTE, 4);
        osg::ref_ptr<osg::State> replacementState = new osg::State; replacementState->setContextID(fixture->contextId);
        DiagnosticInputBorrowOwner stale;
        check(stale.publish(larger) == DiagnosticBorrowStatus::Observed, "Stale metadata alone can remain structurally valid");
        BorrowedGlState beforeLost(false);
        check(stale.withBorrow({larger.identity, 128, 64}, [&](const DiagnosticInputBorrow& borrow)
            { check(lostBackend->import(borrow, {}, *replacementState) == DiagnosticImportStatus::WrongContext,
                "Destroyed source namespace cannot import into a recreated context"); }) == DiagnosticBorrowStatus::Observed,
            "Backend independently refuses stale actual namespace");
        check(!lostBackend->retireCompleted() && !lostBackend->close(),
            "Old resource names cannot be deleted through the replacement context");
        check(lostBackend->abandonLostContext() && lostBackend->close(), "Lost owner forgets names without GL calls");
        check(lostBackend->bytes() == 0 && lostBackend->abandonedBytes() == lostBytes,
            "Abandoned payload is separately recorded, never called resident memory savings");
        BorrowedGlState afterLost(false); check(beforeLost.same(afterLost), "Namespace refusal/abandonment preserves new raw state");
        for (const auto& image : collision) check(glIsTexture(image.gl), "Old namespace cleanup did not delete same-name new resources");
        check(pixels<unsigned char>(collision[0], GL_RGBA, GL_UNSIGNED_BYTE, 4) == sentinel,
            "New namespace pixels survive stale backend cleanup");
        DiagnosticContextLifetime newLifetime(fixture->contextId, 2);
        DiagnosticGlMirrors recreated(newLifetime); check(recreated.close(), "Fresh owner-issued epoch creates and closes a new backend");
        delete lostBackend;
        glDeleteTextures(GLsizei(reserved.size()), reserved.data()); replacementState = nullptr;
        check(newLifetime.invalidate(), "New namespace owner invalidates before destruction"); replacement.close();
        evidence << "{\"schema\":1,\"check\":\"diagnostic_namespace_recreation\",\"status\":\"pass\","
            "\"actual_old_context_destroyed\":true,\"new_namespace_same_names_intact\":true,"
            "\"owner_epoch_required\":true,\"abandoned_bytes\":" << lostBytes
            << ",\"gpu_device_removal_proved\":false,\"actual_game_frames\":0,\"actual_dlss_evaluations\":0}" << '\n';
        evidence << "{\"schema\":1,\"check\":\"diagnostic_gl_import_complete\",\"status\":\"pass\",\"checks\":" << checks
            << ",\"principal_backend_imports\":" << fixture->mirrors.copies() << ",\"fault_recovery_imports\":5,\"lost_namespace_imports\":1,\"generations\":" << fixture->mirrors.generations()
            << ",\"retired_generations\":" << fixture->mirrors.retiredGenerations() << ",\"peak_mirror_bytes\":" << fixture->mirrors.peakBytes()
            << ",\"ordinary_geometry_draws\":" << fixture->ordinaryDraws << ",\"actual_game_frames\":0,\"actual_dlss_evaluations\":0,"
            "\"dense_motion_proved\":false,\"performance_promoted\":false}" << '\n'; evidence.flush(); check(evidence.good(), "Final evidence flushed");
        delete fixture; std::cout << "Diagnostic actual GL import checks passed: " << checks << '\n'; return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Diagnostic GL import fixture failed during " << phase << ": " << error.what() << '\n'; return 1;
    }
}
