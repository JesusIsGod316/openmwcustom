#include <components/resource/ramretention.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/multiobjectcache.hpp>

#include <osg/Group>

#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>

namespace
{
    unsigned checks = 0;
    void require(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); ++checks; }
    Resource::OpenGlPressureSample sample(std::uint64_t generation = 1, std::uint64_t time = 1000)
    {
        Resource::OpenGlPressureSample s;
        s.generation = generation; s.sampledAtMs = time;
        s.memory.physicalValid = s.memory.commitValid = true;
        s.memory.physicalTotal = 32ull * 1024 * 1024 * 1024;
        s.memory.physicalAvailable = 16ull * 1024 * 1024 * 1024;
        s.memory.commitAvailable = 16ull * 1024 * 1024 * 1024;
        s.decision.state = Resource::OpenGlPressure::Normal; s.decision.admissionsPerSample = 4;
        s.decision.limits = Resource::OpenGlPressurePolicy{}.limits(s.memory.physicalTotal);
        return s;
    }
    class SharedPayload final : public osg::Object
    {
    public:
        SharedPayload() : payload(std::make_shared<int>(42)) {}
        SharedPayload(const SharedPayload& other, const osg::CopyOp& copyop)
            : osg::Object(other, copyop), payload(other.payload) {}
        META_Object(Fixture, SharedPayload)
        std::shared_ptr<int> payload;
    };
}

int main() try
{
    using namespace Resource;
    RamHeadroomPolicy pressure;
    require(pressure.update(sample()).compressionAllowed, "healthy sample refused cold work");
    auto early = sample(2, 2000);
    early.memory.physicalAvailable = early.decision.limits.physicalReserve + 1;
    auto d = pressure.update(early);
    require(d.valid && d.contract && !d.compressionAllowed
        && early.decision.state == OpenGlPressure::Normal, "early contraction changed or ignored P1A state");
    auto missing = early; missing.generation = 0;
    require(!pressure.update(missing).valid, "missing sample became reclaim permission");
    require(pressure.update(sample(3, 3000)).contract, "headroom contraction lost its recovery hold");
    require(pressure.update(sample(3, 8000)).contract, "one repeated generation supplied recovery time");
    require(!pressure.update(sample(4, 8000)).contract, "fresh recovered sample did not release hold");
    auto commitEarly = sample(5, 9000);
    commitEarly.memory.commitAvailable = commitEarly.decision.limits.commitReserve + 1;
    require(pressure.update(commitEarly).contract, "independent commit headroom was ignored");
    auto critical = sample(6, 10000); critical.decision.state = OpenGlPressure::Critical;
    require(pressure.update(critical).contract && !pressure.update(critical).compressionAllowed,
        "Critical enabled allocations");
    auto degraded = sample(7, 11000); degraded.decision.state = OpenGlPressure::Degraded;
    require(!pressure.update(degraded).valid, "degraded state invented destructive pressure");

    ColdOwnerRetention<int> owners;
    for (int key = 0; key < 100; ++key) owners.request(key, 1, false);
    owners.near(0, 35); owners.demanded(1);
    std::set<int> retired;
    auto retire = [&](int key) { if (key == 2) return false; retired.insert(key); return true; };
    require(owners.retire(36, 30, retire, 3, 4) == 4, "retirement cap or protected owner handling failed");
    require(!retired.contains(0) && !retired.contains(1) && !retired.contains(2)
        && !retired.contains(3) && !retired.contains(4), "near/current/ticket/return/running owner retired");
    require(retired.contains(5), "eligible cold owners remained behind the historical floor");
    owners.request(6, 36, false);
    retired.clear();
    owners.retire(37, 30, retire, {}, {}, 32, 32);
    require(!retired.contains(6), "refreshed exact request was retired");
    ColdOwnerRetention<int> bounded;
    for (int key = 0; key < 100; ++key) bounded.request(key, 1, false);
    unsigned visits = 0;
    bounded.retire(100, 30, [&](int) { ++visits; return false; });
    require(visits == 32, "victim selection performed an unbounded scan");
    bounded.retire(100, 30, [&](int key) { require(key >= 32, "blocked prefix starved later keys"); return true; });
    bounded.clear(); require(bounded.size() == 0, "cleared owner index retained identities");

    // Actual generic cache seam used by C2: wrapper refcount alone is not the
    // shared parsed payload's lifetime or reuse protection.
    osg::ref_ptr<GenericObjectCache<std::string>> cache = new GenericObjectCache<std::string>;
    cache->setExternalOwnerTest(+[](const osg::Object* object) noexcept {
        const auto* holder = dynamic_cast<const SharedPayload*>(object);
        return holder && holder->payload.use_count() > 1;
    });
    osg::ref_ptr<SharedPayload> holder = new SharedPayload;
    auto external = holder->payload;
    cache->addEntryToObjectCache("parsed", holder); holder = nullptr;
    cache->update(1, 30); cache->update(100, 30);
    require(cache->getStats().mSize == 1 && *external == 42, "live shared parsed payload lost its cache protection");
    external.reset();
    cache->update(129, 30); require(cache->getStats().mSize == 1, "last external use did not receive TTL");
    { CacheMaintenanceBudget budget(128, 8, std::chrono::seconds(1)); CacheMaintenanceScope scope(budget);
      cache->update(131, 30); }
    require(cache->getStats().mSize == 0, "cache-only parsed wrapper did not expire on bounded maintenance");

    osg::ref_ptr<MultiObjectCache> control = new MultiObjectCache;
    for (int i = 0; i < 4; ++i) control->addEntryToObjectCache(VFS::Path::NormalizedView("shape"), new osg::Group);
    control->updateIdleRetention(100, 4);
    require(control->getStats().mSize == 4, "C3-off changed the existing idle pool control");
    osg::ref_ptr<MultiObjectCache> pool = new MultiObjectCache;
    pool->setIdleRetention(1, 30);
    osg::ref_ptr<osg::Group> live = new osg::Group;
    pool->addEntryToObjectCache(VFS::Path::NormalizedView("live"), live);
    for (int i = 0; i < 4; ++i) pool->addEntryToObjectCache(VFS::Path::NormalizedView("shape"), new osg::Group);
    pool->updateIdleRetention(1, 65536);
    require(pool->getStats().mSize == 5, "newly idle instance lost its short-return grace");
    { CacheMaintenanceBudget budget(128, 8, std::chrono::seconds(1)); CacheMaintenanceScope scope(budget);
      pool->updateIdleRetention(32, 65536); }
    require(pool->getStats().mSize == 2 && live->referenceCount() == 2,
        "cold idle cap did not contract independently or destroyed live instance");
    auto reused = pool->takeFromObjectCache(VFS::Path::NormalizedView("shape"));
    require(reused.valid(), "retained collision instance was not reusable");
    pool->addEntryToObjectCache(VFS::Path::NormalizedView("shape"), new osg::Group);
    pool->addEntryToObjectCache(VFS::Path::NormalizedView("shape"), new osg::Group);
    pool->updateIdleRetention(33, 65536); pool->updateIdleRetention(62, 65536);
    require(pool->getStats().mSize == 3, "recently demanded shape key was evicted prematurely");
    pool->updateIdleRetention(64, 65536);
    require(pool->getStats().mSize == 2, "expired short-return protection never released cold excess");
    require(pool->idleStats().retired >= 4 && pool->idleStats().demandKeys == 1, "idle recency receipts missing");
    pool->trimUnused(8);
    require(live->referenceCount() == 2, "existing pressure trim destroyed live collision owner");
    pool->clear();
    require(pool->getStats().mSize == 0 && pool->idleStats().observedIdle == 0, "clear left stale idle iterators");
    std::cout << "RAM retention native checks passed: " << checks << '\n';
    return 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
