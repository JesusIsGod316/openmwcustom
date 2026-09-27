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
        std::cout << "change notifications, ownership, hidden/disabled/unload/epoch and immutable frames PASS\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
