#include <components/sceneutil/persistentopaquepackets.hpp>
#include <osg/Geode>
#include <osg/Shader>
#include <osg/Uniform>
#include <osgViewer/Viewer>

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

namespace Packet = SceneUtil::PersistentOpaquePackets;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Pixels final : osg::Camera::DrawCallback
{
    mutable std::array<unsigned char, 32 * 32 * 4> data{};
    void operator()(osg::RenderInfo&) const override
    {
        glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
    }
};

struct CullCallback final : osg::DrawableCullCallback
{
    mutable unsigned calls = 0;
    bool cull(osg::NodeVisitor*, osg::Drawable*, osg::RenderInfo*) const override
    {
        ++calls;
        return false;
    }
};

struct GeometryCounter final : osg::NodeVisitor
{
    GeometryCounter() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
    unsigned calls = 0;
    void apply(osg::Geometry&) override { ++calls; }
};

struct CaptureAttribute final : osg::StateAttribute
{
    CaptureAttribute() = default;
    CaptureAttribute(const CaptureAttribute& copy, const osg::CopyOp& op) : osg::StateAttribute(copy, op) {}
    META_StateAttribute(Test, CaptureAttribute, FOG)
    int compare(const osg::StateAttribute& other) const override
    { return this == &other ? 0 : (this < &other ? -1 : 1); }
    void apply(osg::State&) const override
    {
        // Deep stores individual leaves only above its .25ms threshold. The
        // deliberately slow fixture attribute guarantees a retained metadata
        // row without relying on an accidentally slow driver or first link.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
};

osg::ref_ptr<osg::Geometry> geometry(float x, osg::StateSet* material)
{
    osg::ref_ptr<osg::Geometry> value = new osg::Geometry;
    value->setName("PersistentPacket-PBR-Original");
    value->setDataVariance(osg::Object::STATIC);
    value->setUseVertexBufferObjects(true);
    value->setUseDisplayList(false);
    value->setStateSet(material);
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    vertices->push_back(osg::Vec3(x, -.75f, -.1f));
    vertices->push_back(osg::Vec3(x + .4f, -.75f, -.1f));
    vertices->push_back(osg::Vec3(x + .2f, .75f, -.1f));
    value->setVertexArray(vertices);
    value->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
    return value;
}

int main(int argc, char** argv) try
{
    const bool deep = argc > 1 && std::string(argv[1]) == "--deep";
    const auto initialBytes = Packet::Counters::liveBytes.load();
    osg::ref_ptr<osg::StateSet> material = new osg::StateSet;
    material->setDataVariance(osg::Object::STATIC);
    material->addUniform(new osg::Uniform("materialColor", osg::Vec4(1, .25f, .05f, 1)));
    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX,
        "#version 120\nvoid main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}\n"));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
        "#version 120\nuniform vec4 materialColor;uniform float viewScale;"
        "void main(){gl_FragColor=vec4(materialColor.rgb*viewScale,materialColor.a);}\n"));
    material->setAttribute(program);
    if (deep) material->setAttribute(new CaptureAttribute);
    osg::ref_ptr<osg::Group> source = new osg::Group;
    source->getOrCreateStateSet()->addUniform(new osg::Uniform("viewScale", 1.f));
    std::array<osg::ref_ptr<osg::Geometry>, 4> children;
    for (unsigned i = 0; i < children.size(); ++i)
    {
        children[i] = geometry(-.9f + .45f * static_cast<float>(i), material);
        source->addChild(children[i]);
    }
    children[3]->setNodeMask(2);
    osg::ref_ptr<CullCallback> custom = new CullCallback;
    children[2]->setCullCallback(custom);

    auto disabled = Packet::build(*source, false);
    require(disabled.get() == source.get(), "disabled control allocated packet topology");
    auto candidate = Packet::build(*source, true);
    auto* packet = dynamic_cast<Packet::Group*>(candidate.get());
    require(packet && packet->rowCount() == 4, "eligible production packet was not built");
    require(Packet::Counters::liveBytes.load() > initialBytes, "metadata quota was not charged");
    GeometryCounter nonCull;
    candidate->accept(nonCull);
    require(nonCull.calls == 4, "compile/update/intersection visitors duplicated or lost original geometry");

    osgViewer::Viewer viewer;
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    viewer.setLightingMode(osgViewer::View::NO_LIGHT);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32; traits->height = 32; traits->doubleBuffer = false; traits->windowDecoration = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(), "no real OpenGL fixture context");
    auto* camera = viewer.getCamera();
    camera->setGraphicsContext(context); camera->setViewport(0, 0, 32, 32);
    camera->setProjectionMatrix(osg::Matrix::identity());
    camera->setViewMatrix(osg::Matrix::identity());
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    camera->setCullingMode(osg::CullSettings::VIEW_FRUSTUM_CULLING);
    camera->setClearColor(osg::Vec4(0, 0, 0, 1));
    osg::ref_ptr<Pixels> pixels = new Pixels;
    camera->setFinalDrawCallback(pixels);
    viewer.setSceneData(source);
    viewer.realize();
    require(viewer.isRealized(), "fixture context did not realize");
    viewer.frame(); // Initialize SceneViews before replacing their cull visitors.
    if (deep)
    {
        require(SceneUtil::DrawPhaseTrace::Capture::instance().enabled(), "deep fixture needs trace output");
        require(SceneUtil::DrawPhaseTrace::install(viewer) == 2, "deep production visitors did not install");
    }
    auto compare = [&] {
        // OSG can publish its near/far clamp back to the camera. Both arms must
        // start from the identical numeric projection, rather than seeding the
        // candidate with the already-clamped control output.
        const osg::Matrix projectionInput = camera->getProjectionMatrix();
        viewer.setSceneData(source); viewer.frame();
        const auto reference = pixels->data;
        const auto nearValue = camera->getProjectionMatrix();
        camera->setProjectionMatrix(projectionInput);
        viewer.setSceneData(candidate); viewer.frame();
        require(pixels->data == reference, "persistent packet changed exact RGBA output");
        if (camera->getProjectionMatrix() != nearValue)
        {
            std::cerr << "near/far mismatch mode=" << camera->getComputeNearFarMode() << '\n';
            for (unsigned r = 0; r < 4; ++r)
            {
                for (unsigned c = 0; c < 4; ++c)
                    std::cerr << r << ',' << c << ':' << nearValue(r, c) << '/'
                        << camera->getProjectionMatrix()(r, c) << ' ';
                std::cerr << '\n';
            }
            throw std::runtime_error("near/far projection result differs");
        }
    };
    compare();
    require(Packet::Counters::submitted.load() >= 3, "candidate actually submitted no eligible geometry");
    require(Packet::Counters::statePushesAvoided.load() >= 1, "same-material StateGraph work was not removed");
    require(custom->calls >= 2, "custom cull callback did not take original path");
    require(std::count(pixels->data.begin(), pixels->data.end(), 255) > 1024, "fixture rendered no geometry");
    for (unsigned view = 0; view < 3; ++view)
    {
        camera->setName(view == 0 ? "SceneCam" : view == 1 ? "RefractionCamera" : "ShadowCamera");
        camera->setViewMatrix(view == 1 ? osg::Matrix::scale(-1, 1, 1)
            : osg::Matrix::translate(.15 * view, .05 * view, -.5 * view));
        camera->setCullMask(view == 2 ? 1 : ~0u);
        source->getStateSet()->getUniform("viewScale")->set(.75f + .1f * view);
        compare();
    }
    camera->setProjectionMatrixAsPerspective(60., 1., .1, 100.);
    camera->setViewMatrix(osg::Matrix::translate(0, 0, -3));
    camera->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);
    compare();
    camera->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_PRIMITIVES);
    compare();

    const auto changes = Packet::Counters::changedProducers.load();
    auto* changed = static_cast<osg::Vec3Array*>(children[0]->getVertexArray());
    (*changed)[0].x() += .2f; changed->dirty(); children[0]->dirtyBound();
    compare();
    require(Packet::Counters::changedProducers.load() > changes, "changed vertex producer did not invalidate packet row");
    children[1]->getPrimitiveSet(0)->setMode(GL_LINE_LOOP);
    compare();
    source->replaceChild(children[1], geometry(0, material));
    compare();
    source->addChild(geometry(-.2f, material));
    compare();
    source->removeChildren(4, 1);
    source->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
    compare();
    source->getStateSet()->setMode(GL_BLEND, osg::StateAttribute::OFF);
    source->setCullCallback(new osg::NodeCallback);
    compare();
    source->setCullCallback(nullptr);
    osg::ref_ptr<osg::StateSet> transparent = new osg::StateSet;
    transparent->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
    require(!Packet::opaque(transparent), "sorted transparent state incorrectly eligible");
    if (deep)
    {
        require(SceneUtil::DrawPhaseTrace::Capture::instance().count() != 0, "deep packet submission lost leaf coverage");
        bool owner = false;
        for (std::size_t i = 0; i < SceneUtil::DrawPhaseTrace::Capture::instance().count(); ++i)
        {
            const auto row = SceneUtil::DrawPhaseTrace::Capture::instance().row(i);
            owner |= std::string(row.ownerName.data()) == "PersistentPacket-PBR-Original"
                && row.vertices == 3 && row.primitiveSets == 1;
        }
        require(owner, "direct packet submission lost exact geometry metadata");
    }

    // Production paging keeps material-bearing Geodes under immutable ancestry.
    // The old immediate-child-only attachment never reached those drawables.
    osg::ref_ptr<osg::Group> nested = new osg::Group;
    osg::ref_ptr<osg::MatrixTransform> nestedTransform = new osg::MatrixTransform(osg::Matrix::translate(.025,0,0));
    nestedTransform->setDataVariance(osg::Object::STATIC);
    osg::ref_ptr<osg::Group> materialAncestor = new osg::Group;
    materialAncestor->setStateSet(material);
    materialAncestor->getStateSet()->getOrCreateUniform("viewScale",osg::Uniform::FLOAT)->set(.9f);
    osg::ref_ptr<osg::Geode> nestedGeode = new osg::Geode;
    for (unsigned i = 0; i < 3; ++i) nestedGeode->addDrawable(geometry(-.8f + .5f*i,nullptr));
    materialAncestor->addChild(nestedGeode); nestedTransform->addChild(materialAncestor); nested->addChild(nestedTransform);
    osg::ref_ptr<osg::Node> nestedControl = osg::clone(nested.get(), osg::CopyOp::DEEP_COPY_NODES
        | osg::CopyOp::DEEP_COPY_DRAWABLES | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES);
    const auto nestedSubmitted = Packet::Counters::submitted.load();
    const auto nestedBuilt = Packet::Counters::built.load();
    auto nestedCandidate = Packet::buildCertifiedSubtrees(*nested,256,true);
    require(Packet::Counters::built > nestedBuilt, "certified material-bearing Geode did not build packets");
    require(nested->getChild(0) == nestedTransform && nestedTransform->getMatrix() == osg::Matrix::translate(.025,0,0),
        "recursive attachment changed transform ancestry");
    camera->setProjectionMatrix(osg::Matrix::identity()); camera->setViewMatrix(osg::Matrix::identity());
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR); camera->setCullMask(~0u);
    viewer.setSceneData(nestedControl); viewer.frame(); const auto nestedPixels = pixels->data;
    viewer.setSceneData(nestedCandidate); viewer.frame();
    require(pixels->data == nestedPixels && Packet::Counters::submitted > nestedSubmitted,
        "nested production material/transform pixel parity and actual submission");
    osg::ref_ptr<osg::Group> badAncestry = new osg::Group;
    badAncestry->setCullCallback(new osg::NodeCallback); badAncestry->addChild(nestedControl);
    require(Packet::buildCertifiedSubtrees(*badAncestry,256,true) == badAncestry,
        "ancestor callback was laundered into packet ancestry");
    const auto boundedBuilt = Packet::Counters::built.load();
    require(Packet::buildCertifiedSubtrees(*nestedControl,1,true) == nestedControl && Packet::Counters::built == boundedBuilt,
        "recursive node bound must preserve untouched original");
    // Later ancestry changes still take the retained original traversal.
    materialAncestor->setCullCallback(new osg::NodeCallback);
    const auto ancestryFallback = Packet::Counters::fallbacks.load(); viewer.frame();
    require(Packet::Counters::fallbacks > ancestryFallback, "changed material ancestor must fall back");
    materialAncestor->setCullCallback(nullptr);
    viewer.setSceneData(nullptr);
    auto* renderer = dynamic_cast<osgViewer::Renderer*>(camera->getRenderer());
    require(renderer != nullptr, "fixture renderer missing at retirement");
    for (unsigned i = 0; i < 2; ++i) renderer->getSceneView(i)->setSceneData(nullptr);
    candidate = nullptr;
    nestedCandidate = nullptr; nested = nullptr; nestedTransform = nullptr; materialAncestor = nullptr; nestedGeode = nullptr;
    require(Packet::Counters::liveBytes.load() == initialBytes, "retired packet metadata quota leaked");
    std::cout << "Persistent opaque packets: pixel, inherited material, camera, masks, near/far, producer revision,"
        " custom callback, structural fallback, visitor, quota retirement and positive submission checks passed\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
