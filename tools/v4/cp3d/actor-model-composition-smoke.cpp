#include <components/nifrender/actormodelcomposer.hpp>

#include <cassert>
#include <memory>

int main()
{
    using namespace RenderCore;
    RenderWorld world;

    auto skeletonPayload = std::make_shared<SkeletonPayload>();
    BoneRecord root;
    root.name = "bip01";
    skeletonPayload->bones.push_back(root);
    BoneRecord hand;
    hand.name = "bip01 hand";
    hand.parent = 0;
    skeletonPayload->bones.push_back(hand);
    const auto skeleton = world.reserveSkeleton();
    SkeletonRecord skeletonRecord;
    skeletonRecord.payload = skeletonPayload;
    assert(skeleton && world.commit(*skeleton, std::move(skeletonRecord)));

    auto basePayload = std::make_shared<ModelPayload>();
    ModelNodeRecord baseRoot;
    baseRoot.name = "Bip01";
    basePayload->nodes.push_back(baseRoot);
    ModelNodeRecord baseHand;
    baseHand.name = "Bip01 Hand";
    baseHand.parent = ModelNodeIndex{ 0u };
    basePayload->nodes.push_back(baseHand);
    basePayload->roots.push_back(ModelNodeIndex{ 0u });
    const auto base = world.reserveModel();
    ModelRecord baseRecord;
    baseRecord.payload = basePayload;
    assert(base && world.commit(*base, std::move(baseRecord)));

    const auto material = world.reserveMaterial();
    assert(material && world.commit(*material, MaterialRecord{}));
    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    meshPayload->indices = { 0, 1, 2 };
    meshPayload->surfaces.push_back({ PrimitiveTopology::Triangles, 0, 3, 0 });
    const auto mesh = world.reserveMesh();
    MeshRecord meshRecord;
    meshRecord.surfaceCount = 1;
    meshRecord.payload = meshPayload;
    assert(mesh && world.commit(*mesh, std::move(meshRecord)));

    auto partPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord partRoot;
    partRoot.name = "weapon root";
    partPayload->nodes.push_back(partRoot);
    ModelNodeRecord geometry;
    geometry.name = "weapon geometry";
    geometry.parent = ModelNodeIndex{ 0u };
    geometry.kind = ModelNodeKind::Geometry;
    geometry.mesh = *mesh;
    geometry.materials = { *material };
    partPayload->nodes.push_back(geometry);
    partPayload->roots.push_back(ModelNodeIndex{ 0u });
    const auto part = world.reserveModel();
    ModelRecord partRecord;
    partRecord.dynamicRequirements = modelDynamicRequirement(ModelDynamicRequirement::NodeEffect);
    partRecord.payload = partPayload;
    assert(part && world.commit(*part, std::move(partRecord)));

    const NifRender::ComposedActorModel composed = NifRender::composeActorModel(world, *base, *skeleton,
        { NifRender::ActorPartModelSource{ *part, "bip01 hand", true } }, "actor:npc");
    assert(composed.valid());
    assert(composed.record.payload->nodes.size() == 4u);
    assert(composed.record.payload->nodes[2].parent == ModelNodeIndex{ 1u });
    assert(composed.record.payload->nodes[3].mesh == mesh);
    assert(composed.record.dynamicRequirements
        == modelDynamicRequirement(ModelDynamicRequirement::NodeEffect));

    const NifRender::ComposedActorModel missing = NifRender::composeActorModel(world, *base, *skeleton,
        { NifRender::ActorPartModelSource{ *part, "missing bone", true } }, "actor:bad");
    assert(!missing.valid());
}
