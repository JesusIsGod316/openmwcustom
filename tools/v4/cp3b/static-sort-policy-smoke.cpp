#include <components/render/backend/vsg/staticassetplan.hpp>

#include <cassert>
#include <memory>

namespace
{
    RenderCore::ModelHandle publishModel(RenderCore::RenderWorld& world,
        std::shared_ptr<RenderCore::ModelPayload> payload, const char* identity)
    {
        const auto handle = world.reserveModel();
        assert(handle);
        RenderCore::ModelRecord record;
        record.sourceIdentity = identity;
        record.contentIdentity = identity;
        record.payload = std::move(payload);
        assert(world.commit(*handle, std::move(record)));
        return *handle;
    }
}

int main()
{
    using namespace RenderCore;
    using RenderVsg::StaticDrawSortPolicy;

    RenderWorld world;

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->normals.assign(3u, glm::vec3(0.0f, 0.0f, 1.0f));
    meshPayload->indices = { 0u, 1u, 2u };
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Triangles, 0u, 3u, 0u });
    assert(validMeshPayload(*meshPayload));

    const auto mesh = world.reserveMesh();
    assert(mesh);
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "sort:mesh";
    meshRecord.surfaceCount = 1u;
    meshRecord.payload = meshPayload;
    assert(world.commit(*mesh, std::move(meshRecord)));

    const auto ordinary = world.reserveMaterial();
    const auto sorted = world.reserveMaterial();
    const auto decal = world.reserveMaterial();
    const auto stencil = world.reserveMaterial();
    assert(ordinary && sorted && decal && stencil);

    MaterialRecord ordinaryRecord;
    ordinaryRecord.sourceIdentity = "sort:ordinary";
    assert(world.commit(*ordinary, std::move(ordinaryRecord)));

    MaterialRecord sortedRecord;
    sortedRecord.sourceIdentity = "sort:alpha";
    sortedRecord.alphaBlendEnabled = true;
    sortedRecord.alphaMode = AlphaMode::Blend;
    sortedRecord.transparentSort = TransparentSortPolicy::Sorted;
    assert(world.commit(*sorted, std::move(sortedRecord)));

    MaterialRecord decalRecord;
    decalRecord.sourceIdentity = "sort:decal";
    decalRecord.decal = true;
    assert(world.commit(*decal, std::move(decalRecord)));

    MaterialRecord stencilRecord;
    stencilRecord.sourceIdentity = "sort:stencil";
    stencilRecord.stencil.enabled = true;
    assert(world.commit(*stencil, std::move(stencilRecord)));

    const auto geometry = [&](ModelNodeIndex parent, MaterialHandle material) {
        ModelNodeRecord node;
        node.parent = parent;
        node.kind = ModelNodeKind::Geometry;
        node.mesh = *mesh;
        node.materials = { material };
        return node;
    };

    // NiSortAdjustNode is loader-global in V3.25. An inactive switch branch
    // containing Off must still affect a later active sibling during load.
    auto suppressedSortPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord rootSwitch;
    rootSwitch.kind = ModelNodeKind::Switch;
    rootSwitch.activeSwitchChild = ModelNodeIndex{ 3u };
    suppressedSortPayload->nodes.push_back(rootSwitch);

    ModelNodeRecord inactiveOff;
    inactiveOff.parent = ModelNodeIndex{ 0u };
    inactiveOff.kind = ModelNodeKind::Sort;
    inactiveOff.sort = ModelSortSemantic{ ModelSortMode::Off, ModelSortAccumulator::Alpha };
    suppressedSortPayload->nodes.push_back(inactiveOff);
    suppressedSortPayload->nodes.push_back(geometry(ModelNodeIndex{ 1u }, *ordinary));
    suppressedSortPayload->nodes.push_back(geometry(ModelNodeIndex{ 0u }, *sorted));
    suppressedSortPayload->roots = { ModelNodeIndex{ 0u } };
    assert(validModelPayloadStructure(*suppressedSortPayload));

    const ModelHandle suppressedSortModel
        = publishModel(world, std::move(suppressedSortPayload), "sort:suppressed-branch");
    const auto suppressedPlan = RenderVsg::buildStaticAssetPlan(world, suppressedSortModel);
    assert(suppressedPlan);
    assert(suppressedPlan->draws.size() == 1u);
    assert(suppressedPlan->switchSuppressedNodes == 2u);
    assert(suppressedPlan->draws[0].sortPolicy == StaticDrawSortPolicy::Traversal);
    assert(suppressedPlan->traversalOrderedDraws == 1u);

    // A missing accumulator is a warning/no-op in V3.25: it must not replace
    // the previously pushed Off sorter.
    auto missingAccumulatorPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord offRoot;
    offRoot.kind = ModelNodeKind::Sort;
    offRoot.sort = ModelSortSemantic{ ModelSortMode::Off, ModelSortAccumulator::Alpha };
    missingAccumulatorPayload->nodes.push_back(offRoot);
    missingAccumulatorPayload->nodes.push_back(geometry(ModelNodeIndex{ 0u }, *ordinary));

    ModelNodeRecord missingRoot;
    missingRoot.kind = ModelNodeKind::Sort;
    missingRoot.sort = ModelSortSemantic{ ModelSortMode::Subsort, ModelSortAccumulator::Missing };
    missingAccumulatorPayload->nodes.push_back(missingRoot);
    missingAccumulatorPayload->nodes.push_back(geometry(ModelNodeIndex{ 2u }, *sorted));
    missingAccumulatorPayload->roots = { ModelNodeIndex{ 0u }, ModelNodeIndex{ 2u } };
    assert(validModelPayloadStructure(*missingAccumulatorPayload));

    const ModelHandle missingAccumulatorModel
        = publishModel(world, std::move(missingAccumulatorPayload), "sort:missing-accumulator");
    const auto missingPlan = RenderVsg::buildStaticAssetPlan(world, missingAccumulatorModel);
    assert(missingPlan && missingPlan->draws.size() == 2u);
    assert(missingPlan->draws[0].sortPolicy == StaticDrawSortPolicy::Traversal);
    assert(missingPlan->draws[1].sortPolicy == StaticDrawSortPolicy::Traversal);

    // Without a pushed sorter, sorted alpha and decal use back-to-front. Once
    // V3.25 encounters any enabled stencil property, its global stencil bit
    // forces later non-alpha-sorted drawables into traversal order; on the
    // stencil drawable itself this final rule overrides the earlier decal path.
    auto defaultPayload = std::make_shared<ModelPayload>();
    defaultPayload->nodes.push_back(geometry({}, *decal));
    defaultPayload->nodes.push_back(geometry({}, *sorted));
    defaultPayload->nodes.push_back(geometry({}, *stencil));
    defaultPayload->nodes.push_back(geometry({}, *ordinary));
    defaultPayload->roots
        = { ModelNodeIndex{ 0u }, ModelNodeIndex{ 1u }, ModelNodeIndex{ 2u }, ModelNodeIndex{ 3u } };
    assert(validModelPayloadStructure(*defaultPayload));

    const ModelHandle defaultModel = publishModel(world, std::move(defaultPayload), "sort:default-rules");
    const auto defaultPlan = RenderVsg::buildStaticAssetPlan(world, defaultModel);
    assert(defaultPlan && defaultPlan->draws.size() == 4u);
    assert(defaultPlan->draws[0].sortPolicy == StaticDrawSortPolicy::BackToFront);
    assert(defaultPlan->draws[1].sortPolicy == StaticDrawSortPolicy::BackToFront);
    assert(defaultPlan->draws[2].sortPolicy == StaticDrawSortPolicy::Traversal);
    assert(defaultPlan->draws[3].sortPolicy == StaticDrawSortPolicy::Traversal);
    assert(defaultPlan->backToFrontDraws == 2u);
    assert(defaultPlan->traversalOrderedDraws == 2u);

    // Inherit uses the last valid non-inherit sorter, while Cluster always
    // forces back-to-front regardless of the material's alpha-sort flag.
    auto inheritancePayload = std::make_shared<ModelPayload>();
    ModelNodeRecord inheritedOff;
    inheritedOff.kind = ModelNodeKind::Sort;
    inheritedOff.sort = ModelSortSemantic{ ModelSortMode::Off, ModelSortAccumulator::Alpha };
    inheritancePayload->nodes.push_back(inheritedOff);
    inheritancePayload->nodes.push_back(geometry(ModelNodeIndex{ 0u }, *ordinary));

    ModelNodeRecord inheritRoot;
    inheritRoot.kind = ModelNodeKind::Sort;
    inheritRoot.sort = ModelSortSemantic{ ModelSortMode::Inherit, ModelSortAccumulator::Alpha };
    inheritancePayload->nodes.push_back(inheritRoot);
    inheritancePayload->nodes.push_back(geometry(ModelNodeIndex{ 2u }, *sorted));

    ModelNodeRecord clusterRoot;
    clusterRoot.kind = ModelNodeKind::Sort;
    clusterRoot.sort = ModelSortSemantic{ ModelSortMode::Subsort, ModelSortAccumulator::Cluster };
    inheritancePayload->nodes.push_back(clusterRoot);
    inheritancePayload->nodes.push_back(geometry(ModelNodeIndex{ 4u }, *ordinary));
    inheritancePayload->roots = { ModelNodeIndex{ 0u }, ModelNodeIndex{ 2u }, ModelNodeIndex{ 4u } };
    assert(validModelPayloadStructure(*inheritancePayload));

    const ModelHandle inheritanceModel
        = publishModel(world, std::move(inheritancePayload), "sort:inheritance");
    const auto inheritancePlan = RenderVsg::buildStaticAssetPlan(world, inheritanceModel);
    assert(inheritancePlan && inheritancePlan->draws.size() == 3u);
    assert(inheritancePlan->draws[0].sortPolicy == StaticDrawSortPolicy::Traversal);
    assert(inheritancePlan->draws[1].sortPolicy == StaticDrawSortPolicy::Traversal);
    assert(inheritancePlan->draws[2].sortPolicy == StaticDrawSortPolicy::BackToFront);

    return 0;
}
