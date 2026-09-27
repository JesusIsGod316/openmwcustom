#include <components/rendercore/staticpopulationproducer.hpp>
#include <iostream>
#include <stdexcept>
#include <limits>

namespace
{
    using namespace RenderCore;
    unsigned checks = 0;
    void check(bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }
    ModelHandle model(RenderWorld& world)
    {
        auto handle = world.reserveModel();
        auto payload = std::make_shared<ModelPayload>();
        payload->nodes.emplace_back();
        payload->roots.push_back(ModelNodeIndex{0});
        ModelRecord record;
        record.payload = payload;
        check(handle && world.commit(*handle, record), "create model");
        return *handle;
    }
    std::map<std::uint64_t, ChunkRecord> snapshot(const RenderWorld& world)
    {
        std::map<std::uint64_t, ChunkRecord> result;
        world.forEachChunk([&](ChunkHandle h, const ChunkRecord& r) {
            result.emplace((std::uint64_t(h.generation()) << 32) | h.slot(), r);
        });
        return result;
    }
    void exercise(bool groups, bool transactions)
    {
        RenderWorld world;
        RenderWorldPublisher publisher(world, transactions);
        StaticPopulationProducer producer(world, publisher, groups);
        constexpr auto applied = StaticPopulationPublishStatus::Applied;
        constexpr auto present = StaticPopulationPublishStatus::AlreadyPresent;
        check(producer.addCell({.identity="a", .worldspaceIdentity="world"}) == applied, "cell a");
        check(producer.addCell({.identity="b", .worldspaceIdentity="world", .groundcover=true}) == applied, "cell b");
        std::vector<StaticPopulationInstanceSource> sources;
        for (unsigned i = 0; i < 32; ++i)
        {
            const auto handle = model(world);
            for (unsigned j = 0; j < 4; ++j)
            {
                StaticPopulationInstanceSource source;
                source.identity = std::to_string(i) + ":" + std::to_string(j);
                source.cellIdentity = "a";
                source.model = handle;
                source.localBounds.maximum = glm::vec3(1);
                check(producer.upsert(source) == applied, "insert");
                sources.push_back(source);
            }
        }
        check(producer.flush() == applied && world.validPublication(), "initial flush");
        const auto before = snapshot(world);
        check(before.size() == (groups ? 32u : 1u), "chunk granularity");
        sources[0].transform.translation.x = 50;
        check(producer.upsert(sources[0]) == applied && producer.flush() == applied, "move flush");
        const auto after = snapshot(world);
        unsigned rebuilt = 0, rebuiltPlacements = 0;
        for (const auto& [key, record] : after)
        {
            if (record.population == before.at(key).population) continue;
            ++rebuilt;
            for (const auto& group : record.population->groups)
                rebuiltPlacements += static_cast<unsigned>(group.instances.size());
        }
        check(rebuilt == 1 && rebuiltPlacements == (groups ? 4u : 128u), "unrelated placements were reconstructed");
        check(producer.upsert(sources[0]) == present && producer.flush() == present, "unchanged update");
        sources[0].model = sources[4].model;
        sources[0].cellIdentity = "b";
        check(producer.upsert(sources[0]) == applied && producer.flush() == applied, "cross-cell/model move");
        check(world.validPublication() && producer.stagedInstanceCount() == 128, "move duplicated object");
        unsigned copies = 0;
        world.forEachChunk([&](ChunkHandle, const ChunkRecord& chunk) {
            for (const auto& group : chunk.population->groups)
                for (const auto& source : group.instances)
                    if (source.sourceIdentity == sources[0].identity)
                    {
                        ++copies;
                        check(group.model == sources[4].model && chunk.kind == ChunkRecord::Kind::Groundcover
                            && (source.semanticFlags & semanticFlag(InstanceSemanticFlag::Groundcover)), "move semantics");
                    }
        });
        check(copies == 1, "object duplicated or lost");
        check(producer.removeCell("a") == applied && producer.stagedInstanceCount() == 1, "unload cell");
        check(producer.remove(sources[0].identity) == applied && producer.flush() == applied, "retire last group");
        check(snapshot(world).empty() && producer.flush() == present && world.validPublication(), "retirement invariant");
        check(world.reset() && producer.stagedInstanceCount() == 0, "epoch reset");
        check(producer.upsert(sources[0]) == StaticPopulationPublishStatus::InvalidSource, "stale model accepted");
    }
    void atomicity(bool fast)
    {
        RenderWorld world;
        RenderWorldPublisher publisher(world, fast);
        const auto mh = model(world);
        std::vector<ChunkHandle> handles;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto h = world.reserveChunk();
            check(h && world.commit(*h, ChunkRecord{}), "initial chunk");
            handles.push_back(*h);
        }
        const auto* modelAddress = world.get(mh);
        const auto revision = world.revision();
        const auto assetRevision = world.assetRevision();
        RenderWorldUpdateBatch invalid(world.epoch(), publisher.nextSequence());
        auto a = *world.get(handles[0]); a.revision = ResourceRevision{2};
        auto b = *world.get(handles[1]); b.revision = ResourceRevision{2};
        b.bounds.minimum.x = std::numeric_limits<float>::quiet_NaN();
        check(invalid.add(UpdateChunk{handles[0], a}) && invalid.add(UpdateChunk{handles[1], b}) && invalid.seal(), "invalid batch build");
        check(publisher.apply(invalid) == PublishStatus::OperationRejected && world.revision() == revision
            && world.get(handles[0])->revision == ResourceRevision{1}, "partial publication on rejection");
        b.bounds.minimum.x = 0;
        RenderWorldUpdateBatch valid(world.epoch(), publisher.nextSequence());
        check(valid.add(UpdateChunk{handles[0], a}) && valid.add(UpdateChunk{handles[1], b}) && valid.seal(), "valid batch build");
        check(publisher.apply(valid) == PublishStatus::Applied && world.validPublication(), "atomic update");
        check(world.assetRevision() == assetRevision && world.staticRevision() == world.revision(), "revision channels");
        if (fast) check(world.get(mh) == modelAddress, "unrelated model storage copied");
        RenderWorldUpdateBatch repeated(world.epoch(), publisher.nextSequence());
        a.revision = ResourceRevision{3}; b = a; b.revision = ResourceRevision{4};
        check(repeated.add(UpdateChunk{handles[0], a}) && repeated.add(UpdateChunk{handles[0], b}) && repeated.seal(), "ordered updates build");
        check(publisher.apply(repeated) == PublishStatus::Applied && world.get(handles[0])->revision == b.revision,
            "repeated handles must retain ordered baseline semantics");
        check(publisher.apply(repeated) == PublishStatus::OutOfOrder, "sequence replay");
        check(world.reset() && publisher.apply(repeated) == PublishStatus::StaleEpoch, "stale epoch");
    }

    void mutationParity()
    {
        RenderWorld baseline, changed;
        RenderWorldPublisher baselinePublisher(baseline), changedPublisher(changed, true);
        StaticPopulationProducer control(baseline, baselinePublisher), repair(changed, changedPublisher, true);
        std::vector<ModelHandle> models;
        for (unsigned i = 0; i < 5; ++i)
        {
            models.push_back(model(baseline));
            check(models.back() == model(changed), "model handle setup");
        }
        for (auto* producer : {&control, &repair})
            for (const auto* cell : {"left", "right"})
                check(producer->addCell({.identity=cell, .worldspaceIdentity="world"})
                    == StaticPopulationPublishStatus::Applied, "parity cell");
        // Deterministic mixed moves, model swaps, repeated updates, deletions,
        // and reinsertions. Compare authoritative placements, not chunk layout.
        using Values = std::map<std::string, std::pair<ModelHandle, PopulationInstanceRecord>>;
        const auto flatten = [](const RenderWorld& world) {
            Values values;
            world.forEachChunk([&](ChunkHandle, const ChunkRecord& chunk) {
                for (const auto& group : chunk.population->groups)
                    for (const auto& placement : group.instances)
                        check(values.emplace(placement.sourceIdentity, std::make_pair(group.model, placement)).second,
                            "duplicate placement identity");
            });
            return values;
        };
        unsigned state = 123456;
        for (unsigned step = 0; step < 240; ++step)
        {
            state = state * 1664525u + 1013904223u;
            StaticPopulationInstanceSource source;
            source.identity = "object:" + std::to_string((state >> 4) % 31);
            source.cellIdentity = state & 1 ? "left" : "right";
            source.model = models[(state >> 8) % models.size()];
            source.transform.translation = {step, -static_cast<double>(step), 3};
            source.localBounds.minimum = glm::vec3(-2);
            source.localBounds.maximum = glm::vec3(4);
            source.lightingEnabled = (state & 4) != 0;
            if (step % 4 == 0) check(control.remove(source.identity) == repair.remove(source.identity), "remove parity");
            else check(control.upsert(source) == repair.upsert(source), "upsert parity");
            check(control.flush() == repair.flush(), "flush parity");
            const auto a = flatten(baseline), b = flatten(changed);
            check(a.size() == b.size() && baseline.validPublication() && changed.validPublication(), "publication parity");
            for (const auto& [identity, entry] : a)
            {
                const auto& other = b.at(identity);
                check(entry.first == other.first && entry.second.transform.translation == other.second.transform.translation
                    && entry.second.localBounds.minimum == other.second.localBounds.minimum
                    && entry.second.localBounds.maximum == other.second.localBounds.maximum
                    && entry.second.lightingEnabled == other.second.lightingEnabled
                    && entry.second.semanticFlags == other.second.semanticFlags, "placement changed across routes");
            }
        }
    }
}
int main()
{
    try
    {
        for (bool groups : {false, true}) for (bool transactions : {false, true}) exercise(groups, transactions);
        atomicity(false); atomicity(true);
        mutationParity();
        std::cout << "PASS group publication/atomic transactions: " << checks << " checks\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
