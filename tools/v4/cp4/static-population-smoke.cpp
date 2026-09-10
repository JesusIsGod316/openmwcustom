#include <components/render/backend/vsg/staticpopulationresidency.hpp>
#include <components/rendercore/staticpopulationproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
    [[nodiscard]] bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP4C static population failure: " << message << '\n';
        return condition;
    }

    RenderCore::ModelHandle publishModel(RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher)
    {
        const auto mesh = world.reserveMesh();
        const auto material = world.reserveMaterial();
        const auto model = world.reserveModel();
        if (!mesh || !material || !model)
            return {};
        auto meshPayload = std::make_shared<RenderCore::MeshPayload>();
        meshPayload->positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
        meshPayload->indices = { 0, 1, 2 };
        meshPayload->surfaces.push_back({ .indexCount = 3 });
        RenderCore::MeshRecord meshRecord;
        meshRecord.sourceIdentity = "meshes/tree.nif";
        meshRecord.surfaceCount = 1;
        meshRecord.payload = std::move(meshPayload);
        RenderCore::MaterialRecord materialRecord;
        materialRecord.sourceIdentity = "materials/tree";
        auto modelPayload = std::make_shared<RenderCore::ModelPayload>();
        RenderCore::ModelNodeRecord node;
        node.kind = RenderCore::ModelNodeKind::Geometry;
        node.mesh = *mesh;
        node.materials.push_back(*material);
        modelPayload->nodes.push_back(std::move(node));
        modelPayload->roots.push_back(RenderCore::ModelNodeIndex{ 0 });
        RenderCore::ModelRecord modelRecord;
        modelRecord.sourceIdentity = "meshes/tree.nif";
        modelRecord.payload = std::move(modelPayload);
        RenderCore::RenderWorldUpdateBatch batch(world.epoch(), publisher.nextSequence(), "cp4c-model");
        if (!batch.add(RenderCore::CreateMesh{ *mesh, std::move(meshRecord) })
            || !batch.add(RenderCore::CreateMaterial{ *material, std::move(materialRecord) })
            || !batch.add(RenderCore::CreateModel{ *model, std::move(modelRecord) }) || !batch.seal()
            || publisher.apply(batch) != RenderCore::PublishStatus::Applied)
            return {};
        return *model;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::RenderWorldPublisher publisher(world);
    RenderCore::StaticPopulationProducer producer(world, publisher);
    const RenderCore::ModelHandle model = publishModel(world, publisher);
    if (!require(model.valid(), "model publication")
        || !require(producer.addCell({ .identity = "cell:0,0", .worldspaceIdentity = "world" })
                == RenderCore::StaticPopulationPublishStatus::Applied,
            "cell staging")
        || !require(producer.addCell({ .identity = "cell:0,0", .worldspaceIdentity = "world" })
                == RenderCore::StaticPopulationPublishStatus::AlreadyPresent,
            "idempotent cell retry"))
        return EXIT_FAILURE;

    for (int index = 3; index >= 0; --index)
    {
        RenderCore::StaticPopulationInstanceSource source;
        source.identity = "ref:" + std::to_string(index);
        source.cellIdentity = "cell:0,0";
        source.model = model;
        source.transform.translation.x = index * 10.0;
        const RenderCore::StaticPopulationPublishStatus status = producer.upsert(std::move(source));
        if (status != RenderCore::StaticPopulationPublishStatus::Applied)
            std::cerr << "placement status=" << static_cast<unsigned int>(status) << '\n';
        if (!require(status == RenderCore::StaticPopulationPublishStatus::Applied, "placement staging"))
            return EXIT_FAILURE;
    }
    if (!require(producer.flush() == RenderCore::StaticPopulationPublishStatus::Applied, "atomic publication")
        || !require(world.instanceCount() == 0 && world.chunkCount() == 1, "data-oriented world shape"))
        return EXIT_FAILURE;

    const RenderVsg::StaticWorldPlan plan = RenderVsg::buildStaticWorldPlan(world);
    if (!require(plan.valid() && plan.populations.size() == 1, "backend population discovery")
        || !require(plan.populations[0].placements.size() == 4, "grouped placements")
        || !require(plan.populations[0].placements.front().sourceIdentity == "ref:0", "deterministic source order"))
        return EXIT_FAILURE;

    RenderVsg::StaticPopulationResidency<std::shared_ptr<int>> residency;
    const RenderVsg::StaticPopulationMutation initial = residency.prepare(world);
    if (!require(initial.valid && initial.upserts.size() == 1, "initial resident upsert"))
        return EXIT_FAILURE;
    const auto first = std::make_shared<int>(1);
    auto duplicate = initial;
    duplicate.upserts.push_back(duplicate.upserts.front());
    if (!require(!residency.commit(world, duplicate, { first, first }).committed,
            "duplicate population mutation rejected before publication"))
        return EXIT_FAILURE;
    if (!require(residency.commit(world, initial, { first }).committed, "initial resident commit")
        || !require(residency.markSubmitted(RenderCore::FrameId{ 4 }), "submission lifetime mark"))
        return EXIT_FAILURE;

    if (!require(producer.remove("ref:3") == RenderCore::StaticPopulationPublishStatus::Applied,
            "placement removal")
        || !require(producer.flush() == RenderCore::StaticPopulationPublishStatus::Applied, "replacement publication"))
        return EXIT_FAILURE;
    const RenderVsg::StaticPopulationMutation replacement = residency.prepare(world);
    if (!require(replacement.upserts.size() == 1, "changed chunk replaces one model group")
        || !require(residency.commit(world, replacement, { std::make_shared<int>(2) }).committed,
            "resident replacement")
        || !require(residency.pendingRetirementCount() == 1, "submitted resident retained")
        || !require(residency.collect(RenderCore::FrameId{ 3 }).empty(), "early collection rejected")
        || !require(residency.collect(RenderCore::FrameId{ 4 }).size() == 1, "completed resident retired"))
        return EXIT_FAILURE;

    std::cout << "V4 CP4C static population: PASS\n";
    return EXIT_SUCCESS;
}
