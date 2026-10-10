#include <components/render/backend/vsg/gpuscenetables.hpp>
#include <components/render/backend/vsg/staticpopulationresidency.hpp>
#include <components/render/backend/vsg/staticworldsyncstate.hpp>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
    using namespace RenderCore;
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    struct Fixture
    {
        RenderWorld world;
        RenderWorldPublisher publisher{world};
        RenderVsg::GpuSceneTables tables;
        MaterialHandle material;
        std::array<ModelHandle, 2> models;
        std::vector<ChunkHandle> chunks;

        Fixture()
        {
            tables.reset(world.epoch(), world.revision());
            publisher.setAppliedObserver([&](const auto& batch, const auto& committed) {
                require(tables.apply(committed, batch), "committed delta rejected by tables");
            });
            const auto mesh = *world.reserveMesh();
            material = *world.reserveMaterial();
            auto geometry = std::make_shared<MeshPayload>();
            geometry->positions = {{0,0,0}, {1,0,0}, {0,1,0}};
            geometry->indices = {0,1,2};
            geometry->surfaces.push_back({.indexCount = 3});
            MeshRecord meshRecord;
            meshRecord.surfaceCount = 1;
            meshRecord.payload = geometry;
            RenderWorldUpdateBatch assets(world.epoch(), publisher.nextSequence(), "population-event-assets");
            require(assets.add(CreateMesh{mesh, meshRecord}) && assets.add(CreateMaterial{material, MaterialRecord{}}),
                "asset batch");
            for (auto& model : models)
            {
                model = *world.reserveModel();
                auto payload = std::make_shared<ModelPayload>();
                ModelNodeRecord node;
                node.kind = ModelNodeKind::Geometry;
                node.mesh = mesh;
                node.materials.push_back(material);
                payload->nodes.push_back(node);
                payload->roots.push_back(ModelNodeIndex{0});
                ModelRecord record;
                record.payload = payload;
                require(assets.add(CreateModel{model, record}), "model batch");
            }
            apply(assets);
            RenderWorldUpdateBatch cells(world.epoch(), publisher.nextSequence(), "population-event-cells");
            for (unsigned cell = 0; cell < 128; ++cell)
            {
                const auto handle = *world.reserveChunk();
                chunks.push_back(handle);
                auto payload = std::make_shared<StaticPopulationPayload>();
                for (unsigned group = 0; group < models.size(); ++group)
                {
                    ModelPopulationRecord population;
                    population.model = models[group];
                    for (unsigned row = 0; row < 4; ++row)
                    {
                        PopulationInstanceRecord placement;
                        placement.sourceIdentity = std::to_string(cell) + ":" + std::to_string(group) + ":" + std::to_string(row);
                        placement.transform.translation.x = row;
                        population.instances.push_back(placement);
                    }
                    payload->groups.push_back(std::move(population));
                }
                ChunkRecord chunk;
                chunk.kind = ChunkRecord::Kind::StaticPopulation;
                chunk.population = payload;
                require(cells.add(CreateChunk{handle, chunk}), "chunk batch");
            }
            apply(cells);
        }
        void apply(RenderWorldUpdateBatch& batch)
        {
            require(batch.seal() && publisher.apply(batch) == PublishStatus::Applied, "authoritative publication failed");
        }
        template <class Edit>
        void change(ChunkHandle handle, Edit edit)
        {
            auto chunk = *world.get(handle);
            chunk.revision = *advanceMonotonic(chunk.revision);
            auto payload = std::make_shared<StaticPopulationPayload>(*chunk.population);
            edit(*payload);
            chunk.population = payload;
            RenderWorldUpdateBatch batch(world.epoch(), publisher.nextSequence(), "population-event-change");
            require(batch.add(UpdateChunk{handle, chunk}), "chunk update");
            apply(batch);
        }
    };
}

int main()
{
    try
    {
        Fixture f;
        RenderVsg::StaticPopulationResidency<std::shared_ptr<int>> residency;
        auto initial = residency.prepareChangedChunks(f.world, {}, f.tables.populationChunks());
        require(initial.valid && !initial.targetedChunks && initial.upserts.size() == 256, "initial load needs complete discovery");
        require(residency.commit(f.world, initial, std::vector<std::shared_ptr<int>>(256, std::make_shared<int>(1))).committed,
            "initial commit");
        require(residency.markSubmitted(FrameId{1}), "initial submission");
        require(f.tables.populationDiscoveryRequired() && f.tables.populationChunks().size() == 128, "load debt missing");
        f.tables.clearDirty();
        require(f.tables.populationChunks().size() == 128, "another consumer acknowledged population debt");
        require(f.tables.acknowledgePopulations(f.world), "initial debt acknowledgement");
        const auto unchanged = initial.orderedPopulations.front();
        const auto* placementStorage = residency.residentPlan(f.world.epoch(), unchanged)->placements.data();
        const auto* drawStorage = residency.residentPlan(f.world.epoch(), unchanged)->asset.draws.data();
        RenderVsg::StaticWorldSyncState instances, complete;
        require(!instances.unchanged(f.world, {}, false) && !complete.unchanged(f.world, {}), "first sync");
        instances.synchronized(); complete.synchronized();
        f.change(f.chunks.back(), [](auto& payload) { payload.groups[0].instances[0].transform.translation.z = 123.; });
        require(instances.unchanged(f.world, {}, false) && !complete.unchanged(f.world, {}), "population inspection not separated");
        require(!f.tables.populationDiscoveryRequired() && f.tables.populationChunks().size() == 1, "one change dirtied all chunks");
        auto delta = residency.prepareChangedChunks(f.world, {}, f.tables.populationChunks());
        const auto control = residency.prepareIncremental(f.world);
        require(delta.valid && delta.targetedChunks && delta.chunksInspected == 1 && delta.cachedAssetPlans == 1
            && delta.upserts.size() == 1 && delta.reusedPlans == 255
            && delta.orderedPopulations == control.orderedPopulations && control.upserts.size() == 1,
            "one changed chunk rediscovered untouched model assets");
        // A prepare failure or an intervening publication must not consume debt.
        const auto light = *f.world.reserveLight();
        RenderWorldUpdateBatch lights(f.world.epoch(), f.publisher.nextSequence(), "population-event-light");
        require(lights.add(CreateLight{light, LightRecord{}}), "light batch");
        f.apply(lights);
        require(!residency.commit(f.world, delta, {std::make_shared<int>(2)}).committed, "stale transaction accepted");
        require(f.tables.populationChunks().size() == 1, "rejected preparation lost pending debt");
        delta = residency.prepareChangedChunks(f.world, {}, f.tables.populationChunks());
        require(residency.commit(f.world, delta, {std::make_shared<int>(2)}).committed, "targeted commit");
        require(residency.residentPlan(f.world.epoch(), unchanged)->placements.data() == placementStorage
            && residency.residentPlan(f.world.epoch(), unchanged)->asset.draws.data() == drawStorage,
            "untouched plans were deep copied");
        require(residency.collect(FrameId{0}).empty() && residency.collect(FrameId{1}).size() == 1, "GPU-safe replacement retirement");
        require(f.tables.acknowledgePopulations(f.world), "movement acknowledgement");
        auto clean = residency.prepareChangedChunks(f.world, {}, f.tables.populationChunks());
        require(clean.valid && clean.targetedChunks && clean.chunksInspected == 0 && clean.upserts.empty(), "clean frame rediscovered chunks");
        require(residency.commit(f.world, clean, {}).committed, "clean commit");
        // Preserve producer order, including changed group-index hints.
        f.change(f.chunks.back(), [](auto& payload) { std::reverse(payload.groups.begin(), payload.groups.end()); });
        auto reordered = residency.prepareChangedChunks(f.world, {}, f.tables.populationChunks());
        const auto reorderControl = residency.prepareIncremental(f.world);
        require(reordered.valid && reordered.upserts.empty() && reordered.orderedPopulations == reorderControl.orderedPopulations,
            "delta arrival order changed authored rendering order");
        require(residency.commit(f.world, reordered, {}).committed && f.tables.acknowledgePopulations(f.world), "reorder commit");
        require(residency.markSubmitted(FrameId{3}), "later submission");
        const auto old = f.chunks.back();
        auto replacement = *f.world.get(old);
        RenderWorldUpdateBatch retire(f.world.epoch(), f.publisher.nextSequence(), "population-event-unload");
        require(retire.add(RetireChunk{old}), "unload batch"); f.apply(retire);
        const auto reloaded = *f.world.reserveChunk();
        require(reloaded != old, "generation not advanced on reload");
        replacement.revision = InitialResourceRevision;
        RenderWorldUpdateBatch reload(f.world.epoch(), f.publisher.nextSequence(), "population-event-reload");
        require(reload.add(CreateChunk{reloaded, replacement}), "reload batch"); f.apply(reload);
        require(f.tables.populationChunks().size() == 2, "old/new generation debt conflated");
        auto lifetime = residency.prepareChangedChunks(f.world, {}, f.tables.populationChunks());
        const auto lifetimeControl = residency.prepareIncremental(f.world);
        require(lifetime.valid && lifetime.targetedChunks && lifetime.chunksInspected == 2 && lifetime.removals.size() == 2
            && lifetime.upserts.size() == 2 && lifetime.orderedPopulations == lifetimeControl.orderedPopulations,
            "reload aliased old resident generation");
        require(residency.commit(f.world, lifetime, {std::make_shared<int>(3), std::make_shared<int>(3)}).committed,
            "reload commit");
        require(residency.collect(FrameId{2}).empty() && residency.collect(FrameId{3}).size() == 2, "unload bypassed GPU retirement");
        require(f.tables.acknowledgePopulations(f.world), "reload acknowledgement");
        auto material = *f.world.get(f.material);
        material.revision = *advanceMonotonic(material.revision);
        RenderWorldUpdateBatch assets(f.world.epoch(), f.publisher.nextSequence(), "population-event-asset");
        require(assets.add(UpdateMaterial{f.material, material}), "asset update"); f.apply(assets);
        auto assetChange = residency.prepareChangedChunks(f.world, {}, f.tables.populationChunks());
        require(f.tables.populationDiscoveryRequired() && !assetChange.targetedChunks && assetChange.upserts.size() == 256,
            "asset mutation escaped correctness fallback");
        require(residency.commit(f.world, assetChange, std::vector<std::shared_ptr<int>>(256, std::make_shared<int>(4))).committed,
            "asset rebuild commit");
        require(f.tables.acknowledgePopulations(f.world), "asset acknowledgement");
        require(!residency.prepareChangedChunks(f.world, {.showMarkers = true}, {}).targetedChunks, "option change bypassed full discovery");
        f.publisher.setAppliedObserver({});
        require(f.world.reset(), "world reset");
        require(!f.tables.acknowledgePopulations(f.world), "stale table acknowledged a new epoch");
        f.tables.reset(f.world.epoch(), f.world.revision());
        auto reset = residency.prepareChangedChunks(f.world, {}, {});
        require(f.tables.populationDiscoveryRequired() && reset.valid && !reset.targetedChunks && reset.removals.size() == 256,
            "new epoch reused stale chunk bindings");
        std::cout << "PASS 128-chunk/256-group event discovery, no untouched payload copies, debt, ordering, generations, fallbacks and retirement\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
