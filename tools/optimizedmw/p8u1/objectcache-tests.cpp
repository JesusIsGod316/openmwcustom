#include <components/resource/objectcache.hpp>
#include <osg/Group>
#include <atomic>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct ReentrantObject final : osg::Group
{
    Resource::GenericObjectCache<std::string>* cache;
    std::atomic_int* destroyed;
    ReentrantObject(Resource::GenericObjectCache<std::string>* c, std::atomic_int* d) : cache(c), destroyed(d) {}
    ~ReentrantObject() override { (void)cache->getRefFromObjectCache("other"); ++*destroyed; }
};
int main()
try
{
    osg::ref_ptr<Resource::GenericObjectCache<std::string>> cache = new Resource::GenericObjectCache<std::string>;
    constexpr unsigned n=16;
    std::barrier ready(n);
    std::array<osg::ref_ptr<osg::Object>, n> results;
    std::atomic_int winners{0}, destroyed{0};
    std::vector<std::thread> workers;
    for (unsigned i=0;i<n;++i) workers.emplace_back([&,i] {
        osg::ref_ptr<ReentrantObject> candidate = new ReentrantObject(cache, &destroyed);
        ready.arrive_and_wait();
        auto [result, inserted] = cache->getOrInsert(std::string("same-terrain-image"), candidate);
        results[i]=result;
        winners += inserted;
    });
    for (auto& t:workers) t.join();
    require(winners==1, "concurrent publication had multiple winners");
    for (const auto& r:results) require(r==results[0], "consumers do not share the winner");
    require(destroyed==n-1, "losers not released outside the cache lock");
    cache->clear();
    require(destroyed==n-1, "clear destroyed a live consumer resource");
    for (auto& r:results) r=nullptr;
    require(destroyed==n, "winner leaked after consumers released it");
    osg::ref_ptr<osg::Group> first = new osg::Group;
    osg::ref_ptr<osg::Group> second = new osg::Group;
    require(cache->getOrInsert("key", first).second, "fresh key not inserted");
    cache->addEntryToObjectCache("key", second);
    require(cache->getRefFromObjectCache("key")==second, "legacy replacement semantics changed");
    require(!cache->getOrInsert("key",first).second, "existing key replaced");
    require(cache->getRefFromObjectCache("key")==second, "canonical publication replaced winner");
    cache->clear();
    require(cache->getOrInsert("key",first).second, "new world cannot republish cleared key");
    std::cout << "PASS: 16 concurrent producers, one canonical winner; reentrant release, live ownership, legacy replacement, reset\n";
}
catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
