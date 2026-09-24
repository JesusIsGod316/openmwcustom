#include <components/render/backend/vsg/staticworldsyncstate.hpp>
#include <components/render/backend/vsg/staticpopulationresidency.hpp>
#include <components/render/backend/vsg/staticworldresidency.hpp>
#include <components/rendercore/staticpopulationproducer.hpp>

#include <iostream>
#include <stdexcept>

namespace
{
    using namespace RenderCore;
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    struct Fixture
    {
        RenderWorld world;
        MeshHandle mesh = *world.reserveMesh();
        TextureHandle texture = *world.reserveTexture();
        MaterialHandle material = *world.reserveMaterial();
        MaterialHandle hiddenMaterial = *world.reserveMaterial();
        ModelHandle model = *world.reserveModel();
        InstanceHandle instance = *world.reserveInstance();

        Fixture()
        {
            auto geometry = std::make_shared<MeshPayload>();
            geometry->positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
            geometry->indices = {0, 1, 2};
            geometry->surfaces = {{PrimitiveTopology::Triangles, 0, 3, 0}};
            MeshRecord meshRecord;
            meshRecord.payload = geometry;
            meshRecord.surfaceCount = 1;
            require(world.commit(mesh, meshRecord), "mesh publication");
            TextureRecord textureRecord;
            textureRecord.contentIdentity = "static-sync-fixture";
            require(world.commit(texture, textureRecord), "texture publication");
            MaterialRecord materialRecord;
            materialRecord.textures.push_back(TextureBinding{.texture = texture});
            require(world.commit(material, materialRecord), "material publication");
            require(world.commit(hiddenMaterial, materialRecord), "hidden material publication");
            auto payload = std::make_shared<ModelPayload>();
            ModelNodeRecord node;
            node.name = "visible";
            node.kind = ModelNodeKind::Geometry;
            node.mesh = mesh;
            node.materials = {material};
            payload->nodes.push_back(node);
            node.name = "hidden";
            node.flags = modelNodeFlag(ModelNodeFlag::Hidden);
            node.materials = {hiddenMaterial};
            payload->nodes.push_back(node);
            payload->roots = {ModelNodeIndex{0}, ModelNodeIndex{1}};
            ModelRecord modelRecord;
            modelRecord.payload = payload;
            require(world.commit(model, modelRecord), "model publication");
            InstanceRecord placement;
            placement.model = model;
            require(world.commit(instance, placement), "instance publication");
        }

        template <class Handle>
        void revise(Handle handle)
        {
            auto record = *world.get(handle);
            record.revision = *advanceMonotonic(record.revision);
            require(world.update(handle, std::move(record)), "resource revision update");
        }
    };

    void populationResidencyRegression(bool incremental)
    {
        Fixture f;
        RenderWorldPublisher publisher(f.world);
        StaticPopulationProducer producer(f.world, publisher);
        require(producer.addCell({.identity = "cell", .worldspaceIdentity = "world"})
                == StaticPopulationPublishStatus::Applied, "population cell creation");
        constexpr std::size_t groupCount = 224;
        StaticPopulationInstanceSource moving;
        for (std::size_t i = 0; i < groupCount; ++i)
        {
            const auto model = *f.world.reserveModel();
            require(f.world.commit(model, *f.world.get(f.model)), "population model publication");
            StaticPopulationInstanceSource source;
            source.identity = "ref:" + std::to_string(i);
            source.cellIdentity = "cell";
            source.model = model;
            source.transform.translation.x = static_cast<double>(i);
            if (i == 0) moving = source;
            require(producer.upsert(source) == StaticPopulationPublishStatus::Applied, "population insertion");
        }
        require(producer.flush() == StaticPopulationPublishStatus::Applied, "population flush");
        RenderVsg::StaticPopulationResidency<std::shared_ptr<int>> residency;
        auto prepare = [&](RenderVsg::StaticPlanOptions options = {}) {
            return incremental ? residency.prepareIncremental(f.world, options) : residency.prepare(f.world, options);
        };
        auto initial = prepare();
        require(initial.valid && initial.upserts.size() == groupCount, "initial population groups");
        std::vector<std::shared_ptr<int>> objects(groupCount);
        for (auto& object : objects) object = std::make_shared<int>(1);
        const auto keptIdentity = RenderVsg::populationIdentity(initial.upserts[1]);
        const auto keptObject = objects[1];
        const auto originalPlan = initial.upserts[1];
        const auto originalPlacement = originalPlan.placements.front();
        auto checkPlacementChange = [&](auto edit) {
            auto changed = originalPlacement;
            edit(changed);
            require(!RenderVsg::equivalentPopulationPlacement(originalPlacement, changed),
                "every authored placement field participates in exact reuse validation");
        };
        require(RenderVsg::equivalentPopulationPlacement(originalPlacement, originalPlacement), "identical placement equality");
        checkPlacementChange([](auto& p) { p.sourceIdentity += "changed"; });
        checkPlacementChange([](auto& p) { p.transform.translation.x += 0.000001; });
        checkPlacementChange([](auto& p) { p.transform.rotation.x += 0.1f; });
        checkPlacementChange([](auto& p) { p.transform.scale.x += 0.1f; });
        checkPlacementChange([](auto& p) { p.localBounds.minimum.x -= 1; });
        checkPlacementChange([](auto& p) { p.localBounds.maximum.x += 1; });
        checkPlacementChange([](auto& p) { p.lod.center.x += 1; });
        checkPlacementChange([](auto& p) { p.lod.minimumDistance += 1; });
        checkPlacementChange([](auto& p) { p.lod.maximumDistance = 100; });
        checkPlacementChange([](auto& p) { p.lod.scale += 1; });
        checkPlacementChange([](auto& p) { p.lod.smallFeatureEligible = !p.lod.smallFeatureEligible; });
        checkPlacementChange([](auto& p) { p.semanticFlags ^= semanticFlag(InstanceSemanticFlag::ShadowCaster); });
        checkPlacementChange([](auto& p) { p.lightingEnabled = !p.lightingEnabled; });
        require(residency.commit(f.world, initial, std::move(objects)).committed, "initial population commit");
        require(residency.markSubmitted(FrameId{1}), "population submission");
        for (int frame = 0; frame < 100; ++frame)
        {
            require(producer.upsert(moving) == StaticPopulationPublishStatus::AlreadyPresent, "idempotent source update");
            require(producer.flush() == StaticPopulationPublishStatus::AlreadyPresent, "clean producer flush");
            f.revise(f.instance); // unrelated world revision, like a live actor
            require(prepare().upserts.empty(), "stable populations do not rebuild");
        }

        // The moved placement also changes the cell bounds/packing origin.
        // Both modes consume the exact same world and residency in one binary.
        moving.transform.translation.x = 10000;
        require(producer.upsert(moving) == StaticPopulationPublishStatus::Applied, "moving source update");
        require(producer.flush() == StaticPopulationPublishStatus::Applied, "moving source publication");
        const auto plan = RenderVsg::buildStaticWorldPlan(f.world);
        auto repaired = incremental ? residency.prepareIncremental(f.world) : residency.prepare(f.world, plan);
        const auto control = residency.prepare(f.world, plan, true);
        require(repaired.upserts.size() == 1 && control.upserts.size() == groupCount,
            "one changed group must not rebuild all 224 groups; coarse control reproduces fanout");
        require(!RenderVsg::staticPopulationPlanCurrent(f.world, originalPlan), "strict commit rejects old chunk stamp");
        require(RenderVsg::staticPopulationPlanCurrent(f.world, originalPlan, true), "unchanged group retains packing origin");
        require(residency.commit(f.world, repaired, {std::make_shared<int>(2)}).committed, "selective replacement commit");
        require(*residency.residentObject(keptIdentity) == keptObject, "unchanged resident object identity retained");
        require(residency.pendingRetirementCount() == 1, "only changed submitted group retired");
        require(residency.collect(FrameId{0}).empty(), "early retirement blocked");
        require(residency.collect(FrameId{1}).size() == 1, "changed group released after completion");
        for (int frame = 0; frame < 100; ++frame)
            require(prepare().upserts.empty(), "retained older stamps must not trigger subsequent rebuilds");

        moving.lod.smallFeatureEligible = false;
        require(producer.upsert(moving) == StaticPopulationPublishStatus::Applied, "small-feature policy change preserved");
        require(producer.flush() == StaticPopulationPublishStatus::Applied, "small-feature policy published");
        repaired = prepare();
        require(repaired.upserts.size() == 1, "small-feature change invalidates only its group");
        require(residency.commit(f.world, repaired, {std::make_shared<int>(3)}).committed, "policy update commit");

        auto checkDependencies = [&] {
            auto mutation = prepare();
            require(mutation.upserts.size() == groupCount, "real shared-resource changes invalidate all dependent groups");
            std::vector<std::shared_ptr<int>> replacements(groupCount, std::make_shared<int>(4));
            require(residency.commit(f.world, mutation, std::move(replacements)).committed, "resource replacement commit");
        };
        f.revise(f.texture); checkDependencies();
        f.revise(f.material); checkDependencies();
        f.revise(f.mesh); checkDependencies();
        auto options = RenderVsg::StaticPlanOptions{.showMarkers = true};
        require(prepare(options).upserts.size() == groupCount, "options still invalidate populations");

        require(producer.remove(moving.identity) == StaticPopulationPublishStatus::Applied, "remove one group");
        require(producer.flush() == StaticPopulationPublishStatus::Applied, "publish removal");
        auto removal = prepare();
        require(removal.removals.size() == 1 && removal.upserts.empty(), "removal retains unrelated groups despite bounds change");
        f.revise(f.texture);
        require(!residency.commit(f.world, removal, {}).committed, "intervening publication rejects stale mutation");
        std::cout << "PASS population regression: 224-to-1 rebuild fanout, stable frames, exact policy changes, "
                     "resource/option invalidation, removal, stale commit and fenced retirement\n";
    }
}

int main()
{
    try
    {
        {
            Fixture f;
            RenderVsg::StaticWorldResidency<std::shared_ptr<int>> cache;
            auto sync = [&] {
                auto mutation = cache.prepareIncremental(f.world);
                std::vector<std::shared_ptr<int>> objects(mutation.upserts.size(), std::make_shared<int>(1));
                require(cache.commit(f.world, mutation, std::move(objects)).committed, "incremental instance commit");
                return mutation;
            };
            require(sync().upserts.size() == 1, "initial instance build");
            require(cache.markSubmitted(FrameId{1}), "instance submit");
            const auto original = *cache.residentObject(f.instance);
            const auto chunk = *f.world.reserveChunk();
            require(f.world.commit(chunk, ChunkRecord{}), "unrelated chunk");
            for (int i = 0; i < 100; ++i)
            {
                f.revise(chunk);
                const auto stable = sync();
                require(stable.upserts.empty() && stable.reusedPlans == 1, "population change replanned stable LAND/instance");
            }
            require(*cache.residentObject(f.instance) == original, "unchanged native graph replaced");
            f.revise(f.hiddenMaterial);
            require(sync().upserts.size() == 1, "hidden material invalidation");
            f.revise(f.texture); require(sync().upserts.size() == 1, "texture invalidation");
            f.revise(f.mesh); require(sync().upserts.size() == 1, "mesh invalidation");
            f.revise(f.instance); require(sync().upserts.size() == 1, "placement invalidation");
            require(cache.prepareIncremental(f.world, {.showMarkers = true}).upserts.size() == 1, "option invalidation");
            auto stale = cache.prepareIncremental(f.world);
            f.revise(f.instance);
            require(!cache.commit(f.world, stale, {}).committed, "stale transaction accepted");
            sync();
            RenderWorld distinct = f.world;
            require(cache.prepareIncremental(distinct).upserts.size() == 1, "different world reused aliases");
            require(cache.collect(FrameId{0}).empty() && cache.collect(FrameId{1}).size() == 1, "instance fence retirement");
            require(f.world.retire(f.instance), "instance removal");
            require(sync().removals.size() == 1 && cache.residentCount() == 0, "retired instance retained");
            require(f.world.reset(), "instance epoch reset");
            require(sync().reusedPlans == 0, "epoch reused");
            std::cout << "PASS incremental instances: stable LAND path, hidden/mesh/texture/placement/options, world identity, stale commit, fences, removal\n";
        }
        populationResidencyRegression(false);
        populationResidencyRegression(true);
        Fixture f;
        RenderVsg::StaticWorldSyncState state;
        auto changed = [&] { return !state.unchanged(f.world, {}); };
        auto acknowledge = [&] {
            state.synchronized();
            require(!changed(), "acknowledged static inputs must remain unchanged");
        };
        require(changed(), "first synchronization must run");
        require(changed(), "failed or unacknowledged synchronization must retry");
        acknowledge();
        {
            const auto staticStamp = f.world.staticRevision();
            const auto assetStamp = f.world.assetRevision();
            const auto reserved = *f.world.reserveInstance();
            require(f.world.cancel(reserved), "reserved instance cancellation");
            require(!f.world.update(f.mesh, *f.world.get(f.mesh)), "same asset revision must be rejected");
            const auto light = *f.world.reserveLight();
            require(f.world.commit(light, LightRecord{}), "light publication");
            f.revise(light);
            require(f.world.retire(light), "light retirement");
            require(f.world.staticRevision() == staticStamp && f.world.assetRevision() == assetStamp,
                "failed publications, reservations or lights dirtied static assets");
            const auto chunk = *f.world.reserveChunk();
            require(f.world.commit(chunk, ChunkRecord{}), "scene chunk publication");
            require(f.world.reparentInstance(f.instance, chunk), "static reparent");
            require(changed(), "reparent did not invalidate static scene"); acknowledge();
            require(f.world.reparentInstance(f.instance, std::nullopt), "static unparent");
            require(changed(), "unparent did not invalidate static scene"); acknowledge();
            require(f.world.retire(chunk), "empty scene chunk retirement");
            require(!changed(), "irrelevant empty chunk changed static dependency stamps");
            require(f.world.assetRevision() == assetStamp, "placement bookkeeping dirtied assets");
        }
        for (int frame = 0; frame < 100; ++frame)
            require(!changed(), "stable static scene must not replan every frame");

        f.revise(f.instance); require(changed(), "instance revision"); acknowledge();
        f.revise(f.model); require(changed(), "model revision"); acknowledge();
        f.revise(f.mesh); require(changed(), "mesh revision"); acknowledge();
        f.revise(f.material); require(changed(), "material revision"); acknowledge();
        f.revise(f.hiddenMaterial); require(changed(), "hidden sort material revision"); acknowledge();
        f.revise(f.texture); require(changed(), "texture revision"); acknowledge();

        const auto extra = *f.world.reserveInstance();
        require(f.world.commit(extra, *f.world.get(f.instance)), "extra instance publication");
        require(changed(), "new instance sharing an existing model"); acknowledge();
        require(f.world.retire(extra), "instance retirement");
        require(changed(), "removed instance"); acknowledge();
        require(f.world.retire(f.instance), "original retirement");
        InstanceRecord replacement;
        replacement.model = f.model;
        f.instance = *f.world.reserveInstance();
        require(f.world.commit(f.instance, replacement), "replacement publication");
        require(changed(), "same population count with a new handle generation"); acknowledge();

        const auto skeleton = *f.world.reserveSkeleton();
        require(f.world.commit(skeleton, SkeletonRecord{}), "skeleton publication");
        const auto actor = *f.world.reserveInstance();
        InstanceRecord actorRecord;
        actorRecord.model = f.model; actorRecord.skeleton = skeleton;
        require(f.world.commit(actor, actorRecord), "actor publication");
        require(!changed(), "new dynamic actor is not static work");
        f.revise(actor); f.revise(skeleton);
        require(!changed(), "dynamic revisions must not invalidate static geometry");
        actorRecord = *f.world.get(actor);
        actorRecord.skeleton.reset();
        actorRecord.revision = *advanceMonotonic(actorRecord.revision);
        require(f.world.update(actor, actorRecord), "actor to static classification");
        require(changed(), "actor becoming static"); acknowledge();
        actorRecord.skeleton = skeleton;
        actorRecord.revision = *advanceMonotonic(actorRecord.revision);
        require(f.world.update(actor, actorRecord), "static to actor classification");
        require(changed(), "static becoming actor"); acknowledge();

        auto options = RenderVsg::StaticPlanOptions{.showMarkers = true};
        require(!state.unchanged(f.world, options), "marker options must invalidate");
        state.synchronized();
        options.nightDaySwitchState = NightDaySwitchState::ExteriorNight;
        require(!state.unchanged(f.world, options), "night/day options must invalidate");
        require(changed(), "restore base options"); acknowledge();

        const auto chunk = *f.world.reserveChunk();
        auto population = std::make_shared<StaticPopulationPayload>();
        PopulationInstanceRecord placement;
        placement.sourceIdentity = "population:one";
        population->groups.push_back({f.model, {placement}});
        ChunkRecord chunkRecord;
        chunkRecord.kind = ChunkRecord::Kind::StaticPopulation;
        chunkRecord.population = population;
        require(f.world.commit(chunk, chunkRecord), "population publication");
        const auto originalPlan = RenderVsg::buildStaticPopulationPlan(f.world, chunk, population->groups.front(), {});
        require(originalPlan.has_value(), "population asset fixture");
        auto movedPlan = *originalPlan;
        movedPlan.placements.front().transform.translation.x += 100;
        movedPlan.coordinateOrigin.x += 200;
        require(RenderVsg::reusablePopulationAsset(*originalPlan, movedPlan), "movement invalidated immutable asset");
        movedPlan.materials.front().revision = *advanceMonotonic(movedPlan.materials.front().revision);
        require(!RenderVsg::reusablePopulationAsset(*originalPlan, movedPlan), "material revision reused stale asset");
        movedPlan = *originalPlan; movedPlan.options.showMarkers = !movedPlan.options.showMarkers;
        require(!RenderVsg::reusablePopulationAsset(*originalPlan, movedPlan), "changed model selection reused asset");
        movedPlan = *originalPlan; movedPlan.sourceEpoch = *advanceMonotonic(movedPlan.sourceEpoch);
        require(!RenderVsg::reusablePopulationAsset(*originalPlan, movedPlan), "world reset reused prior resource");
        require(changed(), "new population"); acknowledge();
        f.revise(chunk); require(changed(), "population replacement"); acknowledge();
        require(f.world.retire(chunk), "population retirement");
        require(changed(), "removed population"); acknowledge();

        const auto meshOnly = *f.world.reserveInstance();
        InstanceRecord unsupported;
        unsupported.mesh = f.mesh;
        require(f.world.commit(meshOnly, unsupported), "mesh-only publication");
        require(changed(), "unsupported populations must still reach full validation");
        require(f.world.retire(meshOnly), "mesh-only removal");
        require(!changed(), "failed attempt must not replace the last successful stamps");

        require(f.world.reset(), "world reset");
        require(changed(), "world epoch reset"); acknowledge();
        std::cout << "PASS static sync: stable frames, resource revisions, hidden materials, population changes, "
                     "actor classification, options, generation, epoch, failure retry\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL static sync: " << error.what() << '\n';
        return 1;
    }
}
