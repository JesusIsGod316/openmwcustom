#include <components/rendercore/renderer.hpp>
#include <components/rendercore/updatebatch.hpp>

#include <cassert>
#include <memory>

int main()
{
    using namespace RenderCore;

    RenderWorld world;
    RenderWorldPublisher publisher(world);
    const auto mesh = world.reserveMesh();
    const auto material = world.reserveMaterial();
    const auto model = world.reserveModel();
    const auto instance = world.reserveInstance();
    assert(mesh && material && model && instance);

    auto modelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord root;
    root.kind = ModelNodeKind::Transform;
    modelPayload->nodes.push_back(root);

    ModelNodeRecord geometry;
    geometry.parent = ModelNodeIndex{ 0u };
    geometry.kind = ModelNodeKind::Geometry;
    geometry.mesh = *mesh;
    geometry.materials.push_back(*material);
    modelPayload->nodes.push_back(geometry);
    modelPayload->roots.push_back(ModelNodeIndex{ 0u });
    assert(validModelPayloadStructure(*modelPayload));

    InstanceRecord instanceRecord;
    instanceRecord.model = *model;

    RenderWorldUpdateBatch batch(world.epoch(), InitialUpdateSequence, "cp3b-smoke");
    assert(batch.add(CreateMesh{ *mesh, MeshRecord{ .sourceIdentity = "smoke:mesh" } }));
    assert(batch.add(CreateMaterial{ *material, MaterialRecord{ .sourceIdentity = "smoke:material" } }));
    assert(batch.add(CreateModel{ *model,
        ModelRecord{ .sourceIdentity = "smoke:model", .payload = std::move(modelPayload) } }));
    assert(batch.add(CreateInstance{ *instance, instanceRecord }));
    assert(batch.seal());
    assert(publisher.apply(batch) == PublishStatus::Applied);
    assert(world.valid());

    assert(!world.retire(*mesh));
    assert(!world.retire(*material));
    assert(!world.retire(*model));
    assert(world.retire(*instance));
    assert(world.retire(*model));
    assert(world.retire(*mesh));
    assert(world.retire(*material));
    assert(world.valid());

    return 0;
}
