#include <components/render/backend/vsg/staticpopulationresidency.hpp>
#include <components/render/backend/vsg/populationvisibility.hpp>
#include <components/rendercore/staticpopulationproducer.hpp>

#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <memory>
#include <string_view>
#include <chrono>

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

    bool checkIncremental(bool indexed)
    {
        using namespace RenderCore;
        RenderWorld world;
        RenderWorldPublisher publisher(world);
        StaticPopulationProducer producer(world, publisher);
        if (!require(producer.addCell({.identity="incremental", .worldspaceIdentity="world"})
            == StaticPopulationPublishStatus::Applied, "incremental cell")) return false;
        constexpr std::size_t count = 96;
        std::vector<ModelHandle> models;
        for (std::size_t i=0;i<count;++i)
        {
            models.push_back(publishModel(world,publisher));
            for (int j=0;j<8;++j)
            {
                StaticPopulationInstanceSource source;
                source.identity="group:"+std::to_string(i)+":"+std::to_string(j);
                source.cellIdentity="incremental";source.model=models.back();
                source.transform.translation.x=j;
                if (!require(producer.upsert(source)==StaticPopulationPublishStatus::Applied,
                    "incremental placement")) return false;
            }
        }
        if (!require(producer.flush()==StaticPopulationPublishStatus::Applied,"incremental flush")) return false;
        RenderVsg::StaticPopulationResidency<std::shared_ptr<int>> residency(indexed);
        auto initial=residency.prepareIncremental(world);
        std::vector<std::shared_ptr<int>> roots(count);
        for (std::size_t i=0;i<count;++i) roots[i]=std::make_shared<int>(static_cast<int>(i));
        if (!require(initial.valid && initial.upserts.size()==count && residency.commit(world,initial,roots).committed,
            "incremental initial commit") || !require(residency.markSubmitted(FrameId{9}),"incremental fence")) return false;
        const auto first=initial.orderedPopulations.front();
        const auto* oldPlan=residency.residentPlan(world.epoch(),first);
        const auto* placements=oldPlan->placements.data();
        const auto* draws=oldPlan->asset.draws.data();
        StaticPopulationInstanceSource change;
        change.identity="group:95:0";change.cellIdentity="incremental";change.model=models.back();
        change.transform.translation.x=123;
        if (!require(producer.upsert(change)==StaticPopulationPublishStatus::Applied
            && producer.flush()==StaticPopulationPublishStatus::Applied,"incremental mutation")) return false;
        auto delta=residency.prepareIncremental(world);
        const auto control=residency.prepare(world);
        if (!require(delta.valid && delta.upserts.size()==1 && delta.reusedPlans==count-1
            && delta.orderedPopulations==control.orderedPopulations && control.upserts.size()==1
            && delta.upserts[0].placements[0].transform.translation.x==123,"delta/control parity")) return false;
        auto invalid=delta;invalid.orderedPopulations.push_back(first);
        if (!require(!residency.commit(world,invalid,{std::make_shared<int>(200)}).committed,
            "invalid delta accepted") || !require(residency.residentPlan(world.epoch(),first)->placements.data()==placements,
            "failed transaction moved a live plan")) return false;
        if (!require(residency.commit(world,delta,{std::make_shared<int>(300)}).committed,"delta commit")) return false;
        const auto* current=residency.residentPlan(world.epoch(),first);
        if (!require(current->placements.data()==placements && current->asset.draws.data()==draws,
            "unchanged plans were deep copied") || !require(residency.collect(FrameId{8}).empty()
                && residency.collect(FrameId{9}).size()==1,"delta lost retirement fence")) return false;
        for (bool incremental : {false,true})
        {
            const auto begin=std::chrono::steady_clock::now();
            for (int i=0;i<40;++i)
            {
                const auto mutation=incremental ? residency.prepareIncremental(world) : residency.prepare(world);
                if (!require(mutation.valid && mutation.upserts.empty() && mutation.removals.empty(),
                    "unchanged population rebuilt")) return false;
            }
            std::cout<<"POPULATION prepare groups="<<count<<" placements=768 repeats=40 incremental="<<incremental
                <<" ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<'\n';
        }
        // Remove a model group, shifting all later indices. Hints may not alias.
        for (int j=0;j<8;++j)
            if (!require(producer.remove("group:0:"+std::to_string(j))==StaticPopulationPublishStatus::Applied,
                "group removal staging")) return false;
        if (!require(producer.flush()==StaticPopulationPublishStatus::Applied,"group removal flush")) return false;
        const auto removed=residency.prepareIncremental(world);
        if (!require(removed.valid && removed.removals.size()==1 && removed.upserts.empty()
            && residency.commit(world,removed,{}).committed,"shifted group hints rejected valid residents")) return false;
        if (!require(producer.removeCell("incremental")==StaticPopulationPublishStatus::Applied,"incremental unload")) return false;
        const auto empty=residency.prepareIncremental(world);
        return require(empty.valid && empty.removals.size()==count-1 && residency.commit(world,empty,{}).committed
            && residency.residentCount()==0,"incremental unload retained groups");
    }

    bool checkManyGroups(bool indexed)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::StaticPopulationProducer producer(world, publisher);
        const auto model = publishModel(world, publisher);
        constexpr std::size_t count = 128;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto identity = "many-cell:" + std::to_string(i);
            if (!require(producer.addCell({ .identity = identity, .worldspaceIdentity = "world" })
                    == RenderCore::StaticPopulationPublishStatus::Applied, "many-cell creation"))
                return false;
            RenderCore::StaticPopulationInstanceSource source;
            source.identity = "many-ref:" + std::to_string(i);
            source.cellIdentity = identity;
            source.model = model;
            if (!require(producer.upsert(std::move(source)) == RenderCore::StaticPopulationPublishStatus::Applied,
                    "many-group placement"))
                return false;
        }
        if (!require(producer.flush() == RenderCore::StaticPopulationPublishStatus::Applied, "many-group publication"))
            return false;
        RenderVsg::StaticPopulationResidency<std::shared_ptr<int>> residency(indexed);
        auto plan = RenderVsg::buildStaticWorldPlan(world);
        const auto initial = residency.prepare(world, plan);
        std::vector<std::shared_ptr<int>> roots;
        for (std::size_t i = 0; i < count; ++i)
            roots.push_back(std::make_shared<int>(static_cast<int>(i)));
        if (!require(initial.upserts.size() == count && residency.commit(world, initial, roots).committed,
                "many-group initial commit")
            || !require(residency.markSubmitted(RenderCore::FrameId{7}), "many-group submission"))
            return false;
        for (std::size_t i = 0; i < count; ++i)
        {
            auto identity = RenderVsg::populationIdentity(plan.populations[i]);
            const auto* object = residency.residentObject(identity);
            if (!require(object && *object == roots[i], "index resolves exact resident"))
                return false;
            identity.chunk = RenderCore::ChunkHandle::fromParts(identity.chunk.slot(), identity.chunk.generation() + 1);
            if (!require(!residency.residentObject(identity), "chunk generation aliases live index"))
                return false;
            identity = RenderVsg::populationIdentity(plan.populations[i]);
            identity.model = RenderCore::ModelHandle::fromParts(identity.model.slot(), identity.model.generation() + 1);
            if (!require(!residency.residentObject(identity), "model generation aliases live index"))
                return false;
        }
        std::reverse(plan.populations.begin(), plan.populations.end());
        const auto reordered = residency.prepare(world, plan);
        auto duplicate = reordered;
        duplicate.orderedPopulations.push_back(duplicate.orderedPopulations.front());
        auto omitted = reordered;
        omitted.orderedPopulations.pop_back();
        if (!require(!residency.commit(world, duplicate, {}).committed, "duplicate order accepted")
            || !require(!residency.commit(world, omitted, {}).committed, "omitted retirement accepted")
            || !require(residency.pendingRetirementCount() == 0 && residency.residentCount() == count,
                "failed commit changed ownership")
            || !require(reordered.upserts.empty() && residency.commit(world, reordered, {}).committed,
                "reorder requires no replacement"))
            return false;
        std::size_t visited = 0;
        bool ordered = true;
        residency.forEachResident([&](const auto& residentPlan, const auto& object) {
            ordered &= visited < count && RenderVsg::populationIdentity(residentPlan)
                == RenderVsg::populationIdentity(plan.populations[visited]) && object == roots[count - 1 - visited];
            ++visited;
        });
        if (!require(ordered && visited == count, "hash iteration changed rendering order"))
            return false;
        const auto removedIdentity = RenderVsg::populationIdentity(plan.populations[count / 2]);
        const auto removedCell = "many-cell:" + plan.populations[count / 2].placements.front().sourceIdentity.substr(9);
        if (!require(producer.removeCell(removedCell) == RenderCore::StaticPopulationPublishStatus::Applied,
                "middle group removal"))
            return false;
        const auto removed = residency.prepare(world);
        if (!require(removed.removals.size() == 1 && removed.upserts.empty(), "middle group diff")
            || !require(residency.commit(world, removed, {}).committed, "middle group commit")
            || !require(!residency.residentObject(removedIdentity) && residency.residentCount() == count - 1,
                "removed index remains live")
            || !require(residency.pendingRetirementCount() == 1 && residency.collect(RenderCore::FrameId{6}).empty(),
                "reorder dropped last-use fence")
            || !require(residency.collect(RenderCore::FrameId{7}).size() == 1, "removed group never retired"))
            return false;
        for (const auto identity : removed.orderedPopulations)
            if (!require(residency.residentObject(identity) != nullptr, "shifted resident index invalid"))
                return false;
        return true;
    }
}

int main(int argc, char** argv)
{
    const bool indexed = !(argc > 1 && std::string_view(argv[1]) == "linear");
    if (!checkIncremental(indexed)) return EXIT_FAILURE;
    if (!checkManyGroups(indexed))
        return EXIT_FAILURE;
    RenderCore::RenderWorld world;
    RenderCore::RenderWorldPublisher publisher(world);
    RenderCore::StaticPopulationProducer producer(world, publisher);
    const RenderCore::ModelHandle model = publishModel(world, publisher);
    if (!require(model.valid(), "model publication")
        || !require(producer.addCell({ .identity = "cell:0,0", .worldspaceIdentity = "world", .bounds = {} })
                == RenderCore::StaticPopulationPublishStatus::Applied,
            "cell staging")
        || !require(producer.addCell({ .identity = "cell:0,0", .worldspaceIdentity = "world", .bounds = {} })
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
        source.lod.maximumDistance = 25.0f;
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
        || !require(plan.populations[0].placements.front().sourceIdentity == "ref:0", "deterministic source order")
        || !require(RenderVsg::populationWithinMaximumDistance(world, plan.populations[0], { 0.0, 0.0, 0.0 }),
            "near population visibility")
        || !require(!RenderVsg::populationWithinMaximumDistance(world, plan.populations[0], { 100.0, 0.0, 0.0 }),
            "distant population culling"))
        return EXIT_FAILURE;
    auto disabledPlan = plan.populations[0];
    for (RenderCore::PopulationInstanceRecord& placement : disabledPlan.placements)
        placement.lod.maximumDistance = 0.0f;
    if (!require(!RenderVsg::populationWithinMaximumDistance(world, disabledPlan, { 0.0, 0.0, 0.0 }),
            "zero-distance population disable"))
        return EXIT_FAILURE;

    RenderVsg::StaticPopulationResidency<std::shared_ptr<int>> residency(indexed);
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

    if (!require(!replacement.causes.empty() && std::string_view(replacement.causes.front().field)=="placement_count"
            && replacement.causes.front().chunkIdentity == world.get(plan.populations.front().chunk)->producerIdentity
            && replacement.causes.front().modelIdentity == world.get(model)->sourceIdentity,
            "exact population stale cause"))
        return EXIT_FAILURE;
    // A cell revision can advance without changing this group. Accept and
    // acknowledge the exact group once, preserving its graph and fence.
    const auto chunkHandle=plan.populations.front().chunk;
    auto chunk=*world.get(chunkHandle); chunk.revision=RenderCore::ResourceRevision{chunk.revision.value()+1};
    if (!require(world.update(chunkHandle,std::move(chunk)), "unrelated chunk revision fixture"))
        return EXIT_FAILURE;
    const auto unchanged=residency.prepare(world);
    const auto incremental = RenderVsg::buildStaticWorldPlan(world, {}, [&](auto handle, auto groupModel) {
        return residency.residentPlan(world.epoch(), {handle, groupModel});
    });
    if (!require(incremental.valid() && incremental.reusedPopulationPlans == 1,
            "unchanged population asset plan reconstructed")
        || !require(incremental.populations.front().chunkRevision == world.get(chunkHandle)->revision,
            "reused plan did not acknowledge revision"))
        return EXIT_FAILURE;
    if (!require(unchanged.upserts.empty() && unchanged.changedChunks==1, "unrelated group rebuilt")
        || !require(residency.commit(world,unchanged,{}).committed,"unchanged group acknowledgement")
        || !require(residency.prepare(world).changedChunks==0,"acknowledged revision remains repeatedly stale"))
        return EXIT_FAILURE;
    auto modelRecord = *world.get(model);
    modelRecord.revision = RenderCore::ResourceRevision{modelRecord.revision.value()+1};
    if (!require(world.update(model, std::move(modelRecord)), "model revision fixture")) return EXIT_FAILURE;
    const auto stale = RenderVsg::buildStaticWorldPlan(world, {}, [&](auto handle, auto groupModel) {
        return residency.residentPlan(world.epoch(), {handle, groupModel});
    });
    if (!require(stale.valid() && stale.reusedPopulationPlans == 0, "changed model reused stale asset plan"))
        return EXIT_FAILURE;
    std::cout << "V4 CP4C static population: PASS\n";
    return EXIT_SUCCESS;
}
