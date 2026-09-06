#include <components/render/backend/vsg/staticassetconformance.hpp>

#include <vsg/all.h>

#include <glm/mat4x4.hpp>

#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
            throw std::runtime_error(std::string(message));
    }

    bool near(double lhs, double rhs, double epsilon = 1e-9)
    {
        return std::abs(lhs - rhs) <= epsilon;
    }

    bool matrixNear(const glm::dmat4& lhs, const glm::dmat4& rhs, double epsilon = 1e-9)
    {
        for (glm::length_t column = 0; column < 4; ++column)
            for (glm::length_t row = 0; row < 4; ++row)
                if (!near(lhs[column][row], rhs[column][row], epsilon))
                    return false;
        return true;
    }
}

int main()
{
    using namespace RenderCore;

    RenderVsg::StaticBillboardViewFrame degenerateFrame;
    degenerateFrame.look = glm::dvec3(0.0);
    glm::dmat4 authored(1.0);
    authored[0][0] = 2.0;
    authored[1][1] = 2.0;
    authored[2][2] = 2.0;
    authored[3] = glm::dvec4(3.0, 4.0, 5.0, 1.0);
    require(matrixNear(RenderVsg::evaluateLegacyBillboardLocal(
                           authored, ModelBillboardMode::RigidFaceCamera, degenerateFrame),
                authored),
        "degenerate camera basis must retain the authored billboard transform");

    RenderVsg::StaticBillboardViewFrame rotateFrame;
    rotateFrame.eye = glm::dvec3(1.0, 0.0, 0.0);
    const glm::dmat4 rotate = RenderVsg::evaluateLegacyBillboardLocal(
        glm::dmat4(1.0), ModelBillboardMode::RotateAboutUp, rotateFrame);
    require(near(rotate[0][0], 0.0) && near(rotate[0][2], -1.0) && near(rotate[2][0], 1.0)
            && near(rotate[2][2], 0.0),
        "rotate-about-up compatibility matrix no longer matches V3.25");

    const auto bins = RenderVsg::createStaticConformanceBins();
    require(bins.size() == 2u, "static conformance View must install exactly two compatibility bins");
    require(bins[0]->binNumber == RenderVsg::StaticTraversalBinNumber && bins[0]->sortOrder == vsg::Bin::NO_SORT,
        "traversal bin configuration changed");
    require(bins[1]->binNumber == RenderVsg::StaticBackToFrontBinNumber
            && bins[1]->sortOrder == vsg::Bin::DESCENDING,
        "back-to-front bin configuration changed");

    RenderWorld world;

    const auto material = world.reserveMaterial();
    require(material.has_value(), "failed to reserve material");
    MaterialRecord materialRecord;
    materialRecord.sourceIdentity = "cp3b3:conformance-material";
    require(world.commit(*material, std::move(materialRecord)), "failed to commit material");

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->normals.assign(3u, glm::vec3(0.0f, 0.0f, 1.0f));
    meshPayload->indices = { 0u, 1u, 2u };
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Triangles, 0u, 3u, 0u });
    require(validMeshPayload(*meshPayload), "synthetic conformance mesh is invalid");

    const auto mesh = world.reserveMesh();
    require(mesh.has_value(), "failed to reserve mesh");
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "cp3b3:conformance-mesh";
    meshRecord.surfaceCount = 1u;
    meshRecord.payload = meshPayload;
    require(world.commit(*mesh, std::move(meshRecord)), "failed to commit mesh");

    auto modelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord billboard;
    billboard.kind = ModelNodeKind::Billboard;
    billboard.billboard = ModelBillboardMode::RigidFaceCamera;
    billboard.localTransform[3][0] = 5.0f;
    modelPayload->nodes.push_back(billboard);

    ModelNodeRecord geometry;
    geometry.parent = ModelNodeIndex{ 0u };
    geometry.kind = ModelNodeKind::Geometry;
    geometry.localTransform[3][1] = 2.0f;
    geometry.mesh = *mesh;
    geometry.materials = { *material };
    modelPayload->nodes.push_back(geometry);
    modelPayload->roots = { ModelNodeIndex{ 0u } };
    require(validModelPayloadStructure(*modelPayload), "single-billboard model graph is invalid");

    const auto model = world.reserveModel();
    require(model.has_value(), "failed to reserve model");
    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "cp3b3:billboard-model";
    modelRecord.contentIdentity = "cp3b3:billboard-model:v1";
    modelRecord.payload = modelPayload;
    require(world.commit(*model, std::move(modelRecord)), "failed to commit billboard model");

    auto plan = RenderVsg::buildStaticAssetPlan(world, *model);
    require(plan.has_value() && plan->draws.size() == 1u && plan->draws[0].billboard.has_value(),
        "billboard draw was not preserved by the neutral planner");

    plan->draws[0].sortPolicy = RenderVsg::StaticDrawSortPolicy::Traversal;
    const RenderVsg::StaticTextureResolver noTextures;
    auto traversal = RenderVsg::realizeStaticAssetConformant(world, *model, *plan, noTextures);
    require(traversal.valid() && traversal.root->children.size() == 1u,
        "traversal-routed billboard realization failed");
    auto* traversalLayer = dynamic_cast<vsg::Layer*>(traversal.root->children.front().get());
    require(traversalLayer != nullptr && traversalLayer->binNumber == RenderVsg::StaticTraversalBinNumber,
        "effective traversal policy did not route through the NO_SORT compatibility bin");
    require(dynamic_cast<vsg::MatrixTransform*>(traversalLayer->child.get()) != nullptr,
        "billboard realization must preserve an explicit parent transform boundary");

    plan->draws[0].sortPolicy = RenderVsg::StaticDrawSortPolicy::BackToFront;
    auto sorted = RenderVsg::realizeStaticAssetConformant(world, *model, *plan, noTextures);
    require(sorted.valid() && sorted.stats.sortedDrawCount == 1u,
        "back-to-front routing did not update realization statistics");
    auto* depthSorted = dynamic_cast<vsg::DepthSorted*>(sorted.root->children.front().get());
    require(depthSorted != nullptr && depthSorted->binNumber == RenderVsg::StaticBackToFrontBinNumber,
        "effective back-to-front policy did not route through the DESCENDING compatibility bin");

    auto nestedPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord outer;
    outer.kind = ModelNodeKind::Billboard;
    outer.billboard = ModelBillboardMode::RigidFaceCamera;
    nestedPayload->nodes.push_back(outer);
    ModelNodeRecord inner;
    inner.parent = ModelNodeIndex{ 0u };
    inner.kind = ModelNodeKind::Billboard;
    inner.billboard = ModelBillboardMode::RotateAboutUp;
    nestedPayload->nodes.push_back(inner);
    ModelNodeRecord nestedGeometry = geometry;
    nestedGeometry.parent = ModelNodeIndex{ 1u };
    nestedGeometry.localTransform = glm::mat4(1.0f);
    nestedPayload->nodes.push_back(nestedGeometry);
    nestedPayload->roots = { ModelNodeIndex{ 0u } };
    require(validModelPayloadStructure(*nestedPayload), "nested-billboard source graph is structurally invalid");

    const auto nestedModel = world.reserveModel();
    require(nestedModel.has_value(), "failed to reserve nested billboard model");
    ModelRecord nestedRecord;
    nestedRecord.sourceIdentity = "cp3b3:nested-billboard-model";
    nestedRecord.contentIdentity = "cp3b3:nested-billboard-model:v1";
    nestedRecord.payload = nestedPayload;
    require(world.commit(*nestedModel, std::move(nestedRecord)), "failed to commit nested billboard model");

    const auto nestedPlan = RenderVsg::buildStaticAssetPlan(world, *nestedModel);
    require(nestedPlan.has_value() && nestedPlan->draws.size() == 1u,
        "nested billboard graph did not reach the conformance gate");
    const auto nestedResult
        = RenderVsg::realizeStaticAssetConformant(world, *nestedModel, *nestedPlan, noTextures);
    require(!nestedResult.valid(), "nested billboard transforms must fail closed instead of flattening silently");

    return 0;
}
