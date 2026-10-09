#include <components/resource/ramretention.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/multiobjectcache.hpp>
#include <components/resource/textureupgradeadmission.hpp>

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

    // C4 uses the same validated contract through Recovering. It retires only
    // completed cold owners at the consumer; C1 alone retains its Normal TTL.
    auto recovering=sample(8,12000);recovering.decision.state=OpenGlPressure::Recovering;
    const auto recoveringContract=pressure.update(recovering);
    require(coldOwnerRetentionTtl(recovering,recoveringContract,false,true)==5.0,
        "C4 lost cold-owner contraction during Recovering");
    require(!coldOwnerRetentionTtl(recovering,recoveringContract,true,false),
        "C1 alone gained destructive Recovering behavior");
    auto normal=sample(9,13000);RamHeadroomPolicy fresh;
    const auto normalContract=fresh.update(normal);
    require(coldOwnerRetentionTtl(normal,normalContract,true,false)==30.0,
        "Normal C1 retention control changed");
    require(!coldOwnerRetentionTtl(normal,normalContract,false,false),
        "disabled C1/C4 enabled retirement");
    auto wrongGeneration=recoveringContract;++wrongGeneration.generation;
    require(!coldOwnerRetentionTtl(recovering,wrongGeneration,false,true)
        && !coldOwnerRetentionTtl(recovering,{},false,true), "invalid C4 contract authorized retirement");
    for(const auto state:{OpenGlPressure::Caution,OpenGlPressure::Critical,OpenGlPressure::Degraded})
    {
        auto unsafe=recovering;unsafe.decision.state=state;
        require(!coldOwnerRetentionTtl(unsafe,recoveringContract,true,true),
            "C4 widened retirement into another pressure state");
    }

    // Exercise the production priority selector and scoped admission callback,
    // including the state race before Stage rather than a model of the policy.
    namespace U=TextureUpgradeAdmission;
    U::VisibleCoarseUse visible{true,true,true,true,true,false,20,19.5,200,199};
    recovering.decision.admissionsPerSample=0;
    recovering.growthWatermarkValid=true;
    require(U::recoveryPromotion(visible,recovering),"current visible coarse use refused recovery lane");
    for(const auto state:{OpenGlPressure::Normal,OpenGlPressure::Caution,OpenGlPressure::Critical,OpenGlPressure::Degraded})
    {
        auto other=recovering;other.decision.state=state;
        require(!U::recoveryPromotion(visible,other),"recovery lane changed another state control");
    }
    for(unsigned guard=0;guard<6;++guard)
    {
        auto use=visible;
        switch(guard) { case 0:use.streaming=false;break;case 1:use.current=false;break;
            case 2:use.mainDraw=false;break;case 3:use.coarse=false;break;
            case 4:use.drawn=false;break;default:use.demoting=true;break; }
        require(!U::recoveryPromotion(use,recovering),"ineligible owner entered visible recovery lane");
    }
    auto old=visible;old.lastDrawFrame=198;
    require(!U::recoveryPromotion(old,recovering),"historical draw substituted for previous exact owner frame");
    old=visible;old.lastDrawTime=19;
    require(!U::recoveryPromotion(old,recovering),"one-second-old draw entered recovery lane");
    old=visible;old.lastDrawTime=21;
    require(!U::recoveryPromotion(old,recovering),"regressed clock supplied visible draw proof");
    auto unstamped=recovering;unstamped.sampledAtMs=0;
    require(!U::recoveryPromotion(visible,unstamped),"unstamped sample enabled priority allocation");
    SpeculativeBudget::Config quota;quota.transientLimit=16*SpeculativeBudget::MiB;quota.reservedNearFutureJobs=1;
    SpeculativeBudget ledger(quota);
    const auto read=+[](void* owner) { return *static_cast<OpenGlPressureSample*>(owner); };
    for(unsigned drift=0;drift<4;++drift)
    {
        auto selected=recovering;selected.generation=20+drift;selected.accountedGrowth=ledger.watermark()->load();
        auto changed=selected;
        if(drift==0) changed.decision.state=OpenGlPressure::Caution;
        if(drift==1) changed.decision.state=OpenGlPressure::Critical;
        if(drift==2) ++changed.generation;
        if(drift==3) ++changed.sampledAtMs;
        U::FrozenRecoverySample frozen{selected,read,&changed};bool reachedGpu=false,caught=false;
        {
            auto job=ledger.tryJob(selected,SpeculativePriority::NearFuture);
            require(bool(job),"recovery fixture permit unexpectedly refused");
            try { SpeculativeScope scope(&ledger,&U::FrozenRecoverySample::read,&frozen,SpeculativePriority::NearFuture);
                SpeculativeScope::Stage stage(1024,1024);reachedGpu=true; }
            catch(const U::SampleChanged& e)
            { caught=true;require(e.observed.decision.state==changed.decision.state
                    && e.observed.generation==changed.generation && e.observed.sampledAtMs==changed.sampledAtMs,
                    "sample drift receipt rewrote actual observation"); }
        }
        require(caught && !reachedGpu && ledger.stats().futureBytes==0 && ledger.stats().jobs==0,
            "sample drift reached GPU work or leaked its stage/permit");
    }
    for(unsigned shortage=0;shortage<2;++shortage)
    {
        auto scarce=recovering;scarce.generation=30+shortage;scarce.accountedGrowth=ledger.watermark()->load();
        if(shortage==0) scarce.memory.physicalAvailable=scarce.decision.limits.physicalReserve-1;
        else scarce.memory.commitAvailable=scarce.decision.limits.commitReserve-1;
        U::FrozenRecoverySample frozen{scarce,read,&scarce};bool refused=false;
        auto job=ledger.tryJob(scarce,SpeculativePriority::NearFuture);require(bool(job),"scarce fixture permit missing");
        try { SpeculativeScope scope(&ledger,&U::FrozenRecoverySample::read,&frozen,SpeculativePriority::NearFuture);
            SpeculativeScope::Stage stage(1024,1024); }
        catch(const SpeculativeDeferred& e)
        { refused=true;require(e.reservation && e.reservation->physicalFloor==scarce.decision.limits.physicalReserve
                && e.reservation->commitFloor==scarce.decision.limits.commitReserve
                && (e.reservation->reasons & (shortage==0 ? SpeculativeBudget::PhysicalHeadroom
                    : SpeculativeBudget::ProcessCommitHeadroom)),
                "Recovering priority weakened the full reserve floor"); }
        job.finish();
        require(refused && ledger.stats().futureBytes==0 && ledger.stats().jobs==0,
            "full-floor refusal leaked future bytes or a permit");
    }
    auto once=recovering;once.generation=40;once.accountedGrowth=ledger.watermark()->load();
    { auto job=ledger.tryJob(once,SpeculativePriority::NearFuture);require(bool(job),"first recovery permit missing"); }
    require(!ledger.tryJob(once,SpeculativePriority::NearFuture),"second job spent the same Recovering sample");

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
