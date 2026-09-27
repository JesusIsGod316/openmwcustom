#include <components/rendercore/persistentdraw.hpp>
#include <components/sceneutil/rendermutation.hpp>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }
    struct Source : SceneUtil::RenderMutationSource
    {
        unsigned mask = SceneUtil::RenderTransform;
        unsigned renderMutationMask() const noexcept override { return mask; }
        void change() { publishRenderMutation(); }
        void replace() { invalidateRenderMutationBindings(); }
    };
    RenderCore::ImmediateEffectDraw draw()
    {
        RenderCore::ImmediateEffectDraw result;
        result.identity = "owner-fixture";
        result.mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
        result.mesh.indices = {0,1,2};
        result.mesh.surfaces = {{RenderCore::PrimitiveTopology::Triangles,0,3,0}};
        result.bounds.minimum = {0,0,0}; result.bounds.maximum = {1,1,0};
        return result;
    }
}
int main()
{
    try
    {
        auto signal = std::make_shared<Source::Subscription>();
        {
            Source source;
            source.subscribeRenderMutations(signal);
            source.subscribeRenderMutations(signal);
            signal->changed = false;
            Source copy(source); copy.change();
            require(!signal->changed, "copy inherited subscription");
            source.change();
            require(signal->changed && !signal->invalidated, "update notification missing");
            signal->changed = false;
            source.mask = SceneUtil::RenderUntracked; source.change();
            require(signal->invalidated, "uncovered mutation did not invalidate");
            signal->invalidated = false; source.replace();
            require(signal->invalidated, "structural invalidation missing");
            signal->invalidated = false;
        }
        require(signal->invalidated, "destroyed source left live bindings");
        Source source;
        for (unsigned i = 0; i < 10000; ++i)
            source.subscribeRenderMutations(std::make_shared<Source::Subscription>());
        source.change(); // expired subscriptions are reclaimed during rebind

        RenderCore::PersistentDrawWorld world(3);
        auto owner = std::make_shared<RenderCore::PersistentDrawOwner>();
        RenderCore::PersistentDrawHandle a, b, legacy;
        const auto value = draw();
        world.begin(1); require(!world.touchOwner(owner), "new owner current");
        require(world.update(a,value,true,owner) && world.update(b,value,true,owner)
            && world.update(legacy,value,true), "initial publication failed");
        const auto initial = world.finish();
        world.begin(1); require(world.touchOwner(owner), "owner not retained");
        auto frame = world.finish();
        require(frame->size()==2 && frame->get(a.slot) && frame->get(b.slot)
            && !frame->get(legacy.slot), "owner liveness changed legacy expiry");
        world.begin(1); require(world.touchOwner(owner), "clean owner stale");
        require(world.finish()==frame, "clean producer copied a frame");
        world.begin(1); world.touchOwner(owner); world.hide(a); auto hidden=world.finish();
        world.begin(1); world.touchOwner(owner);
        require(world.finish()==hidden && !hidden->get(a.slot)->visible, "hidden owner lost");
        world.begin(1); require(world.finish()->size()==0, "disabled/unloaded owner retained");
        require(owner->invalidated && initial->size()==3, "retirement mutated old frame");
        world.begin(1); require(!world.touchOwner(owner), "disabled owner skipped republish");
        world.update(a,value,false,owner); world.update(b,value,false,owner); world.finish();
        world.begin(2); require(!world.touchOwner(owner), "epoch reset kept obsolete handles");
        require(!world.get(a) && world.update(a,value,false,owner), "epoch recovery failed");
        require(world.finish()->size()==1, "recovered owner wrong size");
        // Demand-driven owners are kept by explicit lifecycle, not frame touch.
        RenderCore::PersistentDrawWorld queued(2);
        auto retained = std::make_shared<RenderCore::PersistentDrawOwner>();
        retained->eventDriven = true;
        RenderCore::PersistentDrawHandle q1, q2;
        queued.begin(10); queued.touchOwner(retained);
        require(queued.update(q1,value,true,retained) && queued.update(q2,value,true,retained), "queued seed failed");
        auto immutable = queued.finish();
        for (unsigned i = 0; i < 10000; ++i)
        {
            queued.begin(10);
            require(queued.finish() == immutable, "clean owner required frame touch or copied a snapshot");
        }
        queued.begin(10); queued.hide(q1); auto invisible = queued.finish();
        queued.begin(10); require(queued.finish() == invisible && !invisible->get(q1.slot)->visible,
            "hidden explicit lifetime was lost");
        require(immutable->get(q1.slot)->visible, "visibility mutation changed submitted frame");
        // Full budget still allows unload/reload in the same frame; an expired
        // owner cannot pin the slot budget while waiting for finish().
        queued.begin(10); retained->retire();
        auto replacement = std::make_shared<RenderCore::PersistentDrawOwner>();
        replacement->eventDriven = true; queued.touchOwner(replacement);
        RenderCore::PersistentDrawHandle fresh;
        require(queued.update(fresh,value,true,replacement), "retired slots blocked same-frame replacement");
        auto reloaded = queued.finish();
        require(reloaded->size() == 1 && !queued.get(q1) && !queued.get(q2), "retirement or generation mismatch");
        require(immutable->size() == 2, "unload changed old frame");
        retained->retire(); queued.begin(10);
        require(queued.finish() == reloaded, "stale retirement removed replacement");
        // Tokens from a previous world stream must never retire new residents.
        queued.begin(11); queued.touchOwner(replacement);
        require(queued.update(fresh,value,false,replacement), "epoch republish failed");
        auto reset = queued.finish(); retained->retire(); queued.begin(11);
        require(queued.finish() == reset && reset->size() == 1, "old stream notification removed current owner");
        replacement->retire(); queued.begin(11);
        require(queued.finish()->size() == 0, "explicit unload did not retire queued owner");
        RenderCore::PersistentDrawHandle rejectedClosed;
        require(!queued.update(rejectedClosed,value,true,replacement), "closed lifetime accepted writes");
        auto live = std::make_shared<RenderCore::PersistentDrawOwner>(); live->eventDriven = true;
        queued.touchOwner(live);
        require(queued.update(rejectedClosed,value,true,live), "rejected closed write leaked budget");
        std::cout << "change notifications, ownership, hidden/disabled/unload/epoch, queued retirement and immutable frames PASS\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
