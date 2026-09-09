#include <components/rendercore/activecellproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3D actor lifecycle failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    using namespace RenderCore;
    RenderWorld world;
    RenderWorldPublisher publisher(world);
    ActiveCellProducer cells(world, publisher);

    ActiveCellSource cell;
    cell.identity = "cell:a";
    if (!require(cells.addCell(cell).applied(), "publish cell"))
        return EXIT_FAILURE;

    auto modelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord modelRoot;
    modelRoot.name = "actor root";
    modelPayload->nodes.push_back(modelRoot);
    modelPayload->roots.push_back(ModelNodeIndex{ 0 });
    const auto model = world.reserveModel();
    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "actor:model";
    modelRecord.contentIdentity = "sha256:actor";
    modelRecord.payload = modelPayload;
    if (!require(model && world.commit(*model, std::move(modelRecord)), "publish model"))
        return EXIT_FAILURE;

    auto skeletonPayload = std::make_shared<SkeletonPayload>();
    BoneRecord root;
    root.name = "bip01";
    skeletonPayload->bones.push_back(root);
    const auto skeleton = world.reserveSkeleton();
    SkeletonRecord skeletonRecord;
    skeletonRecord.sourceIdentity = "actor:skeleton";
    skeletonRecord.payload = skeletonPayload;
    if (!require(skeleton && world.commit(*skeleton, std::move(skeletonRecord)), "publish skeleton"))
        return EXIT_FAILURE;

    DynamicInstanceSource actor;
    actor.identity = "ref:actor";
    actor.cellIdentity = cell.identity;
    actor.model = *model;
    actor.skeleton = *skeleton;
    const ActiveCellPublishResult created = cells.upsertDynamicInstance(actor);
    if (!require(created.applied() && created.instance.valid(), "publish actor instance"))
        return EXIT_FAILURE;
    const InstanceRecord* first = world.get(created.instance);
    if (!require(first && first->skeleton == skeleton && first->chunk.has_value(), "actor references retained"))
        return EXIT_FAILURE;
    const ResourceRevision firstRevision = first->revision;

    actor.transform.translation.x = 12.0;
    const ActiveCellPublishResult updated = cells.upsertDynamicInstance(actor);
    if (!require(updated.applied() && updated.instance == created.instance, "stable actor handle on update"))
        return EXIT_FAILURE;
    const InstanceRecord* second = world.get(created.instance);
    if (!require(second && second->revision > firstRevision && second->transform.translation.x == 12.0,
            "actor transform update revisioned"))
        return EXIT_FAILURE;

    if (!require(cells.removeCell(cell.identity).applied(), "retire actor with cell")
        || !require(world.get(created.instance) == nullptr && cells.instanceCount() == 0,
            "cell retirement owns actor retirement"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3D active-cell actor lifecycle: PASS\n";
    return EXIT_SUCCESS;
}
