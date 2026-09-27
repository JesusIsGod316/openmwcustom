#include <components/nifrender/actormodelcomposer.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    using namespace RenderCore;
    unsigned checks = 0;

    void check(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
            throw std::runtime_error(message);
    }

    bool near(const glm::mat4& a, const glm::mat4& b)
    {
        for (glm::length_t c = 0; c < 4; ++c)
            for (glm::length_t r = 0; r < 4; ++r)
                if (std::abs(a[c][r] - b[c][r]) > 1e-4f)
                    return false;
        return true;
    }

    ModelNodeRecord node(std::string name, ModelNodeIndex parent = {}, glm::mat4 transform = glm::mat4(1))
    {
        ModelNodeRecord result;
        result.name = std::move(name);
        result.parent = parent;
        result.localTransform = transform;
        return result;
    }

    ModelRecord model(std::vector<ModelNodeRecord> nodes)
    {
        auto payload = std::make_shared<ModelPayload>();
        payload->nodes = std::move(nodes);
        for (std::size_t i = 0; i < payload->nodes.size(); ++i)
            if (!payload->nodes[i].parent.valid())
                payload->roots.push_back(ModelNodeIndex{ static_cast<std::uint32_t>(i) });
        ModelRecord record;
        record.sourceIdentity = "fixture:winning-base.nif";
        record.payload = std::move(payload);
        return record;
    }

    void ordinaryAndCollapsedPaths()
    {
        const glm::mat4 hip = glm::translate(glm::mat4(1), glm::vec3(3, 4, 5));
        const glm::mat4 ancestor = glm::rotate(glm::mat4(1), 0.7f, glm::vec3(0, 0, 1));
        const glm::mat4 attachment = glm::scale(glm::mat4(1), glm::vec3(2));
        const glm::mat4 hand = glm::translate(glm::mat4(1), glm::vec3(1, -2, 3));
        const ModelRecord base = model({ node("BASE_ANIM.NIF"), node("Bip01", ModelNodeIndex{ 0 }, hip),
            node("", ModelNodeIndex{ 1 }, ancestor), node("Tri Helper", ModelNodeIndex{ 2 }, attachment),
            node("Bip01 Hand", ModelNodeIndex{ 3 }, hand) });
        const auto forced = NifRender::buildForcedActorSkeleton(base);
        check(forced.valid(), "valid source hierarchy rejected");
        const auto& bones = forced.record.payload->bones;
        check(bones.size() == 2, "structural/unnamed/Tri nodes became required bones");
        check(bones[0].name == "bip01" && bones[0].parent == -1, "root first-match bone identity changed");
        check(bones[1].name == "bip01 hand" && bones[1].parent == 0, "child bone identity changed");
        for (const auto& bone : bones)
        {
            check(bone.sourceAnimationBoundary, "forced skeleton lost authored source-animation boundary");
            check(bone.sourceControllerFlags == 0 && bone.sourceParentControllerFlags == 0,
                "controller-free hierarchy acquired controller flags");
            check(near(bone.sourceParentPath * bone.sourceLocal, bone.bindLocal),
                "authored boundary does not reconstruct existing bind local");
        }
        check(bones[0].sourceParentPathNodes == std::vector<std::string>{ "base_anim.nif" },
            "structural model root provenance lost");
        check(near(bones[0].sourceLocal, hip), "root source local is not its own authored transform");
        check(near(bones[1].sourceParentPath, ancestor * attachment), "collapsed ancestor order changed");
        check(near(bones[1].sourceLocal, hand), "child source local includes a collapsed parent");
        check(bones[1].sourceParentPathNodes == std::vector<std::string>({ "", "tri helper" }),
            "collapsed parent names/order lost");
        check(near(bones[1].inverseBind * hip * ancestor * attachment * hand, glm::mat4(1)),
            "inverse bind changed while adding metadata");

        // Native KF replaces only the bone-local transform, not collapsed ancestors.
        const glm::mat4 sampled = glm::rotate(glm::mat4(1), -0.3f, glm::vec3(1, 0, 0));
        check(near(bones[1].sourceParentPath * sampled, ancestor * attachment * sampled),
            "native replacement would overwrite the collapsed source path");
        check(base.payload->nodes[4].localTransform == hand, "source model mutated");
    }

    void duplicateNamesAndControllerGuards()
    {
        const auto transformFlag = modelControllerFlag(ModelControllerFlag::Transform);
        const auto visibilityFlag = modelControllerFlag(ModelControllerFlag::Visibility);
        const glm::mat4 duplicateLocal = glm::translate(glm::mat4(1), glm::vec3(9, 0, 0));
        auto first = node("Hip", ModelNodeIndex{ 0 });
        auto duplicate = node("hIP", ModelNodeIndex{ 1 }, duplicateLocal);
        duplicate.controllerFlags = transformFlag;
        duplicate.flags |= modelNodeFlag(ModelNodeFlag::ControllerTarget);
        auto helper = node("Tri Helper", ModelNodeIndex{ 2 });
        helper.controllerFlags = visibilityFlag;
        helper.flags |= modelNodeFlag(ModelNodeFlag::ControllerTarget);
        auto hand = node("Hand", ModelNodeIndex{ 3 });
        hand.controllerFlags = transformFlag;
        hand.flags |= modelNodeFlag(ModelNodeFlag::ControllerTarget);
        const auto forced = NifRender::buildForcedActorSkeleton(model({ node("root.nif"), first, duplicate, helper, hand }));
        check(forced.valid(), "duplicate first-match fixture rejected");
        const auto& bones = forced.record.payload->bones;
        check(bones.size() == 2 && bones[0].name == "hip", "duplicate name first-wins contract changed");
        check(bones[0].sourceControllerFlags == 0, "duplicate controller incorrectly assigned to first bone");
        check(bones[1].sourceControllerFlags == transformFlag, "bone embedded controller guard lost");
        check(bones[1].sourceParentControllerFlags == (transformFlag | visibilityFlag),
            "collapsed embedded controller guard lost");
        check(bones[1].sourceParentPathNodes == std::vector<std::string>({ "hip", "tri helper" }),
            "duplicate collapsed source provenance lost");
        check(near(bones[1].sourceParentPath, duplicateLocal), "duplicate source transform dropped");
        check(near(bones[1].bindLocal, duplicateLocal), "legacy duplicate bind behavior changed");

        auto controlledRoot = node("Controlled Root");
        controlledRoot.controllerFlags = transformFlag;
        controlledRoot.flags |= modelNodeFlag(ModelNodeFlag::ControllerTarget);
        const auto root = NifRender::buildForcedActorSkeleton(model({ controlledRoot }));
        check(root.valid() && root.record.payload->bones.size() == 1, "controlled identity root was discarded");
        check(root.record.payload->bones[0].sourceControllerFlags == transformFlag,
            "root controller metadata was fabricated as controller-free");
        check(root.record.payload->bones[0].sourceParentPathNodes.empty(), "root invented a source parent");
    }

    void publicationCompositionAndRebuild()
    {
        RenderWorld world;
        const auto baseRecord = model({ node("base.nif"), node("Hip", ModelNodeIndex{ 0 }),
            node("Hand", ModelNodeIndex{ 1 }, glm::translate(glm::mat4(1), glm::vec3(2, 3, 4))) });
        const auto base = world.reserveModel();
        check(base && world.commit(*base, baseRecord), "base publication failed");
        const auto forced = NifRender::buildForcedActorSkeleton(*world.get(*base), "runtime:forced:base");
        check(forced.valid(), "published base cannot produce skeleton");
        const auto skeleton = world.reserveSkeleton();
        check(skeleton && world.commit(*skeleton, forced.record), "skeleton publication failed");
        const auto originalPayload = world.get(*skeleton)->payload;
        check(originalPayload == forced.record.payload, "publication replaced metadata payload");

        // Equipment may repeat base names with unrelated donor transforms. Those
        // names rebind to the master skeleton; donor locals must not become its metadata.
        const auto part = world.reserveModel();
        const auto partRecord = model({ node("Hand", {}, glm::translate(glm::mat4(1), glm::vec3(99))),
            node("ArrowBone", ModelNodeIndex{ 0 }) });
        check(part && world.commit(*part, partRecord), "part publication failed");
        NifRender::ActorPartModelSource source;
        source.model = *part;
        source.attachmentBone = "Hand";
        for (bool visible : { true, false })
        {
            source.visible = visible;
            const auto composed = NifRender::composeActorModel(world, *base, *skeleton, { source }, "runtime:npc");
            check(composed.valid(), "equipment composition rejected fixture");
            check(world.get(*skeleton)->payload == originalPayload, "composition rewrote master skeleton");
            const auto& hand = world.get(*skeleton)->payload->bones[1];
            check(hand.sourceAnimationBoundary && hand.sourceLocal == baseRecord.payload->nodes[2].localTransform,
                "composition replaced base provenance with donor metadata");
        }
        source.attachmentBone = "Absent";
        check(!NifRender::composeActorModel(world, *base, *skeleton, { source }, "bad").valid(),
            "unsafe missing attachment no longer fails closed");

        const auto rebuiltBase = model({ node("firstperson.nif"),
            node("Hip", ModelNodeIndex{ 0 }, glm::translate(glm::mat4(1), glm::vec3(-5, 0, 0))) });
        const auto rebuilt = NifRender::buildForcedActorSkeleton(rebuiltBase, "runtime:forced:firstperson");
        check(rebuilt.valid() && rebuilt.record.payload != originalPayload, "rebuild reused unrelated payload");
        check(rebuilt.record.payload->bones[0].sourceLocal == rebuiltBase.payload->nodes[1].localTransform,
            "rebuild inherited old source local");
        check(rebuilt.record.payload->bones[0].sourceParentPathNodes == std::vector<std::string>{ "firstperson.nif" },
            "rebuild inherited old source path");
        check(originalPayload->bones[1].sourceLocal == baseRecord.payload->nodes[2].localTransform,
            "rebuild mutated the previous actor skeleton");
    }

    void invalidAndMultipleRoots()
    {
        check(!NifRender::buildForcedActorSkeleton(ModelRecord{}).valid(), "missing payload accepted");
        check(!NifRender::buildForcedActorSkeleton(model({ node("invalid", ModelNodeIndex{ 4 }) })).valid(),
            "invalid source topology accepted");
        check(!NifRender::buildForcedActorSkeleton(model({ node("singular", {}, glm::mat4(0)) })).valid(),
            "singular bind accepted");
        glm::mat4 invalid(1);
        invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
        check(!NifRender::buildForcedActorSkeleton(model({ node("nonfinite", {}, invalid) })).valid(),
            "nonfinite source accepted");
        const auto roots = NifRender::buildForcedActorSkeleton(model({
            node("One", {}, glm::translate(glm::mat4(1), glm::vec3(1, 0, 0))),
            node("Two", {}, glm::translate(glm::mat4(1), glm::vec3(0, 2, 0))) }));
        check(roots.valid() && roots.record.payload->bones.size() == 2, "multiple roots rejected");
        for (const auto& bone : roots.record.payload->bones)
        {
            check(bone.sourceAnimationBoundary && bone.parent == -1, "multiple root metadata lost");
            check(bone.sourceParentPathNodes.empty() && bone.sourceParentPath == glm::mat4(1),
                "independent root inherited unrelated ancestry");
        }
    }
}

int main()
{
    try
    {
        ordinaryAndCollapsedPaths();
        duplicateNamesAndControllerGuards();
        publicationCompositionAndRebuild();
        invalidAndMultipleRoots();
        std::cout << "Phase 3C metadata: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Phase 3C metadata FAILED: " << error.what() << '\n';
        return 1;
    }
}
