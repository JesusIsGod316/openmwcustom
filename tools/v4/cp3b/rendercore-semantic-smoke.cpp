#include <components/nifrender/translationbundle.hpp>
#include <components/rendercore/renderer.hpp>
#include <components/rendercore/updatebatch.hpp>

#include <cassert>
#include <limits>
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
    root.localTransform[0][0] = -1.0f;
    root.localTransform[1][1] = 2.0f;
    root.localTransform[2][2] = 0.5f;
    modelPayload->nodes.push_back(root);

    ModelNodeRecord geometry;
    geometry.parent = ModelNodeIndex{ 0u };
    geometry.kind = ModelNodeKind::Geometry;
    geometry.mesh = *mesh;
    geometry.materials.push_back(*material);
    modelPayload->nodes.push_back(geometry);
    modelPayload->roots.push_back(ModelNodeIndex{ 0u });
    assert(validModelPayloadStructure(*modelPayload));

    ModelPayload invalidAffine = *modelPayload;
    invalidAffine.nodes[0].localTransform[2][1] = std::numeric_limits<float>::infinity();
    assert(!validModelPayloadStructure(invalidAffine));

    ModelPayload sortPayload;
    ModelNodeRecord sortRoot;
    sortRoot.kind = ModelNodeKind::Sort;
    sortRoot.sort = ModelSortSemantic{ .mode = ModelSortMode::Subsort, .accumulator = ModelSortAccumulator::Cluster };
    sortPayload.nodes.push_back(sortRoot);
    sortPayload.roots.push_back(ModelNodeIndex{ 0u });
    assert(validModelPayloadStructure(sortPayload));
    sortPayload.nodes[0].sort.reset();
    assert(!validModelPayloadStructure(sortPayload));

    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "smoke:mesh";

    MaterialRecord materialRecord;
    materialRecord.sourceIdentity = "smoke:material";

    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "smoke:model";
    modelRecord.payload = std::move(modelPayload);

    InstanceRecord instanceRecord;
    instanceRecord.model = *model;

    RenderWorldUpdateBatch batch(world.epoch(), InitialUpdateSequence, "cp3b-smoke");
    assert(batch.add(CreateMesh{ *mesh, std::move(meshRecord) }));
    assert(batch.add(CreateMaterial{ *material, std::move(materialRecord) }));
    assert(batch.add(CreateModel{ *model, std::move(modelRecord) }));
    assert(batch.add(CreateInstance{ *instance, std::move(instanceRecord) }));
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

    NifRender::TranslationBundle translated;
    translated.sourceIdentity = "meshes/affine.nif";
    translated.contentIdentity = "hash:affine";
    translated.model.sourceIdentity = translated.sourceIdentity;
    translated.model.contentIdentity = translated.contentIdentity;

    NifRender::TranslatedMesh stagedMesh;
    stagedMesh.record.sourceIdentity = translated.sourceIdentity + "#mesh:0";
    translated.meshes.push_back(stagedMesh);

    NifRender::TranslatedMaterial stagedMaterial;
    stagedMaterial.state.sourceIdentity = translated.sourceIdentity + "#material:0";
    translated.materials.push_back(stagedMaterial);

    NifRender::TranslatedModelNode stagedRoot;
    stagedRoot.kind = ModelNodeKind::Transform;
    stagedRoot.localTransform[0][0] = -1.0f;
    stagedRoot.localTransform[1][1] = 2.0f;
    stagedRoot.localTransform[2][2] = 0.5f;
    translated.model.nodes.push_back(stagedRoot);

    NifRender::TranslatedModelNode stagedGeometry;
    stagedGeometry.parent = ModelNodeIndex{ 0u };
    stagedGeometry.kind = ModelNodeKind::Geometry;
    stagedGeometry.mesh = NifRender::MeshIndex{ 0u };
    stagedGeometry.materials.push_back(NifRender::MaterialIndex{ 0u });
    translated.model.nodes.push_back(stagedGeometry);
    translated.model.roots.push_back(ModelNodeIndex{ 0u });

    NifRender::TranslationOutcome outcome;
    outcome.disposition = NifRender::TranslationDisposition::Rendered;
    outcome.sourceRecordId = 7u;
    outcome.sourceRecordType = "NiTriShape";
    translated.outcomes.push_back(outcome);

    NifRender::TranslationDiagnostic diagnostic;
    diagnostic.severity = NifRender::DiagnosticSeverity::Warning;
    diagnostic.sourceRecordId = 7u;
    diagnostic.sourceRecordType = "NiTriShape";
    diagnostic.code = "smoke.warning.a";
    translated.diagnostics.push_back(diagnostic);
    diagnostic.code = "smoke.warning.b";
    translated.diagnostics.push_back(diagnostic);

    assert(translated.valid());
    assert(!translated.hasErrors());
    assert(translated.summary().rendered == 1u);

    translated.model.nodes[0].localTransform[3][0] = std::numeric_limits<float>::quiet_NaN();
    assert(!translated.valid());

    return 0;
}
