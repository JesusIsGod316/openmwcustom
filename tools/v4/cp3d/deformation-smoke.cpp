#include <components/rendercore/deformation.hpp>
#include <components/rendercore/frameproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3D deformation failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    using namespace RenderCore;
    RenderWorld world;

    auto skeletonPayload = std::make_shared<SkeletonPayload>();
    BoneRecord root;
    root.name = "root";
    skeletonPayload->bones.push_back(root);
    BoneRecord hand;
    hand.name = "hand";
    hand.parent = 0;
    hand.bindLocal[3][0] = 1.0f;
    skeletonPayload->bones.push_back(hand);
    const auto skeleton = world.reserveSkeleton();
    SkeletonRecord skeletonRecord;
    skeletonRecord.payload = skeletonPayload;
    if (!require(skeleton && world.commit(*skeleton, std::move(skeletonRecord)), "publish skeleton"))
        return EXIT_FAILURE;

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->normals.resize(3, { 0.0f, 0.0f, 1.0f });
    meshPayload->indices = { 0, 1, 2 };
    meshPayload->surfaces.push_back({ PrimitiveTopology::Triangles, 0, 3, 0 });
    auto skin = std::make_shared<SkinPayload>();
    skin->meshToSkeleton[0][0] = 2.0f;
    skin->meshToSkeleton[1][1] = 2.0f;
    skin->meshToSkeleton[2][2] = 2.0f;
    // OpenMW resolves authored bone names case-insensitively. Keep that
    // compatibility when compact mesh palettes bind to canonical skeletons.
    skin->bones.push_back({ "HaNd", glm::mat4{ 1.0f } });
    skin->vertexInfluences.resize(3);
    skin->vertexInfluences[0].push_back({ 0, 1.0f });
    const auto mesh = world.reserveMesh();
    MeshRecord meshRecord;
    meshRecord.surfaceCount = 1;
    meshRecord.skinned = true;
    meshRecord.payload = meshPayload;
    meshRecord.skin = skin;
    if (!require(mesh && world.commit(*mesh, std::move(meshRecord)), "publish skin"))
        return EXIT_FAILURE;

    const auto actor = world.reserveInstance();
    InstanceRecord actorRecord;
    actorRecord.mesh = *mesh;
    actorRecord.skeleton = *skeleton;
    if (!require(actor && world.commit(*actor, std::move(actorRecord)), "publish actor"))
        return EXIT_FAILURE;

    SingleViewFrameProducer producer;
    SingleViewFrameInput input;
    input.renderExtent = { 800, 600 };
    input.outputExtent = input.renderExtent;
    input.environment.skyEnabled = false;
    SkeletonPoseInput pose;
    pose.instance = *actor;
    pose.skeleton = *skeleton;
    pose.localTransforms = { root.bindLocal, hand.bindLocal };
    pose.localTransforms[1][3][1] = 2.0f;
    input.skeletonPoses.push_back(pose);
    const auto frame = producer.produce(world, input);
    if (!require(frame.has_value(), "publish pose"))
        return EXIT_FAILURE;

    const DeformedMeshPayload deformed = deformMesh(world, *frame, *actor, *mesh);
    if (!require(deformed.ready(), "skin deformation ready")
        || !require(deformed.positions[0] == glm::vec3(2.0f, 4.0f, 0.0f),
            "skin transform preserves OpenMW row-vector operation order")
        || !require(deformed.positions[1] == meshPayload->positions[1], "uninfluenced vertex remains unchanged"))
        return EXIT_FAILURE;

    auto morphPayload = std::make_shared<MeshPayload>(*meshPayload);
    auto morphs = std::make_shared<MorphPayload>();
    MorphTargetPayload target;
    target.name = "blink";
    target.sourceIndex = 1;
    target.positionOffsets.resize(3, { 0.0f, 0.0f, 2.0f });
    morphs->targets.push_back(target);
    const auto morphMesh = world.reserveMesh();
    MeshRecord morphRecord;
    morphRecord.surfaceCount = 1;
    morphRecord.morphed = true;
    morphRecord.payload = morphPayload;
    morphRecord.morphs = morphs;
    if (!require(morphMesh && world.commit(*morphMesh, std::move(morphRecord)), "publish morph mesh"))
        return EXIT_FAILURE;
    const auto face = world.reserveInstance();
    InstanceRecord faceRecord;
    faceRecord.mesh = *morphMesh;
    if (!require(face && world.commit(*face, std::move(faceRecord)), "publish morph instance"))
        return EXIT_FAILURE;

    SingleViewFrameInput morphInput = input;
    morphInput.skeletonPoses.clear();
    MorphWeightInput weights;
    weights.instance = *face;
    weights.mesh = *morphMesh;
    weights.weights = { 0.25f };
    morphInput.morphWeights.push_back(weights);
    const auto morphFrame = producer.produce(world, morphInput);
    if (!require(morphFrame.has_value(), "publish morph weights"))
        return EXIT_FAILURE;
    const DeformedMeshPayload morphed = deformMesh(world, *morphFrame, *face, *morphMesh);
    if (!require(morphed.ready(), "morph deformation ready")
        || !require(morphed.positions[0] == glm::vec3(0.0f, 0.0f, 0.5f), "morph offset weighted"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3D CPU deformation compatibility path: PASS\n";
    return EXIT_SUCCESS;
}
