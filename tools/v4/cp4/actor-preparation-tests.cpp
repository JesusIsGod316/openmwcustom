#include <components/render/backend/vsg/dynamicactorpreparation.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <iostream>
#include <stdexcept>

using namespace RenderCore;
using namespace RenderVsg;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main()
{
    try
    {
        RenderWorld world;
        auto skeleton = *world.reserveSkeleton();
        auto bones = std::make_shared<SkeletonPayload>(); BoneRecord bone; bone.name = "root";
        bones->bones.push_back(bone); SkeletonRecord sr; sr.payload = bones;
        require(world.commit(skeleton, sr), "skeleton");
        auto geometry = std::make_shared<MeshPayload>();
        geometry->positions = {{0,0,0},{1,0,0},{0,1,0}}; geometry->normals.assign(3, {0,0,1});
        geometry->indices = {0,1,2}; geometry->surfaces = {{PrimitiveTopology::Triangles,0,3,0}};
        geometry->colors.assign(3, {1, 0.5f, 0.25f, 1});
        geometry->texCoordSets = {{{0, 0}, {1, 0}, {0, 1}}};
        auto skin = std::make_shared<SkinPayload>(); skin->bones.push_back({"root", glm::mat4(1)});
        skin->vertexInfluences.resize(3, {SkinInfluence{0,1}});
        MeshRecord mr; mr.payload = geometry; mr.surfaceCount = 1; mr.skinned = true; mr.skin = skin;
        auto mesh = *world.reserveMesh(); require(world.commit(mesh, mr), "mesh");
        auto material = *world.reserveMaterial(); require(world.commit(material, MaterialRecord{}), "material");
        auto nodes = std::make_shared<ModelPayload>();
        ModelNodeRecord root; root.name = "root";
        ModelNodeRecord node; node.name = "body"; node.kind = ModelNodeKind::Geometry;
        node.parent = ModelNodeIndex{0}; node.mesh = mesh; node.materials = {material};
        nodes->nodes = {root,node}; nodes->roots = {ModelNodeIndex{0}};
        ModelRecord modelRecord; modelRecord.payload = nodes;
        auto model = *world.reserveModel(); require(world.commit(model, modelRecord), "model");
        SingleViewFrameInput input; input.renderExtent = input.outputExtent = {640,480}; input.environment.skyEnabled = false;
        for (unsigned i = 0; i < 48; ++i)
        {
            auto actor = *world.reserveInstance(); InstanceRecord record; record.model = model; record.skeleton = skeleton;
            require(world.commit(actor, record), "actor");
            DynamicTransformInput transform; transform.instance = actor;
            input.dynamicTransforms.push_back(transform);
            SkeletonPoseInput pose; pose.instance = actor; pose.skeleton = skeleton;
            pose.localTransforms = {glm::mat4(1)}; pose.localTransforms[0][3][0] = float(i);
            input.skeletonPoses.push_back(pose);
        }
        SingleViewFrameProducer producer; auto frame = producer.produce(world, input); require(bool(frame), "frame");
        const auto plan = buildDynamicActorWorldPlan(world); require(plan.valid() && plan.actors.size() == 48, "plan");
        PersistentActorPlanCache retained;
        constexpr std::uint64_t actorSourceSerial = 17;
        const auto& persistent = retained.prepare(world, {}, actorSourceSerial);
        require(persistent.valid() && persistent.actors.size() == 48 && retained.rebuilt == 48, "persistent plan");
        const auto owner = persistent.actors.front();
        require(owner->program != nullptr, "native program not bound");
        require(retained.prepare(world, {}, actorSourceSerial).actors.front() == owner
                && retained.rebuilt == 0 && retained.reused == 48,
            "unchanged actor source rebuilt persistent actor plans");
        // A light-only world revision is unrelated to actor planning. The exact
        // P2 source serial must acknowledge the new global revision without
        // scanning/revalidating all 48 actor dependencies.
        auto unrelatedLight = *world.reserveLight();
        require(world.commit(unrelatedLight, LightRecord{}), "unrelated light");
        const auto& afterLight = retained.prepare(world, {}, actorSourceSerial);
        require(afterLight.actors.front() == owner && retained.rebuilt == 0 && retained.reused == 48
                && afterLight.revision == world.revision(),
            "light-only world delta invalidated persistent actor plans");
        BoundedParallelFor workers(3,4,1);
        auto serial = prepareDynamicActors(world, *frame, plan);
        const auto native = preparePersistentActors(world, *frame, persistent.actors, &workers);
        for (std::size_t i = 0; i < native.size(); ++i)
        {
            require(native[i].diagnostic.empty() && native[i].assetPlan() == &persistent.actors[i]->asset,
                "native evaluator did not retain draw metadata");
            require(!native[i].asset && native[i].drawTransforms.size() == serial[i].asset->draws.size(),
                "native frame copied an asset plan");
            require(native[i].drawTransforms[0] == serial[i].asset->draws[0].worldTransform, "bound transform parity");
            require(native[i].deformed.at(1).positions == serial[i].deformed.at(1).positions
                && native[i].deformed.at(1).normals == serial[i].deformed.at(1).normals, "native deformation parity");
        }
        const auto streamed = prepareDynamicActors(world, *frame, plan, &workers, true);
        for (std::size_t i = 0; i < streamed.size(); ++i)
        {
            const auto& sparse = streamed[i].deformed.at(1);
            const auto& full = serial[i].deformed.at(1);
            require(streamed[i].diagnostic.empty() && sparse.positions == full.positions
                && sparse.normals == full.normals && sparse.tangents == full.tangents
                && sparse.bitangents == full.bitangents, "streamed deformation differs");
            require(sparse.indices.empty() && sparse.surfaces.empty() && sparse.colors.empty()
                && sparse.texCoordSets.empty(), "warm deformation copied static attributes");
            require(streamed[i].realizationPayloads.empty(), "cold payload allocated before demand");
            const auto* cold = streamed[i].realizationPayload(world, mesh, ModelNodeIndex{1});
            require(cold && validMeshPayload(*cold) && cold->positions == full.positions
                && cold->normals == full.normals && cold->indices == full.indices
                && cold->colors == full.colors && cold->texCoordSets == full.texCoordSets
                && cold->surfaces.size() == full.surfaces.size(), "cold realization lost authored attributes");
            require(cold == streamed[i].realizationPayload(world, mesh, ModelNodeIndex{1})
                && streamed[i].realizationPayloads.size() == 1, "cold payload duplicated");
            require(!streamed[i].realizationPayload(world, mesh, ModelNodeIndex{42}), "unknown node resolved");
        }
        require(geometry->positions[0] == glm::vec3(0), "immutable authored mesh was changed");
        for (unsigned repeat = 0; repeat < 30; ++repeat)
        {
            const auto parallel = prepareDynamicActors(world, *frame, plan, &workers);
            for (std::size_t i = 0; i < serial.size(); ++i)
            {
                require(parallel[i].diagnostic.empty() && serial[i].diagnostic.empty(), "preparation failed");
                require(parallel[i].transform == serial[i].transform, "actor order changed");
                require(parallel[i].asset->draws[0].worldTransform == serial[i].asset->draws[0].worldTransform, "pose changed");
                const auto& a = parallel[i].deformed.at(1); const auto& b = serial[i].deformed.at(1);
                require(a.positions == b.positions && a.normals == b.normals && a.indices == b.indices, "parallel skin differs");
                require(a.positions[0].x == float(i), "deformation oracle");
            }
        }
        for (std::size_t begin = 0; begin < plan.actors.size(); begin += 7)
        {
            const auto batch = std::span<const DynamicActorPlan>(plan.actors).subspan(begin,
                std::min(std::size_t{7}, plan.actors.size() - begin));
            const auto partial = prepareDynamicActors(world, *frame, batch, &workers);
            require(partial.size() == batch.size(), "batch size");
            for (std::size_t i = 0; i < partial.size(); ++i)
                require(partial[i].deformed.at(1).positions == serial[begin+i].deformed.at(1).positions,
                    "bounded batch differs from full actor order");
        }
        // Reuse a stale frame against newly discovered actors: deterministic
        // failure, no unchecked iterator and no worker-side publication.
        const auto extra = *world.reserveInstance(); InstanceRecord ir; ir.model = model; ir.skeleton = skeleton;
        require(world.commit(extra, ir), "extra actor");
        const auto missing = prepareDynamicActors(world, *frame, buildDynamicActorWorldPlan(world), &workers);
        require(!missing.back().diagnostic.empty(), "missing pose/transform accepted");
        const auto& changed = retained.prepare(world);
        require(changed.valid() && changed.actors.size() == 49 && retained.rebuilt == 1
            && changed.actors.front() == owner, "membership change rebuilt existing actor plans");
        const auto nativeMissing = preparePersistentActors(world, *frame, changed.actors, &workers);
        require(!nativeMissing.back().diagnostic.empty(), "native missing frame input accepted");
        // A published mesh revision replaces the program, while already-owned
        // plans remain alive for GPU-safe retirement and never mutate in place.
        auto replacement = *world.get(mesh);
        auto replacementPayload = std::make_shared<MeshPayload>(*replacement.payload);
        replacementPayload->positions[0].x = 5;
        replacement.payload = replacementPayload;
        replacement.revision = *advanceMonotonic(replacement.revision);
        require(world.update(mesh, replacement), "mesh replacement");
        const auto& rebound = retained.prepare(world);
        require(rebound.actors.front() != owner && rebound.actors.front()->program != owner->program,
            "mesh revision did not rebind native program");
        require(owner->program->meshes().front().source->positions[0].x == 0, "retired program mutated");
        require(owner->program->meshes().front().influenceGroups.size() == 1,
            "identical bone weights were not grouped at binding");
        // Morphs are frame data, not resource changes; retain their immutable
        // offsets and evaluate exact authored weights, including negative ones.
        auto morphPayload = std::make_shared<MorphPayload>();
        MorphTargetPayload target; target.sourceIndex = 1;
        target.positionOffsets = {{1,2,3}, {-1,0,2}, {0,1,-1}};
        morphPayload->targets.push_back(target);
        auto morphMesh = *world.get(mesh);
        morphMesh.skinned = false; morphMesh.skin.reset(); morphMesh.morphed = true; morphMesh.morphs = morphPayload;
        morphMesh.revision = *advanceMonotonic(morphMesh.revision);
        require(world.update(mesh, morphMesh), "morph replacement");
        const auto morphProgram = ActorProgram::bind(world, model, skeleton);
        require(bool(morphProgram), "morph program");
        ActorProgram::Pose evaluated;
        require(morphProgram->evaluatePose(frame->skeletonPoses().front(), evaluated), "morph pose");
        MorphWeightState morph; morph.current = {-0.75f};
        MeshPayload morphed;
        require(morphProgram->deform(morphProgram->meshes().front(), evaluated, &morph, morphed), "morph evaluation");
        for (std::size_t i = 0; i < morphed.positions.size(); ++i)
            require(morphed.positions[i] == morphMesh.payload->positions[i] + target.positionOffsets[i] * -0.75f,
                "morph offsets changed");
        require(!morphProgram->deform(morphProgram->meshes().front(), evaluated, nullptr, morphed), "missing morph accepted");
        morph.current = {std::numeric_limits<float>::quiet_NaN()};
        require(!morphProgram->deform(morphProgram->meshes().front(), evaluated, &morph, morphed), "nonfinite morph accepted");
        auto stalePose = frame->skeletonPoses().front(); stalePose.current.clear();
        require(!morphProgram->evaluatePose(stalePose, evaluated), "truncated pose accepted");
        require(world.retire(extra), "actor retirement");
        require(retained.prepare(world).actors.size() == 48, "retired actor remains in native registry");
        require(world.reset() && retained.prepare(world).actors.empty(), "world reset retained actor bindings");
        std::cout << "PASS parallel actor preparation: 48 distinct poses x 30, exact serial parity, missing frame input\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
