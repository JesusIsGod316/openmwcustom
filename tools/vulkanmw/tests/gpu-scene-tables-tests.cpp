#include <components/render/backend/vsg/gpuscenetables.hpp>
#include <components/render/backend/vsg/locallightplan.hpp>

#include <cassert>
#include <iostream>

int main()
{
    using namespace RenderCore;
    RenderWorld world;
    RenderWorldPublisher publisher(world);
    RenderVsg::GpuSceneTables tables;
    tables.reset(world.epoch(), world.revision());
    const std::uint64_t initialLightSerial = tables.lightSerial();
    assert(initialLightSerial != 0);
    const std::uint64_t initialActorPlanSerial = tables.actorPlanSerial();
    assert(initialActorPlanSerial != 0);

    std::size_t observerCalls = 0;
    publisher.setAppliedObserver([&](const RenderWorldUpdateBatch& batch, const RenderWorld& committed) {
        ++observerCalls;
        assert(tables.apply(committed, batch));
    });

    const auto first = world.reserveLight();
    const auto second = world.reserveLight();
    assert(first && second);

    RenderWorldUpdateBatch create(world.epoch(), publisher.nextSequence(), "gpu-scene-table-create");
    assert(create.add(CreateLight{*first, LightRecord{}}));
    assert(create.add(CreateLight{*second, LightRecord{}}));
    assert(create.seal());
    assert(publisher.apply(create) == PublishStatus::Applied);
    assert(observerCalls == 1);
    assert(tables.current(world));
    assert(tables.live(RenderVsg::GpuSceneTables::Kind::Light) == 2);
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).first
        == std::min(first->slot(), second->slot()));
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).last
        == std::max(first->slot(), second->slot()) + 1);
    assert(tables.stats().creates == 2);
    const auto mirroredCreateLights = RenderVsg::buildLocalLightWorldPlan(tables, world);
    const auto scannedCreateLights = RenderVsg::buildLocalLightWorldPlan(world);
    assert(mirroredCreateLights.valid() && mirroredCreateLights.lights.size() == 2);
    assert(mirroredCreateLights.lights.size() == scannedCreateLights.lights.size());
    for (std::size_t i = 0; i < mirroredCreateLights.lights.size(); ++i)
    {
        assert(mirroredCreateLights.lights[i].light == scannedCreateLights.lights[i].light);
        assert(mirroredCreateLights.lights[i].revision == scannedCreateLights.lights[i].revision);
        assert(mirroredCreateLights.lights[i].record.position == scannedCreateLights.lights[i].record.position);
    }
    const std::uint64_t createdLightSerial = tables.lightSerial();
    assert(createdLightSerial == initialLightSerial + 1);
    assert(tables.actorPlanSerial() == initialActorPlanSerial);

    // Unrelated authoritative world deltas do not invalidate the packed light
    // source. This is the P2 O(1) fast path used by every derived view.
    const auto mesh = world.reserveMesh();
    assert(mesh);
    RenderWorldUpdateBatch meshBatch(world.epoch(), publisher.nextSequence(), "gpu-scene-table-non-light");
    assert(meshBatch.add(CreateMesh{*mesh, MeshRecord{}}));
    assert(meshBatch.seal());
    assert(publisher.apply(meshBatch) == PublishStatus::Applied);
    assert(observerCalls == 2);
    assert(tables.lightSerial() == createdLightSerial);
    const std::uint64_t meshActorPlanSerial = tables.actorPlanSerial();
    assert(meshActorPlanSerial == initialActorPlanSerial + 1);

    tables.clearDirty();
    LightRecord stale = *world.get(*first);
    RenderWorldUpdateBatch rejected(world.epoch(), publisher.nextSequence(), "gpu-scene-table-stale");
    assert(rejected.add(UpdateLight{*first, stale}));
    assert(rejected.seal());
    assert(publisher.apply(rejected) == PublishStatus::OperationRejected);
    assert(observerCalls == 2);
    assert(tables.lightSerial() == createdLightSerial);
    assert(tables.actorPlanSerial() == meshActorPlanSerial);
    assert(!tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).dirty());

    LightRecord moved = *world.get(*first);
    moved.revision = ResourceRevision{moved.revision.value() + 1};
    moved.position.x += 1.0;
    RenderWorldUpdateBatch update(world.epoch(), publisher.nextSequence(), "gpu-scene-table-update");
    assert(update.add(UpdateLight{*first, moved}));
    assert(update.seal());
    assert(publisher.apply(update) == PublishStatus::Applied);
    assert(observerCalls == 3);
    assert(tables.lightSerial() == createdLightSerial + 1);
    assert(tables.actorPlanSerial() == meshActorPlanSerial);
    assert(tables.current(world));
    const auto& firstSlot = tables.table(RenderVsg::GpuSceneTables::Kind::Light).at(first->slot());
    assert(firstSlot.live && firstSlot.generation == first->generation());
    assert(firstSlot.revision == moved.revision);
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).first == first->slot());
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).last == first->slot() + 1);
    assert(tables.stats().updates == 1);
    const auto mirroredUpdateLights = RenderVsg::buildLocalLightWorldPlan(tables, world);
    assert(mirroredUpdateLights.lights.size() == 2);
    const auto movedEntry = std::find_if(mirroredUpdateLights.lights.begin(), mirroredUpdateLights.lights.end(),
        [&](const RenderVsg::LocalLightPlan& entry) { return entry.light == *first; });
    assert(movedEntry != mirroredUpdateLights.lights.end() && movedEntry->record.position.x == 1.0);

    tables.clearDirty();
    RenderWorldUpdateBatch retire(world.epoch(), publisher.nextSequence(), "gpu-scene-table-retire");
    assert(retire.add(RetireLight{*second}));
    assert(retire.seal());
    assert(publisher.apply(retire) == PublishStatus::Applied);
    assert(observerCalls == 4);
    assert(tables.lightSerial() == createdLightSerial + 2);
    assert(tables.actorPlanSerial() == meshActorPlanSerial);
    assert(tables.current(world));
    assert(tables.live(RenderVsg::GpuSceneTables::Kind::Light) == 1);
    const auto& secondSlot = tables.table(RenderVsg::GpuSceneTables::Kind::Light).at(second->slot());
    assert(!secondSlot.live && secondSlot.generation == second->generation());
    assert(tables.stats().retires == 1);
    const auto mirroredRetireLights = RenderVsg::buildLocalLightWorldPlan(tables, world);
    assert(mirroredRetireLights.lights.size() == 1 && mirroredRetireLights.lights.front().light == *first);

    // Post-commit observers see final world state. A valid create->retire
    // transaction must produce a tombstone rather than trying to replay the
    // already-retired intermediate record.
    tables.clearDirty();
    const auto transient = world.reserveLight();
    assert(transient);
    RenderWorldUpdateBatch transientBatch(world.epoch(), publisher.nextSequence(), "gpu-scene-table-transient");
    assert(transientBatch.add(CreateLight{*transient, LightRecord{}}));
    assert(transientBatch.add(RetireLight{*transient}));
    assert(transientBatch.seal());
    assert(publisher.apply(transientBatch) == PublishStatus::Applied);
    assert(observerCalls == 5);
    assert(tables.lightSerial() == createdLightSerial + 3);
    assert(tables.actorPlanSerial() == meshActorPlanSerial);
    assert(tables.current(world));
    const auto& transientSlot = tables.table(RenderVsg::GpuSceneTables::Kind::Light).at(transient->slot());
    assert(!transientSlot.live && transientSlot.generation == transient->generation());
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).first == transient->slot());
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).last == transient->slot() + 1);
    // DeltaStats are global across table kinds; the non-light mesh create
    // above is intentionally counted even though it did not advance lightSerial.
    assert(tables.stats().creates == 4);
    assert(tables.stats().retires == 2);

    publisher.setAppliedObserver({});
    assert(world.reset());
    assert(!tables.current(world));
    tables.reset(world.epoch(), world.revision());
    assert(tables.current(world));
    assert(tables.live(RenderVsg::GpuSceneTables::Kind::Light) == 0);
    assert(!tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).dirty());

    std::cout << "VulkanMW P2 persistent GPU scene table delta tests passed\n";
}
