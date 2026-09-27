#include <components/render/backend/vsg/frameresourcepool.hpp>
#include <components/render/backend/vsg/effectidlebudget.hpp>
#include <iostream>
#include <stdexcept>

using RenderCore::FrameId;
using Pool = RenderVsg::FrameResourcePool<int>;
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main()
{
    try
    {
        const auto cost = [](int value) -> std::size_t { return value > 0 ? std::size_t(value) : 0; };
        Pool pool(3);
        pool.beginFrame(FrameId(1), {});
        pool.acquire("returning") = 20;
        pool.acquire("large") = 101;
        pool.acquire("unsupported") = -1;
        require(pool.markSubmitted(FrameId(1)), "first submit");
        pool.beginFrame(FrameId(2), {});
        require(pool.collectUnused({2, 100, 3}, cost).versions == 0 && pool.size() == 3,
            "in-flight resources were evicted or charged as idle");
        require(pool.markSubmitted(FrameId(2)), "gap submit");
        pool.beginFrame(FrameId(3), FrameId(1));
        auto usage = pool.collectUnused({2, 100, 3}, cost);
        require(usage.versions == 1 && usage.bytes == 20 && pool.size() == 1,
            "cost admission retained oversize/unsupported resources");
        unsigned selected = 0;
        pool.inspect([&](const auto&, int, bool chosen, bool, const auto&) { selected += chosen; });
        require(selected == 0, "idle resource was selected for rendering");
        require(pool.markSubmitted(FrameId(3)), "idle submit");
        pool.beginFrame(FrameId(4), FrameId(2));
        require(pool.acquire("returning") == 20 && pool.size() == 1, "returning slot was not reused");
        require(pool.markSubmitted(FrameId(4)), "return submit");
        pool.beginFrame(FrameId(5), FrameId(3));
        pool.acquire("returning") = 21;
        require(pool.size() == 2, "in-flight returning slot overwritten");
        require(pool.markSubmitted(FrameId(5)), "copy-on-write submit");
        pool.beginFrame(FrameId(6), FrameId(5));
        usage = pool.collectUnused({1, 100, 3}, cost);
        require(usage.versions == 1 && usage.bytes == 21 && pool.size() == 1,
            "count budget did not retain newest completed slot");
        require(pool.markSubmitted(FrameId(6)), "bounded submit");
        pool.beginFrame(FrameId(9), FrameId(8));
        pool.collectUnused({1, 100, 3}, cost);
        require(pool.size() == 0, "idle slot survived age limit");
        pool.acquire("a") = 60; pool.acquire("b") = 60;
        require(pool.markSubmitted(FrameId(9)), "byte budget setup");
        pool.beginFrame(FrameId(10), FrameId(9));
        usage = pool.collectUnused({10, 100, 100}, cost);
        require(usage.versions == 1 && usage.bytes == 60 && pool.size() == 1, "byte budget exceeded");
        pool.collectUnused();
        require(pool.size() == 0, "legacy control must immediately collect completed idle resources");

        RenderCore::ImmediateEffectDraw quad;
        quad.mesh.positions.resize(4); quad.mesh.indices.resize(6);
        quad.mesh.surfaces = {{RenderCore::PrimitiveTopology::Triangles,0,6,0}};
        RenderCore::EffectTextureSnapshot texture;
        texture.texture.width = texture.texture.height = 64;
        quad.textures.push_back(texture);
        require(RenderVsg::smallEffectIdleCost(quad) == 64 * 1024 + 64 * 64 * 32,
            "texture storage missing from retention charge");
        quad.textures.front().texture.width = 0;
        require(RenderVsg::smallEffectIdleCost(quad) == 0, "unknown image size retained");
        quad.textures.front().texture.width = 2048;
        require(RenderVsg::smallEffectIdleCost(quad) == 0, "large texture retained");
        quad.textures.clear(); quad.mesh.positions.resize(5);
        require(RenderVsg::smallEffectIdleCost(quad) == 0, "non-quad mesh retained");
        std::cout << "PASS bounded idle residents: fences, gaps, reappearance, bytes, count, age, admission, control\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
