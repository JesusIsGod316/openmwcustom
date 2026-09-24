#include <components/render/backend/vsg/nativevisibility.hpp>
#include <components/render/backend/vsg/effectvisibility.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <iostream>
#include <stdexcept>

using namespace RenderVsg;
using namespace RenderCore;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
int main()
{
    try
    {
        ImmediateEffectDraw effect;
        effect.mesh.positions = {{-2,-3,-4}, {5,6,7}, {1,0,0}};
        effect.mesh.indices = {0,1,2}; effect.mesh.surfaces = {{PrimitiveTopology::Triangles,0,3,0}};
        effect.bounds = {}; // deliberately wrong authored bounds must not hide live vertices
        auto bound = effectCullBound(effect);
        require(bound.has_value(), "ordinary evaluated geometry must be bounded");
        for (const auto& p : effect.mesh.positions)
            require(glm::length(glm::dvec3(p) - glm::dvec3(bound->x,bound->y,bound->z)) <= bound->r, "effect bound missed vertex");
        effect.meshSnapshot = std::make_shared<const FrozenEffectMesh>(effect.mesh);
        effect.mesh = {};
        require(effectCullBound(effect)->r == bound->r, "frozen bounds changed");
        effect.billboard = ModelBillboardMode::AlwaysFaceCamera;
        bound = effectCullBound(effect);
        require(bound && bound->x == 0 && bound->y == 0 && bound->z == 0 && bound->r >= std::sqrt(110.0), "billboard rotation envelope");
        effect.material.treeAnimation = true;
        require(!effectCullBound(effect), "shader displacement must fail open");
        effect.material.treeAnimation = false; effect.material.stencil.enabled = true;
        require(!effectCullBound(effect), "stencil side effects must fail open");
        effect.material.stencil.enabled = false; effect.material.depthTest = false;
        require(!effectCullBound(effect), "depthless effect must fail open");
        effect.material.depthTest = true; effect.mesh = effect.meshSnapshot->mesh(); effect.meshSnapshot.reset();
        effect.mesh.surfaces[0].topology = PrimitiveTopology::Points;
        require(!effectCullBound(effect), "screen-space points must fail open");
        effect.mesh.surfaces[0].topology = PrimitiveTopology::Lines;
        require(!effectCullBound(effect), "screen-space lines must fail open");
        FrameView view;
        view.current.projection.matrix = glm::perspectiveRH_ZO(glm::radians(90.f), 1.f, 100.f, .1f);
        view.current.projection.matrix[1][1] *= -1.f;
        auto wall = MainViewVisibility::create();
        wall->bounded = wall->terrain = true;
        wall->minimum = {-4,-4,-5}; wall->maximum = {4,4,-5};
        auto mesh = std::make_shared<MeshPayload>();
        mesh->positions = {{-4,-4,-5},{4,-4,-5},{4,4,-5},{-4,4,-5}};
        mesh->indices = {0,1,2,0,2,3};
        wall->occluders.push_back({mesh, {PrimitiveTopology::Triangles,0,6,0}, glm::dmat4(1.0),
            CullMode::Back, FrontFaceWinding::CounterClockwise});
        auto object = MainViewVisibility::create();
        object->bounded = true; object->mainViewId = 7;
        object->minimum = {-1,-1,-11}; object->maximum = {1,1,-10};
        auto outside = MainViewVisibility::create();
        outside->bounded = true; outside->minimum = {200,0,-10}; outside->maximum = {201,1,-9};
        auto unknown = MainViewVisibility::create();
        std::vector<vsg::ref_ptr<MainViewVisibility>> nodes{wall,object,outside,unknown};
        NativeVisibility visibility;
        auto stats = visibility.update(nodes, view, true);
        require(stats.triangles == 2 && stats.occluded == 1 && stats.frustum == 1, "terrain occlusion/frustum counts");
        require(!object->visibleFor(7) && object->visibleFor(8), "auxiliary views must stay visible");
        require(wall->visible && unknown->visible, "terrain/unknown fail open");
        visibility.update(nodes, view, false);
        require(object->visible, "occlusion control");
        wall->occluders[0].winding = FrontFaceWinding::Clockwise;
        stats = visibility.update(nodes, view, true);
        require(stats.triangles == 0 && object->visible, "GPU winding/backface must be respected");
        wall->occluders[0].winding = FrontFaceWinding::CounterClockwise;
        wall->occluders[0].surface.indexCount = 3;
        visibility.update(nodes, view, true);
        require(object->visible, "hole in terrain must stay visible");
        wall->occluders[0].surface.indexCount = 6;
        object->minimum.z = -6; object->maximum.z = -.05;
        visibility.update(nodes, view, true);
        require(object->visible, "near plane crossing fail open");
        object->minimum.z = -11; object->maximum.z = -10;
        nodes.erase(nodes.begin()); // epoch/cell removal cannot leave old depth
        visibility.update(nodes, view, true);
        require(object->visible, "removed terrain must not leave stale occlusion");
        std::cout << "PASS native visibility: occlusion, control, holes, near plane, winding, removal, auxiliary views\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
