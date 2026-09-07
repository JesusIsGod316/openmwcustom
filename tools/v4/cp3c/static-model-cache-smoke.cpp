#include <components/nifrender/staticmodelcache.hpp>
#include <components/rendercore/activecellproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C static model cache failure: " << message << '\n';
        return condition;
    }

    NifRender::TranslationBundle makeBundle(std::string contentIdentity)
    {
        NifRender::TranslationBundle result;
        result.sourceIdentity = "meshes/mod-winner.nif";
        result.contentIdentity = std::move(contentIdentity);
        result.model.sourceIdentity = result.sourceIdentity;
        result.model.contentIdentity = result.contentIdentity;
        return result;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::RenderWorldPublisher publisher(world);
    NifRender::StaticModelCache models(world, publisher);
    RenderCore::ActiveCellProducer cells(world, publisher);
    const NifRender::TranslationBundle original = makeBundle("sha256:original");

    const NifRender::StaticModelCacheResult first = models.publish(original);
    if (!require(first.status == NifRender::StaticModelCacheStatus::Published && first.model.valid(),
            "publish winning VFS model")
        || !require(publisher.lastSequence() == RenderCore::InitialUpdateSequence, "asset uses first shared sequence"))
        return EXIT_FAILURE;

    const RenderCore::RenderWorldRevision beforeReuse = world.revision();
    const NifRender::StaticModelCacheResult reused = models.publish(original);
    if (!require(reused.status == NifRender::StaticModelCacheStatus::Reused && reused.model == first.model,
            "same path and content reuses stable model")
        || !require(world.revision() == beforeReuse && publisher.lastSequence() == RenderCore::InitialUpdateSequence,
            "cache hit does not republish resources"))
        return EXIT_FAILURE;

    RenderCore::ActiveCellSource cell;
    cell.identity = "world:morrowind/exterior:0,-2";
    if (!require(cells.addCell(cell).applied(), "cell producer shares publisher")
        || !require(publisher.lastSequence() == RenderCore::UpdateSequence{ 2 }, "cell uses next shared sequence"))
        return EXIT_FAILURE;

    const RenderCore::RenderWorldRevision beforeConflict = world.revision();
    const NifRender::StaticModelCacheResult conflict = models.publish(makeBundle("sha256:changed"));
    if (!require(conflict.status == NifRender::StaticModelCacheStatus::ContentConflict,
            "mid-epoch content replacement fails closed")
        || !require(conflict.model == first.model && world.revision() == beforeConflict,
            "conflict preserves published model"))
        return EXIT_FAILURE;

    if (!require(world.reset(), "world reset") || !require(!models.find(original.sourceIdentity), "stale cache cleared")
        || !require(models.size() == 0, "empty post-reset cache"))
        return EXIT_FAILURE;
    const NifRender::StaticModelCacheResult afterReset = models.publish(original);
    if (!require(afterReset.status == NifRender::StaticModelCacheStatus::Published,
            "content may republish in new epoch")
        || !require(afterReset.model != first.model, "new epoch uses generation-safe model handle")
        || !require(publisher.lastSequence() == RenderCore::InitialUpdateSequence,
            "new epoch restarts shared sequence"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C static model cache: PASS\n";
    return EXIT_SUCCESS;
}
