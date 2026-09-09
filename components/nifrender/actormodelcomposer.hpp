#ifndef OPENMW_COMPONENTS_NIFRENDER_ACTORMODELCOMPOSER_H
#define OPENMW_COMPONENTS_NIFRENDER_ACTORMODELCOMPOSER_H

#include <components/rendercore/renderworld.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NifRender
{
    struct ActorPartModelSource
    {
        RenderCore::ModelHandle model;
        std::string attachmentBone;
        bool visible = true;
    };

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
