#ifndef OPENMW_COMPONENTS_RENDERCORE_DEFORMATION_H
#define OPENMW_COMPONENTS_RENDERCORE_DEFORMATION_H

#include "framerenderstate.hpp"
#include "renderworld.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace RenderCore
{
    enum class DeformationStatus : std::uint8_t
    {
        Ready,
        NotDeformable,
        InvalidReference,
        MissingPose,
        MissingMorphWeights,
        IncompatibleSkeleton,
    };

    struct DeformedMeshPayload
    {
        DeformationStatus status = DeformationStatus::InvalidReference;
        std::vector<glm::vec3> positions;
        std::vector<glm::vec3> normals;
        std::vector<glm::vec3> tangents;
        std::vector<glm::vec3> bitangents;

        [[nodiscard]] bool ready() const noexcept { return status == DeformationStatus::Ready; }
    };

    namespace deformation_detail
    {
        [[nodiscard]] inline const SkeletonPoseState* findPose(
            const FrameRenderState& frame, InstanceHandle instance) noexcept
        {
            const auto found = std::find_if(frame.skeletonPoses().begin(), frame.skeletonPoses().end(),
                [&](const SkeletonPoseState& value) { return value.instance == instance; });
            return found == frame.skeletonPoses().end() ? nullptr : &*found;
        }

        [[nodiscard]] inline const MorphWeightState* findMorph(const FrameRenderState& frame,
            InstanceHandle instance, MeshHandle mesh, std::optional<ModelNodeIndex> modelNode) noexcept
        {
            const auto found = std::find_if(frame.morphWeights().begin(), frame.morphWeights().end(),
                [&](const MorphWeightState& value) {
                    return value.instance == instance && value.mesh == mesh && value.modelNode == modelNode;
                });
            return found == frame.morphWeights().end() ? nullptr : &*found;
        }

        [[nodiscard]] inline std::optional<std::size_t> findBone(
            const SkeletonPayload& skeleton, std::string_view name) noexcept
        {
            for (std::size_t i = 0; i < skeleton.bones.size(); ++i)
            {
                const std::string& candidate = skeleton.bones[i].name;
                if (candidate.size() == name.size()
                    && std::equal(candidate.begin(), candidate.end(), name.begin(),
                        [](unsigned char left, unsigned char right) {
                            return std::tolower(left) == std::tolower(right);
                        }))
                    return i;
            }
            return std::nullopt;
        }

        [[nodiscard]] inline std::vector<glm::mat4> globalPose(
            const SkeletonPayload& skeleton, const std::vector<glm::mat4>& local)
        {
            std::vector<glm::mat4> result(local.size());
            for (std::size_t i = 0; i < local.size(); ++i)
            {
                const std::int32_t parent = skeleton.bones[i].parent;
                result[i] = parent < 0 ? local[i] : result[static_cast<std::size_t>(parent)] * local[i];
            }
            return result;
        }

        inline void transformDirection(glm::vec3& destination, const glm::vec3& source, const glm::mat4& matrix)
        {
            destination = glm::mat3(matrix) * source;
        }
    }

    // CPU compatibility deformation. This mirrors V3.25's serial RigGeometry
    // and MorphGeometry behavior and is the correctness fallback for CP3D.
    // Backend GPU skinning may consume the same immutable records later.
    [[nodiscard]] inline DeformedMeshPayload deformMesh(const RenderWorld& world, const FrameRenderState& frame,
        InstanceHandle instanceHandle, MeshHandle meshHandle, std::optional<ModelNodeIndex> modelNode = {},
        bool previous = false)
    {
        DeformedMeshPayload result;
        const InstanceRecord* instance = world.get(instanceHandle);
        const MeshRecord* mesh = world.get(meshHandle);
        if (!instance || !mesh || !mesh->payload || !validMeshPayload(*mesh->payload))
            return result;

        const MeshPayload& source = *mesh->payload;
        result.positions = source.positions;
        result.normals = source.normals;
        result.tangents = source.tangents;
        result.bitangents = source.bitangents;

        if (!mesh->skinned && !mesh->morphed)
        {
            result.status = DeformationStatus::NotDeformable;
            return result;
        }

        if (mesh->morphed)
        {
            const MorphWeightState* state
                = deformation_detail::findMorph(frame, instanceHandle, meshHandle, modelNode);
            if (!state || !mesh->morphs || state->current.size() != mesh->morphs->targets.size())
            {
                result.status = DeformationStatus::MissingMorphWeights;
                return result;
            }
            const std::vector<float>& weights = previous ? state->previous : state->current;
            for (std::size_t target = 0; target < mesh->morphs->targets.size(); ++target)
            {
                if (weights[target] == 0.0f)
                    continue;
                const auto& offsets = mesh->morphs->targets[target].positionOffsets;
                for (std::size_t vertex = 0; vertex < result.positions.size(); ++vertex)
                    result.positions[vertex] += offsets[vertex] * weights[target];
            }
            result.status = DeformationStatus::Ready;
            return result;
        }

        const SkeletonPoseState* pose = deformation_detail::findPose(frame, instanceHandle);
        const SkeletonRecord* skeleton = instance->skeleton ? world.get(*instance->skeleton) : nullptr;
        if (!pose)
        {
            result.status = DeformationStatus::MissingPose;
            return result;
        }
        if (!mesh->skin || !skeleton || !skeleton->payload || pose->skeleton != *instance->skeleton
            || pose->current.size() != skeleton->payload->bones.size())
        {
            result.status = DeformationStatus::IncompatibleSkeleton;
            return result;
        }

        const std::vector<glm::mat4>& local = previous ? pose->previous : pose->current;
        const std::vector<glm::mat4> global = deformation_detail::globalPose(*skeleton->payload, local);
        std::vector<glm::mat4> palette;
        palette.reserve(mesh->skin->bones.size());
        for (const SkinBoneBinding& binding : mesh->skin->bones)
        {
            const std::optional<std::size_t> index
                = deformation_detail::findBone(*skeleton->payload, binding.name);
            if (!index)
            {
                result.status = DeformationStatus::IncompatibleSkeleton;
                return result;
            }
            // Transpose OpenMW's row-vector RigGeometry order exactly:
            // inverseBind * boneInSkeleton * skinTransform.
            palette.push_back(mesh->skin->meshToSkeleton * global[*index] * binding.inverseBind);
        }

        for (std::size_t vertex = 0; vertex < result.positions.size(); ++vertex)
        {
            const auto& influences = mesh->skin->vertexInfluences[vertex];
            if (influences.empty())
                continue;

            glm::mat4 blended(0.0f);
            for (const SkinInfluence& influence : influences)
                blended += palette[influence.boneIndex] * influence.weight;

            result.positions[vertex] = glm::vec3(blended * glm::vec4(source.positions[vertex], 1.0f));
            if (!result.normals.empty())
                deformation_detail::transformDirection(result.normals[vertex], source.normals[vertex], blended);
            if (!result.tangents.empty())
            {
                deformation_detail::transformDirection(result.tangents[vertex], source.tangents[vertex], blended);
                deformation_detail::transformDirection(result.bitangents[vertex], source.bitangents[vertex], blended);
            }
        }
        result.status = DeformationStatus::Ready;
        return result;
    }
}

#endif
