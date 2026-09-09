#include <components/render/backend/vsg/staticassetplan.hpp>

#include <cassert>
#include <memory>

int main()
{
    using namespace RenderCore;

    RenderWorld world;

    const auto texture = world.reserveTexture();
    assert(texture);
    TextureRecord textureRecord;
    textureRecord.sourceIdentity = "textures/shared.dds";
    textureRecord.contentIdentity = "hash:shared";
    assert(world.commit(*texture, std::move(textureRecord)));

    const auto materialA = world.reserveMaterial();
    const auto materialB = world.reserveMaterial();
    assert(materialA && materialB);

    MaterialRecord firstMaterial;
    firstMaterial.sourceIdentity = "material:a";
    TextureBinding firstBinding;
    firstBinding.texture = *texture;
    firstBinding.role = TextureRole::Diffuse;
    firstBinding.colorSpace = TextureColorSpace::Srgb;
    firstBinding.formatClass = TextureFormatClass::Color;
    firstBinding.sampler.wrapU = TextureWrap::Repeat;
    firstMaterial.textures.push_back(firstBinding);
    assert(world.commit(*materialA, std::move(firstMaterial)));

    MaterialRecord secondMaterial;
    secondMaterial.sourceIdentity = "material:b";
    secondMaterial.alphaBlendEnabled = true;
    secondMaterial.transparentSort = TransparentSortPolicy::Sorted;
    TextureBinding secondBinding;
    secondBinding.texture = *texture;
    secondBinding.role = TextureRole::Normal;
    secondBinding.colorSpace = TextureColorSpace::Data;
    secondBinding.formatClass = TextureFormatClass::Normal;
    secondBinding.sampler.wrapU = TextureWrap::Clamp;
    secondBinding.sampler.maxAnisotropy = 8.0f;
    secondMaterial.textures.push_back(secondBinding);
    assert(world.commit(*materialB, std::move(secondMaterial)));

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = {
        { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 1.0f },
    };
    meshPayload->normals.assign(meshPayload->positions.size(), glm::vec3{ 0.0f, 0.0f, 1.0f });
    meshPayload->texCoordSets.push_back(std::vector<glm::vec2>(meshPayload->positions.size(), glm::vec2{ 0.5f, 0.5f }));
    meshPayload->indices = { 0u, 1u, 2u, 3u, 4u, 5u };
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Triangles, 0u, 3u, 0u });
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Lines, 3u, 2u, 1u });

    const auto mesh = world.reserveMesh();
    assert(mesh);
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "mesh:static";
    meshRecord.surfaceCount = static_cast<std::uint32_t>(meshPayload->surfaces.size());
    meshRecord.payload = meshPayload;
    assert(world.commit(*mesh, std::move(meshRecord)));

    const auto dynamicMesh = world.reserveMesh();
    assert(dynamicMesh);
    MeshRecord dynamicMeshRecord;
    dynamicMeshRecord.sourceIdentity = "mesh:dynamic";
    dynamicMeshRecord.surfaceCount = static_cast<std::uint32_t>(meshPayload->surfaces.size());
    dynamicMeshRecord.payload = meshPayload;
    dynamicMeshRecord.skinned = true;
    auto dynamicSkin = std::make_shared<SkinPayload>();
    dynamicSkin->bones.push_back({ "root", glm::mat4{ 1.0f } });
    dynamicSkin->vertexInfluences.resize(meshPayload->positions.size());
    for (auto& influences : dynamicSkin->vertexInfluences)
        influences.push_back({ 0u, 1.0f });
    dynamicMeshRecord.skin = std::move(dynamicSkin);
    assert(world.commit(*dynamicMesh, std::move(dynamicMeshRecord)));

    auto modelPayload = std::make_shared<ModelPayload>();

    ModelNodeRecord rootSwitch;
    rootSwitch.kind = ModelNodeKind::Switch;
    rootSwitch.activeSwitchChild = ModelNodeIndex{ 1u };
    modelPayload->nodes.push_back(rootSwitch);

    ModelNodeRecord lod;
    lod.parent = ModelNodeIndex{ 0u };
    lod.kind = ModelNodeKind::Lod;
    ModelLodSemantic lodSemantic;
    lodSemantic.ranges.push_back(ModelLodRange{ ModelNodeIndex{ 2u }, 0.0f, 100.0f });
    lodSemantic.ranges.push_back(ModelLodRange{ ModelNodeIndex{ 3u }, 0.0f, 100.0f });
    lod.lod = lodSemantic;
    modelPayload->nodes.push_back(lod);

    ModelNodeRecord firstLodChild;
    firstLodChild.parent = ModelNodeIndex{ 1u };
    firstLodChild.localTransform[3][0] = 100.0f;
    modelPayload->nodes.push_back(firstLodChild);

    ModelNodeRecord secondLodChild;
    secondLodChild.parent = ModelNodeIndex{ 1u };
    secondLodChild.localTransform[3][0] = 5.0f;
    modelPayload->nodes.push_back(secondLodChild);

    ModelNodeRecord suppressedGeometry;
    suppressedGeometry.parent = ModelNodeIndex{ 2u };
    suppressedGeometry.kind = ModelNodeKind::Geometry;
    suppressedGeometry.mesh = *mesh;
    suppressedGeometry.materials = { *materialA, *materialB };
    modelPayload->nodes.push_back(suppressedGeometry);

    ModelNodeRecord visibleGeometry;
    visibleGeometry.parent = ModelNodeIndex{ 3u };
    visibleGeometry.kind = ModelNodeKind::Geometry;
    visibleGeometry.mesh = *mesh;
    visibleGeometry.materials = { *materialA, *materialB };
    modelPayload->nodes.push_back(visibleGeometry);

    ModelNodeRecord hiddenBranch;
    hiddenBranch.parent = ModelNodeIndex{ 3u };
    hiddenBranch.flags = modelNodeFlag(ModelNodeFlag::Hidden);
    modelPayload->nodes.push_back(hiddenBranch);

    ModelNodeRecord hiddenGeometry;
    hiddenGeometry.parent = ModelNodeIndex{ 6u };
    hiddenGeometry.kind = ModelNodeKind::Geometry;
    hiddenGeometry.mesh = *mesh;
    hiddenGeometry.materials = { *materialA, *materialB };
    modelPayload->nodes.push_back(hiddenGeometry);

    ModelNodeRecord inactiveSwitchChild;
    inactiveSwitchChild.parent = ModelNodeIndex{ 0u };
    modelPayload->nodes.push_back(inactiveSwitchChild);

    ModelNodeRecord markerRoot;
    markerRoot.flags = modelNodeFlag(ModelNodeFlag::Marker);
    modelPayload->nodes.push_back(markerRoot);

    ModelNodeRecord markerGeometry;
    markerGeometry.parent = ModelNodeIndex{ 9u };
    markerGeometry.kind = ModelNodeKind::Geometry;
    markerGeometry.mesh = *mesh;
    markerGeometry.materials = { *materialA, *materialB };
    modelPayload->nodes.push_back(markerGeometry);

    ModelNodeRecord dynamicRoot;
    dynamicRoot.kind = ModelNodeKind::Geometry;
    dynamicRoot.mesh = *dynamicMesh;
    dynamicRoot.materials = { *materialA, *materialB };
    modelPayload->nodes.push_back(dynamicRoot);

    modelPayload->roots = { ModelNodeIndex{ 0u }, ModelNodeIndex{ 9u }, ModelNodeIndex{ 11u } };
    assert(validModelPayloadStructure(*modelPayload));

    const auto model = world.reserveModel();
    assert(model);
    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "meshes/static-plan.nif";
    modelRecord.contentIdentity = "hash:model";
    modelRecord.payload = modelPayload;
    assert(world.commit(*model, std::move(modelRecord)));
    assert(world.valid());

    RenderVsg::StaticPlanOptions options;
    options.lodEyeDistance = 50.0f;
    const auto plan = RenderVsg::buildStaticAssetPlan(world, *model, options);
    assert(plan);
    assert(plan->draws.size() == 2u);
    assert(plan->lodSuppressedNodes == 2u);
    assert(plan->switchSuppressedNodes == 1u);
    assert(plan->hiddenNodes == 2u);
    assert(plan->markerNodes == 2u);
    assert(plan->dynamicMeshesDeferred == 1u);

    RenderVsg::StaticPlanOptions deformableOptions = options;
    deformableOptions.includeDeformableMeshes = true;
    const auto deformablePlan = RenderVsg::buildStaticAssetPlan(world, *model, deformableOptions);
    assert(deformablePlan);
    assert(deformablePlan->draws.size() == 4u);
    assert(deformablePlan->dynamicMeshesDeferred == 0u);

    assert(plan->draws[0].surface.topology == PrimitiveTopology::Triangles);
    assert(plan->draws[1].surface.topology == PrimitiveTopology::Lines);
    assert(plan->draws[0].worldTransform[3][0] == 5.0f);
    assert(plan->draws[0].textures.size() == 1u);
    assert(plan->draws[0].textures[0].view.texture == *texture);
    assert(plan->draws[1].textures[0].view.colorSpace == TextureColorSpace::Data);
    assert(plan->draws[0].samplers[0] != plan->draws[1].samplers[0]);
    assert(plan->draws[0].pipeline != plan->draws[1].pipeline);

    options.showMarkers = true;
    const auto markerPlan = RenderVsg::buildStaticAssetPlan(world, *model, options);
    assert(markerPlan);
    assert(markerPlan->draws.size() == 4u);
    assert(markerPlan->markerNodes == 0u);

    ModelRecord particleModel = *world.get(*model);
    particleModel.revision = ResourceRevision{ particleModel.revision.value() + 1u };
    particleModel.dynamicRequirements = modelDynamicRequirement(ModelDynamicRequirement::ParticleSystem);
    assert(world.update(*model, std::move(particleModel)));
    assert(!RenderVsg::buildStaticAssetPlan(world, *model, options));

    return 0;
}
