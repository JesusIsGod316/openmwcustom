#ifndef OPENMW_RENDERCORE_ACTORPROGRAM_H
#define OPENMW_RENDERCORE_ACTORPROGRAM_H

#include "deformation.hpp"
#include <memory>
#include <span>
#include <map>

namespace RenderCore
{
    // Immutable, load-bound execution data, independent of either scenegraph.
    // The owner invalidates this program on model/skeleton/mesh revisions. It
    // retains published payloads, never an InstanceRecord or a live scene node.
    // Per-frame evaluation does not validate vertices, resolve bone names, or
    // rediscover geometry cancellation paths. Unknown bindings use the caller's
    // compatibility path rather than dropping geometry.
    class ActorProgram final
    {
    public:
        struct MeshBinding
        {
            struct InfluenceGroup
            {
                std::size_t representative = 0;
                std::vector<std::size_t> vertices;
            };
            ModelNodeIndex node;
            MeshHandle mesh;
            std::shared_ptr<const MeshPayload> source;
            std::shared_ptr<const SkinPayload> skin;
            std::shared_ptr<const MorphPayload> morphs;
            std::vector<std::size_t> bones;
            ModelNodeIndex cancellation;
            std::vector<InfluenceGroup> influenceGroups;
        };
        struct Pose
        {
            std::vector<glm::mat4> bones;
            std::vector<glm::mat4> nodes;
        };

        static std::shared_ptr<const ActorProgram> bind(const RenderWorld& world,
            ModelHandle modelHandle, SkeletonHandle skeletonHandle)
        {
            const auto* model = world.get(modelHandle);
            const auto* skeleton = world.get(skeletonHandle);
            if (!model || !model->payload || !skeleton || !skeleton->payload
                || !validSkeletonPayload(*skeleton->payload)) return {};
            auto program = std::shared_ptr<ActorProgram>(new ActorProgram);
            program->mModel = model->payload;
            program->mSkeleton = skeleton->payload;
            program->mSkeletonHandle = skeletonHandle;
            program->mNodeBones.assign(model->payload->nodes.size(), NoBone);
            for (std::size_t i = 0; i < model->payload->nodes.size(); ++i)
            {
                const auto& node = model->payload->nodes[i];
                if (node.parent.valid() && node.parent.value() >= i) return {};
                if (!node.mesh && node.kind != ModelNodeKind::Geometry)
                {
                    const auto bone = deformation_detail::findBone(*skeleton->payload, node.name);
                    if (bone) program->mNodeBones[i] = *bone;
                }
                if (!node.mesh) continue;
                const auto* mesh = world.get(*node.mesh);
                if (!mesh || !mesh->payload) return {};
                if (!mesh->skinned && !mesh->morphed) continue;
                if (!validMeshPayload(*mesh->payload)) return {};
                MeshBinding binding;
                binding.node = ModelNodeIndex{static_cast<std::uint32_t>(i)};
                binding.mesh = *node.mesh;
                binding.source = mesh->payload;
                // Preserve the compatibility evaluator's morph-first rule.
                if (mesh->morphed)
                {
                    if (!mesh->morphs || !validMorphPayload(*mesh->morphs, mesh->payload->positions.size())) return {};
                    binding.morphs = mesh->morphs;
                }
                else
                {
                    if (!mesh->skin || !validSkinPayload(*mesh->skin, mesh->payload->positions.size())) return {};
                    binding.skin = mesh->skin;
                    for (const auto& bone : mesh->skin->bones)
                    {
                        const auto index = deformation_detail::findBone(*skeleton->payload, bone.name);
                        if (!index) return {};
                        binding.bones.push_back(*index);
                    }
                    // RigGeometry already exploits repeated weight sets. Bind
                    // that equivalence once, retaining indices rather than a
                    // duplicate copy of every vertex's authored weights.
                    const auto less = [&](std::size_t left, std::size_t right) {
                        const auto& a = mesh->skin->vertexInfluences[left];
                        const auto& b = mesh->skin->vertexInfluences[right];
                        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                            [](const auto& x, const auto& y) {
                                return x.boneIndex != y.boneIndex ? x.boneIndex < y.boneIndex : x.weight < y.weight;
                            });
                    };
                    std::map<std::size_t, std::size_t, decltype(less)> groups(less);
                    for (std::size_t vertex = 0; vertex < mesh->skin->vertexInfluences.size(); ++vertex)
                    {
                        if (mesh->skin->vertexInfluences[vertex].empty()) continue;
                        const auto [group, inserted] = groups.emplace(vertex, binding.influenceGroups.size());
                        if (inserted) binding.influenceGroups.push_back({vertex, {}});
                        binding.influenceGroups[group->second].vertices.push_back(vertex);
                    }
                    // Near-unique smooth weights gain nothing from grouping.
                    // Bound metadata overhead rather than duplicating them.
                    if (binding.influenceGroups.size() > mesh->skin->vertexInfluences.size() / 2)
                        std::vector<MeshBinding::InfluenceGroup>().swap(binding.influenceGroups);
                    if (mesh->skin->geometryBindTransform)
                    {
                        binding.cancellation = node.parent;
                        if (!mesh->skin->rootBoneName.empty())
                            for (auto cursor = binding.node; cursor.valid(); cursor = model->payload->nodes[cursor.value()].parent)
                                if (equalBoneName(model->payload->nodes[cursor.value()].name, mesh->skin->rootBoneName))
                                    binding.cancellation = cursor;
                    }
                }
                program->mMeshes.push_back(std::move(binding));
            }
            return program;
        }

        bool evaluatePose(const SkeletonPoseState& input, Pose& output) const
        {
            if (input.skeleton != mSkeletonHandle || input.current.size() != mSkeleton->bones.size()) return false;
            output.bones.resize(input.current.size());
            for (std::size_t i = 0; i < input.current.size(); ++i)
            {
                const auto parent = mSkeleton->bones[i].parent;
                output.bones[i] = parent < 0 ? input.current[i]
                    : output.bones[static_cast<std::size_t>(parent)] * input.current[i];
                if (!semantic_detail::finite(output.bones[i])) return false;
            }
            output.nodes.resize(mModel->nodes.size());
            for (std::size_t i = 0; i < output.nodes.size(); ++i)
            {
                const auto& node = mModel->nodes[i];
                output.nodes[i] = mNodeBones[i] != NoBone ? output.bones[mNodeBones[i]]
                    : node.parent.valid() ? output.nodes[node.parent.value()] * node.localTransform : node.localTransform;
                if (!semantic_detail::finite(output.nodes[i])) return false;
            }
            return true;
        }

        const std::vector<MeshBinding>& meshes() const noexcept { return mMeshes; }

        bool deform(const MeshBinding& binding, const Pose& pose, const MorphWeightState* morph,
            MeshPayload& output) const
        {
            const auto& source = *binding.source;
            output.positions = source.positions;
            output.normals = source.normals;
            output.tangents = source.tangents;
            output.bitangents = source.bitangents;
            if (binding.morphs)
            {
                if (!morph || morph->current.size() != binding.morphs->targets.size()) return false;
                for (std::size_t target = 0; target < morph->current.size(); ++target)
                {
                    const float weight = morph->current[target];
                    if (!std::isfinite(weight)) return false;
                    if (weight == 0.0f) continue;
                    const auto& offsets = binding.morphs->targets[target].positionOffsets;
                    for (std::size_t vertex = 0; vertex < output.positions.size(); ++vertex)
                        output.positions[vertex] += offsets[vertex] * weight;
                }
                return true;
            }
            if (!binding.skin || pose.bones.size() != mSkeleton->bones.size()
                || pose.nodes.size() != mModel->nodes.size()) return false;
            const auto& skin = *binding.skin;
            glm::mat4 skinTransform = skin.meshToSkeleton;
            if (skin.geometryBindTransform)
            {
                skinTransform = *skin.geometryBindTransform;
                if (binding.cancellation.valid())
                {
                    const auto& matrix = pose.nodes[binding.cancellation.value()];
                    const float determinant = glm::determinant(matrix);
                    if (!std::isfinite(determinant) || std::abs(determinant) <= 1e-8f) return false;
                    skinTransform *= glm::inverse(matrix);
                }
            }
            std::vector<glm::mat4> palette(binding.bones.size());
            for (std::size_t i = 0; i < palette.size(); ++i)
                palette[i] = pose.bones[binding.bones[i]] * skin.bones[i].inverseBind;
            const auto blend = [&](const std::vector<SkinInfluence>& influences) {
                glm::mat4 blended(0.0f);
                for (const auto& influence : influences)
                    blended += palette[influence.boneIndex] * influence.weight;
                // Do not normalize/truncate authored weights. RigGeometry
                // blends affine rows and restores homogeneous w independently.
                for (int column = 0; column != 4; ++column) blended[column][3] = column == 3 ? 1.f : 0.f;
                return skinTransform * blended;
            };
            const auto apply = [&](std::size_t vertex, const glm::mat4& blended) {
                output.positions[vertex] = glm::vec3(blended * glm::vec4(source.positions[vertex], 1));
                if (!source.normals.empty()) output.normals[vertex] = glm::mat3(blended) * source.normals[vertex];
                if (!source.tangents.empty())
                {
                    output.tangents[vertex] = glm::mat3(blended) * source.tangents[vertex];
                    output.bitangents[vertex] = glm::mat3(blended) * source.bitangents[vertex];
                }
            };
            if (!binding.influenceGroups.empty())
                for (const auto& group : binding.influenceGroups)
                {
                    const auto matrix = blend(skin.vertexInfluences[group.representative]);
                    for (const auto vertex : group.vertices) apply(vertex, matrix);
                }
            else
                for (std::size_t vertex = 0; vertex < output.positions.size(); ++vertex)
                    if (!skin.vertexInfluences[vertex].empty()) apply(vertex, blend(skin.vertexInfluences[vertex]));
            return true;
        }

    private:
        static constexpr std::size_t NoBone = std::numeric_limits<std::size_t>::max();
        SkeletonHandle mSkeletonHandle;
        std::shared_ptr<const ModelPayload> mModel;
        std::shared_ptr<const SkeletonPayload> mSkeleton;
        std::vector<std::size_t> mNodeBones;
        std::vector<MeshBinding> mMeshes;
    };
}
#endif
