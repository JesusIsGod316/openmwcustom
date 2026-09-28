#include <components/sceneutil/groundcoverbatch.hpp>
#include <osg/Geode>
#include <osg/Viewport>
#include <osgUtil/SceneView>
#include <iostream>
#include <stdexcept>

namespace G = SceneUtil::GroundcoverBatch;
namespace P = SceneUtil::GroundcoverPolicy;
static int checks = 0;
static void require(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
struct GeometryList : osg::NodeVisitor
{
    GeometryList() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
    std::vector<osg::Geometry*> values;
    void apply(osg::Geometry& geometry) override { values.push_back(&geometry); }
};
static osg::ref_ptr<osg::Geometry> mesh()
{
    osg::ref_ptr<osg::Geometry> value = new osg::Geometry;
    // Off-centre geometry exercises the old origin-centred radius weakness.
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    vertices->push_back({ 10.f, -1.f, 0.f });
    vertices->push_back({ 12.f, -1.f, 4.f });
    vertices->push_back({ 10.f, 1.f, 4.f });
    value->setVertexArray(vertices);
    osg::ref_ptr<osg::Vec2Array> uv = new osg::Vec2Array;
    uv->push_back({ 0.f, 0.f }); uv->push_back({ 1.f, 0.f }); uv->push_back({ 0.f, 1.f });
    value->setTexCoordArray(0, uv);
    osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    indices->push_back(0); indices->push_back(1); indices->push_back(2);
    value->addPrimitiveSet(indices);
    return value;
}
static std::vector<P::Instance> instances()
{
    std::vector<P::Instance> result(256);
    for (std::size_t i = 0; i < result.size(); ++i)
    {
        result[i].position = { (static_cast<float>(i % 16) - 8.f) * 10.f, 0.f, static_cast<float>(i / 16) * 10.f };
        result[i].rotation = { .07f * float(i % 4), -.03f * float(i % 5), float(i) * .07f };
        result[i].scale = .5f + float(i % 7) * .25f;
        result[i].identity = i;
        result[i].rank = P::rank(i);
    }
    P::sortRanks(result);
    return result;
}
int main()
try
{
    auto source = mesh();
    const auto vertexPointer = source->getVertexArray();
    const auto uvPointer = source->getTexCoordArray(0);
    const auto primitivePointer = source->getPrimitiveSet(0);
    osg::ref_ptr<osg::Geode> templateRoot = new osg::Geode;
    templateRoot->addDrawable(source);
    G::EligibilityVisitor allowed;
    templateRoot->accept(allowed);
    require(allowed.eligible && allowed.drawables == 1, "plain static geometry must qualify");
    auto values = instances();
    P::Options options;
    auto counter = std::make_shared<G::Counters>();
    auto tile = G::buildTile(*templateRoot, values, true, options, 20000.f, {}, counter);
    GeometryList list;
    tile->accept(list);
    require(list.values.size() == 3, "compile visitor must see all three tiers");
    for (unsigned int i = 0; i < 3; ++i)
    {
        auto* geometry = list.values[i];
        require(geometry->getVertexArray() == vertexPointer, "immutable source vertices must be shared");
        require(geometry->getTexCoordArray(0) == uvPointer, "UV array must not be duplicated/rebound");
        require(geometry->getPrimitiveSet(0) != primitivePointer, "instance counts must be private");
        require(geometry->getPrimitiveSet(0)->getNumInstances() == static_cast<int>(P::tierCount(values, i)), "real instance count differs from LOD prefix");
        require(geometry->getVertexAttribArray(6) == list.values[0]->getVertexAttribArray(6), "tiers duplicate offsets");
        require(geometry->getVertexAttribArray(7) == list.values[0]->getVertexAttribArray(7), "tiers duplicate rotations");
        require(geometry->getVertexAttribArray(8) == nullptr && geometry->getVertexAttribArray(9) == nullptr, "UV-aliased generic slots are forbidden");
        auto* rotations = dynamic_cast<osg::Vec4Array*>(geometry->getVertexAttribArray(7));
        require(rotations && rotations->size() == values.size(), "rank must use slot7.w, not new attributes");
        for (std::size_t j = 0; j < values.size(); ++j)
            require((*rotations)[j] == osg::Vec4f(values[j].rotation[0], values[j].rotation[1], values[j].rotation[2], values[j].rank), "Euler/rank data differs");
        require(geometry->getVertexAttribArray(6)->getVertexBufferObject() == rotations->getVertexBufferObject(), "instance streams should share one VBO");
        for (const auto& v : values)
            for (unsigned int corner = 0; corner < 8; ++corner)
            {
                const auto p = source->getBoundingBox().corner(corner);
                const auto rotated = P::rotate({ p.x(), p.y(), p.z() }, v.rotation);
                require(geometry->getBoundingBox().contains(osg::Vec3f(rotated[0]*v.scale+v.position[0], rotated[1]*v.scale+v.position[1], rotated[2]*v.scale+v.position[2])), "transformed off-centre vertex escaped bounds");
            }
    }
    require(source->getPrimitiveSet(0)->getNumInstances() == 0, "source instance count was mutated");
    require(source->getVertexAttribArray(6) == nullptr && source->getVertexAttribArray(7) == nullptr, "source attributes were mutated");
    auto tile2 = G::buildTile(*templateRoot, values, false, options, 20000.f, {}, counter);
    GeometryList second;
    tile2->accept(second);
    require(second.values.size() == 1, "nonLOD hierarchy must retain one draw per source mesh");
    require(second.values[0]->getVertexArray() == vertexPointer, "tiles must share immutable vertex data");
    require(dynamic_cast<osg::Vec3Array*>(second.values[0]->getVertexAttribArray(7)), "nonLOD uses original Vec3 rotation layout");

    auto distantValues = values;
    for (auto& value : distantValues) value.position[0] += 10000.f;
    auto distantTile = G::buildTile(*templateRoot, distantValues, false, options, 20000.f, {}, counter);
    GeometryList distantGeometry;
    distantTile->accept(distantGeometry);
    require(distantGeometry.values[0]->getBoundingBox().xMin() > 9000.f,
        "source vertices at the chunk origin must not inflate instance tile bounds");

    // Real OSG cull traversal, alternating independent current-view camera states.
    osg::ref_ptr<osgUtil::SceneView> scene = new osgUtil::SceneView;
    scene->setDefaults();
    scene->setViewport(new osg::Viewport(0, 0, 512, 512));
    scene->setProjectionMatrixAsPerspective(60.0, 1.0, 1.0, 30000.0);
    scene->setSceneData(tile);
    scene->getCamera()->setName("test-main");
    auto state = tile->getOrCreateStateSet();
    state->addUniform(new osg::Uniform("windSpeed", 5.f));
    state->addUniform(new osg::Uniform("projectionMatrix", osg::Matrixf(scene->getProjectionMatrix())));
    auto cull = [&](double distance) {
        auto before = counter->submittedInstances.load();
        scene->setViewMatrixAsLookAt(osg::Vec3f(0.f, static_cast<float>(-distance), 80.f),
            osg::Vec3f(0.f, 0.f, 80.f), osg::Vec3f(0.f, 0.f, 1.f));
        scene->cull();
        return counter->submittedInstances.load() - before;
    };
    require(cull(1000.0) == 256, "near camera lost full detail");
    require(cull(15000.0) == P::tierCount(values, 2), "far camera failed to reduce actual submitted instances");
    require(cull(1000.0) == 256, "camera cut must restore full detail immediately");
    osg::ref_ptr<osgUtil::SceneView> reflection = new osgUtil::SceneView;
    reflection->setDefaults();
    reflection->setViewport(new osg::Viewport(0,0,512,512));
    reflection->setProjectionMatrixAsPerspective(60.0, 1.0, 1.0, 30000.0);
    reflection->setViewMatrixAsLookAt({0.0,-1000.0,80.0}, {0.0,0.0,80.0}, {0.0,0.0,1.0});
    reflection->setSceneData(tile);
    reflection->getCamera()->setName("test-reflection");
    cull(15000.0);
    auto before = counter->submittedInstances.load();
    reflection->cull();
    require(counter->submittedInstances.load()-before == 256, "main-camera LOD contaminated another camera");
    before = counter->submittedInstances.load();
    scene->setViewMatrixAsLookAt({0.0,-1000.0,80.0},{0.0,-2000.0,80.0},{0.0,0.0,1.0});
    scene->cull();
    require(counter->submittedInstances.load() == before, "offscreen tile wasn't frustum-rejected");

    // Occlusion result is current-view supplied; invalid state must not consume rejection.
    auto rejected = G::buildTile(*templateRoot, values, false, options, 20000.f,
        [](osgUtil::CullVisitor&, const osg::BoundingBox&, std::uint64_t){return false;}, counter);
    rejected->getOrCreateStateSet()->addUniform(new osg::Uniform("windSpeed", 5.f));
    scene->setSceneData(rejected);
    before = counter->submittedInstances.load();
    cull(1000.0);
    require(counter->submittedInstances.load() == before, "current occlusion did not reject hidden tile");
    rejected->getOrCreateStateSet()->getUniform("windSpeed")->set(std::numeric_limits<float>::quiet_NaN());
    require(cull(1000.0) == 256, "invalid wind bounds must fail open");

    osg::ref_ptr<osg::StateSet> order = new osg::StateSet;
    const auto defaultSort = osgUtil::RenderBin::getDefaultRenderBinSortMode();
    G::enableFrontToBack(*order);
    require(order->getBinNumber() == 1, "grass ordering bin must be separate");
    require(osgUtil::RenderBin::getRenderBinPrototype("OptimizedMWGroundcoverFrontToBack")->getSortMode() == osgUtil::RenderBin::SORT_BY_STATE_THEN_FRONT_TO_BACK, "ordering mode not implemented");
    require(osgUtil::RenderBin::getDefaultRenderBinSortMode() == defaultSort, "global sorter was changed");
    source->setUserValue("shaderPrefix", std::string("custom-grass"));
    G::EligibilityVisitor customShader;
    templateRoot->accept(customShader);
    require(!customShader.eligible, "custom shader prefixes require the established fallback");
    source->setUserValue("shaderPrefix", std::string("groundcover"));
    G::EligibilityVisitor builtInShader;
    templateRoot->accept(builtInShader);
    require(builtInShader.eligible, "built-in groundcover prefix should remain eligible");
    source->setDataVariance(osg::Object::DYNAMIC);
    G::EligibilityVisitor blocked;
    templateRoot->accept(blocked);
    require(!blocked.eligible, "dynamic templates must fall back");
    std::cout << checks << " native OSG P8G3 checks passed\n";
}
catch(const std::exception& e){std::cerr << e.what() << '\n'; return 1;}
