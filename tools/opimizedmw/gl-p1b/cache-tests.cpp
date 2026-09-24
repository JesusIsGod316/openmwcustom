#include <components/resource/objectcache.hpp>
#include <components/resource/multiobjectcache.hpp>
#include <components/resource/deferredrelease.hpp>
#include <components/resource/sharedstatecache.hpp>
#include <osg/Group>
#include <osg/Image>
#include <osg/Texture2D>
#include <osg/observer_ptr>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using Cache = Resource::GenericObjectCache<std::string>;
    using Budget = Resource::CacheMaintenanceBudget;
    using Scope = Resource::CacheMaintenanceScope;
    void require(bool v, const char* message) { if (!v) throw std::runtime_error(message); }
    struct Reentrant : osg::Group
    {
        explicit Reentrant(std::function<void()> fn) : callback(std::move(fn)) {}
        ~Reentrant() override { callback(); }
        std::function<void()> callback;
    };
    struct Shared : Resource::SharedStateManager
    {
        void insert(osg::Texture* t) { _sharedTextureList.insert(t); }
        void insert(osg::StateSet* s) { _sharedStateSetList.insert(s); }
    };
    Budget budget(std::size_t scans=4096, std::size_t releases=64)
    { return Budget(scans, releases, std::chrono::seconds(5)); }
}
int main()
{
    unsigned passed=0, failed=0;
    const auto test = [&](const char* name, auto fn) {
        try { fn(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("clear final destruction can reenter the same cache", [] {
        Cache c; bool done=false;
        c.addEntryToObjectCache("old", new Reentrant([&] { done=c.getStats().mSize==0; }),1);
        c.clear(); require(done,"clear held cache mutex");
    });
    test("remove final destruction can reenter the same cache", [] {
        Cache c; bool done=false;
        c.addEntryToObjectCache("old",new Reentrant([&] { done=c.getStats().mSize==0; }),1);
        c.removeFromObjectCache("old"); require(done,"remove held cache mutex");
    });
    test("replacement publishes before old final destruction", [] {
        Cache c; bool done=false;
        auto replacement=osg::ref_ptr<osg::Group>(new osg::Group);
        c.addEntryToObjectCache("key",new Reentrant([&] { done=c.getRefFromObjectCache("key")==replacement; }),1);
        c.addEntryToObjectCache("key",replacement,2); require(done,"replacement released under lock");
    });
    test("known shared image payload counted once across cache aliases", [] {
        Resource::SpeculativeBudget ledger; Cache a,b; a.setSpeculativeBudget(&ledger); b.setSpeculativeBudget(&ledger);
        auto image=osg::ref_ptr<osg::Image>(new osg::Image);
        image->allocateImage(8,8,1,GL_RGBA,GL_UNSIGNED_BYTE);
        a.addEntryToObjectCache("a",image,1); b.addEntryToObjectCache("b",image,1);
        require(ledger.stats().knownOwnerBytes==256,"shared image double charge");
        a.clear(); require(ledger.stats().knownOwnerBytes==256,"alias release erased surviving charge");
        b.clear(); require(ledger.stats().knownOwnerBytes==0 && image->data(),"cache ownership mistaken for active payload");
    });
    test("logical charge survives payload destructor", [] {
        Resource::SpeculativeBudget ledger; Cache c; c.setSpeculativeBudget(&ledger); bool saw=false;
        c.addEntryToObjectCache("old",new Reentrant([&] { saw=ledger.stats().unknownOwners==1; }),1);
        c.update(2000,1800); require(saw && ledger.stats().unknownOwners==0,"charge dropped before object");
    });
    test("normal budgeted expiry retains healthy TTL", [] {
        Cache c; c.addEntryToObjectCache("a",new osg::Group,1);
        { auto b=budget(); Scope scope(b); c.update(100,1800); }
        require(c.getStats().mSize==1,"healthy retention shortened");
        { auto b=budget(); Scope scope(b); c.update(1802,1800); }
        require(c.getStats().mSize==0,"expired item not removed");
    });
    test("fresh pressure grace and active refs remain safe", [] {
        Cache c; auto pin=osg::ref_ptr<osg::Group>(new osg::Group);
        c.addEntryToObjectCache("a",new osg::Group,1); c.addEntryToObjectCache("z",pin,1);
        { auto b=budget(); Scope s(b); require(c.trimUnused(8)==0,"fresh data trimmed"); }
        { auto b=budget(); Scope s(b); require(c.trimUnused(8)==1,"unused object not trimmed"); }
        require(c.getStats().mSize==1 && c.getRefFromObjectCache("z")==pin,"active identity changed");
    });
    test("global scan budget spans multiple managers", [] {
        Cache a,b; for(int i=0;i<20;++i) { a.addEntryToObjectCache(std::to_string(i),new osg::Group,1); b.addEntryToObjectCache(std::to_string(i),new osg::Group,1); }
        auto pass=budget(5,64); Scope s(pass); a.update(2000,1800); b.update(2000,1800);
        require(pass.scanned==5 && a.getStats().mSize+b.getStats().mSize==35,"per-manager budget multiplied");
    });
    test("expiry cursor advances across active and erased keys", [] {
        Cache c; auto pin=osg::ref_ptr<osg::Group>(new osg::Group);
        for(int i=0;i<200;++i) c.addEntryToObjectCache("a"+std::to_string(i),pin,1);
        c.addEntryToObjectCache("z",new osg::Group,1);
        for(int i=0;i<30;++i) { auto b=budget(9,2); Scope s(b); c.update(2000,1800); }
        require(c.getStats().mSize==200,"later expiry starved");
        c.clear(); c.addEntryToObjectCache("new",new osg::Group,1);
        auto b=budget(); Scope s(b); c.update(2000,1800); require(c.getStats().mSize==0,"clear invalidated cursor");
    });
    test("all pinned cache terminates within scan budget", [] {
        Cache c; auto pin=osg::ref_ptr<osg::Group>(new osg::Group);
        for(int i=0;i<500;++i) c.addEntryToObjectCache(std::to_string(i),pin,1);
        auto b=budget(17); Scope s(b); require(c.trimUnused(64)==0 && b.scanned==17,"pinned work spun");
    });
    test("pool clear destructor may reenter pool", [] {
        Resource::MultiObjectCache p; bool done=false;
        p.addEntryToObjectCache(VFS::Path::NormalizedView("a"),new Reentrant([&] { done=p.getStats().mSize==0; }));
        p.clear(); require(done,"pool clear holds mutex");
    });
    test("pool expiry respects retained count across slices", [] {
        Resource::MultiObjectCache p;
        for(int i=0;i<300;++i) p.addEntryToObjectCache(VFS::Path::NormalizedView("same"),new osg::Group);
        for(int i=0;i<12;++i) { auto b=budget(60,60); Scope s(b); p.removeUnreferencedObjectsInCache(10); }
        require(p.getStats().mSize==10,"pool keep-count restarted in each slice");
    });
    test("pool pressure and demand removal repair expiry iterator", [] {
        Resource::MultiObjectCache p;
        p.addEntryToObjectCache(VFS::Path::NormalizedView("a"),new osg::Group);
        p.addEntryToObjectCache(VFS::Path::NormalizedView("b"),new osg::Group);
        p.addEntryToObjectCache(VFS::Path::NormalizedView("c"),new osg::Group);
        { auto b=budget(1,1); Scope s(b); p.removeUnreferencedObjectsInCache(1); }
        require(p.trimUnused(1)==1,"unbudgeted trim failed");
        static_cast<void>(p.takeFromObjectCache(VFS::Path::NormalizedView("b")));
        { auto b=budget(); Scope s(b); p.removeUnreferencedObjectsInCache(); }
        require(p.getStats().mSize==0,"pool expiry cursor invalid");
    });
    test("deferred owner release is outside queue mutex and still charged", [] {
        Resource::DeferredReleaseQueue q; bool done=false;
        auto owner=osg::ref_ptr<Reentrant>(new Reentrant([&] { auto st=q.stats(); done=st.owners==1 && st.estimatedBytes==64; }));
        require(q.push(owner,64),"queue rejected small owner"); owner=nullptr;
        auto b=budget(); q.drain(b); require(done && q.stats().owners==0,"release no longer charged or mutex held");
    });
    test("deferred queue bounds counts and permits one oversized owner", [] {
        Resource::DeferredReleaseQueue q(2,100); auto a=osg::ref_ptr<osg::Group>(new osg::Group);
        require(q.push(a,200) && !a,"oversized owner cannot transfer");
        a=new osg::Group; require(!q.push(a,1) && a,"rejected owner not preserved");
        auto b=budget(); q.drain(b); require(q.stats().owners==0,"oversized owner stuck");
        require(q.push(a,10) && !a,"first owner not transferred"); a=new osg::Group;
        require(q.push(a,10) && !a,"second owner not transferred"); a=new osg::Group;
        require(!q.push(a,1) && a,"count cap not honored");
    });
    test("deferred destructor can add another owner without losing accounting", [] {
        Resource::DeferredReleaseQueue q; auto later=osg::ref_ptr<osg::Group>(new osg::Group); bool added=false;
        auto first=osg::ref_ptr<Reentrant>(new Reentrant([&] { added=q.push(later,20); }));
        q.push(first,10); first=nullptr; auto b=budget(4096,1); q.drain(b);
        require(added && q.stats().owners==1 && q.stats().estimatedBytes==20,"reentrant addition lost");
        auto next=budget(); q.drain(next); require(q.stats().owners==0,"remaining owner stuck");
    });
    test("queue handoff cannot leave final destruction on submitting thread", [] {
        Resource::DeferredReleaseQueue q(8, 1024);
        const auto submitting=std::this_thread::get_id();
        std::atomic<unsigned> destroyed{0}, wrongThread{0}; std::atomic<bool> stop{false};
        std::thread worker([&] { while(!stop.load() || q.stats().owners) { auto b=budget(); q.drain(b); std::this_thread::yield(); } });
        for(unsigned i=0;i<200;++i) {
            auto object=osg::ref_ptr<Reentrant>(new Reentrant([&] {
                if(std::this_thread::get_id()==submitting) ++wrongThread;
                ++destroyed;
            }));
            while(!q.push(object,1)) std::this_thread::yield();
            require(!object,"ownership not transferred");
        }
        stop=true; worker.join(); require(destroyed==200 && wrongThread==0,"last owner returned to main thread");
    });
    test("shared-state incremental pruning protects live state", [] {
        Shared manager;
        auto pin=osg::ref_ptr<osg::StateSet>(new osg::StateSet); pin->setRenderBinDetails(1,"RenderBin");
        manager.insert(pin);
        auto cold=osg::ref_ptr<osg::StateSet>(new osg::StateSet); cold->setRenderBinDetails(2,"RenderBin");
        osg::observer_ptr<osg::StateSet> weak(cold); manager.insert(cold); cold=nullptr;
        std::vector<osg::ref_ptr<osg::Object>> release; auto b=budget(); manager.pruneBudgeted(release,b);
        require(manager.getNumSharedStateSets()==1 && weak.valid(),"removed state died under shared lock or pinned state evicted");
        release.clear(); require(!weak.valid(),"shared release retained");
        auto detached=manager.detachAll(); require(manager.getNumSharedStateSets()==0 && pin.valid(),"detach destroyed active state");
        manager.insert(pin); auto next=budget(); manager.pruneBudgeted(release,next);
        require(manager.getNumSharedStateSets()==1,"detach invalidated iterator");
    });
    test("shared-state global budget is honored", [] {
        Shared manager;
        for(int i=0;i<100;++i) { auto s=osg::ref_ptr<osg::StateSet>(new osg::StateSet); s->setRenderBinDetails(i,"RenderBin"); manager.insert(s); }
        auto b=budget(3,2); std::vector<osg::ref_ptr<osg::Object>> release; manager.pruneBudgeted(release,b);
        require(b.scanned<=3 && release.size()<=2,"shared-state budget ignored");
        while(manager.getNumSharedStateSets()) { release.clear(); auto more=budget(); manager.pruneBudgeted(release,more); }
    });
    test("parallel lookups and bounded trim retain ownership safely", [] {
        Resource::SpeculativeBudget ledger; Cache c; c.setSpeculativeBudget(&ledger);
        for(int i=0;i<100;++i) c.addEntryToObjectCache(std::to_string(i),new osg::Group,1);
        std::vector<std::thread> readers;
        for(int k=0;k<4;++k) readers.emplace_back([&] { for(int i=0;i<1000;++i) static_cast<void>(c.getRefFromObjectCache(std::to_string(i%100))); });
        for(int i=0;i<100;++i) { auto b=budget(20,8); Scope s(b); c.trimUnused(8); }
        for(auto& r:readers) { r.join(); } c.clear(); require(ledger.stats().unknownOwners==0,"concurrent owner leak");
    });
    std::cout << passed << " passed; " << failed << " failed\n";
    return failed?1:0;
}
