#include <components/render/backend/vsg/dynamicactorplan.hpp>
#include <components/rendercore/frameproducer.hpp>

#include <cassert>
#include <memory>
#include <iostream>

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

    {
        RenderWorld cachedWorld = world;
        RenderVsg::DynamicActorPlanCache cache;
        assert(cache.prepare(cachedWorld).valid() && cache.rebuilt == 1);
        for (unsigned i = 0; i < 100; ++i)
        {
            const auto cached = cache.prepare(cachedWorld);
            assert(cached.valid() && cache.rebuilt == 0 && cache.reused == 1);
            assert(cached.actors.front().asset.draws.size() == plan->asset.draws.size());
        }
        assert(cache.prepare(cachedWorld, {}, true).valid() && cache.rebuilt == 1 && cache.reused == 0);
        auto changed = *cachedWorld.get(*material);
        changed.revision = ResourceRevision{changed.revision.value() + 1};
        changed.alpha = 0.5f;
        assert(cachedWorld.update(*material, changed));
        assert(cache.prepare(cachedWorld).valid() && cache.rebuilt == 1);
        auto moved = *cachedWorld.get(*instance);
        moved.revision = ResourceRevision{moved.revision.value() + 1};
        moved.transform.translation.x = 9.0;
        assert(cachedWorld.update(*instance, moved));
        auto movedPlan = cache.prepare(cachedWorld);
        assert(movedPlan.valid() && cache.rebuilt == 1);
        assert(movedPlan.actors.front().placement.translation.x == 9.0);
        auto changedMesh = *cachedWorld.get(*mesh);
        changedMesh.revision = ResourceRevision{changedMesh.revision.value() + 1};
        assert(cachedWorld.update(*mesh, changedMesh));
        assert(cache.prepare(cachedWorld).valid() && cache.rebuilt == 1);
        auto changedSkeleton = *cachedWorld.get(*skeleton);
        changedSkeleton.revision = ResourceRevision{changedSkeleton.revision.value() + 1};
        assert(cachedWorld.update(*skeleton, changedSkeleton));
        assert(cache.prepare(cachedWorld).valid() && cache.rebuilt == 1);
        RenderVsg::StaticPlanOptions changedOptions;
        changedOptions.showMarkers = true;
        assert(cache.prepare(cachedWorld, changedOptions).valid() && cache.rebuilt == 1);
        assert(cachedWorld.retire(*instance));
        assert(cache.prepare(cachedWorld).valid() && cache.size() == 0);
        assert(cachedWorld.reset());
        assert(cache.prepare(cachedWorld).valid() && cache.size() == 0);
        std::cout << "PASS actor plan cache: 100 stable frames, forced control, dependency/options/movement/removal/epoch\n";
    }

    {
        RenderWorld cachedWorld = world;
        const auto hiddenTexture = cachedWorld.reserveTexture();
        TextureRecord textureRecord;
        textureRecord.sourceIdentity = "actor:hidden-texture";
        textureRecord.contentIdentity = "hidden-v1";
        assert(hiddenTexture && cachedWorld.commit(*hiddenTexture, textureRecord));
        const auto hiddenMaterial = cachedWorld.reserveMaterial();
        MaterialRecord hiddenRecord;
        hiddenRecord.sourceIdentity = "actor:hidden-material";
        TextureBinding binding;
        binding.texture = *hiddenTexture;
        hiddenRecord.textures.push_back(binding);
        assert(hiddenMaterial && cachedWorld.commit(*hiddenMaterial, hiddenRecord));
        auto hiddenModel = *cachedWorld.get(*model);
        auto hiddenPayload = std::make_shared<ModelPayload>(*hiddenModel.payload);
        hiddenPayload->nodes[2].flags |= modelNodeFlag(ModelNodeFlag::Hidden);
        hiddenPayload->nodes[2].materials = {*hiddenMaterial};
        hiddenModel.payload = hiddenPayload;
        hiddenModel.revision = ResourceRevision{hiddenModel.revision.value() + 1};
        assert(cachedWorld.update(*model, hiddenModel));
        RenderVsg::DynamicActorPlanCache cache;
        assert(cache.prepare(cachedWorld).valid() && cache.rebuilt == 1);
        textureRecord.revision = ResourceRevision{textureRecord.revision.value() + 1};
        textureRecord.contentIdentity = "hidden-v2";
        assert(cachedWorld.update(*hiddenTexture, textureRecord));
        assert(cache.prepare(cachedWorld).valid() && cache.rebuilt == 1);
        hiddenRecord.revision = ResourceRevision{hiddenRecord.revision.value() + 1};
        assert(cachedWorld.update(*hiddenMaterial, hiddenRecord));
        assert(cache.prepare(cachedWorld).valid() && cache.rebuilt == 1);
        hiddenModel.revision = ResourceRevision{hiddenModel.revision.value() + 1};
        hiddenModel.dynamicRequirements = modelDynamicRequirement(ModelDynamicRequirement::ParticleSystem);
        assert(cachedWorld.update(*model, hiddenModel));
        assert(!cache.prepare(cachedWorld).valid() && cache.size() == 0);
        assert(!cache.prepare(cachedWorld).valid() && cache.reused == 0);
        std::cout << "PASS actor plan cache: hidden material/texture invalidation, invalid models not reused\n";
    }

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

    // A creature may animate rigid pieces only, with no skin or morph streams.
    ModelRecord rigidModel = *world.get(*model);
    rigidModel.revision = ResourceRevision{ rigidModel.revision.value() + 1u };
    auto rigidPayload = std::make_shared<ModelPayload>(*rigidModel.payload);
    rigidPayload->nodes[1].mesh = *rigidMesh;
    rigidModel.payload = rigidPayload;
    assert(world.update(*model, std::move(rigidModel)));
    const auto rigidPlan = RenderVsg::buildDynamicActorPlan(world, *instance);
    assert(rigidPlan && rigidPlan->asset.draws.size() == 2u);
    const auto rigidFrame = producer.produce(world, frameInput);
    assert(rigidFrame);
    const auto rigidEvaluated = RenderVsg::evaluateDynamicActorAssetPlan(world, *rigidFrame, *rigidPlan);
    assert(rigidEvaluated && rigidEvaluated->draws[0].worldTransform[3][0] == 5.0f);
    assert(rigidEvaluated->draws[1].worldTransform[3][0] == 7.0f);

    // Resident reuse must reject immutable material changes even when topology
    // and array lengths have not changed.
    MaterialRecord changedMaterial = *world.get(*material);
    changedMaterial.revision = ResourceRevision{ changedMaterial.revision.value() + 1u };
    changedMaterial.alpha = 0.5f;
    assert(world.update(*material, std::move(changedMaterial)));
    assert(!RenderVsg::dynamicActorPlanCurrent(world, *plan));
    const auto materialRefresh = RenderVsg::buildDynamicActorPlan(world, *instance);
    assert(materialRefresh && RenderVsg::dynamicActorPlanCurrent(world, *materialRefresh));

    // Missing morph data on real geometry must remain an error. The translator
    // normalizes only deliberately omitted geometry, not arbitrary bad models.
    const ModelRecord validModel = *world.get(*model);
    ModelRecord missingMorphModel = validModel;
    missingMorphModel.revision = ResourceRevision{ missingMorphModel.revision.value() + 1u };
    auto missingMorphPayload = std::make_shared<ModelPayload>(*missingMorphModel.payload);
    missingMorphPayload->nodes[1].controllerFlags |= modelControllerFlag(ModelControllerFlag::Morph);
    missingMorphPayload->nodes[1].flags |= modelNodeFlag(ModelNodeFlag::ControllerTarget);
    missingMorphModel.payload = missingMorphPayload;
    assert(world.update(*model, std::move(missingMorphModel)));
    std::string missingMorphDiagnostic;
    assert(!RenderVsg::buildDynamicActorPlan(world, *instance, {}, &missingMorphDiagnostic));
    assert(missingMorphDiagnostic.find("actor:model") != std::string::npos);
    ModelRecord restoredModel = validModel;
    restoredModel.revision = ResourceRevision{ world.get(*model)->revision.value() + 1u };
    assert(world.update(*model, std::move(restoredModel)));

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
