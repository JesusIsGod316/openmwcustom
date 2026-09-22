#include <components/nifrender/actormodelcomposer.hpp>
#include <components/render/backend/vsg/dynamicactorplan.hpp>
#include <components/render/backend/vsg/staticassetrealizer.hpp>
#include <components/rendercore/deformation.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <apps/openmw/mwrender/v4rigidactorpose.hpp>
#include <osg/MatrixTransform>
#include <osg/PositionAttitudeTransform>
#include <iostream>
#include <stdexcept>

namespace
{
    using namespace RenderCore;
    void check(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    glm::mat4 column(const osg::Matrixf& row)
    {
        glm::mat4 value;
        for (int c = 0; c != 4; ++c)
            for (int r = 0; r != 4; ++r) value[c][r] = row(c, r);
        return value;
    }
    ModelHandle publishModel(RenderWorld& world, std::shared_ptr<ModelPayload> payload)
    {
        ModelRecord record;
        record.sourceIdentity = "skin-space-fixture";
        record.payload = std::move(payload);
        auto handle = world.reserveModel();
        check(handle && world.commit(*handle, std::move(record)), "model publication failed");
        return *handle;
    }
    void rigidAttachmentParity()
    {
        for (bool left : {false, true}) for (bool withOffset : {false, true})
        {
            RenderWorld world;
            auto basePayload = std::make_shared<ModelPayload>();
            ModelNodeRecord root; root.name = "Base";
            ModelNodeRecord bone; bone.name = left ? "Left Clavicle" : "Right Clavicle";
            bone.parent = ModelNodeIndex{0};
            basePayload->nodes = {root, bone}; basePayload->roots = {ModelNodeIndex{0}};
            const auto base = publishModel(world, basePayload);
            auto forced = NifRender::buildForcedActorSkeleton(*world.get(base));
            check(forced.valid(), "rigid fixture skeleton failed");
            auto skeleton = world.reserveSkeleton();
            check(skeleton && world.commit(*skeleton, forced.record), "rigid fixture skeleton publication failed");
            auto material = world.reserveMaterial();
            check(material && world.commit(*material, MaterialRecord{}), "rigid fixture material failed");
            MeshRecord meshRecord;
            auto meshPayload = std::make_shared<MeshPayload>();
            meshPayload->positions = {{1,0,0}, {0,1,0}, {0,0,1}};
            meshPayload->indices = {0,1,2};
            meshPayload->surfaces = {{PrimitiveTopology::Triangles,0,3,0}};
            meshRecord.payload = meshPayload;
            meshRecord.surfaceCount = 1;
            auto mesh = world.reserveMesh();
            check(mesh && world.commit(*mesh, meshRecord), "rigid fixture mesh failed");
            auto partPayload = std::make_shared<ModelPayload>();
            ModelNodeRecord partRoot; partRoot.name = "Part";
            partRoot.localTransform = column(osg::Matrixf::rotate(0.4f, osg::Vec3f(0,0,1)));
            ModelNodeRecord geometry; geometry.name = "Pauldron"; geometry.kind = ModelNodeKind::Geometry;
            geometry.parent = ModelNodeIndex{0}; geometry.mesh = *mesh; geometry.materials = {*material};
            partPayload->nodes = {partRoot, geometry}; partPayload->roots = {ModelNodeIndex{0}};
            if (withOffset)
            {
                ModelNodeRecord offset; offset.name = "BoneOffset"; offset.parent = ModelNodeIndex{0};
                // Canonical attach uses only this translation, NOT the rotation.
                offset.localTransform = column(osg::Matrixf::rotate(1.f, osg::Vec3f(0,1,0)));
                offset.localTransform[3] = glm::vec4(2,3,4,1);
                partPayload->nodes.push_back(offset);
            }
            const auto part = publishModel(world, partPayload);
            auto result = NifRender::composeActorModel(world, base, *skeleton, {{part, bone.name, true}}, "rigid");
            check(result.valid(), result.diagnostic.c_str());
            auto handle = world.reserveModel();
            check(handle && world.commit(*handle, result.record), "composed rigid fixture failed");
            const auto plan = RenderVsg::buildStaticAssetPlan(world, *handle);
            check(plan && plan->draws.size() == 1, "rigid draw plan failed");
            auto oracle = osg::ref_ptr<osg::PositionAttitudeTransform>(new osg::PositionAttitudeTransform);
            if (left) oracle->setScale(osg::Vec3d(-1,1,1));
            if (withOffset) oracle->setPosition(osg::Vec3d(2,3,4));
            osg::Matrixd matrix;
            oracle->computeLocalToWorldMatrix(matrix, nullptr);
            const glm::mat4 expected = column(osg::Matrixf(matrix)) * partRoot.localTransform;
            check(plan->draws[0].worldTransform == expected, "rigid BoneOffset/reflection differs from canonical PAT");
            check(plan->draws[0].pipeline.fixedFunction.raster.frontFace
                == (left ? FrontFaceWinding::Clockwise : FrontFaceWinding::CounterClockwise),
                "left reflection failed to preserve visible triangle winding");
            check(world.get(part)->payload->nodes[0].localTransform == partRoot.localTransform,
                "rigid repair mutated shared source part");
        }
    }
    void attachmentSkeleton()
    {
        RenderWorld world;
        auto basePayload = std::make_shared<ModelPayload>();
        ModelNodeRecord structural; structural.name = "base.nif";
        ModelNodeRecord hip; hip.name = "Hip"; hip.parent = ModelNodeIndex{0};
        ModelNodeRecord slot; slot.name = "Equipment"; slot.parent = ModelNodeIndex{1};
        slot.localTransform[3][0] = 3;
        basePayload->nodes = {structural, hip, slot}; basePayload->roots = {ModelNodeIndex{0}};
        const auto base = publishModel(world, basePayload);
        auto reduced = std::make_shared<SkeletonPayload>();
        BoneRecord hipBone; hipBone.name = "Hip"; reduced->bones = {hipBone};
        SkeletonRecord reducedRecord; reducedRecord.payload = reduced;
        auto reducedHandle = world.reserveSkeleton();
        check(reducedHandle && world.commit(*reducedHandle, reducedRecord), "reduced skeleton publication");
        auto partPayload = std::make_shared<ModelPayload>();
        ModelNodeRecord weapon; weapon.name = "Weapon";
        ModelNodeRecord arrow; arrow.name = "ArrowBone"; arrow.parent = ModelNodeIndex{0};
        arrow.localTransform[3][1] = 2;
        partPayload->nodes = {weapon, arrow}; partPayload->roots = {ModelNodeIndex{0}};
        const auto part = publishModel(world, partPayload);
        auto ammoPayload = std::make_shared<ModelPayload>();
        ModelNodeRecord ammoNode; ammoNode.name = "Ammo";
        ammoPayload->nodes = {ammoNode}; ammoPayload->roots = {ModelNodeIndex{0}};
        const auto ammo = publishModel(world, ammoPayload);
        const std::vector<NifRender::ActorPartModelSource> parts{{part, "eQuIpMeNt", true}, {ammo, "ArrowBone", true}};
        // Characterize the old runtime choice: skin-required bones alone discard
        // a valid, animated attachment node even though it exists in the base NIF.
        check(!NifRender::composeActorModel(world, base, *reducedHandle, parts, "reduced").valid(),
            "fixture no longer reproduces reduced-skeleton attachment rejection");
        auto full = NifRender::buildForcedActorSkeleton(*world.get(base));
        check(full.valid() && full.record.payload->bones.size() == 2, "full attachment skeleton failed");
        auto fullHandle = world.reserveSkeleton();
        check(fullHandle && world.commit(*fullHandle, full.record), "full skeleton publication");
        auto composed = NifRender::composeActorModel(world, base, *fullHandle, parts, "full");
        check(composed.valid() && composed.record.payload->nodes.size() == 6, "attachment/weapon-local ammo lost");
        for (float x : {3.f, 8.f})
        {
            std::vector<glm::mat4> pose(2, glm::mat4(1)); pose[1][3][0] = x;
            const auto placed = posedModelTransforms(*composed.record.payload, *full.record.payload, pose);
            check(glm::length(glm::vec3(placed.back()[3]) - glm::vec3(x, 2, 0)) < 0.0001f,
                "equipment/ammo failed to follow animated attachment bone");
        }
        const auto missing = NifRender::composeActorModel(world, base, *fullHandle,
            {{part, "NoSuchBone", true}}, "missing");
        check(!missing.valid(), "genuinely missing attachment silently skipped");
        const auto stale = NifRender::composeActorModel(world, base, *fullHandle,
            {{ModelHandle{}, "Equipment", true}}, "stale");
        check(!stale.valid(), "missing model silently skipped");
        check(missing.diagnostic != stale.diagnostic, "missing model and bone diagnoses still conflated");
        check(missing.diagnostic.find("NoSuchBone") != std::string::npos
            && missing.diagnostic.find("skin-space-fixture") != std::string::npos,
            "attachment diagnostic lacks bone/model identity");
    }
    void parity(bool detached, bool namedRoot, bool weighted, bool identityLeaf, bool nameCollision)
    {
        RenderWorld world;
        const osg::Matrixf skinTransform = osg::Matrixf::rotate(0.3f, osg::Vec3f(0, 0, 1))
            * osg::Matrixf::translate(0, 2, 0);
        const osg::Matrixf inverseBind = osg::Matrixf::translate(-1, 0, 0);
        const osg::Matrixf leaf = identityLeaf ? osg::Matrixf::identity()
            : osg::Matrixf::scale(0.8f, 1.2f, 1.4f) * osg::Matrixf::rotate(0.4f, osg::Vec3f(1, 0, 0))
                * osg::Matrixf::translate(3, -2, 1);
        const osg::Matrixf donor = osg::Matrixf::translate(7, 9, 2);
        auto source = osg::ref_ptr<osg::Geometry>(new osg::Geometry);
        auto vertices = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array);
        vertices->push_back({1, 2, 0}); vertices->push_back({-2, 1, 3}); vertices->push_back({0, 0, 1});
        source->setVertexArray(vertices);
        auto normals = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array(3));
        for (auto& normal : *normals) normal = {0, 0, 1};
        source->setNormalArray(normals, osg::Array::BIND_PER_VERTEX);
        source->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        auto rig = osg::ref_ptr<SceneUtil::RigGeometry>(new SceneUtil::RigGeometry);
        const std::string geometryName = nameCollision ? "Hand" : "Tri Hand";
        rig->setName(geometryName);
        rig->setSourceGeometry(source);
        rig->setBoneInfo({ { "Root", {}, osg::Matrixf::identity() }, { "Hand", {}, inverseBind } });
        const float secondWeight = weighted ? 0.45f : 0.75f;
        rig->setInfluences({ {{0, 0.25f}, {1, secondWeight}}, {{1, 1.f}}, {} });
        rig->setTransform(osg::Matrixf(skinTransform));
        rig->setRootBone(namedRoot ? "rOoT" : "removed donor");
        auto skeleton = osg::ref_ptr<SceneUtil::Skeleton>(new SceneUtil::Skeleton);
        auto root = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        auto hand = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        auto geometry = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        root->setName("Root"); hand->setName("Hand"); geometry->setName(geometryName);
        skeleton->addChild(root); root->addChild(hand);
        geometry->setMatrix(leaf); geometry->addChild(rig);
        if (detached) skeleton->addChild(geometry); else root->addChild(geometry);
        osg::NodePath path{ skeleton, root, geometry, rig };
        if (detached) path.erase(path.begin() + 1);

        auto meshPayload = std::make_shared<MeshPayload>();
        for (const auto& v : *vertices) meshPayload->positions.emplace_back(v.x(), v.y(), v.z());
        meshPayload->normals.resize(3, {0, 0, 1});
        meshPayload->indices = {0, 1, 2};
        meshPayload->surfaces.push_back({PrimitiveTopology::Triangles, 0, 3, 0});
        auto skin = std::make_shared<SkinPayload>();
        skin->rootBoneName = namedRoot ? "rOoT" : "removed donor";
        // Reproduce the old translator's donor-space bake. The attached tree
        // below deliberately differs from this original donor parent.
        skin->meshToSkeleton = column(skinTransform) * glm::inverse(column(donor));
        skin->geometryBindTransform = column(skinTransform);
        skin->bones = {{"Root", glm::mat4(1)}, {"Hand", column(inverseBind)}};
        skin->vertexInfluences = {{{0, 0.25f}, {1, secondWeight}}, {{1, 1.f}}, {}};
        MeshRecord meshRecord;
        meshRecord.payload = meshPayload; meshRecord.skin = skin; meshRecord.skinned = true; meshRecord.surfaceCount = 1;
        auto mesh = world.reserveMesh();
        check(mesh && world.commit(*mesh, std::move(meshRecord)), "mesh publication failed");
        auto material = world.reserveMaterial();
        check(material && world.commit(*material, MaterialRecord{}), "material publication failed");
        auto skeletonPayload = std::make_shared<SkeletonPayload>();
        BoneRecord rootBone; rootBone.name = "Root";
        BoneRecord handBone; handBone.name = "Hand"; handBone.parent = 0;
        skeletonPayload->bones = {rootBone, handBone};
        SkeletonRecord skeletonRecord; skeletonRecord.payload = skeletonPayload;
        auto skeletonHandle = world.reserveSkeleton();
        check(skeletonHandle && world.commit(*skeletonHandle, std::move(skeletonRecord)), "skeleton publication failed");
        auto modelPayload = std::make_shared<ModelPayload>();
        ModelNodeRecord rootNode; rootNode.name = "Root";
        ModelNodeRecord handNode; handNode.name = "Hand"; handNode.parent = ModelNodeIndex{0};
        ModelNodeRecord leafNode; leafNode.name = geometryName; leafNode.localTransform = column(leaf);
        leafNode.kind = ModelNodeKind::Geometry; leafNode.mesh = *mesh; leafNode.materials = {*material};
        leafNode.parent = ModelNodeIndex{0};
        modelPayload->nodes = {rootNode, handNode}; modelPayload->roots = {ModelNodeIndex{0}};
        ModelHandle model;
        if (detached)
        {
            const auto base = publishModel(world, modelPayload);
            auto part = std::make_shared<ModelPayload>();
            rootNode.localTransform = column(donor);
            part->nodes = {rootNode, leafNode}; part->roots = {ModelNodeIndex{0}};
            auto composed = NifRender::composeActorModel(world, base, *skeletonHandle,
                {{publishModel(world, part), "Hand", true}}, "attached-fixture");
            check(composed.valid(), "actual actor composer rejected fixture");
            auto handle = world.reserveModel();
            check(handle && world.commit(*handle, std::move(composed.record)), "composition publication failed");
            model = *handle;
        }
        else
        {
            modelPayload->nodes.push_back(leafNode);
            model = publishModel(world, modelPayload);
        }
        auto instance = world.reserveInstance();
        InstanceRecord instanceRecord; instanceRecord.model = model; instanceRecord.skeleton = *skeletonHandle;
        check(instance && world.commit(*instance, std::move(instanceRecord)), "instance publication failed");
        auto plan = RenderVsg::buildDynamicActorPlan(world, *instance);
        check(plan && plan->asset.draws.size() == 1, "actor plan rejected fixture");
        SingleViewFrameProducer producer;
        std::vector<glm::vec3> previousPositions;
        std::vector<RenderVsg::StaticRealizationResult::MutableDrawStreams> resident(1);
        resident[0].positions = vsg::vec3Array::create(3);
        resident[0].normals = vsg::vec3Array::create(3);
        resident[0].transform = vsg::MatrixTransform::create();
        const auto* originalBuffer = resident[0].positions.get();
        for (unsigned phase = 1; phase <= 2; ++phase)
        {
            root->setMatrix(osg::Matrixf::rotate(phase * 0.2f, osg::Vec3f(0, 1, 0)) * osg::Matrixf::translate(static_cast<float>(phase), 4, 1));
            hand->setMatrix(osg::Matrixf::rotate(phase * 0.5f, osg::Vec3f(0, 0, 1)) * osg::Matrixf::translate(2, 0, 0));
            auto* evaluated = rig->evaluateGeometry(phase, path);
            check(evaluated != nullptr, "canonical rig evaluation failed");
            const auto* expected = static_cast<osg::Vec3Array*>(evaluated->getVertexArray());
            const auto* expectedNormals = static_cast<osg::Vec3Array*>(evaluated->getNormalArray());
            SingleViewFrameInput input;
            input.renderExtent = input.outputExtent = {800, 600}; input.environment.skyEnabled = false;
            SkeletonPoseInput pose;
            pose.instance = *instance; pose.skeleton = *skeletonHandle;
            pose.localTransforms = {column(osg::Matrixf(root->getMatrix())), column(osg::Matrixf(hand->getMatrix()))};
            input.skeletonPoses.push_back(pose);
            auto frame = producer.produce(world, input);
            check(frame.has_value(), "pose frame rejected");
            std::vector<glm::mat4> evaluatedNodes;
            auto asset = RenderVsg::evaluateDynamicActorAssetPlan(world, *frame, *plan, &evaluatedNodes);
            check(asset.has_value(), "evaluated draw rejected");
            const auto& draw = asset->draws.front();
            auto actual = deformMesh(world, *frame, *instance, draw.mesh, draw.node, false, &evaluatedNodes);
            check(actual.ready(), "neutral deformation rejected");
            auto uncached = deformMesh(world, *frame, *instance, draw.mesh, draw.node);
            check(uncached.ready() && uncached.positions == actual.positions, "shared posed-node evaluation differs");
            if (phase == 2)
            {
                auto previous = deformMesh(world, *frame, *instance, draw.mesh, draw.node, true, &evaluatedNodes);
                check(previous.ready() && previous.positions == previousPositions, "previous pose reused current model transforms");
            }
            previousPositions = actual.positions;
            MeshPayload updated = *meshPayload;
            updated.positions = actual.positions; updated.normals = actual.normals;
            check(RenderVsg::updateDeformedAssetRealization(world, *asset,
                [&](MeshHandle, ModelNodeIndex) { return &updated; }, resident), "resident update rejected skin-local geometry");
            check(resident[0].positions.get() == originalBuffer, "skin repair reallocated resident stream");
            check(!deformMesh(world, *frame, *instance, draw.mesh, ModelNodeIndex{9999}).ready(),
                "invalid geometry path silently accepted");
            for (std::size_t i = 0; i < expected->size(); ++i)
            {
                auto a = glm::vec3(draw.worldTransform * glm::vec4(actual.positions[i], 1));
                auto e = (*expected)[i] * osg::computeLocalToWorld(path);
                if (glm::length(a - glm::vec3(e.x(), e.y(), e.z())) > 0.0001f)
                    throw std::runtime_error("canonical/neutral final-space vertex mismatch, vertex=" + std::to_string(i));
                const auto& n = (*expectedNormals)[i];
                check(glm::length(actual.normals[i] - glm::vec3(n.x(), n.y(), n.z())) < 0.0001f,
                    "canonical/neutral local normal mismatch");
                const auto& p = (*resident[0].positions)[i];
                const auto residentPosition = resident[0].transform->matrix * vsg::dvec3(p.x, p.y, p.z);
                check(glm::length(glm::dvec3(residentPosition.x, residentPosition.y, residentPosition.z)
                    - glm::dvec3(e.x(), e.y(), e.z())) < 0.0001, "reused VSG draw lost geometry placement");
            }
        }
        MeshRecord changed = *world.get(*mesh);
        auto changedSkin = std::make_shared<SkinPayload>(*changed.skin);
        (*changedSkin->geometryBindTransform)[3][0] += 1;
        changed.skin = changedSkin;
        changed.revision = ResourceRevision{changed.revision.value() + 1};
        check(world.update(*mesh, std::move(changed)) && !RenderVsg::dynamicActorPlanCurrent(world, *plan),
            "skin-contract revision failed to invalidate actor residency");
    }
}
int main()
{
    unsigned failures = 0;
    try
    {
        auto root = osg::ref_ptr<osg::Group>(new osg::Group);
        auto parent = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        auto child = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        auto duplicate = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        parent->setName("Body"); child->setName("Wing"); duplicate->setName("WING");
        parent->addChild(child); root->addChild(parent); root->addChild(duplicate);
        duplicate->setMatrix(osg::Matrixf::translate(99, 0, 0));
        auto oracle = osg::ref_ptr<SceneUtil::Skeleton>(new SceneUtil::Skeleton);
        oracle->addChild(root);
        RenderCore::SkeletonPayload payload;
        RenderCore::BoneRecord bone; bone.name = "body"; payload.bones.push_back(bone);
        bone.name = "wing"; bone.parent = 0; payload.bones.push_back(bone);
        for (unsigned int frame = 1; frame <= 2; ++frame)
        {
            parent->setMatrix(osg::Matrixf::translate(float(frame), 3, 4));
            child->setMatrix(osg::Matrixf::rotate(0.5f * frame, osg::Vec3f(0, 0, 1)));
            std::vector<glm::mat4> captured;
            std::string diagnostic;
            check(MWRender::captureV4RigidActorPose(*root, payload, captured, diagnostic), diagnostic.c_str());
            const auto* body = oracle->getBone("body"); const auto* wing = oracle->getBone("wing");
            oracle->updateBoneMatrices(frame);
            check(captured[0] == column(body->mMatrixInSkeletonSpace)
                && captured[1] == column(wing->mMatrixInSkeletonSpace), "rigid creature pose differs from canonical skeleton");
        }
        payload.bones.back().name = "missing";
        std::vector<glm::mat4> captured;
        std::string diagnostic;
        check(!MWRender::captureV4RigidActorPose(*root, payload, captured, diagnostic) && captured.empty(),
            "missing rigid transform silently accepted");
        std::cout << "Rigid creature pose parity: PASS\n";
    }
    catch (const std::exception& e) { ++failures; std::cerr << "Rigid creature pose: " << e.what() << '\n'; }
    for (bool detached : {false, true})
        for (bool named : {false, true})
            for (bool weighted : {false, true})
                for (bool identity : {false, true})
                for (bool collision : {false, true})
                {
                    try { parity(detached, named, weighted, identity, collision); }
                    catch (const std::exception& e) { ++failures; std::cerr << detached << named << weighted << identity << collision << ": " << e.what() << '\n'; }
                }
    std::cout << "Actor skin-space parity: " << (32 - failures) << "/32 PASS\n";
    try { rigidAttachmentParity(); std::cout << "Rigid attachment parity: 4/4 PASS\n"; }
    catch (const std::exception& e) { ++failures; std::cerr << "Rigid attachment parity: " << e.what() << '\n'; }
    try { attachmentSkeleton(); std::cout << "Attachment skeleton regression: PASS\n"; }
    catch (const std::exception& e) { ++failures; std::cerr << "Attachment skeleton regression: " << e.what() << '\n'; }
    return failures ? 1 : 0;
}
