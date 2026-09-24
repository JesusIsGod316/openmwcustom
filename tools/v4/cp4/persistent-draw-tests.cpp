#include <components/rendercore/persistentdraw.hpp>
#include <components/render/backend/vsg/persistentdrawscene.hpp>
#include <components/render/backend/vsg/retainedscenemembership.hpp>
#include <iostream>
#include <stdexcept>
#include <type_traits>
using namespace RenderCore;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
ImmediateEffectDraw fixture()
{
    ImmediateEffectDraw d; d.identity = "persistent-fixture";
    d.mesh.positions = {{0,0,0},{1,0,0},{0,1,0}};
    d.mesh.indices = {0,1,2}; d.mesh.surfaces = {{PrimitiveTopology::Triangles,0,3,0}};
    d.bounds.minimum = {0,0,0}; d.bounds.maximum = {1,1,0}; return d;
}
int main()
{
    try
    {
        static_assert(!std::is_copy_assignable_v<PersistentDrawFrame>);
        static_assert(!std::is_copy_assignable_v<PersistentDrawResource>);
        PersistentDrawWorld world(2); PersistentDrawHandle a, b, c;
        auto draw = fixture();
        PersistentDrawWorld rejectedWorld(1); PersistentDrawHandle rejected;
        rejectedWorld.begin(1);
        auto invalid = draw; invalid.mesh.indices = {99, 99, 99};
        bool rejectedAsset = false;
        try { rejectedWorld.update(rejected, invalid, true); }
        catch (const std::invalid_argument&) { rejectedAsset = true; }
        require(rejectedAsset && rejected.stream == 0 && rejectedWorld.update(rejected, draw, true),
            "rejected asset consumed identity/slot budget");
        world.begin(1); require(world.update(a, draw, true), "create");
        auto first = world.finish(); const auto original = first->get(a.slot)->resource;
        draw.mesh.positions[0].x = 77;
        require(original->draw().meshData().positions[0].x == 0, "mesh alias escaped");
        world.begin(1); world.update(a, draw, false);
        require(world.finish() == first, "unchanged frame was copied");
        world.begin(1); draw.worldTransform[3].x = 4; world.update(a, draw, false);
        auto moved = world.finish();
        require(moved->changes().size() == 1 && moved->baseRevision() == first->revision(), "placement delta");
        require(moved->get(a.slot)->resource == original && first->get(a.slot)->transform[3].x == 0,
            "placement mutated old frame or copied asset");
        world.begin(1); world.hide(a); auto hidden = world.finish();
        require(!hidden->get(a.slot)->visible && moved->get(a.slot)->visible, "hidden frame alias");
        world.begin(1); world.update(a, draw, false); auto visible = world.finish();
        require(visible->get(a.slot)->visible && visible->get(a.slot)->resource == original, "unhide rebuilt resource");
        world.begin(1); world.update(a, draw, false); world.update(b, draw, true);
        require(!world.update(c, draw, true), "slot limit ignored");
        auto pair = world.finish(); require(pair->size() == 2, "second identity missing");
        world.begin(1); world.update(b, draw, false); auto removed = world.finish();
        require(!removed->get(a.slot) && first->get(a.slot), "unload invalidated retained frame");
        const auto oldA = a;
        world.begin(1); world.update(b, draw, false); world.update(c, draw, true); auto recycled = world.finish();
        require(c.slot == oldA.slot && c.generation != oldA.generation && !world.get(oldA), "slot ABA");
        world.begin(2); require(!world.get(b) && !world.get(c), "world reset kept stale handles");
        world.update(a, draw, false); auto reset = world.finish();
        require(reset->stream() != recycled->stream() && reset->size() == 1, "reset recovery snapshot");
        world.begin(2); world.update(a, draw, false); world.remove(a); auto empty = world.finish();
        require(!empty->size(), "explicit fallback withdrawal");

        // Exercise the production backend resource path without a GPU window.
        // Realized nodes use production shaders; the compile callback is a spy.
        RenderVsg::PersistentDrawScene scene;
        auto root = scene.root(); unsigned compiles = 0; std::string diagnostic;
        RenderVsg::StaticTextureResolver resolver = [](const TextureRecord&, const TextureRealizationKey&) {
            return vsg::ref_ptr<vsg::Data>{};
        };
        const auto compile = [&](auto graph) { require(!graph->children.empty(), "empty compile"); ++compiles; return true; };
        require(scene.synchronize(first, {}, resolver, compile, diagnostic), diagnostic.c_str());
        RenderVsg::PersistentDrawScene failedRealizer;
        require(!failedRealizer.synchronize(first, {}, {}, compile, diagnostic)
            && failedRealizer.size() == 0, "failed realizer published a phantom resident");
        require(failedRealizer.synchronize(empty, {}, resolver, compile, diagnostic)
            && failedRealizer.size() == 0, "reset after failed realization was not safe");
        require(failedRealizer.synchronize(first, {}, resolver, [](auto) { return true; }, diagnostic)
            && failedRealizer.size() == 1, "failed realization prevented recovery");
        RenderVsg::PersistentDrawScene retry;
        require(!retry.synchronize(first, {}, resolver, [](auto) { return false; }, diagnostic),
            "compile failure accepted");
        unsigned retries = 0;
        require(retry.synchronize(first, {}, resolver, [&](auto) { ++retries; return true; }, diagnostic)
            && retries == 1 && retry.realized == 1, "failed compile trusted on retry");
        require(scene.size() == 1 && scene.realized == 1 && compiles == 1, "initial backend create");
        scene.markSubmitted(FrameId(1));
        require(scene.synchronize(first, {}, resolver, compile, diagnostic) && scene.realized == 0, "unchanged backend realize");
        require(scene.synchronize(moved, {}, resolver, compile, diagnostic) && scene.placements == 1
            && scene.realized == 0 && compiles == 1 && scene.root() == root, "movement rebuilt GPU resource/root");
        require(scene.synchronize(visible, {}, resolver, compile, diagnostic) && scene.realized == 0,
            "skipped hidden frame failed recovery");
        require(scene.synchronize(recycled, {}, resolver, compile, diagnostic) && scene.size() == 2,
            "skipped create/remove failed full snapshot recovery");
        require(scene.pendingRetirements() == 1, "in-flight replacement released early");
        require(scene.synchronize(reset, FrameId(1), resolver, compile, diagnostic) && scene.size() == 1
            && scene.pendingRetirements() == 0, "epoch reset/fence collection");
        scene.markSubmitted(FrameId(2));
        require(scene.synchronize(empty, FrameId(1), resolver, compile, diagnostic) && !scene.size()
            && scene.pendingRetirements() == 1, "unload ignored GPU use");
        require(scene.synchronize(empty, FrameId(2), resolver, compile, diagnostic)
            && scene.pendingRetirements() == 0, "unchanged frame blocked retirement");

        RenderVsg::RetainedSceneMembership members;
        auto x = vsg::Group::create(), y = vsg::Group::create();
        members.begin(); members.select("x", x); members.select("y", y); members.finish();
        auto stable = members.root();
        const auto contains = [&](const vsg::Node* expected) {
            struct Find : vsg::ConstVisitor
            {
                const vsg::Node* expected; bool found = false;
                explicit Find(const vsg::Node* value) : expected(value) {}
                void apply(const vsg::Object& object) override
                { found |= &object == expected; object.traverse(*this); }
            } find(expected);
            stable->accept(find); return find.found;
        };
        members.begin(); members.select("y", x); members.finish();
        require(members.root() == stable && contains(x) && !contains(y),
            "retained membership replacement/removal");
        members.begin(); members.finish(); require(!contains(x) && !contains(y), "membership unload");
        std::cout << "Persistent draw ownership, deltas, recovery, backend realization, retirement and membership PASS\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
