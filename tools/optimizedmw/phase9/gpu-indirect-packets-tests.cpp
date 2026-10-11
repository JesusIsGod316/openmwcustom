#include <components/sceneutil/gpuindirectpackets.hpp>
#include <osg/NodeCallback>
#include <osg/Switch>
#include <osgViewer/Viewer>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace Packet = SceneUtil::GpuIndirectPackets;
namespace Contract = Shader::IndirectProjection;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
Resource::OpenGlPressureSample sampleValue;
Resource::OpenGlPressureSample sample(void*) { return sampleValue; }

struct GlState final : osg::Camera::DrawCallback
{
    mutable std::array<GLuint, 3> buffers{};
    mutable GLint alignment = 0;
    void operator()(osg::RenderInfo& info) const override
    {
        auto* ext = info.getState()->get<osg::GLExtensions>();
        require(ext && ext->glDispatchCompute && ext->glMultiDrawElementsIndirect, "real GL 4.3 fixture required");
        if (!buffers[0])
        {
            glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &alignment);
            ext->glGenBuffers(3, buffers.data());
            for (GLuint buffer : buffers)
            {
                ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
                ext->glBufferData(GL_SHADER_STORAGE_BUFFER, alignment * 4, nullptr, GL_STATIC_DRAW);
            }
        }
        ext->glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 0, buffers[0], alignment, alignment);
        ext->glBindBufferRange(GL_SHADER_STORAGE_BUFFER, 1, buffers[1], alignment * 2, alignment);
        ext->glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffers[2]);
    }
};

struct Pixels final : osg::Camera::DrawCallback
{
    mutable std::array<unsigned char, 32 * 32 * 4> data{};
    mutable std::vector<osg::DrawElementsIndirectCommand> commands;
    const Packet::Primitive* inspect = nullptr;
    const GlState* previous = nullptr;
    void operator()(osg::RenderInfo& info) const override
    {
        glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
        if (inspect) commands = inspect->readLastCommands(*info.getState());
        auto* ext = info.getState()->get<osg::GLExtensions>();
        using GetIndexed = void (GL_APIENTRY*)(GLenum, GLuint, GLint*);
        using GetIndexed64 = void (GL_APIENTRY*)(GLenum, GLuint, GLint64*);
        const auto indexed = reinterpret_cast<GetIndexed>(osg::getGLExtensionFuncPtr("glGetIntegeri_v"));
        const auto indexed64 = reinterpret_cast<GetIndexed64>(osg::getGLExtensionFuncPtr("glGetInteger64i_v"));
        if (previous)
        {
            require(indexed && indexed64, "fixture cannot verify SSBO binding restoration");
            GLint generic = 0; glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &generic);
            require(static_cast<GLuint>(generic) == previous->buffers[2], "generic SSBO binding was changed");
            for (unsigned i = 0; i < 2; ++i)
            {
                GLint buffer = 0; GLint64 offset = 0, size = 0;
                indexed(GL_SHADER_STORAGE_BUFFER_BINDING, i, &buffer);
                indexed64(GL_SHADER_STORAGE_BUFFER_START, i, &offset);
                indexed64(GL_SHADER_STORAGE_BUFFER_SIZE, i, &size);
                require(static_cast<GLuint>(buffer) == previous->buffers[i]
                    && offset == previous->alignment * (i + 1) && size == previous->alignment,
                    "indexed SSBO name/range was changed");
            }
        }
        GLint program = 0, indirect = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING, &indirect);
        const auto* pcp = info.getState()->getLastAppliedProgramObject();
        require(static_cast<GLuint>(program) == (pcp ? pcp->getHandle() : 0), "OSG consumer program/cache differs");
        require(indirect == 0, "indirect binding/cache was not restored");
        require(ext && glGetError() == GL_NO_ERROR, "GL error in indirect fixture");
    }
};

osg::ref_ptr<osg::Geometry> geometry()
{
    osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
    result->setDataVariance(osg::Object::STATIC);
    result->setUseDisplayList(false); result->setUseVertexBufferObjects(true);
    osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
    osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
    for (unsigned i = 0; i < 3; ++i)
    {
        const float x = i == 0 ? -.9f : i == 1 ? .2f : 3.f;
        positions->push_back(osg::Vec3(x, -.8f, 0));
        positions->push_back(osg::Vec3(x + .7f, -.8f, 0));
        positions->push_back(osg::Vec3(x + .3f, .8f, 0));
        for (unsigned n = 0; n < 3; ++n) colors->push_back(osg::Vec4(i == 0 ? 1.f : .1f, .25f, .5f, 1));
        osg::ref_ptr<osg::DrawElementsUShort> indices = new osg::DrawElementsUShort(GL_TRIANGLES);
        for (unsigned n = 0; n < 3; ++n) indices->push_back(static_cast<unsigned short>(i * 3 + n));
        result->addPrimitiveSet(indices);
    }
    result->setVertexArray(positions);
    result->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
    return result;
}

osg::ref_ptr<osg::Group> nestedRoot(osg::Geometry* geometry)
{
    osg::ref_ptr<osg::Geode> geode = new osg::Geode;
    geode->addDrawable(geometry);
    osg::ref_ptr<osg::MatrixTransform> transform = new osg::MatrixTransform;
    transform->setMatrix(osg::Matrix::translate(.05, .03, 0));
    transform->addChild(geode);
    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(transform);
    return root;
}

osg::ref_ptr<osg::Program> fixtureProgram(bool certified)
{
    // The fixture exercises the contract consumer with small producer sources.
    // The separate ShaderManager test proves only exact shipped raw templates
    // and their complete include closure may receive this tag in production.
    osg::ref_ptr<osg::Program> result = new osg::Program;
    osg::ref_ptr<osg::Shader> vertex = new osg::Shader(osg::Shader::VERTEX,
        "#version 120\nuniform mat4 projectionMatrix;varying vec4 color;"
        "vec4 modelToClip(vec4 pos);void main(){gl_Position=modelToClip(gl_Vertex);color=gl_Color;}\n");
    osg::ref_ptr<osg::Shader> core = new osg::Shader(osg::Shader::VERTEX,
        "#version 120\nuniform mat4 projectionMatrix;"
        "vec4 modelToClip(vec4 pos){return projectionMatrix*(gl_ModelViewMatrix*pos);}\n");
    osg::ref_ptr<osg::Shader> fragment = new osg::Shader(osg::Shader::FRAGMENT,
        "#version 120\nvarying vec4 color;void main(){gl_FragColor=color;}\n");
    result->addShader(vertex); result->addShader(core); result->addShader(fragment);
    if (certified)
    {
        vertex->getOrCreateUserDataContainer()->addUserObject(new Contract::Source(*vertex, "compatibility/objects.vert"));
        core->getOrCreateUserDataContainer()->addUserObject(new Contract::Source(*core, "lib/core/vertex.glsl"));
        fragment->getOrCreateUserDataContainer()->addUserObject(new Contract::Source(*fragment, "compatibility/objects.frag"));
        Contract::certify(*result);
        require(Contract::matches(*result), "fixture program certificate was not created");
    }
    return result;
}

int main(int argc, char** argv) try
{
    const bool reuse = argc > 1 && std::string_view(argv[1]) == "--reuse-commands";
    Packet::reuseIndirectCommands.store(reuse);
    const auto initialBytes = Packet::Counters::liveBytes.load();
    auto original = geometry();
    osg::ref_ptr<osg::Geometry> control = new osg::Geometry(*original, osg::CopyOp::SHALLOW_COPY);
    auto group = nestedRoot(original);
    auto controlRoot = nestedRoot(control);
    require(!Packet::install(*original, false, true), "disabled indirect allocated callback");
    require(Packet::installCertifiedSubtrees(*group, 1024, false, true) == 0,
        "disabled nested installation allocated callbacks");
    Resource::SpeculativeBudget ledger;
    Packet::sample = &sample; Packet::ledger = &ledger;
    require(!Packet::install(*original, true, true), "unknown pressure admitted indirect allocation");
    sampleValue.generation = 1; sampleValue.decision.state = Resource::OpenGlPressure::Normal;
    sampleValue.growthWatermarkValid = true; sampleValue.decision.admissionsPerSample = 1;
    sampleValue.memory.physicalValid = true; sampleValue.memory.physicalTotal = 32ull * 1024 * 1024 * 1024;
    sampleValue.memory.physicalAvailable = 16ull * 1024 * 1024 * 1024;
    sampleValue.memory.commitValid = true; sampleValue.memory.commitAvailable = 16ull * 1024 * 1024 * 1024;
    sampleValue.decision.limits.physicalReserve = 4ull * 1024 * 1024 * 1024;
    sampleValue.decision.limits.commitReserve = 1024ull * 1024 * 1024;
    require(SceneUtil::PersistentOpaquePackets::sourceAncestryCertified(*group),
        "fixture must certify untouched original ancestry before installation");
    auto* transform = static_cast<osg::MatrixTransform*>(group->getChild(0));
    transform->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
    require(Packet::installCertifiedSubtrees(*group, 1024, true, true) == 0 && !original->getDrawCallback(),
        "absolute transform admitted nested callbacks");
    transform->setReferenceFrame(osg::Transform::RELATIVE_RF);
    group->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    require(Packet::installCertifiedSubtrees(*group, 1024, true, true) == 0 && !original->getDrawCallback(),
        "transparent ancestor admitted nested callbacks");
    group->setStateSet(nullptr);
    group->setDataVariance(osg::Object::DYNAMIC);
    require(Packet::installCertifiedSubtrees(*group, 1024, true, true) == 0 && !original->getDrawCallback(),
        "dynamic ancestry admitted nested callbacks");
    group->setDataVariance(osg::Object::STATIC);
    group->setCullCallback(new osg::NodeCallback);
    require(Packet::installCertifiedSubtrees(*group, 1024, true, true) == 0 && !original->getDrawCallback(),
        "custom ancestry callback admitted nested geometry");
    group->setCullCallback(nullptr);
    osg::ref_ptr<osg::Switch> customRoot = new osg::Switch;
    customRoot->addChild(original);
    require(Packet::installCertifiedSubtrees(*customRoot, 1024, true, true) == 0 && !original->getDrawCallback(),
        "custom group semantics were reinterpreted as certified ancestry");
    customRoot = nullptr;
    require(Packet::installCertifiedSubtrees(*group, 2, true, true) == 0 && !original->getDrawCallback(),
        "node capacity limit did not preserve ordinary geometry");
    require(Packet::installCertifiedSubtrees(*group, 1024, true, true) == 1,
        "eligible nested production indirect callback not installed");
    const auto* draw = dynamic_cast<const Packet::Draw*>(original->getDrawCallback());
    require(draw, "candidate is not the production callback");
    const auto builtPackets = SceneUtil::PersistentOpaquePackets::Counters::built.load();
    auto packet = SceneUtil::PersistentOpaquePackets::buildCertifiedSubtrees(*group, 1024, true, true);
    require(SceneUtil::PersistentOpaquePackets::Counters::built.load() > builtPackets,
        "nested MDI and persistent packets did not stack");

    osgViewer::Viewer viewer;
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.setLightingMode(osgViewer::View::NO_LIGHT);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32; traits->height = 32; traits->doubleBuffer = false; traits->windowDecoration = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context, "no actual GL fixture context");
    auto* camera = viewer.getCamera();
    camera->setGraphicsContext(context); camera->setViewport(0, 0, 32, 32);
    camera->setProjectionMatrix(osg::Matrix::identity()); camera->setViewMatrix(osg::Matrix::identity());
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    camera->setCullingMode(osg::CullSettings::NO_CULLING);
    camera->setClearColor(osg::Vec4(0, 0, 0, 1));
    osg::ref_ptr<GlState> previous = new GlState;
    osg::ref_ptr<Pixels> pixels = new Pixels;
    pixels->previous = previous;
    camera->setPreDrawCallback(previous); camera->setFinalDrawCallback(pixels);
    viewer.setSceneData(controlRoot); viewer.realize();
    require(viewer.isRealized(), "fixture context did not realize");
    auto compare = [&] {
        pixels->inspect = nullptr;
        viewer.setSceneData(controlRoot); viewer.frame(); const auto reference = pixels->data;
        pixels->inspect = draw->testPrimitive();
        viewer.setSceneData(packet); viewer.frame();
        require(pixels->data == reference, "MDI/GPU visibility changed exact RGBA output");
    };
    compare();
    require(Packet::Counters::draws.load() > 0 && Packet::Counters::dispatches.load() > 0,
        "candidate performed no real indirect draw/compute visibility dispatch");
    const auto uploads = Packet::Counters::commandUploads.load();
    const auto avoided = Packet::Counters::commandUploadsAvoided.load();
    const auto consumed = Packet::Counters::draws.load();
    // At least one slot contains a real GPU-written visibility result. Return
    // to CPU commands, then reuse both slots, comparing actual pixels each time.
    Packet::Primitive::forceCpuCommands = true;
    for (unsigned i=0;i<12;++i) compare();
    Packet::Primitive::forceCpuCommands = false;
    require(Packet::Counters::draws.load() == consumed+12,"command reuse fixture fell back instead of drawing");
    if (reuse)
        require(Packet::Counters::commandUploads.load() <= uploads+2
            && Packet::Counters::commandUploads.load() > uploads
            && Packet::Counters::commandUploadsAvoided.load() >= avoided+10,
            "GPU-written commands were not restored once before immutable slot reuse");
    else
        require(Packet::Counters::commandUploads.load() == uploads+12
            && Packet::Counters::commandUploadsAvoided.load() == avoided,
            "control no longer performs its original per-draw command upload");
    std::cout<<"INDIRECT_COMMAND_REUSE_PROOF enabled="<<reuse
        <<" uploads="<<Packet::Counters::commandUploads.load()-uploads
        <<" avoided="<<Packet::Counters::commandUploadsAvoided.load()-avoided<<" exact_pixels=1\n";
    compare(); // Restore a GPU visibility result before inspecting its rejection.
    require(pixels->commands.size() == 3 && pixels->commands[0].instanceCount == 1
        && pixels->commands[1].instanceCount == 1 && pixels->commands[2].instanceCount == 0,
        "GPU did not conservatively reject the actual offscreen cluster");
    require(std::count(pixels->data.begin(), pixels->data.end(), 255) > 1024, "fixture drew no visible geometry");
    for (unsigned view = 0; view < 6; ++view)
    {
        camera->setName(view % 3 == 0 ? "SceneCam" : view % 3 == 1 ? "RefractionCamera" : "ShadowCamera");
        camera->setViewMatrix(view % 2 ? osg::Matrix::scale(-1, 1, 1) : osg::Matrix::translate(.1 * view, 0, 0));
        compare();
    }
    camera->setViewMatrix(osg::Matrix::identity());
    auto program = fixtureProgram(true);
    osg::ref_ptr<osg::StateSet> material = new osg::StateSet;
    material->setAttribute(program);
    material->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf::identity()));
    original->setStateSet(material); control->setStateSet(material);
    // Changing the material producer invalidates the installed aggregate. Build
    // a fresh exact original callback and packet after the producer mutation.
    original->setDrawCallback(nullptr);
    group = nestedRoot(original);
    require(Packet::installCertifiedSubtrees(*group, 1024, true, true) == 1, "nested PBR contract candidate did not install");
    draw = dynamic_cast<const Packet::Draw*>(original->getDrawCallback());
    packet = SceneUtil::PersistentOpaquePackets::buildCertifiedSubtrees(*group, 1024, true, true);
    compare();
    const auto dispatched = Packet::Counters::dispatches.load();
    require(dispatched >= 8, "certified PBR projection did not reach GPU");
    osg::ref_ptr<osg::Uniform> inherited = new osg::Uniform("projectionMatrix", osg::Matrixf::scale(.5f, .5f, 1));
    camera->getOrCreateStateSet()->addUniform(inherited, osg::StateAttribute::OVERRIDE);
    compare();
    material->addUniform(material->getUniform("projectionMatrix"), osg::StateAttribute::PROTECTED);
    compare();
    camera->getStateSet()->removeUniform("projectionMatrix");

    auto requireOriginal = [&] {
        const auto before = Packet::Counters::draws.load();
        const auto fallback = Packet::Counters::fallbacks.load();
        compare();
        require(Packet::Counters::draws.load() == before && Packet::Counters::fallbacks.load() > fallback,
            "unsupported current material/inherited mode did not use original primitives");
    };
    // The producer pointer remains unchanged: these are real in-place material
    // mutations and inherited effective modes after callback installation.
    material->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    requireOriginal();
    material->setRenderingHint(osg::StateSet::OPAQUE_BIN);
    material->setMode(GL_BLEND, osg::StateAttribute::ON);
    requireOriginal();
    material->removeMode(GL_BLEND);
    for (GLenum mode : { GL_BLEND, GL_ALPHA_TEST, GL_STENCIL_TEST })
    {
        camera->getOrCreateStateSet()->setMode(mode, osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
        requireOriginal();
        camera->getStateSet()->removeMode(mode);
    }
    const auto beforeResume = Packet::Counters::draws.load();
    compare();
    require(Packet::Counters::draws.load() > beforeResume, "restored opaque state never resumed indirect drawing");

    const auto unknown = Packet::Counters::unknownShaders.load();
    auto* shader = program->getShader(0);
    shader->setShaderSource(shader->getShaderSource() + "\n// consumed source revision changed\n");
    require(!Contract::matches(*program), "source revision mutation retained certificate");
    compare();
    require(Packet::Counters::unknownShaders.load() > unknown, "changed shader did not fail open");
    require(Packet::Counters::dispatches.load() == dispatched + 3,
        "unknown shader consumed old GPU rejection rather than original draw");
    // Restore a fresh actual consumed-source certificate for this controlled
    // fixture before exercising command-slot release/publication.
    program = fixtureProgram(true); material->setAttribute(program);
    const auto busy = Packet::Counters::busySlots.load();
    Packet::Primitive::forceBusy = true; compare(); Packet::Primitive::forceBusy = false;
    require(Packet::Counters::busySlots.load() > busy, "busy command slots did not take control");
    context->makeCurrent();
    for (unsigned generation = 0; generation < 4; ++generation)
    {
        Packet::SyncRetirement::holdRetirement = true;
        draw->testPrimitive()->releaseGLObjects(context->getState());
        // Deliberately keep the previous storage in the exact owner's pending
        // queue. Repeated draws/releases must not allocate another generation.
        const auto before = Packet::Counters::draws.load();
        context->releaseContext(); compare(); context->makeCurrent();
        require(Packet::Counters::draws.load() == before, "pending storage generation was reallocated");
        draw->testPrimitive()->releaseGLObjects(context->getState());
        Packet::SyncRetirement::holdRetirement = false;
        context->getState()->get<Packet::SyncRetirement>()->flushAllDeletedGLObjects();
        context->releaseContext(); compare(); context->makeCurrent();
        require(Packet::Counters::draws.load() > before, "retired storage generation never resumed");
    }
    context->releaseContext();
    auto* positions = static_cast<osg::Vec3Array*>(original->getVertexArray());
    (*positions)[0].x() += .1f; positions->dirty(); original->dirtyBound(); control->dirtyBound();
    const auto fallback = Packet::Counters::fallbacks.load();
    compare();
    require(Packet::Counters::fallbacks.load() > fallback, "changed producer used stale aggregate");
    auto unsupported = geometry(); unsupported->setDataVariance(osg::Object::DYNAMIC);
    require(!Packet::install(*unsupported, true, true), "dynamic geometry was incorrectly admitted");
    unsupported->setDataVariance(osg::Object::STATIC);
    unsupported->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    require(!Packet::install(*unsupported, true, true), "sorted geometry was incorrectly admitted");
    unsupported->getStateSet()->setRenderingHint(osg::StateSet::OPAQUE_BIN);
    // Both CPU and GPU variants cluster a single long primitive. A primitive-
    // set count of one does not explain zero paging installations.
    auto single = geometry();
    single->removePrimitiveSet(0, single->getNumPrimitiveSets());
    osg::ref_ptr<osg::DrawElementsUShort> longPrimitive = new osg::DrawElementsUShort(GL_TRIANGLES);
    for (unsigned i = 0; i < 1025; ++i)
        for (unsigned short vertex = 0; vertex < 3; ++vertex) longPrimitive->push_back(vertex);
    single->addPrimitiveSet(longPrimitive);
    osg::ref_ptr<osg::Geometry> singleControl = new osg::Geometry(*single, osg::CopyOp::SHALLOW_COPY);
    auto singleRoot = nestedRoot(single);
    auto singleControlRoot = nestedRoot(singleControl);
    require(Packet::installCertifiedSubtrees(*singleRoot, 1024, true, false) == 1,
        "CPU indirect must support one long primitive under retained Geode/transform ancestry");
    const auto* singleDraw = dynamic_cast<const Packet::Draw*>(single->getDrawCallback());
    require(singleDraw, "single primitive is not the real production callback");
    pixels->inspect = nullptr; viewer.setSceneData(singleControlRoot); viewer.frame(); const auto singlePixels = pixels->data;
    pixels->inspect = singleDraw->testPrimitive(); viewer.setSceneData(singleRoot); viewer.frame();
    require(pixels->data == singlePixels && pixels->commands.size() == 2,
        "single long primitive clustering changed exact nested draw pixels or command count");
    std::cout << "INDIRECT_NESTED_PROOF cpu_single_primitive_commands=2 exact_pixels=1 original_ancestry=1"
        " transform_preserved=1 opacity_dynamic_capacity_fallback=1 nested_built=" << Packet::Counters::nestedBuilt.load() << '\n';
    viewer.setSceneData(nullptr); pixels->inspect = nullptr; single->setDrawCallback(nullptr);
    sampleValue.decision.state = Resource::OpenGlPressure::Critical;
    require(!Packet::install(*unsupported, true, true), "critical pressure admitted candidate allocation");
    viewer.setSceneData(nullptr);
    auto* renderer = dynamic_cast<osgViewer::Renderer*>(camera->getRenderer());
    for (unsigned i = 0; i < 2; ++i) renderer->getSceneView(i)->setSceneData(nullptr);
    // Recursive packet construction replaces children in the private root.
    // Release that owner as well as the returned root before checking quotas.
    packet = nullptr; group = nullptr; original->setDrawCallback(nullptr);
    context->makeCurrent();
    context->getState()->get<Packet::SyncRetirement>()->flushAllDeletedGLObjects();
    require(Packet::Counters::liveBytes.load() == initialBytes && ledger.stats().estimatedOwnerBytes == 0,
        "indirect capacity/ledger fence retirement leaked");
    context->getState()->get<osg::GLExtensions>()->glDeleteBuffers(3, previous->buffers.data());
    context->releaseContext();
    std::cout << "Actual MDI/compute pixels, positive rejection, view/PBR uniform parity, exact binding restoration,"
        " current opacity/inherited modes, producer/shader revisions, bounded busy fallback and quota retirement passed\n";
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
