#ifndef OPENMW_COMPONENTS_NIFRENDER_ACTORMODELCOMPOSER_H
#define OPENMW_COMPONENTS_NIFRENDER_ACTORMODELCOMPOSER_H

#include <components/rendercore/renderworld.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

namespace NifRender
{
    struct ActorPartModelSource
    {
        RenderCore::ModelHandle model;
        std::string attachmentBone;
        bool visible = true;
    };

    struct ForcedActorSkeleton
    {
        RenderCore::SkeletonRecord record;
        std::string diagnostic;

        [[nodiscard]] bool valid() const noexcept { return record.payload && diagnostic.empty(); }
    };

    // OpenMW can force an actor skeleton around a base NIF even when that NIF
    // contains no skinned geometry. Static NIF translation intentionally only
    // emits skin-required bones, so reproduce the source-side forced-skeleton
    // contract from the immutable model hierarchy when an actor needs it.
    //
    // Bone candidates are named ordinary transform nodes. Their hierarchy is
    // collapsed across non-bone ancestors exactly like the normal NIF skeleton
    // translator, and all names are case-folded to match OpenMW's bone lookup.
    // OpenMW's source Skeleton cache keeps the first case-insensitive name match,
    // so duplicate named transforms are skipped here rather than rejected.
    // Non-invertible transforms remain fail-closed.
    [[nodiscard]] inline ForcedActorSkeleton buildForcedActorSkeleton(
        const RenderCore::ModelRecord& base, std::string sourceIdentity = {})
    {
        using namespace RenderCore;
        ForcedActorSkeleton result;
        if (!base.payload || !validModelPayloadStructure(*base.payload))
        {
            result.diagnostic = "forced actor skeleton requires a valid base model payload";
            return result;
        }

        const auto foldName = [](std::string_view value) {
            std::string folded(value);
            std::transform(folded.begin(), folded.end(), folded.begin(),
                [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
            return folded;
        };

        auto payload = std::make_shared<SkeletonPayload>();
        std::vector<std::optional<std::size_t>> modelToBone(base.payload->nodes.size());
        std::vector<glm::mat4> globalBind;
        std::unordered_map<std::string, std::size_t> names;

        for (std::size_t modelNode = 0; modelNode < base.payload->nodes.size(); ++modelNode)
        {
            const ModelNodeRecord& source = base.payload->nodes[modelNode];
            if (source.kind != ModelNodeKind::Transform || source.name.empty())
                continue;

            const std::string folded = foldName(source.name);
            // SceneUtil::Skeleton::InitBoneCacheVisitor uses unordered_map::emplace,
            // which gives OpenMW first-match semantics for duplicate names. Match
            // that behavior so mod-authored duplicate transforms do not become a
            // V4-only incompatibility. Descendants still fold through this skipped
            // transform while searching for the nearest retained parent bone.
            if (names.find(folded) != names.end())
                continue;
            names.emplace(folded, payload->bones.size());

            std::vector<std::size_t> path;
            std::optional<std::size_t> parentBone;
            ModelNodeIndex cursor{ static_cast<std::uint32_t>(modelNode) };
            while (cursor.valid())
            {
                const std::size_t index = cursor.value();
                if (index != modelNode && modelToBone[index])
                {
                    parentBone = modelToBone[index];
                    break;
                }
                path.push_back(index);
                cursor = base.payload->nodes[index].parent;
            }

            glm::mat4 bindLocal(1.0f);
            for (auto it = path.rbegin(); it != path.rend(); ++it)
                bindLocal *= base.payload->nodes[*it].localTransform;
            const glm::mat4 global = parentBone ? globalBind[*parentBone] * bindLocal : bindLocal;
            const float determinant = glm::determinant(global);
            if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8f)
            {
                result.diagnostic = "forced actor skeleton contains a non-invertible bind transform at bone: "
                    + source.name;
                return result;
            }

            BoneRecord bone;
            bone.name = folded;
            bone.parent = parentBone ? static_cast<std::int32_t>(*parentBone) : -1;
            bone.bindLocal = bindLocal;
            bone.inverseBind = glm::inverse(global);
            modelToBone[modelNode] = payload->bones.size();
            payload->bones.push_back(std::move(bone));
            globalBind.push_back(global);
        }

        if (!validSkeletonPayload(*payload))
        {
            result.diagnostic = payload->bones.empty()
                ? "forced actor skeleton found no named transform bones in the base model"
                : "forced actor skeleton violates the neutral skeleton contract";
            return result;
        }

        if (sourceIdentity.empty())
            sourceIdentity = base.sourceIdentity + "#forced-actor-skeleton";
        result.record.sourceIdentity = std::move(sourceIdentity);
        result.record.payload = std::move(payload);
        return result;
    }

    struct ComposedActorModel
    {
        RenderCore::ModelRecord record;
        std::string diagnostic;

        [[nodiscard]] bool valid() const noexcept { return record.payload && diagnostic.empty(); }
    };

    // Compose OpenMW's selected NPC body/equipment parts without importing an
    // OSG graph. Skeleton nodes come from the winning base actor NIF; each part
    // reuses already-published mesh/material handles and is rebound by authored
    // bone name, matching SceneUtil::attach's source-side ownership.
    [[nodiscard]] inline ComposedActorModel composeActorModel(const RenderCore::RenderWorld& world,
        RenderCore::ModelHandle baseModel, RenderCore::SkeletonHandle skeletonHandle,
        const std::vector<ActorPartModelSource>& parts, std::string sourceIdentity)
    {
        using namespace RenderCore;
        ComposedActorModel result;
        const ModelRecord* base = world.get(baseModel);
        const SkeletonRecord* skeleton = world.get(skeletonHandle);
        if (!base || !base->payload || !skeleton || !skeleton->payload)
        {
            result.diagnostic = "actor composition is missing its base model or skeleton";
            return result;
        }

        const auto equalFolded = [](std::string_view left, std::string_view right) {
            return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
                [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
        };
        const auto isBone = [&](std::string_view name) {
            return std::any_of(skeleton->payload->bones.begin(), skeleton->payload->bones.end(),
                [&](const BoneRecord& bone) { return equalFolded(bone.name, name); });
        };

        auto payload = std::make_shared<ModelPayload>();
        std::vector<bool> retain(base->payload->nodes.size(), false);
        for (std::size_t i = 0; i < base->payload->nodes.size(); ++i)
        {
            if (!isBone(base->payload->nodes[i].name))
                continue;
            for (ModelNodeIndex cursor{ static_cast<std::uint32_t>(i) }; cursor.valid();
                 cursor = base->payload->nodes[cursor.value()].parent)
                retain[cursor.value()] = true;
        }
        std::vector<ModelNodeIndex> baseMap(base->payload->nodes.size());
        for (std::size_t i = 0; i < base->payload->nodes.size(); ++i)
        {
            if (!retain[i])
                continue;
            ModelNodeRecord node = base->payload->nodes[i];
            const std::uint32_t unsupportedControllers = node.controllerFlags
                & ~modelControllerFlag(ModelControllerFlag::Transform);
            if (unsupportedControllers != 0 || node.kind == ModelNodeKind::Switch
                || node.kind == ModelNodeKind::Lod)
            {
                result.diagnostic
                    = "base NPC skeleton hierarchy requires unsupported controller or selection semantics";
                return result;
            }
            node.parent = node.parent.valid() ? baseMap[node.parent.value()] : ModelNodeIndex{};
            node.kind = ModelNodeKind::Transform;
            node.mesh.reset();
            node.materials.clear();
            node.activeSwitchChild.reset();
            node.lod.reset();
            node.billboard.reset();
            node.sort.reset();
            node.flags &= ~modelNodeFlag(ModelNodeFlag::Hidden);
            node.controllerFlags &= modelControllerFlag(ModelControllerFlag::Transform);
            if (node.controllerFlags == 0)
                node.flags &= ~modelNodeFlag(ModelNodeFlag::ControllerTarget);
            baseMap[i] = ModelNodeIndex{ static_cast<std::uint32_t>(payload->nodes.size()) };
            if (!node.parent.valid())
                payload->roots.push_back(baseMap[i]);
            payload->nodes.push_back(std::move(node));
        }

        const auto findBoneNode = [&](std::string_view name) -> std::optional<ModelNodeIndex> {
            for (std::size_t i = 0; i < payload->nodes.size(); ++i)
                if (equalFolded(payload->nodes[i].name, name))
                    return ModelNodeIndex{ static_cast<std::uint32_t>(i) };
            return std::nullopt;
        };
        for (const ActorPartModelSource& part : parts)
        {
            const ModelRecord* model = world.get(part.model);
            const std::optional<ModelNodeIndex> attachment = findBoneNode(part.attachmentBone);
            if (!model || !model->payload || !attachment)
            {
                result.diagnostic = "NPC part cannot resolve its published model or attachment bone";
                return result;
            }
            for (const ModelNodeRecord& node : model->payload->nodes)
            {
                const MeshRecord* mesh = node.mesh ? world.get(*node.mesh) : nullptr;
                if (!mesh || !mesh->skin)
                    continue;
                for (const SkinBoneBinding& binding : mesh->skin->bones)
                {
                    if (!isBone(binding.name))
                    {
                        result.diagnostic = "NPC part skin references a bone absent from the base skeleton";
                        return result;
                    }
                }
            }
            std::vector<ModelNodeIndex> remap(model->payload->nodes.size());
            for (std::size_t i = 0; i < model->payload->nodes.size(); ++i)
            {
                const ModelNodeRecord& source = model->payload->nodes[i];
                if (source.kind == ModelNodeKind::Switch || source.kind == ModelNodeKind::Lod)
                {
                    result.diagnostic = "NPC part switch/LOD composition requires an explicit remapping facet";
                    return result;
                }
                if (isBone(source.name) && source.kind != ModelNodeKind::Geometry)
                {
                    if (source.controllerFlags != 0)
                    {
                        result.diagnostic
                            = "NPC part bone controller cannot be merged into the authoritative base pose";
                        return result;
                    }
                    const std::optional<ModelNodeIndex> existing = findBoneNode(source.name);
                    if (!existing)
                    {
                        result.diagnostic = "NPC part references a bone absent from the base skeleton";
                        return result;
                    }
                    remap[i] = *existing;
                    continue;
                }

                ModelNodeRecord node = source;
                node.parent = source.parent.valid() ? remap[source.parent.value()] : *attachment;
                if (!node.parent.valid())
                {
                    result.diagnostic = "NPC part topology could not be rebound to the base skeleton";
                    return result;
                }
                if (!part.visible)
                    node.flags |= modelNodeFlag(ModelNodeFlag::Hidden);
                remap[i] = ModelNodeIndex{ static_cast<std::uint32_t>(payload->nodes.size()) };
                payload->nodes.push_back(std::move(node));
            }
        }

        if (!validModelPayloadStructure(*payload))
        {
            result.diagnostic = "composed NPC model violates the neutral topology contract";
            return result;
        }
        result.record.sourceIdentity = std::move(sourceIdentity);
        result.record.contentIdentity = "runtime:openmw-npc-composition";
        result.record.payload = std::move(payload);
        result.record.bounds = base->bounds;
        result.record.dynamicRequirements = base->dynamicRequirements;
        for (const ActorPartModelSource& part : parts)
        {
            if (const ModelRecord* model = world.get(part.model))
                result.record.dynamicRequirements |= model->dynamicRequirements;
        }
        return result;
    }
}

#endif
