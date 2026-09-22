#include <components/resource/objectcache.hpp>
#include <components/resource/multiobjectcache.hpp>
#include <osg/Group>
#include <osg/Image>
#include <osg/observer_ptr>
#include <atomic>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using Cache = Resource::GenericObjectCache<std::string>;
    void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
    struct ReentrantGroup : osg::Group
    {
        std::function<void()> onDelete;
        explicit ReentrantGroup(std::function<void()> fn) : onDelete(std::move(fn)) {}
        ~ReentrantGroup() override { onDelete(); }
    };
    osg::ref_ptr<osg::Group> group() { return new osg::Group; }
}
int main()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, auto&& body) {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("normal thirty-minute retention is unchanged", [] {
        Cache cache; cache.addEntryToObjectCache("a", group(), 1);
        cache.update(100, 1800); require(cache.getStats().mSize == 1, "normal retention shortened");
        cache.update(1802, 1800); require(cache.getStats().mSize == 0, "normal expiry stopped");
    });
    test("fresh and recently requested objects get a grace sweep", [] {
        Cache cache; cache.addEntryToObjectCache("a", group(), 1);
        require(cache.trimUnused(4) == 0, "fresh object discarded immediately");
        static_cast<void>(cache.getRefFromObjectCache("a"));
        require(cache.trimUnused(4) == 0, "recent request discarded");
        require(cache.trimUnused(4) == 1, "inactive object not reclaimed");
        require(cache.getStats().mExpired == 0, "pressure eviction reported as normal expiry");
    });
    test("active object and its payload remain owned", [] {
        Cache cache; auto active = group(); cache.addEntryToObjectCache("a", active, 1);
        cache.trimUnused(4); require(cache.trimUnused(4) == 0, "active object evicted");
        require(cache.getRefFromObjectCache("a") == active, "active identity changed");
        active = nullptr; cache.trimUnused(4);
        require(cache.trimUnused(4) == 1, "released active object stayed pinned");
    });
    test("count and scan bounds are honored", [] {
        Cache cache; for (int i = 0; i < 20; ++i) cache.addEntryToObjectCache(std::to_string(i), group(), 1);
        cache.trimUnused(20);
        require(cache.trimUnused(3, 20) == 3, "remove budget exceeded");
        require(cache.trimUnused(20, 2) == 2, "scan budget exceeded");
        require(cache.trimUnused(0) == 0 && cache.trimUnused(20, 0) == 0, "zero work budget ignored");
    });
    test("pinned early keys do not starve later unreferenced entries", [] {
        Cache cache; auto pin = group();
        for (int i = 0; i < 50; ++i) cache.addEntryToObjectCache("a" + std::to_string(i), pin, 1);
        cache.addEntryToObjectCache("z", group(), 1);
        std::size_t removed = 0;
        for (int i = 0; i < 60; ++i) removed += cache.trimUnused(2, 3);
        require(removed == 1 && cache.getStats().mSize == 50, "cursor failed to visit eligible later key");
    });
    test("image-only trim does not pretend external owners vanished", [] {
        Cache images, scenes; Resource::MultiObjectCache prepared;
        auto image = osg::ref_ptr<osg::Image>(new osg::Image);
        image->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        osg::observer_ptr<osg::Image> weak(image);
        auto scene = group(); scene->getOrCreateUserDataContainer()->addUserObject(image);
        auto instance = group(); instance->addChild(scene);
        images.addEntryToObjectCache("image", image, 1); scenes.addEntryToObjectCache("scene", scene, 1);
        prepared.addEntryToObjectCache(VFS::Path::NormalizedView("instance"), instance);
        image = nullptr; scene = nullptr; instance = nullptr;
        images.trimUnused(8); scenes.trimUnused(8);
        require(images.trimUnused(8) == 0 && scenes.trimUnused(8) == 0 && weak.valid(), "live dependency discarded");
        require(prepared.trimUnused(8) == 1, "prepared owner not released");
        require(scenes.trimUnused(8) == 1, "scene owner not released");
        require(images.trimUnused(8) == 1 && !weak.valid(), "image retained after final owner was dropped");
    });
    test("cache destruction occurs outside cache mutex", [] {
        Cache cache; bool destroyed = false;
        cache.addEntryToObjectCache("a", new ReentrantGroup([&] { destroyed = cache.getStats().mSize == 0; }), 1);
        cache.trimUnused(1); require(cache.trimUnused(1) == 1 && destroyed, "destruction not completed");
    });
    test("instance pool preserves checked-out objects", [] {
        Resource::MultiObjectCache pool; auto active = group();
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("a"), active);
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("b"), group());
        require(pool.trimUnused(1) == 1, "unused instance was not released");
        require(pool.getStats().mExpired == 0, "pool pressure eviction reported as normal expiry");
        require(pool.trimUnused(1) == 0 && pool.getStats().mSize == 1, "active pool instance discarded");
    });
    test("pool scan cursor passes pinned duplicate keys", [] {
        Resource::MultiObjectCache pool; auto active = group();
        for (int i = 0; i < 50; ++i) pool.addEntryToObjectCache(VFS::Path::NormalizedView("same"), active);
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("same"), group());
        std::size_t removed = 0;
        for (int i = 0; i < 30; ++i) removed += pool.trimUnused(1, 3);
        require(removed == 1 && pool.getStats().mSize == 50, "duplicate-key cursor starved eligible instance");
    });
    test("ordinary pool erases repair the trim cursor", [] {
        Resource::MultiObjectCache pool; auto active = group();
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("a"), active);
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("b"), group());
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("c"), group());
        require(pool.trimUnused(1, 1) == 0, "scan limit not respected");
        require(pool.takeFromObjectCache(VFS::Path::NormalizedView("b")).valid(), "take failed");
        require(pool.trimUnused(1, 1) == 1, "take invalidated trim cursor");
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("b"), group());
        pool.trimUnused(1, 1);
        pool.removeUnreferencedObjectsInCache();
        require(pool.trimUnused(1) == 0, "normal prune invalidated cursor");
        pool.clear(); pool.addEntryToObjectCache(VFS::Path::NormalizedView("new"), group());
        require(pool.trimUnused(0) == 0 && pool.trimUnused(1, 0) == 0, "zero budget ignored");
        require(pool.trimUnused(1) == 1, "clear invalidated cursor");
    });
    test("pool destruction is outside its mutex", [] {
        Resource::MultiObjectCache pool; bool deleted = false;
        pool.addEntryToObjectCache(VFS::Path::NormalizedView("a"),
            new ReentrantGroup([&] { deleted = pool.getStats().mSize == 0; }));
        require(pool.trimUnused(1) == 1 && deleted, "pool object not released");
    });
    test("concurrent get and bounded trim retain valid ref_ptr ownership", [] {
        Cache cache; std::atomic<bool> bad{false};
        for (unsigned i = 0; i < 64; ++i) cache.addEntryToObjectCache(std::to_string(i), group(), 1);
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 4; ++t) threads.emplace_back([&, t] {
            for (unsigned i = 0; i < 2000; ++i)
            {
                const auto key = std::to_string((i + t) % 64);
                auto object = cache.getRefFromObjectCache(key);
                if (!object) cache.addEntryToObjectCache(key, group(), 1);
                else if (object->referenceCount() < 1 || object->className() != std::string("Group")) bad = true;
            }
        });
        for (unsigned i = 0; i < 1000; ++i) cache.trimUnused(4, 16);
        for (auto& thread : threads) thread.join();
        require(!bad, "invalid reference during concurrent trim");
    });
    std::cout << passed << '/' << passed + failed << " cache pressure tests passed\n";
    return failed ? 1 : 0;
}
