#ifndef OPENMW_COMPONENTS_RENDERCORE_POSEDMODEL_H
#define OPENMW_COMPONENTS_RENDERCORE_POSEDMODEL_H

#include "records.hpp"
#include <algorithm>
#include <cctype>

namespace RenderCore
{
    inline bool equalBoneName(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
            [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    }

    // Geometry transforms are not skeleton bones, even if their names collide
    // with a bone. This mirrors the cleaned canonical skeleton's first match.
    inline std::vector<glm::mat4> posedModelTransforms(const ModelPayload& model,
        const SkeletonPayload& skeleton, const std::vector<glm::mat4>& globalBones)
    {
        std::vector<glm::mat4> result(model.nodes.size());
        for (std::size_t i = 0; i < model.nodes.size(); ++i)
        {
            const auto& node = model.nodes[i];
            if (!node.mesh && node.kind != ModelNodeKind::Geometry)
            {
                auto bone = std::find_if(skeleton.bones.begin(), skeleton.bones.end(),
                    [&](const BoneRecord& value) { return equalBoneName(value.name, node.name); });
                if (bone != skeleton.bones.end())
                {
                    result[i] = globalBones[static_cast<std::size_t>(bone - skeleton.bones.begin())];
                    continue;
                }
            }
            result[i] = node.parent.valid() ? result[node.parent.value()] * node.localTransform : node.localTransform;
        }
        return result;
    }

    // Column-vector counterpart of RigGeometry::updateSkinToSkelMatrix. Search
    // only the actual path, not similarly named nodes elsewhere in the model.
    inline std::optional<glm::mat4> geometrySkinTransform(const SkinPayload& skin,
        const ModelPayload& model, ModelNodeIndex geometry, const std::vector<glm::mat4>& nodeWorld)
    {
        if (!skin.geometryBindTransform || !geometry.valid() || geometry.value() >= model.nodes.size()
            || nodeWorld.size() != model.nodes.size())
            return std::nullopt;
        ModelNodeIndex cancellation = model.nodes[geometry.value()].parent;
        if (!skin.rootBoneName.empty())
        {
            // Walking inward-to-outward and keeping the last match implements
            // canonical outermost-first matching, including a named trishape.
            for (auto cursor = geometry; cursor.valid(); cursor = model.nodes[cursor.value()].parent)
                if (equalBoneName(model.nodes[cursor.value()].name, skin.rootBoneName)) cancellation = cursor;
        }
        if (!cancellation.valid()) return skin.geometryBindTransform;
        const auto& matrix = nodeWorld[cancellation.value()];
        const float determinant = glm::determinant(matrix);
        if (!semantic_detail::finite(matrix) || !std::isfinite(determinant) || std::abs(determinant) <= 1e-8f)
            return std::nullopt;
        return *skin.geometryBindTransform * glm::inverse(matrix);
    }
}
#endif
