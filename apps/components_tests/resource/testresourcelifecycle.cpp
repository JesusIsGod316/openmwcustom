#include <components/debug/resourcelifecycle.hpp>
#include <components/misc/osguservalues.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/clone.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/vfs/manager.hpp>

#include <gtest/gtest.h>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>

#include <array>
#include <barrier>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <thread>

namespace
{
    namespace Lifecycle = Debug::ResourceLifecycle;
    using Cache = Resource::GenericObjectCache<int>;

    TEST(ResourceLifecycleRegistry, CapacityFailureDoesNotReuseRetiredIdentity)
    {
        if (!Lifecycle::enabled()) GTEST_SKIP() << "Requires separately launched Deep fixture";
        const VFS::Manager vfs;
        int owner = 0, resource = 0;
        Lifecycle::Registry registry(1, false);
        const Lifecycle::Context context{Lifecycle::Kind::Template, "bounded.nif", &vfs,
            vfs.getIndexGeneration(), 0, 2, &owner};
        const auto token = registry.publish(&owner, &resource, context);
        ASSERT_NE(token, 0);
        registry.retire(token, &resource, Lifecycle::Retirement::Expired);
        EXPECT_EQ(registry.publish(&owner, &resource, context), 0);
        const auto stats = registry.stats();
        EXPECT_EQ(stats.publications, 1);
        EXPECT_EQ(stats.active, 0);
        EXPECT_EQ(stats.retirements, 1);
        EXPECT_EQ(stats.capacityFailures, 1);
    }

    TEST(ResourceLifecycleRegistry, PointerReuseGetsNewIdentityAndRetirementIsIdempotent)
    {
        if (!Lifecycle::enabled()) GTEST_SKIP() << "Requires separately launched Deep fixture";
        const VFS::Manager vfs;
        int owner = 0, resource = 0;
        Lifecycle::Registry registry(2, false);
        Lifecycle::Context context{Lifecycle::Kind::Nif, "reused.nif", &vfs,
            vfs.getIndexGeneration(), 0, 2, &owner};
        const auto first = registry.publish(&owner, &resource, context);
        registry.retire(first, &resource, Lifecycle::Retirement::Replaced);
        registry.retire(first, &resource, Lifecycle::Retirement::Cleared);
        const auto second = registry.publish(&owner, &resource, context);
        ASSERT_GT(second, first);
        context.origin = 1;
        registry.observe(first, &resource, context); // Old residency must not be revived by pointer reuse.
        registry.observe(second, &resource, context);
        registry.observe(second, &resource, context);
        const auto stats = registry.stats();
        EXPECT_EQ(stats.publications, 2);
        EXPECT_EQ(stats.firstDemands, 1);
        EXPECT_EQ(stats.retirements, 1);
        EXPECT_EQ(stats.active, 1);
        EXPECT_EQ(stats.unknownUses, 1);
    }

    TEST(ResourceLifecycleRegistry, DifferentVfsAndChangedGenerationAreExplicitlyUnknown)
    {
        if (!Lifecycle::enabled()) GTEST_SKIP() << "Requires separately launched Deep fixture";
        VFS::Manager vfs;
        const VFS::Manager otherVfs;
        int owner = 0, resource = 0;
        Lifecycle::Registry registry(2, false);
        Lifecycle::Context context{Lifecycle::Kind::Keyframe, "identity.kf", &vfs,
            vfs.getIndexGeneration(), 0, 2, &owner};
        const auto token = registry.publish(&owner, &resource, context);
        context.vfs = &otherVfs; context.origin = 1;
        registry.observe(token, &resource, context);
        vfs.buildIndex(); // No concurrent index reads: production VFS contract.
        context.vfs = &vfs; context.generation = vfs.getIndexGeneration();
        registry.observe(token, &resource, context);
        const auto stats = registry.stats();
        EXPECT_EQ(stats.unknownGenerations, 1);
        EXPECT_EQ(stats.unknownUses, 2);
        EXPECT_EQ(stats.firstDemands, 0);
        Lifecycle::Registry initiallyStale(1, false);
        context.generation = 0; // VFS index has already advanced above zero.
        ASSERT_NE(initiallyStale.publish(&owner, &resource, context), 0);
        EXPECT_EQ(initiallyStale.stats().unknownGenerations, 1);
        EXPECT_EQ(initiallyStale.stats().firstDemands, 0);
    }

    TEST(ResourceLifecycleRegistry, AllocationFailureAndBoundedMetadataAreReportedWithoutResourceOwnership)
    {
        if (!Lifecycle::enabled()) GTEST_SKIP() << "Requires separately launched Deep fixture";
        Lifecycle::Registry failed((std::numeric_limits<std::size_t>::max)(), false);
        failed.initialize();
        EXPECT_EQ(failed.stats().allocationFailures, 1);
        const VFS::Manager vfs;
        int owner = 0, resource = 0;
        const std::string longPath(300, 'x');
        Lifecycle::Registry bounded(1, false);
        const Lifecycle::Context context{Lifecycle::Kind::Nif, longPath, &vfs,
            vfs.getIndexGeneration(), 0, 2, &owner};
        ASSERT_NE(bounded.publish(&owner, &resource, context), 0);
        EXPECT_EQ(bounded.stats().metadataTruncations, 1);
        EXPECT_EQ(bounded.stats().allocationFailures, 0);
    }

    TEST(ResourceLifecycleRegistry, OwnerMismatchDoesNotCreateFirstDemand)
    {
        if (!Lifecycle::enabled()) GTEST_SKIP() << "Requires separately launched Deep fixture";
        const VFS::Manager vfs;
        int owner = 0, unrelated = 0, resource = 0;
        Lifecycle::Registry registry(1, false);
        Lifecycle::Context context{Lifecycle::Kind::Template, "owner.nif", &vfs,
            vfs.getIndexGeneration(), 0, 2, &owner};
        const auto token = registry.publish(&owner, &resource, context);
        context.origin = 1; context.cacheOwner = &unrelated;
        registry.observe(token, &resource, context);
        EXPECT_EQ(registry.stats().firstDemands, 0);
        EXPECT_EQ(registry.stats().unknownUses, 1);
    }

    TEST(ResourceLifecycleProduction, OptionalBoundaryToFirstDemandKeepsExactPayloadAndNoExtraOwner)
    {
        const VFS::Manager vfs;
        osg::ref_ptr<Cache> cache = new Cache;
        osg::ref_ptr<osg::Group> value = new osg::Group;
        const auto before = Lifecycle::registry().stats();
        {
            Lifecycle::PreparationScope preparation("fixture_optional");
            Lifecycle::RequestScope request(Lifecycle::Kind::Template, "prepared.nif", &vfs, true, cache.get());
            cache->addEntryToObjectCache(1, value);
        }
        EXPECT_EQ(value->referenceCount(), 2); // Caller + original cache; no registry owner.
        {
            Lifecycle::RequestScope demand(Lifecycle::Kind::Template, "prepared.nif", &vfs, false, cache.get());
            EXPECT_EQ(cache->getRefFromObjectCache(1), value);
            EXPECT_EQ(cache->getRefFromObjectCache(1), value);
        }
        if (Lifecycle::enabled())
        {
            const auto after = Lifecycle::registry().stats();
            EXPECT_EQ(after.publications - before.publications, 1);
            EXPECT_EQ(after.firstDemands - before.firstDemands, 1);
            EXPECT_EQ(after.unknownOrigins - before.unknownOrigins, 0);
        }
        cache->clear();
        EXPECT_EQ(value->referenceCount(), 1);
    }

    TEST(ResourceLifecycleProduction, NestedUnrelatedCacheCannotInheritResourceIdentity)
    {
        const VFS::Manager vfs;
        osg::ref_ptr<Cache> templateCache = new Cache, unrelatedCache = new Cache;
        osg::ref_ptr<osg::Group> value = new osg::Group;
        const auto before = Lifecycle::registry().stats();
        {
            Lifecycle::RequestScope request(Lifecycle::Kind::Template, "nested.nif", &vfs, false, templateCache.get());
            unrelatedCache->addEntryToObjectCache(1, value);
            EXPECT_EQ(unrelatedCache->getRefFromObjectCache(1), value);
            templateCache->addEntryToObjectCache(1, value);
        }
        if (Lifecycle::enabled())
        {
            const auto after = Lifecycle::registry().stats();
            EXPECT_EQ(after.publications - before.publications, 1);
            EXPECT_EQ(after.unknownUses - before.unknownUses, 0);
        }
    }

    TEST(ResourceLifecycleProduction, ConcurrentCanonicalWinnerAndSingleFirstDemandAreUnchanged)
    {
        const VFS::Manager vfs;
        osg::ref_ptr<Cache> cache = new Cache;
        constexpr std::size_t count = 8;
        std::barrier start(static_cast<std::ptrdiff_t>(count));
        std::array<osg::ref_ptr<osg::Object>, count> winners;
        std::array<bool, count> inserted{};
        std::array<std::thread, count> threads;
        const auto before = Lifecycle::registry().stats();
        for (std::size_t i = 0; i < count; ++i)
            threads[i] = std::thread([&, i] {
                osg::ref_ptr<osg::Group> candidate = new osg::Group;
                Lifecycle::PreparationScope preparation("fixture_concurrent_optional");
                Lifecycle::RequestScope request(Lifecycle::Kind::Template, "winner.nif", &vfs, true, cache.get());
                start.arrive_and_wait();
                auto result = cache->getOrInsert(1, candidate);
                winners[i] = result.first; inserted[i] = result.second;
            });
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(std::count(inserted.begin(), inserted.end(), true), 1);
        for (const auto& winner : winners) EXPECT_EQ(winner, winners.front());
        {
            Lifecycle::RequestScope demand(Lifecycle::Kind::Template, "winner.nif", &vfs, false, cache.get());
            EXPECT_EQ(cache->getRefFromObjectCache(1), winners.front());
        }
        if (Lifecycle::enabled())
        {
            const auto after = Lifecycle::registry().stats();
            EXPECT_EQ(after.publications - before.publications, 1);
            EXPECT_EQ(after.firstDemands - before.firstDemands, 1);
            EXPECT_EQ(after.prepareHits - before.prepareHits, 1);
        }
    }

    TEST(ResourceLifecycleProduction, UndeclaredSpeculationRemainsUnknownAfterAnExactPreparationHit)
    {
        const VFS::Manager vfs;
        osg::ref_ptr<Cache> cache = new Cache;
        osg::ref_ptr<osg::Group> value = new osg::Group;
        const auto before = Lifecycle::registry().stats();
        {
            Lifecycle::RequestScope unclassified(Lifecycle::Kind::Template, "unknown-origin.nif", &vfs, true, cache.get());
            cache->addEntryToObjectCache(1, value);
        }
        {
            Lifecycle::PreparationScope preparation("fixture_declared_optional");
            Lifecycle::RequestScope request(Lifecycle::Kind::Template, "unknown-origin.nif", &vfs, true, cache.get());
            EXPECT_EQ(cache->getRefFromObjectCache(1), value);
        }
        {
            Lifecycle::RequestScope demand(Lifecycle::Kind::Template, "unknown-origin.nif", &vfs, false, cache.get());
            EXPECT_EQ(cache->getRefFromObjectCache(1), value);
        }
        if (Lifecycle::enabled())
        {
            const auto after = Lifecycle::registry().stats();
            EXPECT_EQ(after.unknownOrigins - before.unknownOrigins, 1);
            EXPECT_EQ(after.prepareHits - before.prepareHits, 1);
            EXPECT_EQ(after.firstDemands - before.firstDemands, 1);
        }
    }

    TEST(ResourceLifecycleProduction, ReplacementExpiryPressureAndClearRetireActualCacheOwners)
    {
        const VFS::Manager vfs;
        osg::ref_ptr<Cache> cache = new Cache;
        osg::ref_ptr<osg::Group> old = new osg::Group, replacement = new osg::Group;
        const auto before = Lifecycle::registry().stats();
        {
            Lifecycle::RequestScope request(Lifecycle::Kind::Template, "retirement.nif", &vfs, false, cache.get());
            cache->addEntryToObjectCache(1, old, 1);
            cache->addEntryToObjectCache(1, replacement, 1);
            EXPECT_EQ(cache->getRefFromObjectCache(1), replacement);
            EXPECT_EQ(old->referenceCount(), 1); // Retiring cache owner never means old payload died.
            cache->addEntryToObjectCache(2, new osg::Group, 1);
            cache->addEntryToObjectCache(3, new osg::Group, 100);
        }
        cache->update(3, 1); // Key 2 expires; external key 1 remains pinned, key 3 young.
        EXPECT_EQ(cache->getRefFromObjectCache(2), nullptr);
        EXPECT_EQ(cache->getRefFromObjectCache(1), replacement);
        EXPECT_EQ(cache->trimUnused(8), 0); // Original recent-use protection.
        EXPECT_EQ(cache->trimUnused(8), 1); // Key 3 only, original cache-only rule.
        cache->clear();
        EXPECT_EQ(replacement->referenceCount(), 1);
        if (Lifecycle::enabled())
        {
            const auto after = Lifecycle::registry().stats();
            EXPECT_EQ(after.publications - before.publications, 4);
            EXPECT_EQ(after.retirements - before.retirements, 4);
            EXPECT_EQ(after.active, before.active);
        }
    }

    struct TargetCallback : osg::NodeCallback, SceneUtil::Controller
    {
        osg::Node* mTarget = nullptr;
        TargetCallback() = default;
        TargetCallback(const TargetCallback& other, const osg::CopyOp& copy)
            : osg::NodeCallback(other, copy), SceneUtil::Controller(other), mTarget(other.mTarget) {}
        META_Object(ResourceLifecycleTest, TargetCallback)
        void remapTargets(const SceneUtil::CopyOp& copy) override { mTarget = copy.getClonedNode(mTarget); }
    };

    TEST(ResourceLifecycleProduction, CloneStageSplittingPreservesSharedGeometryAndControllerRemap)
    {
        osg::ref_ptr<osg::Group> source = new osg::Group;
        osg::ref_ptr<osg::Group> target = new osg::Group;
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
        vertices->push_back({1, 2, 3}); geometry->setVertexArray(vertices);
        geode->addDrawable(geometry); target->addChild(geode); source->addChild(target);
        osg::ref_ptr<TargetCallback> callback = new TargetCallback;
        callback->mTarget = target; source->setUpdateCallback(callback);

        SceneUtil::CopyOp controlCopy;
        osg::ref_ptr<osg::Group> control = static_cast<osg::Group*>(source->clone(controlCopy));
        controlCopy.remapControllerTargets();
        osg::ref_ptr<osg::Node> candidate = Resource::SceneManager::cloneNode(source);
        ASSERT_NE(candidate, source);
        ASSERT_TRUE(candidate->asGroup());
        ASSERT_EQ(candidate->asGroup()->getNumChildren(), control->getNumChildren());
        EXPECT_NE(candidate->asGroup()->getChild(0), target);
        auto* cloneCallback = dynamic_cast<TargetCallback*>(candidate->getUpdateCallback());
        ASSERT_TRUE(cloneCallback);
        EXPECT_NE(cloneCallback, callback);
        EXPECT_EQ(cloneCallback->mTarget, candidate->asGroup()->getChild(0));
        EXPECT_EQ(dynamic_cast<TargetCallback*>(control->getUpdateCallback())->mTarget, control->getChild(0));
        const auto* clonedGeode = candidate->asGroup()->getChild(0)->asGroup()->getChild(0)->asGeode();
        ASSERT_TRUE(clonedGeode);
        EXPECT_EQ(clonedGeode->getDrawable(0), geometry);
        EXPECT_EQ(geometry->getVertexArray(), vertices);
        EXPECT_EQ(callback->mTarget, target); // Original callback is untouched.
        EXPECT_EQ(candidate->getNumChildrenRequiringUpdateTraversal(), control->getNumChildrenRequiringUpdateTraversal());
    }

    TEST(ResourceLifecycleProduction, ModelConversionStagesPreserveHierarchyTransformAndHash)
    {
        Nif::NIFFile file(VFS::Path::NormalizedView("fixture/model.nif"));
        file.mVersion = Nif::NIFFile::VER_MW; file.mHash = "fixture-content-identity";
        auto makeNode = [](std::string name, unsigned index) {
            auto node = std::make_unique<Nif::NiNode>();
            node->mRecordType = Nif::RC_NiNode; node->mRecordName = "NiNode"; node->mRecordIndex = index;
            node->mName = std::move(name); node->mFlags = 0; node->mTransform = Nif::NiTransform::getIdentity();
            node->mController = Nif::NiTimeControllerPtr(nullptr); node->mExtra = Nif::ExtraPtr(nullptr);
            node->mCollision = Nif::NiCollisionObjectPtr(nullptr);
            return node;
        };
        auto root = makeNode("root", 0), child = makeNode("child", 1);
        child->mTransform.mTranslation = {2, 3, 4}; child->mParents.push_back(root.get());
        root->mChildren.emplace_back(child.get()); file.mRoots.push_back(root.get());
        file.mRecords.push_back(std::move(root)); file.mRecords.push_back(std::move(child));
        const auto converted = NifOsg::Loader::load(file, nullptr, nullptr);
        ASSERT_TRUE(converted->asGroup()); ASSERT_EQ(converted->asGroup()->getNumChildren(), 1);
        const auto* convertedRoot = converted->asGroup()->getChild(0)->asGroup();
        ASSERT_TRUE(convertedRoot); EXPECT_EQ(convertedRoot->getName(), "root");
        ASSERT_EQ(convertedRoot->getNumChildren(), 1);
        const auto* convertedChild = dynamic_cast<const osg::MatrixTransform*>(convertedRoot->getChild(0));
        ASSERT_TRUE(convertedChild); EXPECT_EQ(convertedChild->getName(), "child");
        EXPECT_EQ(convertedChild->getMatrix().getTrans(), osg::Vec3d(2, 3, 4));
        std::string hash; ASSERT_TRUE(converted->getUserValue(Misc::OsgUserValues::sFileHash, hash));
        EXPECT_EQ(hash, file.mHash);
        EXPECT_EQ(static_cast<const Nif::NiNode*>(file.mRoots.front())->mChildren.front()->mName, "child");
    }

    TEST(ResourceLifecycleFinalStatus, PreservesLargeReceiptIdsAndCoverageOutsideTimeline)
    {
        const auto path = std::filesystem::temp_directory_path()
            / ("p9-lifecycle-status-" + std::to_string(Debug::HitchTrace::Clock::now().time_since_epoch().count()) + ".txt");
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
        Lifecycle::Stats stats;
        stats.publications = 6; stats.active = 2; stats.retirements = 4;
        stats.firstDemands = 3; stats.prepareHits = 1; stats.unknownOrigins = 1;
        stats.unknownGenerations = 2; stats.unknownUses = 3; stats.metadataTruncations = 4;
        constexpr std::uint64_t receipt = 9007199254740993ULL;
        ASSERT_TRUE(Lifecycle::writeStatusFile(path.string(), stats, receipt, receipt + 1));
        std::ifstream input(path);
        std::map<std::string, std::string> fields;
        for (std::string line; std::getline(input, line);)
        {
            const auto separator = line.find('=');
            ASSERT_NE(separator, std::string::npos);
            ASSERT_TRUE(fields.emplace(line.substr(0, separator), line.substr(separator + 1)).second);
        }
        EXPECT_EQ(fields.at("status_span_id"), "9007199254740993");
        EXPECT_EQ(fields.at("coverage_span_id"), "9007199254740994");
        EXPECT_EQ(fields.at("publications"), "6");
        EXPECT_EQ(fields.at("active"), "2");
        EXPECT_EQ(fields.at("first_demands"), "3");
        EXPECT_EQ(fields.at("prepare_hits"), "1");
        EXPECT_EQ(fields.at("retirements"), "4");
        EXPECT_EQ(fields.at("unknown_origins"), "1");
        EXPECT_EQ(fields.at("unknown_generations"), "2");
        EXPECT_EQ(fields.at("unknown_uses"), "3");
        EXPECT_EQ(fields.at("metadata_truncations"), "4");
        EXPECT_EQ(fields.at("normal_finish"), "1");
        EXPECT_EQ(fields.at("output_ok"), "1");
        input.close();
        ASSERT_TRUE(Lifecycle::writeStatusFile(path.string(), {}, 1, 2));
        std::ifstream zero(path);
        const std::string zeroText{std::istreambuf_iterator<char>(zero), std::istreambuf_iterator<char>()};
        EXPECT_NE(zeroText.find("publications=0\n"), std::string::npos);
    }

    TEST(ResourceLifecycleFinalStatus, RefusesUnwritableOutput)
    {
        const auto path = std::filesystem::temp_directory_path()
            / ("p9-lifecycle-directory-" + std::to_string(Debug::HitchTrace::Clock::now().time_since_epoch().count()));
        ASSERT_TRUE(std::filesystem::create_directory(path));
        struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
        EXPECT_FALSE(Lifecycle::writeStatusFile(path.string(), {}, 1, 2));
    }

    // Declared last: all prior fixture workers and resource owners have ended.
    // Separate-process runner must select the complete ResourceLifecycle* filter.
    TEST(ResourceLifecycleProduction, EmitsStatusAfterJoinedFixtures)
    {
        Lifecycle::emitStatus();
        if (Lifecycle::enabled())
        {
            const auto stats = Lifecycle::registry().stats();
            EXPECT_EQ(stats.active, 0);
            EXPECT_EQ(stats.capacityFailures, 0);
            EXPECT_EQ(stats.allocationFailures, 0);
            EXPECT_EQ(stats.metadataTruncations, 0);
        }
    }
}
