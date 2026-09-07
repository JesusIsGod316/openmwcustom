#include <components/render/backend/vsg/staticworldresidency.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C static world plan failure: " << message << '\n';
        return condition;
    }

    struct WorldFixture
    {
        RenderCore::MeshHandle mesh;
        RenderCore::MaterialHandle material;
        RenderCore::ModelHandle model;
        RenderCore::ChunkHandle chunk;
        RenderCore::InstanceHandle instance;
    };

    WorldFixture populate(RenderCore::RenderWorld& world)
    {
        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto model = world.reserveModel();
        const auto chunk = world.reserveChunk();
        const auto instance = world.reserveInstance();
        if (!mesh || !material || !model || !chunk || !instance)
            return {};

        auto meshPayload = std::make_shared<RenderCore::MeshPayload>();
        meshPayload->positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        meshPayload->indices = { 0, 1, 2 };
        meshPayload->surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3 });
        RenderCore::MeshRecord meshRecord;
        meshRecord.sourceIdentity = "meshes/cp3c.nif";
        meshRecord.surfaceCount = 1;
        meshRecord.payload = meshPayload;
        if (!world.commit(*mesh, std::move(meshRecord)))
            return {};
        RenderCore::MaterialRecord materialRecord;
        materialRecord.sourceIdentity = "material:cp3c";
        if (!world.commit(*material, std::move(materialRecord)))
            return {};

        auto modelPayload = std::make_shared<RenderCore::ModelPayload>();
        RenderCore::ModelNodeRecord geometry;
        geometry.name = "geometry";
        geometry.kind = RenderCore::ModelNodeKind::Geometry;
        geometry.mesh = *mesh;
        geometry.materials.push_back(*material);
        modelPayload->nodes.push_back(std::move(geometry));
        modelPayload->roots.push_back(RenderCore::ModelNodeIndex{ 0 });
        RenderCore::ModelRecord modelRecord;
        modelRecord.sourceIdentity = "meshes/cp3c.nif";
        modelRecord.payload = std::move(modelPayload);
        if (!world.commit(*model, std::move(modelRecord)))
            return {};
        RenderCore::ChunkRecord chunkRecord;
        chunkRecord.producerIdentity = "cell:cp3c";
        if (!world.commit(*chunk, std::move(chunkRecord)))
            return {};

        RenderCore::InstanceRecord placement;
        placement.chunk = *chunk;
        placement.model = *model;
        placement.transform.translation = { 1000.0, -2000.0, 3000.0 };
        placement.transform.scale = { 2.0f, 3.0f, 4.0f };
        if (!world.commit(*instance, placement))
            return {};
        return { *mesh, *material, *model, *chunk, *instance };
    }
}

int main()
{
    RenderCore::RenderWorld world;
    const WorldFixture fixture = populate(world);
    if (!require(fixture.instance.valid(), "fixture publication") || !require(world.valid(), "world validity"))
        return EXIT_FAILURE;

    const RenderVsg::StaticWorldPlan first = RenderVsg::buildStaticWorldPlan(world);
    if (!require(first.valid(), "first plan validity") || !require(first.instances.size() == 1, "one static instance")
        || !require(first.simpleMeshInstancesDeferred == 0, "no simple mesh deferral")
        || !require(first.dynamicInstancesDeferred == 0, "no dynamic deferral"))
        return EXIT_FAILURE;

    const RenderVsg::StaticInstancePlan& planned = first.instances.front();
    const glm::dmat4 placement = RenderVsg::staticInstancePlacementMatrix(planned.placement);
    if (!require(planned.instance == fixture.instance, "instance identity")
        || !require(planned.model == fixture.model, "model identity")
        || !require(placement[3][0] == 1000.0 && placement[3][1] == -2000.0 && placement[3][2] == 3000.0,
            "double precision placement translation")
        || !require(RenderVsg::staticInstancePlanCurrent(world, planned), "fresh plan accepted"))
        return EXIT_FAILURE;

    RenderVsg::StaticWorldResidency<std::shared_ptr<int>> residency;
    const RenderVsg::StaticWorldMutation initialMutation = residency.prepare(world);
    auto initialObject = std::make_shared<int>(10);
    auto initialCommit = residency.commit(world, initialMutation, { initialObject });
    if (!require(initialMutation.valid && initialMutation.upserts.size() == 1, "initial persistent upsert")
        || !require(initialCommit.committed && initialCommit.immediatelyReleased.empty(), "initial commit")
        || !require(residency.residentCount() == 1, "one persistent resident")
        || !require(residency.markSubmitted(RenderCore::FrameId{ 10 }), "mark first GPU use"))
        return EXIT_FAILURE;
    const RenderVsg::StaticWorldMutation unchangedMutation = residency.prepare(world);
    if (!require(unchangedMutation.valid && unchangedMutation.upserts.empty() && unchangedMutation.removals.empty(),
            "unchanged world preserves resident object"))
        return EXIT_FAILURE;
    const RenderVsg::StaticWorldMutation optionMutation
        = residency.prepare(world, RenderVsg::StaticPlanOptions{ .showMarkers = true });
    if (!require(optionMutation.valid && optionMutation.upserts.size() == 1,
            "compatibility-affecting plan option stages replacement"))
        return EXIT_FAILURE;

    const auto secondInstance = world.reserveInstance();
    RenderCore::InstanceRecord secondPlacement = *world.get(fixture.instance);
    secondPlacement.transform.translation.x = 2000.0;
    if (!require(secondInstance && world.commit(*secondInstance, secondPlacement), "second instance publication"))
        return EXIT_FAILURE;
    const RenderVsg::StaticWorldMutation secondMutation = residency.prepare(world);
    auto secondCommit = residency.commit(world, secondMutation, { std::make_shared<int>(15) });
    if (!require(secondMutation.orderedInstances.size() == 2 && secondMutation.upserts.size() == 1,
            "deterministic two-instance mutation")
        || !require(secondCommit.committed && residency.residentCount() == 2, "second resident commit"))
        return EXIT_FAILURE;

    RenderCore::InstanceRecord moved = *world.get(fixture.instance);
    moved.revision = RenderCore::ResourceRevision{ 2 };
    moved.transform.translation.x = 4000.0;
    if (!require(world.update(fixture.instance, moved), "revisioned move")
        || !require(!RenderVsg::staticInstancePlanCurrent(world, planned), "stale placement rejected"))
        return EXIT_FAILURE;

    const RenderVsg::StaticWorldMutation replacementMutation = residency.prepare(world);
    auto replacementObject = std::make_shared<int>(20);
    auto replacementCommit = residency.commit(world, replacementMutation, { replacementObject });
    if (!require(replacementMutation.upserts.size() == 1, "changed instance stages replacement")
        || !require(replacementCommit.committed, "replacement commit")
        || !require(residency.pendingRetirementCount() == 1, "old GPU object retained")
        || !require(residency.collect(RenderCore::FrameId{ 9 }).empty(), "in-flight object cannot retire"))
        return EXIT_FAILURE;
    std::vector<int> residentOrder;
    residency.forEachResident(
        [&](const RenderVsg::StaticInstancePlan&, const std::shared_ptr<int>& value) { residentOrder.push_back(*value); });
    if (!require(residentOrder == std::vector<int>{ 20, 15 }, "replacement preserves planner traversal order"))
        return EXIT_FAILURE;
    auto firstRetired = residency.collect(RenderCore::FrameId{ 10 });
    if (!require(firstRetired.size() == 1 && *firstRetired.front() == 10, "completed object retires"))
        return EXIT_FAILURE;

    const RenderVsg::StaticWorldPlan second = RenderVsg::buildStaticWorldPlan(world);
    if (!require(second.valid() && second.instances.size() == 2, "replacement plan")
        || !require(second.instances.front().instanceRevision == RenderCore::ResourceRevision{ 2 }, "new revision")
        || !require(RenderVsg::staticInstancePlanCurrent(world, second.instances.front()), "new plan current"))
        return EXIT_FAILURE;

    RenderCore::MaterialRecord changedMaterial = *world.get(fixture.material);
    changedMaterial.revision = RenderCore::ResourceRevision{ 2 };
    changedMaterial.alpha = 0.5f;
    if (!require(world.update(fixture.material, changedMaterial), "material revision update")
        || !require(!RenderVsg::staticInstancePlanCurrent(world, second.instances.front()),
            "dependent material revision invalidates instance plan"))
        return EXIT_FAILURE;

    RenderCore::InstanceRecord simple;
    simple.mesh = fixture.mesh;
    const auto simpleHandle = world.reserveInstance();
    if (!require(simpleHandle && world.commit(*simpleHandle, simple), "simple mesh publication"))
        return EXIT_FAILURE;
    const RenderVsg::StaticWorldPlan withDeferred = RenderVsg::buildStaticWorldPlan(world);
    if (!require(withDeferred.valid(), "deferred population does not poison static plan")
        || !require(withDeferred.simpleMeshInstancesDeferred == 1, "simple mesh explicitly deferred"))
        return EXIT_FAILURE;

    const RenderVsg::StaticWorldMutation stagedBeforeNewChange = residency.prepare(world);
    RenderCore::MaterialRecord changedAgain = *world.get(fixture.material);
    changedAgain.revision = RenderCore::ResourceRevision{ 3 };
    changedAgain.alpha = 0.25f;
    if (!require(world.update(fixture.material, changedAgain), "second material revision")
        || !require(!residency.commit(world, stagedBeforeNewChange, { std::make_shared<int>(30) }).committed,
            "stale asynchronously realized mutation rejected"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C static world planning: PASS\n";
    return EXIT_SUCCESS;
}
