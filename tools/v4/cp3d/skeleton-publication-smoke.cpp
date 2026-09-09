#include <components/nifrender/staticmodelcache.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3D skeleton publication failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    using namespace RenderCore;

    NifRender::TranslationBundle bundle;
    bundle.sourceIdentity = "meshes\\actor.nif";
    bundle.contentIdentity = "sha256:actor";
    bundle.model.sourceIdentity = bundle.sourceIdentity;
    bundle.model.contentIdentity = bundle.contentIdentity;
    bundle.model.skeleton = NifRender::SkeletonIndex{ 0 };

    auto payload = std::make_shared<SkeletonPayload>();
    BoneRecord root;
    root.name = "bip01";
    payload->bones.push_back(root);
    BoneRecord head;
    head.name = "bip01 head";
    head.parent = 0;
    payload->bones.push_back(head);
    NifRender::TranslatedSkeleton translated;
    translated.record.sourceIdentity = bundle.sourceIdentity + "#skeleton";
    translated.record.payload = payload;
    bundle.skeletons.push_back(std::move(translated));

    NifRender::TranslatedModelNode rootNode;
    rootNode.name = "actor root";
    bundle.model.nodes.push_back(rootNode);
    bundle.model.roots.push_back(ModelNodeIndex{ 0 });
    if (!require(bundle.valid(), "synthetic actor translation bundle"))
        return EXIT_FAILURE;

    NifRender::TranslationBundle invalidRequirements = bundle;
    invalidRequirements.model.dynamicRequirements = 1u << 31u;
    if (!require(!invalidRequirements.valid(), "unknown dynamic model requirement rejected"))
        return EXIT_FAILURE;

    RenderWorld world;
    RenderWorldPublisher publisher(world);
    NifRender::StaticModelCache cache(world, publisher);
    const NifRender::StaticModelCacheResult published = cache.publish(bundle);
    if (!require(published.available() && published.model.valid() && published.skeleton.has_value(),
            "model and skeleton published atomically")
        || !require(world.get(*published.skeleton) != nullptr, "published skeleton is live"))
        return EXIT_FAILURE;

    const NifRender::StaticModelCacheResult reused = cache.publish(bundle);
    if (!require(reused.status == NifRender::StaticModelCacheStatus::Reused
            && reused.skeleton == published.skeleton,
            "cache reuses skeleton binding"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3D skeleton translation publication: PASS\n";
    return EXIT_SUCCESS;
}
