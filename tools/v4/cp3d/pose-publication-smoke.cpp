#include <components/rendercore/frameproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3D pose publication failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    using namespace RenderCore;

    RenderWorld world;

    auto skeletonPayload = std::make_shared<SkeletonPayload>();
    BoneRecord root;
    root.name = "bip01";
    skeletonPayload->bones.push_back(root);

    const auto skeleton = world.reserveSkeleton();
    if (!require(skeleton.has_value(), "reserve skeleton"))
        return EXIT_FAILURE;
    SkeletonRecord skeletonRecord;
    skeletonRecord.sourceIdentity = "actor:test#skeleton";
    skeletonRecord.payload = skeletonPayload;
    if (!require(world.commit(*skeleton, std::move(skeletonRecord)), "commit skeleton"))
        return EXIT_FAILURE;

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->indices = { 0, 1, 2 };
    meshPayload->surfaces.push_back({ PrimitiveTopology::Triangles, 0, 3, 0 });

    auto skin = std::make_shared<SkinPayload>();
    skin->rootBoneName = "bip01";
    skin->bones.push_back({ "bip01", glm::mat4{ 1.0f } });
    skin->vertexInfluences.resize(meshPayload->positions.size());
    for (auto& influences : skin->vertexInfluences)
        influences.push_back({ 0, 1.0f });

    const auto mesh = world.reserveMesh();
    if (!require(mesh.has_value(), "reserve mesh"))
        return EXIT_FAILURE;
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "actor:test#mesh";
    meshRecord.surfaceCount = 1;
    meshRecord.skinned = true;
    meshRecord.payload = meshPayload;
    meshRecord.skin = skin;
    if (!require(world.commit(*mesh, std::move(meshRecord)), "commit deformable mesh"))
        return EXIT_FAILURE;

    const auto actor = world.reserveInstance();
    if (!require(actor.has_value(), "reserve actor instance"))
        return EXIT_FAILURE;
    InstanceRecord actorRecord;
    actorRecord.mesh = *mesh;
    actorRecord.skeleton = *skeleton;
    actorRecord.semanticFlags |= semanticFlag(InstanceSemanticFlag::OwnerBody);
    if (!require(world.commit(*actor, std::move(actorRecord)), "commit actor instance"))
        return EXIT_FAILURE;

    auto morphs = std::make_shared<MorphPayload>();
    MorphTargetPayload blink;
    blink.name = "blink";
    blink.sourceIndex = 1;
    blink.positionOffsets.resize(meshPayload->positions.size(), glm::vec3{ 0.0f, 0.0f, 0.1f });
    morphs->targets.push_back(std::move(blink));
    const auto morphMesh = world.reserveMesh();
    if (!require(morphMesh.has_value(), "reserve morph mesh"))
        return EXIT_FAILURE;
    MeshRecord morphMeshRecord;
    morphMeshRecord.sourceIdentity = "actor:test#face";
    morphMeshRecord.surfaceCount = 1;
    morphMeshRecord.morphed = true;
    morphMeshRecord.payload = meshPayload;
    morphMeshRecord.morphs = morphs;
    if (!require(world.commit(*morphMesh, std::move(morphMeshRecord)), "commit morph mesh"))
        return EXIT_FAILURE;
    const auto face = world.reserveInstance();
    if (!require(face.has_value(), "reserve face instance"))
        return EXIT_FAILURE;
    InstanceRecord faceRecord;
    faceRecord.mesh = *morphMesh;
    faceRecord.semanticFlags |= semanticFlag(InstanceSemanticFlag::OwnerHead);
    if (!require(world.commit(*face, std::move(faceRecord)), "commit face instance"))
        return EXIT_FAILURE;

    SingleViewFrameProducer producer;
    SingleViewFrameInput input;
    input.renderExtent = { 1280, 720 };
    input.outputExtent = input.renderExtent;
    input.environment.skyEnabled = false;

    DynamicTransformInput actorTransform;
    actorTransform.instance = *actor;
    input.dynamicTransforms.push_back(actorTransform);

    SkeletonPoseInput pose;
    pose.instance = *actor;
    pose.skeleton = *skeleton;
    pose.localTransforms.push_back(root.bindLocal);
    input.skeletonPoses.push_back(pose);

    MorphWeightInput morph;
    morph.instance = *face;
    morph.mesh = *morphMesh;
    morph.weights = { 0.0f };
    input.morphWeights.push_back(morph);

    const auto first = producer.produce(world, input);
    if (!require(first && first->valid(), "first actor frame")
        || !require(first->skeletonPoses().size() == 1 && !first->skeletonPoses()[0].historyValid,
            "first pose has cold history")
        || !require(first->morphWeights().size() == 1 && !first->morphWeights()[0].historyValid,
            "first morph has cold history")
        || !require(first->dynamicTransforms().size() == 1 && !first->dynamicTransforms()[0].historyValid,
            "first actor transform has cold history"))
        return EXIT_FAILURE;

    input.skeletonPoses[0].localTransforms[0][3][0] = 2.0f;
    input.morphWeights[0].weights[0] = 0.5f;
    input.dynamicTransforms[0].transform.translation.x = 3.0;
    const auto second = producer.produce(world, input);
    if (!require(second && second->valid(), "second actor frame")
        || !require(second->skeletonPoses()[0].historyValid
                && second->skeletonPoses()[0].previous[0][3][0] == 0.0f,
            "previous pose retained")
        || !require(second->morphWeights()[0].historyValid && second->morphWeights()[0].previous[0] == 0.0f,
            "previous morph retained")
        || !require(second->dynamicTransforms()[0].historyValid
                && second->dynamicTransforms()[0].previous.translation.x == 0.0,
            "previous actor transform retained"))
        return EXIT_FAILURE;

    input.skeletonPoses[0].localTransforms.clear();
    if (!require(!producer.produce(world, input), "wrong bone count rejected")
        || !require(producer.nextFrameId() == FrameId{ 3 }, "rejected pose does not consume frame"))
        return EXIT_FAILURE;

    input.skeletonPoses[0].localTransforms.push_back(root.bindLocal);
    input.skeletonPoses[0].localTransforms[0][3][0] = 4.0f;
    input.invalidateHistory = true;
    const auto cut = producer.produce(world, input);
    if (!require(cut && !cut->historyValid(), "cut frame")
        || !require(!cut->skeletonPoses()[0].historyValid
                && cut->skeletonPoses()[0].previous[0][3][0] == 4.0f,
            "cut resets pose history")
        || !require(!cut->morphWeights()[0].historyValid
                && cut->morphWeights()[0].previous == cut->morphWeights()[0].current,
            "cut resets morph history"))
        return EXIT_FAILURE;

    MeshRecord invalidMesh;
    invalidMesh.surfaceCount = 1;
    invalidMesh.skinned = true;
    invalidMesh.payload = meshPayload;
    auto invalidSkin = std::make_shared<SkinPayload>(*skin);
    invalidSkin->vertexInfluences[0].push_back({ 0, 0.5f });
    invalidMesh.skin = invalidSkin;
    const auto invalidMeshHandle = world.reserveMesh();
    if (!require(invalidMeshHandle && !world.commit(*invalidMeshHandle, std::move(invalidMesh)),
            "duplicate bone influence rejected"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3D deformable mesh and pose publication: PASS\n";
    return EXIT_SUCCESS;
}
