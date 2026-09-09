#include <components/render/backend/vsg/dynamicactorplan.hpp>
#include <components/rendercore/frameproducer.hpp>

#include <cassert>
#include <memory>

int main()
{
    using namespace RenderCore;

    RenderWorld world;

    const auto material = world.reserveMaterial();
    assert(material);
    MaterialRecord materialRecord;
    materialRecord.sourceIdentity = "actor:material";
    assert(world.commit(*material, std::move(materialRecord)));

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->indices = { 0u, 1u, 2u };
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Triangles, 0u, 3u, 0u });

    const auto mesh = world.reserveMesh();
    assert(mesh);
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "actor:body";
    meshRecord.surfaceCount = 1u;
    meshRecord.payload = meshPayload;
    meshRecord.skinned = true;
    auto skin = std::make_shared<SkinPayload>();
    skin->bones.push_back({ "root", glm::mat4{ 1.0f } });
    skin->vertexInfluences.resize(3u, { SkinInfluence{ 0u, 1.0f } });
    meshRecord.skin = std::move(skin);
    assert(world.commit(*mesh, std::move(meshRecord)));

    const auto rigidMesh = world.reserveMesh();
    assert(rigidMesh);
    MeshRecord rigidMeshRecord;
    rigidMeshRecord.sourceIdentity = "actor:weapon";
    rigidMeshRecord.surfaceCount = 1u;
    rigidMeshRecord.payload = meshPayload;
    assert(world.commit(*rigidMesh, std::move(rigidMeshRecord)));

    auto modelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord rootNode;
    rootNode.name = "root";
    rootNode.flags |= modelNodeFlag(ModelNodeFlag::ControllerTarget);
    rootNode.controllerFlags |= modelControllerFlag(ModelControllerFlag::Transform);
    modelPayload->nodes.push_back(std::move(rootNode));
    ModelNodeRecord geometry;
    geometry.name = "body";
    geometry.parent = ModelNodeIndex{ 0u };
    geometry.kind = ModelNodeKind::Geometry;
    geometry.mesh = *mesh;
    geometry.materials = { *material };
    modelPayload->nodes.push_back(std::move(geometry));
    ModelNodeRecord attachment;
    attachment.name = "weapon";
    attachment.parent = ModelNodeIndex{ 0u };
    attachment.localTransform[3][0] = 2.0f;
    attachment.kind = ModelNodeKind::Geometry;
    attachment.mesh = *rigidMesh;
    attachment.materials = { *material };
    modelPayload->nodes.push_back(std::move(attachment));
    modelPayload->roots.push_back(ModelNodeIndex{ 0u });
    const auto model = world.reserveModel();
    assert(model);
    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "actor:model";
    modelRecord.payload = modelPayload;
    assert(world.commit(*model, std::move(modelRecord)));

    auto skeletonPayload = std::make_shared<SkeletonPayload>();
    BoneRecord root;
    root.name = "root";
    skeletonPayload->bones.push_back(root);
    const auto skeleton = world.reserveSkeleton();
    assert(skeleton);
    SkeletonRecord skeletonRecord;
    skeletonRecord.sourceIdentity = "actor:skeleton";
    skeletonRecord.payload = skeletonPayload;
    assert(world.commit(*skeleton, std::move(skeletonRecord)));

    const auto instance = world.reserveInstance();
    assert(instance);
    InstanceRecord instanceRecord;
    instanceRecord.model = *model;
    instanceRecord.skeleton = *skeleton;
    assert(world.commit(*instance, std::move(instanceRecord)));

    const auto plan = RenderVsg::buildDynamicActorPlan(world, *instance);
    assert(plan);
    assert(plan->instance == *instance);
    assert(plan->skeleton == *skeleton);
    assert(plan->asset.draws.size() == 2u);
    assert(plan->asset.dynamicMeshesDeferred == 0u);
    assert(plan->meshes.size() == 2u);
    assert(RenderVsg::dynamicActorPlanCurrent(world, *plan));

    SingleViewFrameProducer producer;
    SingleViewFrameInput frameInput;
    frameInput.renderExtent = { 800u, 600u };
    frameInput.outputExtent = frameInput.renderExtent;
    frameInput.environment.skyEnabled = false;
    SkeletonPoseInput pose;
    pose.instance = *instance;
    pose.skeleton = *skeleton;
    pose.localTransforms = { glm::mat4{ 1.0f } };
    pose.localTransforms[0][3][0] = 5.0f;
    frameInput.skeletonPoses.push_back(pose);
    const auto frame = producer.produce(world, frameInput);
    assert(frame);
    const auto evaluated = RenderVsg::evaluateDynamicActorAssetPlan(world, *frame, *plan);
    assert(evaluated && evaluated->draws.size() == 2u);
    assert(evaluated->draws[0].worldTransform == glm::mat4(1.0f));
    assert(evaluated->draws[1].worldTransform[3][0] == 7.0f);

    ModelRecord unsupportedModel = *world.get(*model);
    unsupportedModel.revision = ResourceRevision{ unsupportedModel.revision.value() + 1u };
    auto unsupportedPayload = std::make_shared<ModelPayload>(*unsupportedModel.payload);
    unsupportedPayload->nodes[0].controllerFlags
        |= modelControllerFlag(ModelControllerFlag::Visibility);
    unsupportedModel.payload = std::move(unsupportedPayload);
    assert(world.update(*model, std::move(unsupportedModel)));
    assert(!RenderVsg::buildDynamicActorPlan(world, *instance));
    assert(!RenderVsg::dynamicActorPlanCurrent(world, *plan));

    ModelRecord deferredModel = *world.get(*model);
    deferredModel.revision = ResourceRevision{ deferredModel.revision.value() + 1u };
    deferredModel.dynamicRequirements
        = modelDynamicRequirement(ModelDynamicRequirement::ParticleSystem);
    assert(world.update(*model, std::move(deferredModel)));
    assert(!RenderVsg::buildDynamicActorPlan(world, *instance));

    InstanceRecord replacement = *world.get(*instance);
    replacement.transform.translation.x = 1.0;
    replacement.revision = ResourceRevision{ replacement.revision.value() + 1u };
    assert(world.update(*instance, std::move(replacement)));
    assert(!RenderVsg::dynamicActorPlanCurrent(world, *plan));
}
