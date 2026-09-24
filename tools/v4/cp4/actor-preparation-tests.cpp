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
        BoundedParallelFor workers(3,4,1);
        auto serial = prepareDynamicActors(world, *frame, plan);
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
        std::cout << "PASS parallel actor preparation: 48 distinct poses x 30, exact serial parity, missing frame input\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
