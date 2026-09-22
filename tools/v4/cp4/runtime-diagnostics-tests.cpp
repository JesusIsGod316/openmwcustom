#include <components/debug/runtimediagnostics.hpp>
#include <components/debug/runtimeprocessmemory.hpp>
#include <components/resource/objectcache.hpp>
#include <components/render/backend/vsg/immediateeffectcontract.hpp>
#include <components/render/backend/vsg/frameresourcepool.hpp>
#include <components/render/backend/vsg/framecompletion.hpp>
#include <osg/Image>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <functional>
#include <vector>
#include <limits>

namespace
{
    void require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }
    [[nodiscard]] inline bool baselineLayoutMatches(
        const RenderCore::ImmediateEffectDraw& resident, const RenderCore::ImmediateEffectDraw& current,
        bool immutableBoundsControl = false) noexcept
    {
        // Only streams updated below may differ. Matching sizes alone does not
        // make a resident index buffer, descriptor, uniform or bound current.
        // Defaulted semantic equality also includes future material fields.
        if (resident.identity != current.identity || resident.billboard != current.billboard
            || (immutableBoundsControl && (resident.bounds.minimum != current.bounds.minimum
                || resident.bounds.maximum != current.bounds.maximum))
            || resident.material != current.material
            || resident.semanticFlags != current.semanticFlags
            || resident.mesh.indices != current.mesh.indices
            || resident.mesh.tangents != current.mesh.tangents || resident.mesh.bitangents != current.mesh.bitangents
            || resident.mesh.positions.size() != current.mesh.positions.size()
            || resident.mesh.normals.size() != current.mesh.normals.size()
            || resident.mesh.colors.size() != current.mesh.colors.size()
            || resident.mesh.texCoordSets.size() != current.mesh.texCoordSets.size()
            || resident.mesh.surfaces.size() != current.mesh.surfaces.size()
            || resident.textures.size() != current.textures.size())
            return false;
        for (std::size_t i = 0; i < resident.mesh.texCoordSets.size(); ++i)
        {
            if (resident.mesh.texCoordSets[i].size() != current.mesh.texCoordSets[i].size())
                return false;
        }
        for (std::size_t i = 0; i < resident.mesh.surfaces.size(); ++i)
        {
            const auto& left = resident.mesh.surfaces[i];
            const auto& right = current.mesh.surfaces[i];
            if (left.topology != right.topology || left.firstIndex != right.firstIndex
                || left.indexCount != right.indexCount || left.materialSlot != right.materialSlot)
                return false;
        }
        for (std::size_t i = 0; i < resident.textures.size(); ++i)
        {
            if (resident.textures[i].texture != current.textures[i].texture
                || resident.textures[i].binding != current.textures[i].binding)
                return false;
        }
        return true;
    }

}
int main(int argc, char** argv)
{
    namespace RD = Debug::RuntimeDiagnostics;
    using Reason = RenderVsg::ImmediateEffectMismatch;
    int failures = 0, tests = 0;
    auto test = [&](const char* name, auto&& fn) {
        ++tests;
        try { fn(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("mode agrees with explicit control", [&] {
        require(argc > 1, "expected off, standard or focused argument");
        const std::string_view value(argv[1]);
        require((value == "off" && RD::mode() == RD::Mode::Off)
            || (value == "standard" && RD::mode() == RD::Mode::Standard)
            || (value == "focused" && RD::mode() == RD::Mode::Focused), "environment mode mismatch");
    });
    test("ring FIFO overflow and reuse", [&] {
        RD::Ring<2> ring;
        RD::Record a, b, out; a.frame = 12; b.frame = 13;
        require(ring.push(a) && ring.push(b) && !ring.push(a), "overflow was not bounded");
        require(ring.pop(out) && out.frame == 12, "first lost");
        require(ring.push(a) && ring.pop(out) && out.frame == 13, "wrap order wrong");
        require(ring.pop(out) && out.frame == 12 && !ring.pop(out), "empty behavior wrong");
    });
    test("record escaping and wide integer are lossless", [&] {
        RD::Record r;
        RD::copyText(r.type, "quote\"\n\\");
        RD::copyText(r.values[0].key, "bytes");
        r.values[0].number = (std::numeric_limits<std::uint64_t>::max)(); r.count = 1;
        std::ostringstream out; RD::writeRecord(out, r);
        require(out.str().find("18446744073709551615") != std::string::npos, "64-bit bytes truncated");
        require(out.str().find("quote\\\"\\u000a\\\\") != std::string::npos, "escaping wrong");
    });
    test("file cap writes one marker and then stops", [&] {
        RD::Record r; RD::copyText(r.type, "limit_fixture");
        std::ostringstream out; bool capped = false;
        require(RD::writeBoundedRecord(out, r, capped, 1), "first record missing");
        require(!RD::writeBoundedRecord(out, r, capped, 1) && capped, "cap not enforced");
        const auto size = out.str().size();
        require(!RD::writeBoundedRecord(out, r, capped, 1) && out.str().size() == size, "capped sink kept growing");
        require(out.str().find("capture_limit") != std::string::npos, "cap marker missing");
    });
    test("unwritable sink does not throw or report a write", [&] {
        RD::Record r; std::ostringstream out; out.setstate(std::ios::badbit); bool capped = false;
        require(!RD::writeBoundedRecord(out, r, capped), "bad sink reported success");
    });
    test("oversized identity explicitly clips", [&] {
        RD::Record r;
        r.truncated = RD::copyText(r.identity, std::string(400, 'a'));
        require(r.truncated && r.identity.back() == 0, "clip not bounded or reported");
        std::ostringstream out; RD::writeRecord(out, r);
        require(out.str().find("\"truncated\":true") != std::string::npos, "missing coverage marker");
    });
    test("embedded NUL is replaced and reported", [&] {
        std::array<char, 8> text{};
        require(RD::copyText(text, std::string_view("a\0b", 3)), "embedded NUL was not reported");
        require(std::string_view(text.data()) == "a?b", "embedded NUL silently truncated identity");
    });
    test("census counts shared image payload once without retaining it", [&] {
        auto image = osg::ref_ptr<osg::Image>(new osg::Image);
        image->allocateImage(8, 8, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        const int before = image->referenceCount();
        Resource::CacheDiagnosticCensus census;
        census.add("first", image, 1, 3); census.add("alias", image, 1, 3);
        require(census.knownPayloadBytes == 256 && census.sharedPayloadReferences == 1, "image payload counted twice");
        require(image->referenceCount() == before, "observer retained image");
        require(census.entries == 2 && census.unmeasuredEntries == 0, "coverage incorrect");
    });
    test("census distinguishes unknown from zero bytes", [&] {
        auto object = osg::ref_ptr<osg::Node>(new osg::Node);
        Resource::CacheDiagnosticCensus census;
        census.add("opaque", object, 1, 3);
        require(census.unmeasuredEntries == 1 && census.knownPayloadBytes == 0, "unknown falsely measured");
    });
    test("census hard entry cap is explicit", [&] {
        Resource::CacheDiagnosticCensus census;
        for (std::size_t i = 0; i < Resource::CacheDiagnosticCensus::Limit + 2; ++i) census.add({}, nullptr, 0, 1);
        require(census.limited && census.entries == Resource::CacheDiagnosticCensus::Limit, "census unbounded");
    });
    test("cache instrumentation does not change expiry or references", [&] {
        auto cache = osg::ref_ptr<Resource::GenericObjectCache<std::string>>(new Resource::GenericObjectCache<std::string>);
        auto image = osg::ref_ptr<osg::Image>(new osg::Image);
        image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        cache->addEntryToObjectCache(std::string("retained"), image, 1);
        require(cache->getRefFromObjectCache("retained").valid(), "cache hit lost");
        const auto stats = cache->getStats(); const int refs = image->referenceCount();
        cache->reportRuntimeDiagnostics("test_images", 2, 10);
        require(image->referenceCount() == refs && cache->getStats().mHit == stats.mHit, "census mutated cache");
        cache->update(50, 10); require(cache->getStats().mSize == 1, "active external object evicted");
        image = nullptr;
        cache->update(61, 10); require(cache->getStats().mSize == 0, "unused object no longer expires");
        cache->reportRuntimeDiagnostics("test_images", 61, 10);
    });
    test("pool inspection does not mark use or complete a frame", [&] {
        using RenderCore::FrameId;
        RenderVsg::FrameResourcePool<int> pool(3);
        pool.beginFrame(FrameId(1), std::nullopt); pool.acquire("effect") = 99;
        require(pool.markSubmitted(FrameId(1)), "submit failed");
        unsigned observed = 0;
        pool.inspect([&](const auto&, const int& value, bool selected, bool writable, const auto& last) {
            ++observed; require(value == 99 && selected && !writable && last == FrameId(1), "wrong resident state");
        });
        pool.beginFrame(FrameId(2), std::nullopt); pool.collectUnused();
        require(pool.size() == 1 && observed == 1, "inspection retired in-flight resource");
        pool.beginFrame(FrameId(3), FrameId(1)); pool.collectUnused();
        require(pool.size() == 0, "completed unused version not released");
    });
    test("retirement inspection borrows roots without releasing them", [&] {
        RenderVsg::FrameRetirementQueue<std::shared_ptr<int>> queue;
        auto root = std::make_shared<int>(7);
        require(queue.queue(RenderCore::FrameId(2), root), "queue failed");
        const auto count = root.use_count();
        queue.inspect([&](auto last, const auto& item) {
            require(last.value() == 2 && *item == 7 && root.use_count() == count, "inspection acquired or altered root");
        });
        require(queue.collect(RenderCore::FrameId(1)).empty() && root.use_count() == count, "early release");
        { const auto retired = queue.collect(RenderCore::FrameId(2)); require(retired.size() == 1, "missing retired root"); }
        require(root.use_count() == 1, "retired root leaked");
    });
    RenderCore::ImmediateEffectDraw resident;
    resident.identity = "fixture";
    resident.mesh.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    resident.mesh.indices = {0, 1, 2};
    resident.mesh.surfaces = {{RenderCore::PrimitiveTopology::Triangles, 0, 3, 0}};
    resident.bounds = {{0, 0, 0}, {1, 1, 0}};
    resident.textures.emplace_back(); resident.textures[0].texture.sourceIdentity = "fixture.dds";
    auto change = [&](const char* name, Reason reason, auto&& fn, bool bounds = false) {
        test(name, [&] {
            auto current = resident; fn(current);
            require(RenderVsg::immediateEffectLayoutMismatch(resident, current, bounds) == reason, "wrong invalidation reason");
            require(RenderVsg::immediateEffectLayoutMatches(resident, current, bounds)
                == baselineLayoutMatches(resident, current, bounds), "prior reuse contract changed");
        });
    };
    change("equal effect retains reuse", Reason::None, [](auto&) {});
    change("identity reason", Reason::Identity, [](auto& x) { x.identity += 'x'; });
    change("material reason", Reason::Material, [](auto& x) { x.material.alpha = 0.3f; });
    change("index reason", Reason::Indices, [](auto& x) { x.mesh.indices = {0, 2, 1}; });
    change("vertex count reason", Reason::VertexLayout, [](auto& x) { x.mesh.positions.pop_back(); });
    change("UV count reason", Reason::UvLayout, [](auto& x) { x.mesh.texCoordSets.emplace_back(); });
    change("surface reason", Reason::Surfaces, [](auto& x) { x.mesh.surfaces[0].materialSlot = 1; });
    change("texture count reason", Reason::TextureCount, [](auto& x) { x.textures.emplace_back(); });
    change("texture identity reason", Reason::TextureIdentity, [](auto& x) { x.textures[0].texture.sourceIdentity += 'x'; });
    change("texture binding reason", Reason::TextureBinding, [](auto& x) { x.textures[0].binding.transform.offset.x = 0.5f; });
    change("bounds control reason", Reason::BoundsControl, [](auto& x) { x.bounds.maximum.x += 2; }, true);
    change("default bounds remain mutable", Reason::None, [](auto& x) { x.bounds.maximum.x += 2; });
    change("same-size mutable stream retains reuse", Reason::None, [](auto& x) { x.mesh.positions[0].x += 2; });
    test("sampler disabled control or interval limit", [&] {
        RD::Sampler sampler;
        const auto expected = RD::enabled();
        require(sampler.due(100000000) == expected && !sampler.due(100000000), "sample limit violated");
    });
    test("buffered multiwriter capture never changes game state", [&] {
        RD::recordEvent("fixture", "tests", "first", {{"bytes", 42}});
        if (!RD::enabled()) { require(RD::recorder() == nullptr && RD::attempted == 0, "off created recorder"); return; }
        RD::sampleProcessMemory();
        try { RD::Operation operation("test_unwinding"); throw std::runtime_error("expected"); } catch (...) {}
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i) threads.emplace_back([i] {
            for (unsigned n = 0; n < 128; ++n) RD::recordEvent("thread_fixture", "tests", {}, {{"thread", i}, {"sequence", n}});
        });
        for (auto& t : threads) t.join();
        require(RD::attempted >= 513, "attempt accounting missing");
        require(RD::recorder() != nullptr, "recorder initialization failed");
        RD::recorder()->stop();
    });
    std::cout << tests - failures << '/' << tests << " runtime diagnostic tests passed\n";
    return failures != 0;
}
