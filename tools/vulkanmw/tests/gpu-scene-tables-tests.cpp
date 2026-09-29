#include <components/render/backend/vsg/gpuscenetables.hpp>

#include <cassert>
#include <iostream>

int main()
{
    using namespace RenderCore;
    RenderWorld world;
    RenderWorldPublisher publisher(world);
    RenderVsg::GpuSceneTables tables;
    tables.reset(world.epoch(), world.revision());

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

    tables.clearDirty();
    LightRecord stale = *world.get(*first);
    RenderWorldUpdateBatch rejected(world.epoch(), publisher.nextSequence(), "gpu-scene-table-stale");
    assert(rejected.add(UpdateLight{*first, stale}));
    assert(rejected.seal());
    assert(publisher.apply(rejected) == PublishStatus::OperationRejected);
    assert(observerCalls == 1);
    assert(!tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).dirty());

    LightRecord moved = *world.get(*first);
    moved.revision = ResourceRevision{moved.revision.value() + 1};
    moved.position.x += 1.0;
    RenderWorldUpdateBatch update(world.epoch(), publisher.nextSequence(), "gpu-scene-table-update");
    assert(update.add(UpdateLight{*first, moved}));
    assert(update.seal());
    assert(publisher.apply(update) == PublishStatus::Applied);
    assert(observerCalls == 2);
    assert(tables.current(world));
    const auto& firstSlot = tables.table(RenderVsg::GpuSceneTables::Kind::Light).at(first->slot());
    assert(firstSlot.live && firstSlot.generation == first->generation());
    assert(firstSlot.revision == moved.revision);
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).first == first->slot());
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).last == first->slot() + 1);
    assert(tables.stats().updates == 1);

    tables.clearDirty();
    RenderWorldUpdateBatch retire(world.epoch(), publisher.nextSequence(), "gpu-scene-table-retire");
    assert(retire.add(RetireLight{*second}));
    assert(retire.seal());
    assert(publisher.apply(retire) == PublishStatus::Applied);
    assert(observerCalls == 3);
    assert(tables.current(world));
    assert(tables.live(RenderVsg::GpuSceneTables::Kind::Light) == 1);
    const auto& secondSlot = tables.table(RenderVsg::GpuSceneTables::Kind::Light).at(second->slot());
    assert(!secondSlot.live && secondSlot.generation == second->generation());
    assert(tables.stats().retires == 1);

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
    assert(observerCalls == 4);
    assert(tables.current(world));
    const auto& transientSlot = tables.table(RenderVsg::GpuSceneTables::Kind::Light).at(transient->slot());
    assert(!transientSlot.live && transientSlot.generation == transient->generation());
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).first == transient->slot());
    assert(tables.dirty(RenderVsg::GpuSceneTables::Kind::Light).last == transient->slot() + 1);
    assert(tables.stats().creates == 3);
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
